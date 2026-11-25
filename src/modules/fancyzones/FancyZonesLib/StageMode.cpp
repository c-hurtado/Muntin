#include "pch.h"
#include "StageMode.h"

#include <algorithm>
#include <set>

#include <dwmapi.h>

#include <common/SettingsAPI/settings_helpers.h>
#include <common/logger/logger.h>
#include <common/utils/elevation.h>
#include <common/utils/json.h>

#include <FancyZonesLib/Arrange.h>
#include <FancyZonesLib/DesktopScroll.h>
#include <FancyZonesLib/ModuleConstants.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/StageDpi.h>
#include <FancyZonesLib/SettingsConstants.h>
#include <FancyZonesLib/WindowUtils.h>

namespace
{
    constexpr UINT TickMillis = 250;
    constexpr ULONGLONG ScanDelayMillis = 1500; // After Enter: windows the desktop scroll was still bringing back
    constexpr ULONGLONG RelayoutDelayMillis = 500; // After a display change: the monitors settle first
    constexpr ULONGLONG VerifyRetryMillis = 400; // A size was seen once: look again before believing it
    constexpr ULONGLONG VerifyDelayMillis = 600; // After a landing: look at how the windows really came out
    constexpr ULONGLONG GuardMillis = 500; // Modal dialog check
    constexpr ULONGLONG OrphanMillis = 1500;
    constexpr int MaxOrphanTries = 20;
    constexpr int ScansAfterEnter = 4; // x ScanRepeatMillis: stragglers (windows still coming back, apps that were busy)
    constexpr ULONGLONG ScanRepeatMillis = 1000;
    constexpr ULONGLONG RehomeDelayMillis = 300; // After a move: wait for it to settle
    constexpr ULONGLONG RehomePollMillis = 1000; // Moves that raise no event are found within this
    constexpr ULONGLONG LeaveSettleMillis = 1000; // Restores are async: Enter must not capture a window that is still on its way back
    constexpr ULONGLONG ReturnWaitMillis = 3000; // How long Enter waits for the desktop scroll to return its windows
    constexpr ULONGLONG GraceMillis = 2000; // After a move of ours that is not the layout settling (display change)
    constexpr int PendingAttempts = 20; // x TickMillis: a new window may start hidden or untitled
    constexpr size_t MaxPending = 64;
    constexpr int MaxLearned = 3;
    constexpr ULONGLONG UnmaximizeMillis = 3000; // How long a window gets to leave the maximized state
    constexpr DWORD MinSizeTimeoutMillis = 150;
    constexpr DWORD HitTestTimeoutMillis = 50;
    constexpr UINT DragTickMillis = 40; // ~25 Hz live feedback while an edge is dragged

    using Rect = StageLayouts::LRect;

    struct MonitorData
    {
        HMONITOR handle{};
        Rect rect;
        Rect work;
    };

    Rect ToRect(const RECT& r)
    {
        return Rect(r.left, r.top, r.right, r.bottom);
    }

    std::vector<MonitorData> EnumMonitors()
    {
        std::vector<MonitorData> monitors;
        EnumDisplayMonitors(
            nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
                MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
                if (GetMonitorInfoW(monitor, &info))
                {
                    reinterpret_cast<std::vector<MonitorData>*>(data)->push_back({ monitor, ToRect(info.rcMonitor), ToRect(info.rcWork) });
                }

                return TRUE;
            },
            reinterpret_cast<LPARAM>(&monitors));
        return monitors;
    }

    std::wstring ClassOf(HWND window)
    {
        wchar_t cls[256]{};
        GetClassNameW(window, cls, ARRAYSIZE(cls));
        return cls;
    }

    bool IsCloaked(HWND window)
    {
        int cloaked{};
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        return cloaked != 0;
    }

    // Another process (StageProto) arranges this window and is alive
    bool MarkedByOther(HWND window)
    {
        const HANDLE property = GetPropW(window, Arrange::HandsOffProperty);
        return property && Arrange::PropertyHandsOff(property, Arrange::ProcessIsAlive);
    }

    std::int64_t Handle(HWND window)
    {
        return static_cast<std::int64_t>(reinterpret_cast<intptr_t>(window));
    }

    HWND Window(std::int64_t handle)
    {
        return reinterpret_cast<HWND>(static_cast<intptr_t>(handle));
    }

    StageState::Probe RealProbe()
    {
        return {
            [](std::int64_t h) { return IsWindow(Window(h)) != FALSE; },
            [](std::int64_t h) {
                DWORD pid{};
                GetWindowThreadProcessId(Window(h), &pid);
                return static_cast<std::uint32_t>(pid);
            },
            [](std::int64_t h) { return ClassOf(Window(h)); },
            [](std::int64_t h) { return IsHungAppWindow(Window(h)) != FALSE; },
        };
    }

    // Windows sets a size for the DPI of the window's monitor: a window moved to a monitor with another scale gets resized by the
    // WM_DPICHANGED that the move causes. The second SetWindowPos (queued after the first, on the window's thread) sets the exact rect.
    void MoveTo(HWND window, const Rect& outer, bool toBottom, HMONITOR target)
    {
        RECT now{};
        if (GetWindowRect(window, &now) && ToRect(now).Same(outer) && !toBottom)
        {
            return;
        }

        const bool crossing = target && MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != target;
        // Async: a window that hangs must not hang Muntin
        const UINT flags = SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (toBottom ? 0 : SWP_NOZORDER);
        SetWindowPos(window, toBottom ? HWND_BOTTOM : nullptr, outer.L, outer.T, outer.W(), outer.H(), flags);
        if (crossing)
        {
            SetWindowPos(window, toBottom ? HWND_BOTTOM : nullptr, outer.L, outer.T, outer.W(), outer.H(), flags);
        }
    }

    // Windows reuses handles and a dialog may sit on it: foreground root
    HWND ForegroundRoot()
    {
        const HWND fg = GetForegroundWindow();
        return fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    }

    std::vector<StageAdopt::DialogCandidate> DialogCandidates()
    {
        std::vector<StageAdopt::DialogCandidate> list;
        EnumWindows(
            [](HWND window, LPARAM data) -> BOOL {
                auto& candidates = *reinterpret_cast<std::vector<StageAdopt::DialogCandidate>*>(data);
                const HWND owner = GetWindow(window, GW_OWNER);
                if (!owner || !IsWindowVisible(window))
                {
                    return TRUE;
                }

                DWORD pid{};
                GetWindowThreadProcessId(window, &pid);
                StageAdopt::DialogCandidate c;
                c.window = window;
                c.owner = owner;
                c.visible = true;
                c.toolOrNoActivate = (GetWindowLongPtrW(window, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0;
                c.ownProcess = pid == GetCurrentProcessId();
                c.cloaked = IsCloaked(window);
                c.ownerEnabled = IsWindowEnabled(owner) != FALSE;
                candidates.push_back(c);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&list));
        return list;
    }
}

std::filesystem::path StageMode::StateFilePath()
{
    return PTSettingsHelper::get_module_save_folder_location(NonLocalizable::ModuleKey) + L"\\stage-state.txt";
}

StageMode::StageMode(HWND timerWindow, UINT_PTR timerId, UINT_PTR dragTimerId) :
    m_timerWindow(timerWindow), m_timerId(timerId), m_dragTimerId(dragTimerId)
{
}

StageMode::~StageMode()
{
    Leave();
}

#pragma region restore

bool StageMode::RestoreEntry(const StageState::Entry& saved, const std::vector<Rect>& monitors, bool minimized)
{
    const StageState::Entry entry = StageState::Sanitized(saved, monitors); // A rect from a coordinate mix-up is brought down to a monitor
    const HWND window = Window(entry.hwnd);
    if (!IsWindow(window))
    {
        return true;
    }

    if (IsHungAppWindow(window))
    {
        return false;
    }

    const bool usable = StageState::RectUsable(entry.rect, monitors);
    const auto putRect = [&] {
        const RECT target{ entry.rect.L, entry.rect.T, entry.rect.R, entry.rect.B };
        const bool crossing = MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST) != MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
        SetWindowPos(window, nullptr, entry.rect.L, entry.rect.T, entry.rect.W(), entry.rect.H(), flags);
        if (crossing)
        {
            SetWindowPos(window, nullptr, entry.rect.L, entry.rect.T, entry.rect.W(), entry.rect.H(), flags); // Exact size once the DPI changed
        }
    };

    // A window that its app has hidden stays hidden: only its rect goes back (a maximized one keeps "restore to maximized")
    const bool hidden = !IsWindowVisible(window);
    const auto plan = StageState::PlacementFor(entry, minimized, hidden);
    if (!plan.usePlacement)
    {
        if (usable)
        {
            putRect();
        }

        return true;
    }

    WINDOWPLACEMENT placement{ .length = sizeof(WINDOWPLACEMENT) };
    placement.flags = static_cast<UINT>(plan.flags); // Async: the call must not block on the app
    placement.showCmd = static_cast<UINT>(plan.showCmd);
    placement.ptMinPosition = { entry.minX, entry.minY };
    placement.ptMaxPosition = { entry.maxX, entry.maxY };
    placement.rcNormalPosition = { entry.normal.L, entry.normal.T, entry.normal.R, entry.normal.B };
    SetWindowPlacement(window, &placement);

    // A docked or snapped window: the placement rect is the un-snapped one, so the rect goes back too (queued after the
    // placement, same thread). Not when its monitor is gone: the clamping of SetWindowPlacement stays in charge then.
    if (!hidden && placement.showCmd == SW_SHOWNORMAL && usable)
    {
        putRect();
    }

    return true;
}

std::vector<StageState::Entry> StageMode::RestoreEntries(const std::vector<StageState::Entry>& entries)
{
    std::vector<Rect> monitors;
    for (const auto& m : EnumMonitors())
    {
        monitors.push_back(m.rect);
    }

    const auto plan = StageState::PlanRestore(entries, RealProbe());
    std::vector<StageState::Entry> remaining;
    for (size_t i = 0; i < entries.size(); i++)
    {
        switch (plan[i])
        {
        case StageState::RestoreAction::Skip:
            break;
        case StageState::RestoreAction::Keep:
            remaining.push_back(entries[i]);
            break;
        case StageState::RestoreAction::Restore:
            if (!RestoreEntry(entries[i], monitors, false))
            {
                remaining.push_back(entries[i]);
            }

            break;
        }
    }

    return remaining;
}

void StageMode::RecoverStateFile(const std::function<bool()>& otherInstanceRuns) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        const auto path = StateFilePath();
        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            return;
        }

        const auto initial = StageState::Read(path);
        const auto remaining = RestoreEntries(initial);

        // A new Muntin may have started meanwhile and written its own file: that one is not ours to overwrite or delete
        if ((otherInstanceRuns && otherInstanceRuns()) || StageState::Read(path) != initial)
        {
            return;
        }

        if (!StageState::Write(path, remaining))
        {
            Logger::error(L"Updating the stage state file failed");
        }
    }
    catch (...)
    {
        Logger::error(L"Recovering the stage windows failed");
    }
}

void StageMode::RetryOrphans()
{
    if (m_orphans.empty() || m_orphanTries >= MaxOrphanTries)
    {
        return; // Gave up: they stay in the state file for the watchdog and the next start
    }

    m_orphanTries++;
    m_orphans = RestoreEntries(m_orphans);
    WriteState();
    m_orphanAt = GetTickCount64() + OrphanMillis;
}

#pragma endregion

#pragma region helpers

std::vector<StageMode::Rect> StageMode::MonitorRects() const
{
    std::vector<Rect> rects;
    for (const auto& m : m_monitors)
    {
        rects.push_back(m.rect);
    }

    return rects;
}

StageLayouts::Spacing StageMode::CurrentSpacing() const
{
    const auto& s = FancyZonesSettings::settings();
    return StageLayouts::Spacing(s.dockGap, s.dockMarginLeft, s.dockMarginTop, s.dockMarginRight, s.dockMarginBottom);
}

void StageMode::Schedule(UINT millis)
{
    if (m_timerWindow)
    {
        SetTimer(m_timerWindow, m_timerId, millis, nullptr);
    }
}

void StageMode::RefreshMonitors()
{
    auto monitors = EnumMonitors();
    if (monitors.empty())
    {
        return; // Keep what is known: a transient state of the display change
    }

    m_monitors.clear();
    for (const auto& m : monitors)
    {
        m_monitors.push_back({ m.handle, m.rect, m.work });
    }

    std::vector<MonitorStage> next;
    for (const auto& monitor : m_monitors)
    {
        const auto old = std::find_if(m_stages.begin(), m_stages.end(), [&](const MonitorStage& s) { return s.monitor.rect.Same(monitor.rect); });
        if (old != m_stages.end())
        {
            next.push_back(std::move(*old));
            m_stages.erase(old);
            next.back().monitor = monitor;
        }
        else
        {
            MonitorStage stage;
            stage.monitor = monitor;
            stage.layout = StageConfig::MakeLayout(m_values);
            next.push_back(std::move(stage));
        }
    }

    // The windows of a monitor that is gone join the primary one
    auto primary = std::find_if(next.begin(), next.end(), [](const MonitorStage& s) { return s.monitor.rect.L <= 0 && 0 < s.monitor.rect.R && s.monitor.rect.T <= 0 && 0 < s.monitor.rect.B; });
    if (primary == next.end())
    {
        primary = next.begin();
    }

    for (auto& gone : m_stages)
    {
        primary->order.insert(primary->order.end(), gone.order.begin(), gone.order.end());
    }

    m_stages = std::move(next);
}

StageMode::MonitorStage* StageMode::StageOfRect(const Rect& monitorRect)
{
    for (auto& s : m_stages)
    {
        if (s.monitor.rect.Same(monitorRect))
        {
            return &s;
        }
    }

    return nullptr;
}

StageMode::MonitorStage* StageMode::StageFor(HWND window)
{
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (!monitor)
    {
        return nullptr;
    }

    for (auto& s : m_stages)
    {
        if (s.monitor.handle == monitor)
        {
            return &s;
        }
    }

    MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
    return GetMonitorInfoW(monitor, &info) ? StageOfRect(ToRect(info.rcMonitor)) : nullptr;
}

StageMode::Managed* StageMode::Find(HWND window)
{
    for (auto& [_, m] : m_windows)
    {
        if (m.window == window)
        {
            return &m;
        }
    }

    return nullptr;
}

std::vector<HWND> StageMode::TopLevelWindows() const
{
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND window, LPARAM data) -> BOOL {
            if (IsWindowVisible(window))
            {
                reinterpret_cast<std::vector<HWND>*>(data)->push_back(window);
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows)); // Front to back
    return windows;
}

StageAdopt::AdoptInfo StageMode::Describe(HWND window)
{
    StageAdopt::AdoptInfo info;
    info.isWindow = IsWindow(window) != FALSE;
    if (!info.isWindow)
    {
        return info;
    }

    info.topLevel = GetAncestor(window, GA_ROOT) == window;
    info.visible = IsWindowVisible(window) != FALSE;
    info.iconic = IsIconic(window) != FALSE;
    info.hasOwner = GetWindow(window, GW_OWNER) != nullptr;
    info.hung = IsHungAppWindow(window) != FALSE;
    info.style = GetWindowLongPtrW(window, GWL_STYLE);
    info.exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    info.cloaked = IsCloaked(window);

    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    info.ownProcess = pid == GetCurrentProcessId();

    // InternalGetWindowText sends no message, so a hung app can't block this
    wchar_t title[2]{};
    info.emptyTitle = InternalGetWindowText(window, title, ARRAYSIZE(title)) <= 0;
    info.cls = ClassOf(window);

    RECT rect{};
    GetWindowRect(window, &rect);
    info.width = rect.right - rect.left;
    info.height = rect.bottom - rect.top;
    info.onScreenPx = StageState::OnScreenPx(ToRect(rect), MonitorRects());
    info.monitorKnown = StageFor(window) != nullptr;
    info.elevatedBlocked = !is_process_elevated() && IsProcessOfWindowElevated(window);
    info.heldElsewhere = DesktopScroll::IsParked(window) || MarkedByOther(window);
    info.stuckMaximized = m_stuck.contains(window);
    if (info.stuckMaximized && !StageState::StillStuck(true, IsZoomed(window) != FALSE))
    {
        m_stuck.erase(window); // It restored by itself: eligible again
        info.stuckMaximized = false;
    }
    return info;
}

std::optional<StageState::Entry> StageMode::Capture(HWND window) const
{
    RECT rect{};
    WINDOWPLACEMENT placement{ .length = sizeof(WINDOWPLACEMENT) };
    DWORD pid{};
    if (!GetWindowRect(window, &rect) || !GetWindowPlacement(window, &placement) || !GetWindowThreadProcessId(window, &pid))
    {
        return std::nullopt;
    }

    StageState::Entry e;
    e.hwnd = Handle(window);
    e.pid = pid;
    e.rect = ToRect(rect);
    e.showCmd = static_cast<int>(placement.showCmd);
    e.flags = static_cast<int>(placement.flags);
    e.minX = placement.ptMinPosition.x;
    e.minY = placement.ptMinPosition.y;
    e.maxX = placement.ptMaxPosition.x;
    e.maxY = placement.ptMaxPosition.y;
    e.normal = ToRect(placement.rcNormalPosition);
    e.cls = ClassOf(window);
    return e;
}

std::vector<StageState::Entry> StageMode::AllOriginals() const
{
    std::vector<StageState::Entry> entries;
    for (const auto& [_, m] : m_windows)
    {
        entries.push_back(m.original);
    }

    entries.insert(entries.end(), m_orphans.begin(), m_orphans.end());
    return entries;
}

bool StageMode::WriteState()
{
    if (!StageState::Write(StateFilePath(), AllOriginals()))
    {
        Logger::error(L"Writing the stage state file failed");
        return false;
    }

    return true;
}

void StageMode::PublishHeld()
{
    std::set<HWND> held;
    for (const auto& [_, m] : m_windows)
    {
        held.insert(m.window);
    }

    Stage::SetHeld(std::move(held));
}

// The insets of the invisible frame and the window's own minimum size. A window that doesn't answer (or is slow) has no known
// minimum: VerifyLanding learns it from how the window comes out.
void StageMode::Prepare(Managed& m, const Rect& work)
{
    if (IsZoomed(m.window))
    {
        // Async: an app that hangs must not hang Muntin. The saved placement puts it back maximized. It is measured and placed
        // once it is no longer maximized (Land waits for that).
        ShowWindowAsync(m.window, SW_RESTORE);
        m.needMeasure = true;
        m.unmaximizeUntil = GetTickCount64() + UnmaximizeMillis;
        return;
    }

    Measure(m, work);
}

void StageMode::Measure(Managed& m, const Rect& work)
{
    m.needMeasure = false;
    RECT window{}, frame{};
    GetWindowRect(m.window, &window);
    if (SUCCEEDED(DwmGetWindowAttribute(m.window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
    {
        m.insets.L = (std::max)(0L, frame.left - window.left);
        m.insets.T = (std::max)(0L, frame.top - window.top);
        m.insets.R = (std::max)(0L, window.right - frame.right);
        m.insets.B = (std::max)(0L, window.bottom - frame.bottom);
    }

    MINMAXINFO info{};
    DWORD_PTR result{};
    if (SendMessageTimeoutW(m.window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&info), SMTO_ABORTIFHUNG | SMTO_BLOCK, MinSizeTimeoutMillis, &result))
    {
        // The app answers in its own DPI (a DPI-unaware app: 96): physical pixels of the monitor it is on
        UINT monitorDpi = 0, ignored = 0;
        const HMONITOR monitor = MonitorFromWindow(m.window, MONITOR_DEFAULTTONEAREST);
        if (!monitor || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &monitorDpi, &ignored)))
        {
            monitorDpi = 0;
        }

        const UINT windowDpi = GetDpiForWindow(m.window);
        const long long reportedW = StageDpi::ScaleBetween(info.ptMinTrackSize.x, monitorDpi, windowDpi);
        const long long reportedH = StageDpi::ScaleBetween(info.ptMinTrackSize.y, monitorDpi, windowDpi);

        // From the app: never trusted, always brought into the work area before the layout sees it
        m.minW = (std::max)(m.minW, StageState::VisibleMin(reportedW, m.insets.L, m.insets.R, work.W()));
        m.minH = (std::max)(m.minH, StageState::VisibleMin(reportedH, m.insets.T, m.insets.B, work.H()));
    }
}

#pragma endregion

#pragma region layout

StageLayouts::Plan StageMode::ComputePlan(MonitorStage& stage)
{
    std::vector<StageLayouts::LWin> wins;
    for (const int id : stage.order)
    {
        const auto& m = m_windows.at(id);
        wins.emplace_back(id, StageLayouts::WinFlags::None, m.minW, m.minH);
    }

    StageState::ClampMinSizes(wins, stage.monitor.work);
    StageLayouts::LayoutInput input;
    input.Work = stage.monitor.work;
    input.Spacing = CurrentSpacing();
    input.Wins = std::move(wins);
    return stage.layout->Compute(input);
}

void StageMode::Land(MonitorStage& stage, int skipId, bool dockedOnly)
{
    if (stage.order.empty())
    {
        return;
    }

    for (const int id : stage.order)
    {
        auto& m = m_windows.at(id);
        if (m.needMeasure && !IsZoomed(m.window) && IsWindow(m.window))
        {
            Measure(m, stage.monitor.work); // Its frame is only the real one once it is not maximized
        }
    }

    const auto plan = ComputePlan(stage);
    const auto monitors = MonitorRects();
    bool waiting = false;
    std::vector<int> stuck;
    const bool anyParked = !dockedOnly && std::any_of(plan.Items.begin(), plan.Items.end(), [](const StageLayouts::PlanItem& i) { return i.Role == StageLayouts::Role::Hidden; });
    const auto dialogs = anyParked ? StageAdopt::DialogOwners(DialogCandidates()) : std::set<HWND>{};

    bool guarded = false;
    for (const auto& item : plan.Items)
    {
        const auto found = m_windows.find(item.Id);
        if (found == m_windows.end())
        {
            continue;
        }

        auto& m = found->second;
        if (m.id == skipId || (m_drag && m_drag->id == m.id))
        {
            continue; // The user has it by the mouse: the stage does not fight the drag, the drop puts it in its slot
        }

        if (dockedOnly && item.Role != StageLayouts::Role::Main && item.Role != StageLayouts::Role::Docked)
        {
            continue;
        }

        if (!IsWindow(m.window) || IsHungAppWindow(m.window) || IsIconic(m.window))
        {
            continue; // Minimized windows are the user's: Windows tells us when they come back
        }

        if (IsZoomed(m.window))
        {
            // Still on its way out of maximized (we asked): look again soon. A window the user maximized is left alone.
            const auto step = StageState::UnmaximizeDecision(m.needMeasure, true, GetTickCount64(), m.unmaximizeUntil);
            if (step == StageState::Unmaximize::Wait)
            {
                waiting = true;
            }
            else if (step == StageState::Unmaximize::GiveUp)
            {
                stuck.push_back(m.id); // Never came out of maximized: it goes back as it was, and is not taken again until it is not maximized
            }

            continue;
        }

        if (item.Role == StageLayouts::Role::Main || item.Role == StageLayouts::Role::Docked)
        {
            m.guarded = false;
            MoveTo(m.window, StagePark::OuterRect(item.Rect, m.insets, stage.monitor.rect, monitors), false, stage.monitor.handle);
            if (m.parked)
            {
                SetWindowPos(m.window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS); // Back from behind everything
                m.parked = false;
            }
        }
        else if (dialogs.contains(m.window))
        {
            m.guarded = true; // Parked once its dialog is closed: alone, the dialog would float with nothing behind it
            guarded = true;
        }
        else
        {
            m.guarded = false;
            const auto park = StagePark::Park(plan.Park, m.insets, stage.monitor.rect, monitors);
            RECT where{};
            if (!(m.parked && GetWindowRect(m.window, &where) && ToRect(where).Same(park.outer)))
            {
                MoveTo(m.window, park.outer, !park.sliver, stage.monitor.handle); // Not again for a window that already sits there (HWND_BOTTOM is a z-order change)
            }

            m.parked = true;
        }
    }

    for (const int id : stuck)
    {
        const auto found = m_windows.find(id);
        if (found != m_windows.end())
        {
            m_stuck.insert(found->second.window);
            LogDecision(found->second.window, L"released: it would not leave the maximized state");
            Release(id, false, true);
        }
    }

    if (dockedOnly)
    {
        return; // A drag preview: no timers, no verification; the drop lands the whole stage
    }

    const auto now = GetTickCount64();
    if (waiting)
    {
        m_relayoutAt = now + TickMillis;
    }

    m_guardAt = guarded ? now + GuardMillis : m_guardAt;
    if (stage.layout->UsesMinSizes())
    {
        m_verifyAt = now + VerifyDelayMillis;
    }

    Schedule(TickMillis);
}

void StageMode::LandAll()
{
    for (auto& stage : m_stages)
    {
        Land(stage);
    }
}

// After the real windows landed: a docked window that came out bigger than its slot has a minimum size we did not know. Remember
// what it needs and plan again. Guards as in StageProto's VerifyTick: more than 2 px bigger, the same size seen on two checks in a
// row, never while the window is hidden, maximized, minimized, just moved by us (grace) or the left mouse button is down (a drag),
// at most MaxLearned times per window.
void StageMode::VerifyLanding()
{
    const auto now = GetTickCount64();
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
    {
        m_verifyAt = now + VerifyRetryMillis; // Mid-drag: not a minimum
        return;
    }

    bool lookAgain = false;
    for (auto& stage : m_stages)
    {
        if (stage.order.empty() || !stage.layout->UsesMinSizes())
        {
            continue;
        }

        const auto plan = ComputePlan(stage);
        bool again = false;
        for (const auto& item : plan.Items)
        {
            const auto found = m_windows.find(item.Id);
            if (found == m_windows.end() || (item.Role != StageLayouts::Role::Main && item.Role != StageLayouts::Role::Docked))
            {
                continue;
            }

            auto& m = found->second;
            RECT frame{};
            if (m.learned >= MaxLearned || !IsWindow(m.window) || IsHungAppWindow(m.window) || !IsWindowVisible(m.window) || IsIconic(m.window) || IsZoomed(m.window) || m.guarded ||
                FAILED(DwmGetWindowAttribute(m.window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
            {
                m.pending = {};
                continue;
            }

            if (now < m.grace)
            {
                m.pending = {};
                m_verifyAt = (std::max)(m_verifyAt, m.grace + 100); // Our own move may not have landed yet
                continue;
            }

            const auto verdict = StageState::VerifyLanding(frame.right - frame.left, frame.bottom - frame.top, item.Rect.W(), item.Rect.H(), m.pending);
            lookAgain = lookAgain || verdict.lookAgain;
            if (verdict.learnW)
            {
                m.minW = (std::max)(m.minW, StageState::ClampMin(static_cast<long long>(frame.right) - frame.left, stage.monitor.work.W()));
            }

            if (verdict.learnH)
            {
                m.minH = (std::max)(m.minH, StageState::ClampMin(static_cast<long long>(frame.bottom) - frame.top, stage.monitor.work.H()));
            }

            if (verdict.learnW || verdict.learnH)
            {
                m.learned++;
                again = true;
            }
        }

        if (again)
        {
            Land(stage); // Schedules the next look
        }
    }

    if (lookAgain)
    {
        m_verifyAt = (std::max)(m_verifyAt, now + VerifyRetryMillis);
    }
}

#pragma endregion

#pragma region windows

// Does the stage take this window? nullptr: yes (stage and original are set); else the reason it does not, which is logged once.
const wchar_t* StageMode::Consider(HWND window, MonitorStage*& stage, std::optional<StageState::Entry>& original)
{
    const wchar_t* reason = StageAdopt::SkipReason(Describe(window));
    if (!reason)
    {
        stage = StageFor(window);
        original = Capture(window);
        if (!stage || !original)
        {
            reason = L"its state could not be read";
        }
        else if (!StageConfig::HasRoom(stage->order.size(), StageConfig::CapacityOf(*stage->layout)))
        {
            reason = L"no room: the stage of its monitor is full"; // Left alone, not held, not parked. Scan takes it once there is room.
        }
    }

    if (reason)
    {
        LogDecision(window, reason);
    }

    return reason;
}

void StageMode::LogDecision(HWND window, const wchar_t* verdict)
{
    auto& last = m_logged[window];
    if (last == verdict)
    {
        return; // Once per decision, not once per look
    }

    last = verdict;
    wchar_t title[256]{};
    const int titleLength = InternalGetWindowText(window, title, ARRAYSIZE(title));
    Logger::info(L"Stage: hwnd={:x} class={} title={} chars: {}", reinterpret_cast<uintptr_t>(window), ClassOf(window), (std::max)(titleLength, 0), verdict);
}

bool StageMode::Adopt(HWND window)
{
    if (Find(window))
    {
        return false;
    }

    MonitorStage* stage = nullptr;
    std::optional<StageState::Entry> original;
    if (Consider(window, stage, original))
    {
        return false;
    }

    Managed m;
    m.id = m_nextId++;
    m.window = window;
    m.original = *original;
    const int id = m.id;
    m_windows.emplace(id, std::move(m));

    // The state file covers the window before anything touches it
    if (!WriteState())
    {
        m_windows.erase(id);
        LogDecision(window, L"the state file could not be written");
        return false;
    }

    PublishHeld();
    Prepare(m_windows.at(id), stage->monitor.work);
    LogDecision(window, L"adopted");

    // A window in front takes master; one that is not (uncloaked, found later) never takes the master slot or the focus
    if (ForegroundRoot() == window || stage->order.empty())
    {
        stage->layout->Add(stage->order, id, true);
    }
    else
    {
        stage->layout->Join(stage->order, id, false);
    }

    Land(*stage);
    return true;
}

// restore: the window goes back where it was (minimized: stays minimized there). Not for a window that is gone.
void StageMode::Release(int id, bool minimized, bool restore)
{
    const auto found = m_windows.find(id);
    if (found == m_windows.end())
    {
        return;
    }

    if (m_drag && m_drag->id == id)
    {
        StopDrag(true); // The window is leaving the stage: the preview of its drag goes with it
    }

    const auto original = found->second.original;
    MonitorStage* home = nullptr;
    for (auto& stage : m_stages)
    {
        if (std::find(stage.order.begin(), stage.order.end(), id) != stage.order.end())
        {
            stage.layout->Remove(stage.order, id);
            home = &stage;
        }
    }

    // The restore comes first: only once the window is back is it out of the file
    if (restore && !RestoreEntry(original, MonitorRects(), minimized))
    {
        m_orphans.push_back(original);
        m_orphanTries = 0;
        m_orphanAt = GetTickCount64() + OrphanMillis;
        Schedule(TickMillis);
    }

    m_logged.erase(found->second.window); // Adopted again later (restored from minimized): that is logged again
    m_windows.erase(found);
    WriteState();
    PublishHeld();
    if (home)
    {
        Land(*home); // The others fill the place
    }

    m_scanAt = GetTickCount64() + TickMillis; // A window that found no room may have room now
    Schedule(TickMillis);
}

int StageMode::MonitorIndex(const Rect& monitorRect) const
{
    for (size_t i = 0; i < m_monitors.size(); i++)
    {
        if (m_monitors[i].rect.Same(monitorRect))
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void StageMode::WindowMoved(HWND window) noexcept
{
    try
    {
        if (!m_active || !window || !Find(window))
        {
            return; // Only held windows: the debounce below is not armed by every move on the desktop
        }

        ArmRehome();
    }
    catch (...)
    {
    }
}

// Arms the debounce unless one is already counting down: a stream of events must not push the scan back (or reset the timer) each time
void StageMode::ArmRehome()
{
    if (!m_rehomeAt)
    {
        m_rehomeAt = GetTickCount64() + RehomeDelayMillis;
        Schedule(TickMillis);
    }
}

void StageMode::ScheduleRehome() noexcept
{
    try
    {
        if (m_active && m_stages.size() > 1)
        {
            ArmRehome();
        }
    }
    catch (...)
    {
    }
}

// A held window that is on another monitor than its stage's: it joins the stage of that monitor (port of StageProto's RehomeScan).
void StageMode::RehomeScan()
{
    const auto now = GetTickCount64();
    for (auto& from : m_stages)
    {
        const auto ids = from.order; // Rehome changes the order
        for (const int id : ids)
        {
            const auto found = m_windows.find(id);
            if (found == m_windows.end() || std::find(from.order.begin(), from.order.end(), id) == from.order.end())
            {
                continue;
            }

            auto& m = found->second;
            const bool usable = IsWindow(m.window) && IsWindowVisible(m.window) && !IsIconic(m.window) && !IsHungAppWindow(m.window);
            MonitorStage* to = usable ? StageFor(m.window) : nullptr;
            StageState::RehomeFacts facts;
            facts.usable = usable;
            facts.mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            facts.now = now;
            facts.grace = m.grace;
            facts.destinationKnown = to != nullptr;
            facts.sameStage = to == &from;
            facts.destinationHasRoom = to && StageConfig::HasRoom(to->order.size(), StageConfig::CapacityOf(*to->layout));
            if (facts.usable && to && !facts.sameStage && (facts.mouseDown || now < m.grace))
            {
                // Held back: tried again as soon as the drag is over or the grace has run out, never dropped
                m_rehomeAt = facts.mouseDown ? (m_rehomeAt ? m_rehomeAt : now + RehomeDelayMillis) : StageState::RearmRehome(m_rehomeAt, m.grace);
            }

            switch (StageState::DecideRehome(facts))
            {
            case StageState::RehomeAction::Move:
                Rehome(id, from, *to);
                break;
            case StageState::RehomeAction::Unmanage:
                Logger::info(L"Stage: rehome hwnd={:x} class={} from monitor {} to monitor {}: no room there, left where it is", reinterpret_cast<uintptr_t>(m.window), ClassOf(m.window),
                             MonitorIndex(from.monitor.rect), MonitorIndex(to->monitor.rect));
                Release(id, false, false);
                break;
            case StageState::RehomeAction::None:
                break;
            }
        }
    }
}

void StageMode::Rehome(int id, MonitorStage& from, MonitorStage& to)
{
    auto& m = m_windows.at(id);
    const bool wasDocked = StageState::WasDocked(*from.layout, from.order, id);

    // The saved original place follows the window (restore must not send it back), written BEFORE anything moves
    const auto before = m.original;
    m.original.rect = StageState::SaneRect(StageState::ShiftIntoMonitor(before.rect, from.monitor.rect, to.monitor.rect), { to.monitor.rect });
    m.original.normal = StageState::ShiftIntoMonitor(before.normal, from.monitor.rect, to.monitor.rect);
    if (!WriteState())
    {
        m.original = before; // Nothing has changed
        return;
    }

    from.layout->Remove(from.order, id); // The first window behind takes the freed place
    to.layout->Join(to.order, id, wasDocked); // The layout decides where: pair and master differ
    m.minW = m.minH = 0; // The minimum size depends on the DPI of the monitor: measured again
    m.learned = 0;
    m.pending = {};
    Prepare(m, to.monitor.work); // Measures; a maximized window (Aero Snap on the top edge) is restored first, and Land waits for that
    m.grace = GetTickCount64() + GraceMillis;
    Logger::info(L"Stage: rehome hwnd={:x} class={} from monitor {} to monitor {}: {}", reinterpret_cast<uintptr_t>(m.window), ClassOf(m.window), MonitorIndex(from.monitor.rect),
                 MonitorIndex(to.monitor.rect), wasDocked ? L"docked there" : L"behind the docked windows there");
    Land(from);
    Land(to);
}

void StageMode::Scan()
{
    for (const HWND window : TopLevelWindows())
    {
        if (!Find(window) && !m_pending.contains(window))
        {
            Adopt(window);
        }
    }
}

void StageMode::ProcessPending()
{
    for (auto it = m_pending.begin(); it != m_pending.end();)
    {
        if (!IsWindow(it->first) || Adopt(it->first) || Find(it->first) || --it->second <= 0)
        {
            it = m_pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void StageMode::WindowAppeared(HWND window) noexcept
{
    try
    {
        if (!m_active || !window || GetAncestor(window, GA_ROOT) != window || Find(window) || m_pending.size() >= MaxPending)
        {
            return;
        }

        m_pending[window] = PendingAttempts;
        Schedule(TickMillis);
    }
    catch (...)
    {
    }
}

void StageMode::WindowVisibility(HWND window, bool minimizing) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (!m_active)
        {
            return;
        }

        auto* m = Find(window);
        if (minimizing)
        {
            if (m)
            {
                Release(m->id, true, true);
            }

            return;
        }

        if (!m)
        {
            WindowAppeared(window); // Restored from minimized
        }
        else if (!IsWindowVisible(window) || IsCloaked(window))
        {
            Release(m->id, false, true); // Hidden by its app, or on another virtual desktop: it is adopted again when it is back
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: window visibility failed");
    }
}

void StageMode::WindowDestroyed(HWND window) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        m_pending.erase(window);
        m_stuck.erase(window);
        m_logged.erase(window); // The handle may be reused
        if (!m_active)
        {
            return;
        }

        if (auto* m = Find(window))
        {
            Release(m->id, false, false);
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: window destroyed failed");
    }
}

#pragma endregion

#pragma region mouse

StageMode::MonitorStage* StageMode::StageOfId(int id, int* index)
{
    for (auto& stage : m_stages)
    {
        const auto it = std::find(stage.order.begin(), stage.order.end(), id);
        if (it != stage.order.end())
        {
            if (index)
            {
                *index = static_cast<int>(it - stage.order.begin());
            }

            return &stage;
        }
    }

    return nullptr;
}

// Ends the drag bookkeeping: the timer, and (restoreLayout) the cut and heights the preview changed
void StageMode::StopDrag(bool restoreLayout)
{
    if (!m_drag)
    {
        return;
    }

    if (m_drag->timer && m_timerWindow && m_dragTimerId)
    {
        KillTimer(m_timerWindow, m_dragTimerId);
    }

    if (restoreLayout && m_drag->ctx)
    {
        if (auto* stage = StageOfId(m_drag->id))
        {
            StageMouse::Restore(*stage->layout, *m_drag->ctx);
        }
    }

    m_drag.reset();
}

void StageMode::DragStart(HWND window) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (!m_active || !window)
        {
            return;
        }

        StopDrag(true); // Whatever was left of an earlier drag
        const auto* m = Find(window);
        int index = -1;
        auto* stage = m ? StageOfId(m->id, &index) : nullptr;
        RECT rect{};
        if (!stage || !GetWindowRect(window, &rect))
        {
            return;
        }

        Drag drag;
        drag.id = m->id;
        drag.window = window;
        drag.start = ToRect(rect);
        // Border or not: asked of the window at the pointer. An app that does not answer quickly, or answers anything but a border, makes
        // it a move: never an edge drag.
        POINT cursor{};
        DWORD_PTR hit = HTCLIENT;
        if (GetCursorPos(&cursor) && SendMessageTimeoutW(window, WM_NCHITTEST, 0, MAKELPARAM(static_cast<WORD>(cursor.x), static_cast<WORD>(cursor.y)), SMTO_ABORTIFHUNG | SMTO_BLOCK, HitTestTimeoutMillis, &hit))
        {
            drag.intent = StageMouse::IntentFromHitTest(static_cast<long long>(static_cast<LRESULT>(hit)));
        }

        if (drag.intent == StageMouse::Intent::Size && !IsZoomed(window) && !IsIconic(window))
        {
            drag.ctx = StageMouse::Capture(*stage->layout, ComputePlan(*stage), index, static_cast<int>(stage->order.size()));
        }

        if (drag.ctx)
        {
            StageMouse::Take(*stage->layout, drag.applied);
            if (m_timerWindow && m_dragTimerId)
            {
                SetTimer(m_timerWindow, m_dragTimerId, DragTickMillis, nullptr);
                drag.timer = true;
            }
        }

        m_drag = std::move(drag);
    }
    catch (...)
    {
        Logger::error(L"Stage: drag start failed");
    }
}

// ~25 Hz while the button is down on an edge drag: the cut / stack heights follow the window's edge, the other windows are laid out
// around them. Nothing is logged or allocated per tick (the layout is only touched when its numbers changed).
void StageMode::OnDragTimer() noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (!m_drag || !m_active || !m_drag->ctx)
        {
            if (m_timerWindow && m_dragTimerId)
            {
                KillTimer(m_timerWindow, m_dragTimerId);
            }

            return;
        }

        Drag& drag = *m_drag;
        int index = -1;
        auto* stage = StageOfId(drag.id, &index);
        const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool valid = stage && StageMouse::StillValid(*drag.ctx, static_cast<int>(stage->order.size()), index);
        if (!stage || !buttonDown || !IsWindow(drag.window) || !valid)
        {
            // The preview ends (the drop event, if the button went up, applies the final layout); a stage that changed under the drag
            // invalidates the numbers taken at its start
            if (stage)
            {
                StageMouse::Restore(*stage->layout, *drag.ctx);
            }

            KillTimer(m_timerWindow, m_dragTimerId);
            drag.timer = false;
            if (stage && !valid)
            {
                drag.ctx.reset();
                Land(*stage);
            }

            return;
        }

        RECT rect{};
        if (!GetWindowRect(drag.window, &rect))
        {
            return;
        }

        // A move is not a resize; going back to the start size (Esc included) puts the layout back as it was
        const Rect now = ToRect(rect);
        StageMouse::ResizeEdge(*stage->layout, *drag.ctx, drag.start, StageMouse::IsResize(drag.start, now) ? now : drag.start);
        StageMouse::Take(*stage->layout, drag.probe);
        if (drag.probe == drag.applied)
        {
            return;
        }

        std::swap(drag.applied, drag.probe);
        Land(*stage, 0, true); // The dragged window is skipped; the other docked windows follow
    }
    catch (...)
    {
    }
}

void StageMode::DragEnd(HWND window) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (!m_active)
        {
            return;
        }

        if (!m_drag || m_drag->window != window)
        {
            WindowMoved(window); // Not a drag the stage followed: it may still have left for another monitor
            return;
        }

        const Drag drag = *m_drag;
        StopDrag(true); // Timer off, the preview's numbers back: the drop below decides from the numbers of the start
        auto* m = Find(window);
        auto* stage = m ? StageOfId(drag.id) : nullptr;
        RECT rect{};
        if (!stage || !IsWindow(window) || !GetWindowRect(window, &rect))
        {
            return;
        }

        const Rect now = ToRect(rect);
        StageMouse::DropFacts facts;
        facts.rectSameAsStart = now.Same(drag.start);
        facts.intent = drag.intent;
        if (!facts.rectSameAsStart)
        {
            facts.onOtherMonitor = StageFor(window) != stage;
            facts.zoomed = IsZoomed(window) != FALSE;
            facts.resized = StageMouse::IsResize(drag.start, now);
            if (!facts.onOtherMonitor && !facts.zoomed)
            {
                if (facts.resized && drag.ctx)
                {
                    facts.edgeChanged = StageMouse::ResizeEdge(*stage->layout, *drag.ctx, drag.start, now);
                }
                else if (!facts.resized)
                {
                    POINT cursor{};
                    GetCursorPos(&cursor);
                    facts.targetId = StageMouse::HitTest(ComputePlan(*stage), stage->order, drag.id, cursor.x, cursor.y);
                }
            }
        }

        const auto plan = StageMouse::PlanDrop(facts);
        if (plan.action != StageMouse::Drop::ResizeEdge && drag.ctx)
        {
            StageMouse::Restore(*stage->layout, *drag.ctx); // Nothing changes in the layout
        }

        if (plan.action != StageMouse::Drop::SnapBack || !facts.rectSameAsStart)
        {
            m->grace = GetTickCount64() + GraceMillis; // Our moves below are not a minimum size
        }

        switch (plan.action)
        {
        case StageMouse::Drop::ResizeEdge:
            // Saved to settings.json; every monitor's stage lays out around the new numbers
            Persist(StageConfig::Read(*stage->layout, m_values));
            break;
        case StageMouse::Drop::Swap:
            StageMouse::Swap(stage->order, drag.id, facts.targetId);
            Land(*stage);
            break;
        case StageMouse::Drop::Unmaximize:
            Prepare(*m, stage->monitor.work); // Restored first, Land waits for that and places it
            Land(*stage);
            break;
        case StageMouse::Drop::Rehome:
            m->grace = 0; // The rehome scan must not wait for a grace of ours
            Land(*stage, plan.skipDragged ? drag.id : 0); // The previewed neighbours go back; the window itself is the rehome's
            ArmRehome();
            break;
        case StageMouse::Drop::SnapBack:
            Land(*stage, plan.skipDragged ? drag.id : 0);
            break;
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: drop failed");
    }
}

#pragma endregion

#pragma region mode

void StageMode::Enter() noexcept
{
    try
    {
        StageDpi::Scope dpi;
        const auto now = GetTickCount64();

        // Entering again is a rescan: whatever is eligible and was missed is taken now
        if (m_active)
        {
            Scan();
            m_scansLeft = ScansAfterEnter;
            m_scanAt = now + ScanRepeatMillis;
            Schedule(TickMillis);
            return;
        }

        // Not while the restores of a Leave are still on their way, nor while the desktop scroll still brings its windows back:
        // a window captured now would be recorded at a place it is only passing through
        if (!m_enterSince)
        {
            m_enterSince = now;
        }

        if (now < m_enterNotBefore || (DesktopScroll::AnyParked() && now - m_enterSince < ReturnWaitMillis))
        {
            m_enterAt = now + TickMillis;
            Schedule(TickMillis);
            return;
        }

        m_enterSince = 0;
        m_enterAt = 0;
        m_logged.clear();
        m_stuck.clear();

        // What an earlier run that died left behind goes back first, and what can't (not responding) stays in the file
        m_orphans = RestoreEntries(StageState::Read(StateFilePath()));
        m_orphanTries = 0;
        m_orphanAt = GetTickCount64() + OrphanMillis; // Not stale: they were just tried
        m_values = StageConfig::FromSettings(FancyZonesSettings::settings());
        m_stages.clear();
        RefreshMonitors();
        if (m_stages.empty())
        {
            Logger::error(L"Stage: no monitors");
            WriteState();
            return;
        }

        // Who is taken: every eligible window, in z-order (front first), the foreground window first of all
        std::vector<std::pair<int, MonitorStage*>> taken;
        const HWND foreground = ForegroundRoot();
        auto windows = TopLevelWindows();
        const auto front = std::find(windows.begin(), windows.end(), foreground);
        if (front != windows.end())
        {
            std::rotate(windows.begin(), front, front + 1); // The window in front is taken first: it must not miss out
        }

        for (const HWND window : windows)
        {
            MonitorStage* stage = nullptr;
            std::optional<StageState::Entry> original;
            if (Consider(window, stage, original))
            {
                continue; // The reason is in the log
            }

            Managed m;
            m.id = m_nextId++;
            m.window = window;
            m.original = *original;
            taken.emplace_back(m.id, stage);
            const int id = m.id;
            m_windows.emplace(id, std::move(m));
            stage->order.push_back(id);
            LogDecision(window, L"adopted");
        }

        Logger::info(L"Stage: entering with {} of {} windows on {} monitors (foreground hwnd={:x} class={})", taken.size(), windows.size(), m_stages.size(), reinterpret_cast<uintptr_t>(foreground), foreground ? ClassOf(foreground) : std::wstring(L"none"));

        // BEFORE the first move: the state file lists every window and the place it has now
        if (!WriteState())
        {
            m_windows.clear();
            for (auto& stage : m_stages)
            {
                stage.order.clear();
            }

            return;
        }

        m_active = true;
        PublishHeld();
        for (auto& [id, stage] : taken)
        {
            Prepare(m_windows.at(id), stage->monitor.work);
        }

        LandAll();
        m_scansLeft = ScansAfterEnter;
        m_scanAt = now + ScanDelayMillis;
        m_pollAt = now + RehomePollMillis;
        Schedule(TickMillis);
    }
    catch (...)
    {
        Logger::error(L"Stage: entering failed");
    }
}

void StageMode::Leave() noexcept
{
    try
    {
        StageDpi::Scope dpi;
        m_enterAt = 0; // A pending Enter is cancelled, whether or not the stage is up
        m_enterSince = 0;
        if (!m_active)
        {
            return;
        }

        m_active = false;
        m_keyTracker.Reset();
        StopDrag(false);
        if (m_timerWindow)
        {
            KillTimer(m_timerWindow, m_timerId);
        }

        m_pending.clear();
        m_scanAt = m_relayoutAt = m_verifyAt = m_guardAt = m_rehomeAt = 0;
        m_scansLeft = 0;

        // Everything goes back; what can't (not responding) stays in the file
        m_orphanTries = 0;
        m_orphans = RestoreEntries(AllOriginals());
        m_windows.clear();
        m_stages.clear();
        WriteState(); // Deletes the file when nothing is left
        PublishHeld();
        m_enterNotBefore = GetTickCount64() + LeaveSettleMillis; // The restores are async: a quick toggle back waits for them
        Logger::info(L"Stage: left, windows restored");
        if (!m_orphans.empty())
        {
            m_orphanAt = GetTickCount64() + OrphanMillis;
            Schedule(OrphanMillis);
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: leaving failed");
    }
}

void StageMode::DisplayChanged() noexcept
{
    if (m_active)
    {
        m_relayoutAt = GetTickCount64() + RelayoutDelayMillis;
        Schedule(TickMillis);
    }
}

void StageMode::OnTimer() noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (m_timerWindow)
        {
            KillTimer(m_timerWindow, m_timerId);
        }

        const auto now = GetTickCount64();
        if (!m_active)
        {
            if (m_enterAt && now >= m_enterAt)
            {
                m_enterAt = 0;
                if (Arrange::IsStage())
                {
                    Enter(); // Was waiting for a Leave or the desktop scroll to finish; it schedules what it needs itself
                    if (m_active)
                    {
                        return;
                    }
                }
            }

            if (!m_orphans.empty() && m_orphanTries < MaxOrphanTries && now >= m_orphanAt)
            {
                RetryOrphans();
            }

            // The earliest of what is due: a pending Enter, or the next orphan retry (which stops after MaxOrphanTries)
            ULONGLONG next = 0;
            if (m_enterAt)
            {
                next = m_enterAt;
            }

            if (!m_orphans.empty() && m_orphanTries < MaxOrphanTries)
            {
                next = next ? (std::min)(next, m_orphanAt) : m_orphanAt;
            }

            if (next)
            {
                const ULONGLONG wait = next > now ? next - now : 1;
                Schedule(static_cast<UINT>(wait < OrphanMillis ? wait : OrphanMillis));
            }

            return;
        }

        if (m_relayoutAt && now >= m_relayoutAt)
        {
            m_relayoutAt = 0;
            RefreshMonitors();
            for (auto& [_, m] : m_windows)
            {
                m.grace = now + GraceMillis; // The moves below are ours: VerifyLanding must not take their transit for a minimum size
            }

            LandAll();
            m_rehomeAt = now + TickMillis; // Windows of a monitor that went away or came: see where each one is
        }

        if (!m_pending.empty())
        {
            ProcessPending();
        }

        if (m_stages.size() > 1 && now >= m_pollAt)
        {
            m_pollAt = now + RehomePollMillis;
            if (!m_rehomeAt)
            {
                m_rehomeAt = now; // Cheap: one monitor lookup per held window
            }
        }

        if (m_rehomeAt && now >= m_rehomeAt)
        {
            m_rehomeAt = 0;
            if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)
            {
                m_rehomeAt = now + RehomeDelayMillis; // A drag is still going on: look again shortly
            }
            else
            {
                RehomeScan();
            }
        }

        if (m_scanAt && now >= m_scanAt)
        {
            m_scanAt = 0;
            Scan();
            if (--m_scansLeft > 0)
            {
                m_scanAt = now + ScanRepeatMillis; // Stragglers: windows that were not ready, or still on their way back
            }
        }

        if (m_guardAt && now >= m_guardAt)
        {
            m_guardAt = 0;
            const bool dialogOpen = std::any_of(m_windows.begin(), m_windows.end(), [](const auto& item) { return item.second.guarded; });
            if (dialogOpen)
            {
                const auto owners = StageAdopt::DialogOwners(DialogCandidates());
                const bool closed = std::any_of(m_windows.begin(), m_windows.end(), [&](const auto& item) { return item.second.guarded && !owners.contains(item.second.window); });
                if (closed)
                {
                    LandAll(); // Land marks the ones that still have a dialog and watches again
                }
                else
                {
                    m_guardAt = now + GuardMillis;
                }
            }
        }

        if (m_verifyAt && now >= m_verifyAt)
        {
            m_verifyAt = 0;
            VerifyLanding();
        }

        if (!m_orphans.empty() && now >= m_orphanAt)
        {
            RetryOrphans();
        }

        if (m_active && m_stages.size() > 1 && !m_scanAt && !m_rehomeAt && m_pending.empty() && !m_relayoutAt && !m_guardAt && !m_verifyAt)
        {
            Schedule(static_cast<UINT>(RehomePollMillis)); // Idle: the slow poll is all that is left
        }

        if (!m_pending.empty() || m_scanAt || m_rehomeAt || m_relayoutAt || m_guardAt || m_verifyAt || (!m_orphans.empty() && m_orphanTries < MaxOrphanTries))
        {
            Schedule(TickMillis);
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: timer failed");
    }
}

#pragma endregion

#pragma region keys

StageKeys::ForegroundFacts StageMode::ForegroundOf(HWND foreground)
{
    StageKeys::ForegroundFacts facts;
    const HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    facts.window = reinterpret_cast<uintptr_t>(root);
    if (!root)
    {
        facts.transient = true; // No foreground window while focus moves: not the user leaving
        return facts;
    }

    facts.held = Find(root) != nullptr && !IsIconic(root); // A minimized window is not held for the keys
    if (!facts.held)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(root, &pid);
        wchar_t cls[128]{};
        GetClassNameW(root, cls, ARRAYSIZE(cls));
        facts.transient = StageKeys::IsTransient(cls, static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE)), pid == GetCurrentProcessId());
    }

    return facts;
}

void StageMode::NoteForeground(HWND foreground) noexcept
{
    try
    {
        if (m_active)
        {
            m_keyTracker.Note(ForegroundOf(foreground));
        }
    }
    catch (...)
    {
    }
}

HWND StageMode::KeyTarget() noexcept
{
    try
    {
        if (!m_active || m_drag)
        {
            return nullptr;
        }

        const auto facts = ForegroundOf(GetForegroundWindow());
        m_keyTracker.Note(facts);
        return reinterpret_cast<HWND>(m_keyTracker.Resolve(facts, [&](uintptr_t window) {
            const auto hwnd = reinterpret_cast<HWND>(window);
            return IsWindow(hwnd) && !IsIconic(hwnd) && Find(hwnd) != nullptr;
        }));
    }
    catch (...)
    {
        return nullptr;
    }
}

void StageMode::KeyAction(StageKeys::Action action, HWND target) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        using StageKeys::Action;
        if (!m_active || m_drag)
        {
            return;
        }

        if (action == Action::CycleLayout)
        {
            const auto next = StageLayouts::NextLayoutName(StageConfig::Narrow(m_values.layout));
            const std::wstring name(next.begin(), next.end());
            Logger::info(L"Stage: key {}: {}", StageKeys::Name(action), name);
            SetLayoutName(name); // Saved to settings.json; the master layout runs until the strip is drawn (Phase 4)
            return;
        }

        auto* m = Find(target);
        MonitorStage* stage = m ? StageOfId(m->id) : nullptr;
        if (!m || !stage || !IsWindow(m->window) || IsIconic(m->window))
        {
            return; // Never act on a minimized window
        }

        if (action == Action::MoveToPrevMonitor || action == Action::MoveToNextMonitor)
        {
            std::vector<StageKeys::MonitorPos> positions;
            int from = -1;
            for (size_t i = 0; i < m_stages.size(); i++)
            {
                positions.push_back({ m_stages[i].monitor.rect.L, m_stages[i].monitor.rect.T });
                if (&m_stages[i] == stage)
                {
                    from = static_cast<int>(i);
                }
            }

            const int to = StageKeys::NextMonitor(positions, from, action == Action::MoveToNextMonitor ? 1 : -1);
            if (to < 0 || to == from)
            {
                return; // One monitor: nothing to do
            }

            auto& destination = m_stages[to];
            if (!StageConfig::HasRoom(destination.order.size(), StageConfig::CapacityOf(*destination.layout)))
            {
                Logger::info(L"Stage: key {} hwnd={:x}: no room on monitor {}", StageKeys::Name(action), reinterpret_cast<uintptr_t>(m->window), to);
                return;
            }

            Logger::info(L"Stage: key {} hwnd={:x} class={}", StageKeys::Name(action), reinterpret_cast<uintptr_t>(m->window), ClassOf(m->window));
            Rehome(m->id, *stage, destination); // Writes the state file (original place shifted) before anything moves
            return;
        }

        const auto iconic = [&](int id) {
            const auto it = m_windows.find(id);
            return it == m_windows.end() || !IsWindow(it->second.window) || IsIconic(it->second.window);
        };
        bool changed = false;
        switch (action)
        {
        case Action::RotateBack:
        case Action::RotateForward:
        {
            const int dir = action == Action::RotateBack ? -1 : 1;
            changed = StageKeys::RotateToLive(
                stage->order, [&](std::vector<int>& o) { return stage->layout->Rotate(o, dir); }, [&](int id) { return !iconic(id); }); // A minimized window does not take the master slot
            break;
        }
        case Action::Promote:
            changed = stage->layout->Promote(stage->order, m->id);
            break;
        case Action::Demote:
            changed = stage->layout->Demote(stage->order, m->id);
            break;
        default:
            return;
        }

        if (!changed)
        {
            return;
        }

        Logger::info(L"Stage: key {} hwnd={:x} class={}", StageKeys::Name(action), reinterpret_cast<uintptr_t>(m->window), ClassOf(m->window));
        Land(*stage); // The order is not in the state file: no write
        const int focusId = action == Action::Promote ? m->id : StageKeys::FocusChoice(stage->order, iconic);
        const auto focus = m_windows.find(focusId);
        if (focusId != 0 && focus != m_windows.end() && IsWindow(focus->second.window) && !IsIconic(focus->second.window) && focus->second.window != GetForegroundWindow())
        {
            FancyZonesWindowUtils::SwitchToWindow(focus->second.window); // The new master (a promoted window keeps the focus)
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: key action failed");
    }
}

#pragma endregion

#pragma region settings

void StageMode::ApplyValues(const StageConfig::Values& values)
{
    m_values = values;
    for (auto& stage : m_stages)
    {
        stage.layout = StageConfig::MakeLayout(m_values);
    }

    LandAll();
}

void StageMode::SettingsChanged() noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (m_active)
        {
            ApplyValues(StageConfig::FromSettings(FancyZonesSettings::settings())); // Also the gap and the margins
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: settings changed failed");
    }
}

void StageMode::Persist(const StageConfig::Values& after)
{
    for (const auto& change : StageConfig::Changes(m_values, after))
    {
        const bool saved = FancyZonesSettings::SaveValue(change.id, change.text ? json::value(change.textValue) : json::value(change.number));
        if (!saved)
        {
            Logger::warn(L"Stage: a setting could not be saved");
        }
    }

    ApplyValues(after);
}

void StageMode::SetLayoutName(const std::wstring& name) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        const auto& names = StageLayouts::LayoutNames();
        if (m_active && std::find(names.begin(), names.end(), StageConfig::Narrow(name)) != names.end())
        {
            auto after = m_values;
            after.layout = name;
            Persist(after);
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: layout change failed");
    }
}

void StageMode::SetMasterCut(double ratio) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (m_active)
        {
            auto after = m_values;
            after.masterCut = StageConfig::ToPermille(ratio);
            Persist(after);
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: cut change failed");
    }
}

void StageMode::SetRows(const std::vector<double>& rows) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (m_active)
        {
            auto after = m_values;
            after.rows = Arrange::FormatRows(rows);
            Persist(after);
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: rows change failed");
    }
}

#pragma endregion
