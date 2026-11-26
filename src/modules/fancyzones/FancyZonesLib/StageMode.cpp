#include "pch.h"
#include "StageMode.h"

#include <algorithm>
#include <limits>
#include <set>

#include <dwmapi.h>

#include <common/SettingsAPI/settings_helpers.h>
#include <common/logger/logger.h>
#include <common/utils/elevation.h>
#include <common/utils/json.h>

#include <FancyZonesLib/Arrange.h>
#include <FancyZonesLib/DesktopScroll.h>
#include <FancyZonesLib/ModuleConstants.h>
#include <FancyZonesLib/MonitorUtils.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/StageDpi.h>
#include <FancyZonesLib/StageStrip.h>
#include <FancyZonesLib/FancyZonesWinHookEventIDs.h>
#include <FancyZonesLib/SettingsConstants.h>
#include <FancyZonesLib/WindowUtils.h>

namespace
{
    constexpr UINT TickMillis = 250;
    constexpr ULONGLONG ReplanDelayMillis = 300; // a held window changed its shape: its strip tile is planned again once it has settled
    constexpr ULONGLONG NewWindowMinAgeMillis = 400; // a new window is looked at once it has existed this long (a capture overlay lives shorter)
    constexpr ULONGLONG ActivateDebounceMillis = 130; // Alt-Tab cycling: only the window the foreground settles on is promoted
    constexpr ULONGLONG ActivateGraceMillis = 600; // foreground events right after the stage moved the focus itself are its own
    constexpr ULONGLONG ScanDelayMillis = 1500; // After Enter: windows the desktop scroll was still bringing back
    constexpr ULONGLONG RelayoutDelayMillis = 500; // After a display change: the monitors settle first
    constexpr ULONGLONG VerifyRetryMillis = 400; // A size was seen once: look again before believing it
    constexpr ULONGLONG VerifyDelayMillis = 600; // After a landing: look at how the windows really came out
    constexpr ULONGLONG GuardMillis = 500; // Modal dialog check
    constexpr ULONGLONG DialogLookMillis = 200; // After a dialog came up: its owner is disabled by then
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
    // A rect from a coordinate mix-up is brought down to a monitor; one on a monitor that is unplugged goes onto the primary one
    const StageState::Entry entry = StageState::OnVisibleMonitors(StageState::Sanitized(saved, monitors), monitors);
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

    // Stable monitor identity (no WMI, no retries): the device id and instance, else the rect origin
    std::map<HMONITOR, std::pair<std::wstring, std::wstring>> devices;
    for (const auto& display : MonitorUtils::Display::GetDisplays().second)
    {
        devices[display.monitor] = { display.deviceId.id, display.deviceId.instanceId };
    }

    const auto previous = m_monitors;
    m_monitors.clear();
    bool unresolved = false;
    for (const auto& m : monitors)
    {
        const auto device = devices.find(m.handle);
        const auto& [id, instance] = device != devices.end() ? device->second : std::pair<std::wstring, std::wstring>{};
        // The key it had: same rect, else same origin (a resolution change moves no origin of the monitor at 0,0)
        // A real key is inherited only for the same rect; the same corner only inherits a key that is itself just a place
        std::vector<StageMonitors::Id> lastLook;
        for (const auto& p : previous)
        {
            lastLook.push_back({ p.key, p.rect });
        }

        const auto choice = StageConfig::ChooseKey(id, instance, m.rect.L, m.rect.T, StageMonitors::InheritKey(lastLook, m.rect));
        unresolved |= !choice.resolved;
        UINT dpi = 0, ignored = 0;
        if (FAILED(GetDpiForMonitor(m.handle, MDT_EFFECTIVE_DPI, &dpi, &ignored)))
        {
            dpi = 0;
        }

        m_monitors.push_back({ m.handle, m.rect, m.work, choice.key, dpi });
    }

    // A monitor that is not identified yet (waking up): look again shortly, a few times, rather than save its cut under a guessed key
    constexpr int MaxKeyRetries = 8;
    if (unresolved && m_keyRetries < MaxKeyRetries)
    {
        m_keyRetries++;
        m_relayoutAt = GetTickCount64() + RelayoutDelayMillis;
        Schedule(RelayoutDelayMillis);
    }
    else if (!unresolved)
    {
        m_keyRetries = 0;
    }

    // Each stage finds its monitor again by the stable key (a resolution change keeps its windows, order, cut and rows), then by place
    std::vector<StageMonitors::Id> oldIds, newIds;
    for (const auto& s : m_stages)
    {
        oldIds.push_back({ s.monitor.key, s.monitor.rect });
    }

    for (const auto& monitor : m_monitors)
    {
        newIds.push_back({ monitor.key, monitor.rect });
    }

    const auto match = StageMonitors::MatchStages(oldIds, newIds);
    std::vector<MonitorStage> next;
    for (size_t n = 0; n < m_monitors.size(); n++)
    {
        const auto& monitor = m_monitors[n];
        if (match[n] >= 0)
        {
            auto& old = m_stages[static_cast<size_t>(match[n])];
            const bool sameMonitor = old.monitor.key == monitor.key;
            if (StageMonitors::SizeChanged(old.monitor.rect, monitor.rect, old.monitor.dpi, monitor.dpi))
            {
                m_remeasure = true; // Their minimum sizes were measured for another size or scale
            }

            next.push_back(std::move(old));
            next.back().monitor = monitor;
            if (!sameMonitor)
            {
                // Another monitor took this place: it has its own cut and rows
                next.back().values = StageConfig::Resolve(m_values, m_monitorMap, monitor.key);
                next.back().layout = StageConfig::MakeLayout(next.back().values);
            }
        }
        else
        {
            MonitorStage stage;
            stage.monitor = monitor;
            stage.values = StageConfig::Resolve(m_values, m_monitorMap, monitor.key);
            stage.layout = StageConfig::MakeLayout(stage.values);
            next.push_back(std::move(stage));
        }
    }

    // The windows of a monitor that is gone join the primary one, and are remembered for when it is back
    auto primary = std::find_if(next.begin(), next.end(), [](const MonitorStage& s) { return s.monitor.rect.L <= 0 && 0 < s.monitor.rect.R && s.monitor.rect.T <= 0 && 0 < s.monitor.rect.B; });
    if (primary == next.end())
    {
        primary = next.begin();
    }

    for (const int index : StageMonitors::GoneStages(m_stages.size(), match))
    {
        auto& gone = m_stages[static_cast<size_t>(index)];
        if (gone.order.empty())
        {
            continue;
        }

        Logger::info(L"Stage: a monitor is gone, its {} window(s) join the primary stage", gone.order.size());
        std::vector<std::uintptr_t> windows;
        for (const int id : gone.order)
        {
            const auto it = m_windows.find(id);
            if (it != m_windows.end())
            {
                windows.push_back(reinterpret_cast<std::uintptr_t>(it->second.window));
                ResetMin(it->second); // Measured for the other monitor's scale
            }
        }

        StageMonitors::Exile(m_exiled, windows, gone.monitor.key);
        m_remeasure = true; // The relayout measures them again before it lands them
        primary->order.insert(primary->order.end(), gone.order.begin(), gone.order.end());
    }

    m_stages = std::move(next);

    // A monitor that is back gets its windows back (the ones that are still held, and where there is room)
    for (auto& stage : m_stages)
    {
        for (const auto handle : StageMonitors::TakeReturning(m_exiled, stage.monitor.key))
        {
            auto* held = Find(reinterpret_cast<HWND>(handle));
            if (!held)
            {
                // Not held right now (cut by minimum sizes, minimized): the record stays, Adopt sends it home when it is taken again
                m_exiled[handle] = stage.monitor.key;
                continue;
            }

            const int id = held->id;
            MonitorStage* from = StageOfId(id);
            if (!from || from == &stage)
            {
                continue;
            }

            if (!StageConfig::HasRoom(stage.order.size(), Capacity(stage)))
            {
                m_exiled[handle] = stage.monitor.key; // No room yet: it stays away and asks again at the next display change
                continue;
            }

            from->layout->Remove(from->order, id);
            stage.layout->Join(stage.order, id, false);
            ResetMin(*held);
            m_remeasure = true;
            Logger::info(L"Stage: a window returns to its monitor");
        }
    }
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
    if (const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST))
    {
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        if (GetMonitorInfoW(monitor, &monitorInfo))
        {
            // The visible frame, not the rect with its invisible resize borders (those make every window look bigger than the screen)
            RECT frame = rect;
            if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
            {
                frame = rect;
            }

            const RECT& m = monitorInfo.rcMonitor;
            info.coversMonitor = StageAdopt::CoversMonitor(frame.left, frame.top, frame.right, frame.bottom, m.left, m.top, m.right, m.bottom);
            const auto since = m_pendingSince.find(window);
            const auto now = GetTickCount64();
            const unsigned long long age = since != m_pendingSince.end() ? now - since->second : (std::numeric_limits<unsigned long long>::max)();
            info.zoomed = IsZoomed(window) != FALSE;
            // Only for a window with a title bar (the fallback for a captioned overlay): a resizable one without is WezTerm or an app window
            info.youngFullSized = StageAdopt::HasCaption(info.style) && StageAdopt::YoungFullSized(info.zoomed, StageAdopt::MonitorShare(frame.left, frame.top, frame.right, frame.bottom, m.left, m.top, m.right, m.bottom), age);
        }
    }

    const auto backoff = m_overlayBackoff.find(window);
    info.inBackoff = StageAdopt::InBackoff(backoff != m_overlayBackoff.end() ? &backoff->second : nullptr, info.style, info.exStyle, GetTickCount64());
    // The same list as the dock mode: the user's escape hatch for apps that do not behave
    std::wstring processPath = get_process_path(window);
    CharUpperBuffW(processPath.data(), static_cast<DWORD>(processPath.length()));
    info.excludedByUser = !processPath.empty() && FancyZonesWindowUtils::IsExcludedByUser(window, processPath);
    info.cutLoose = m_loose.contains(window);
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
    auto plan = stage.layout->Compute(input);
    // Each strip tile hugs its window (the height of the window's visible frame over its width); the aspect is kept while it stays within 5%
    StageStripPlan::LayoutTiles(plan, [&](int id) {
        const auto it = m_windows.find(id);
        RECT rect{};
        if (it == m_windows.end() || !IsWindow(it->second.window) || !GetWindowRect(it->second.window, &rect))
        {
            return 0.0;
        }

        auto& m = it->second;
        const int w = rect.right - rect.left - m.insets.L - m.insets.R;
        const int h = rect.bottom - rect.top - m.insets.T - m.insets.B;
        if (w <= 0 || h <= 0)
        {
            return m.tileAspect;
        }

        m.tileAspect = StageStripPlan::StableAspect(m.tileAspect, static_cast<double>(h) / w);
        return m.tileAspect;
    });
    return plan;
}

// The plan as the strip is drawn right now (scrolled, cut at the usable area): what the mouse has to hit for a drop to swap
StageLayouts::Plan StageMode::ScrolledPlan(MonitorStage& stage)
{
    auto plan = ComputePlan(stage);
    if (m_strip)
    {
        StageStripPlan::ApplyScroll(plan, m_strip->ScrollOffset(StageStripPlan::KeyOf(stage.monitor.rect)));
    }

    return plan;
}

void StageMode::Land(MonitorStage& stage, int skipId, bool dockedOnly)
{
    if (stage.order.empty())
    {
        stage.strip.reset();
        if (!dockedOnly)
        {
            SyncStrip();
        }

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
    const auto oldStrip = stage.strip; // the tiles as they were shown: where a promoted window's glide may start
    // The preview of a strip-width drag also lays the tile column out again each tick (the tiles follow the new share live)
    const bool stripLive = dockedOnly && m_drag && m_drag->ctx && m_drag->ctx->stripOk;
    if (!dockedOnly || stripLive)
    {
        stage.strip = StageStripPlan::Build(
            StageStripPlan::KeyOf(stage.monitor.rect), plan,
            [&](int id) {
                StageStripPlan::Source source;
                const auto it = m_windows.find(id);
                if (it != m_windows.end())
                {
                    const auto& m = it->second;
                    source.window = reinterpret_cast<std::uintptr_t>(m.window);
                    // A minimized or cloaked window has a blank thumbnail (a minimized one is released from the stage anyway)
                    source.live = IsWindow(m.window) && IsWindowVisible(m.window) && !IsIconic(m.window) && !IsCloaked(m.window);
                    source.insets = { m.insets.L, m.insets.T, m.insets.R, m.insets.B };
                }

                return source;
            });
        if (stage.strip)
        {
            stage.strip->layout = stage.layout->Name();
        }

        if (!dockedOnly)
        {
            stage.stripHidden = StripCovered(stage);
        }
    }

    const auto monitors = MonitorRects();
    bool waiting = false;
    std::vector<int> stuck;
    const bool anyParked = !dockedOnly && std::any_of(plan.Items.begin(), plan.Items.end(), [](const StageLayouts::PlanItem& i) { return i.Role == StageLayouts::Role::Hidden; });
    const auto dialogs = anyParked ? StageAdopt::DialogOwners(DialogCandidates()) : std::set<HWND>{};

    // Windows the stack has no room for at their minimum size (a layout without a strip): they are let go, not parked out of sight
    if (!dockedOnly)
    {
        const auto cut = StageState::MinSizeCut(plan, [&](int id) {
            const auto it = m_windows.find(id);
            return it == m_windows.end() || id == skipId || (m_drag && m_drag->id == id) || it->second.guarded || it->second.needMeasure || dialogs.contains(it->second.window) ||
                   !IsWindow(it->second.window) || IsIconic(it->second.window) || IsZoomed(it->second.window);
        });
        if (!cut.empty())
        {
            LooseCut(cut); // Re-lands this stage (Release does)
            return;
        }
    }

    bool guarded = false;
    // The moves of the windows that land docked wait until the loop is done: the ones that glide are held first
    struct DockedMove
    {
        Managed* window{};
        Rect outer;
        bool wasParked{};
        StageMotion::Candidate facts;
        std::optional<Rect> tile; // wasParked: its strip tile (as an outer rect), when known
        Rect oldRect; // where it is now (outer)
    };
    std::vector<DockedMove> dockedMoves;

    // The promote and demote animations (the optional setting, with Animations): the windows that move between a strip tile and a slot
    const bool promoteAnimated = StageMotion::PromoteGlideAvailable && FancyZonesSettings::settings().stageAnimatePromote && FancyZonesSettings::settings().dockAnimation && !dockedOnly && m_motion.holdSpecs && m_motion.release;
    const auto promoteStyle = StageMotion::StyleOf(FancyZonesSettings::settings().stagePromoteStyle);
    const auto moveStyle = StageMotion::MoveStyleOf(FancyZonesSettings::settings().stageMoveStyle); // docked to docked, same monitor
    auto outerOfTile = [](const Rect& tile, const Managed& m, const RECT& at) {
        return StageMotion::OuterForTile(tile, { m.insets.L, m.insets.T, m.insets.R, m.insets.B }, at.right - at.left, at.bottom - at.top);
    };
    const int scrollOffset = promoteAnimated && m_strip ? m_strip->ScrollOffset(StageStripPlan::KeyOf(stage.monitor.rect)) : 0;
    // The tile of a window in the strip as it was shown (old plan), where the strip is scrolled to now; not cut at the area; none when out of view
    auto tileRect = [&](const std::optional<StageStripPlan::Strip>& strip, HWND window) -> std::optional<Rect> {
        if (strip)
        {
            std::vector<Rect> rects;
            size_t index = SIZE_MAX;
            for (const auto& t : strip->tiles)
            {
                if (t.rect.W() > 0 && t.rect.H() > 0)
                {
                    if (t.window == reinterpret_cast<std::uintptr_t>(window))
                    {
                        index = rects.size();
                    }

                    rects.push_back(t.rect);
                }
            }

            if (index != SIZE_MAX)
            {
                return StageMotion::ScrolledTile(rects, index, strip->clip, scrollOffset);
            }
        }

        return std::nullopt;
    };

    struct DemotedPark
    {
        Managed* window{};
        Rect outer; // where it parks
        bool toBottom{};
        StageMotion::Candidate facts;
        Rect oldRect;
        std::optional<Rect> tile; // its new tile as an outer rect
    };
    std::vector<DemotedPark> demoted; // parked after the hold, so their sprites start from where they are
    for (const auto& item : plan.Items)
    {
        const auto found = m_windows.find(item.Id);
        if (found == m_windows.end())
        {
            continue;
        }

        auto& m = found->second;
        if (m.fullscreen)
        {
            continue; // Fullscreen: not moved, not parked (its slot stays empty); it is landed again when it is a normal window
        }

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
            DockedMove move;
            move.window = &m;
            move.outer = StagePark::OuterRect(item.Rect, m.insets, stage.monitor.rect, monitors);
            move.wasParked = m.parked;
            RECT now{};
            move.facts.landsDocked = true;
            move.facts.wasParked = m.parked;
            if (m.parked)
            {
                RECT at{};
                const bool haveAt = GetWindowRect(m.window, &at) != FALSE;
                const bool behind = !haveAt || !StagePark::LooksParked(ToRect(at), stage.monitor.rect); // Not a sliver: it sits in the master's place
                if (promoteAnimated && haveAt)
                {
                    const auto tile = m.id == m_clickedId && m_clickedTile.W() > 0 && m_clickedTile.H() > 0 ? std::optional<Rect>(m_clickedTile) : tileRect(oldStrip, m.window);
                    if (tile)
                    {
                        move.tile = outerOfTile(*tile, m, at);
                    }
                }

                move.facts.parkedBehind = StageMotion::NeedsExtraHolds(promoteStyle, behind, move.tile.has_value());
            }
            move.facts.moves = !GetWindowRect(m.window, &now) || !ToRect(now).Same(move.outer);
            move.oldRect = ToRect(now);
            move.facts.cloaked = IsCloaked(m.window);
            move.facts.fullscreen = m.fullscreen;
            move.facts.changesMonitor = MonitorFromWindow(m.window, MONITOR_DEFAULTTONEAREST) != stage.monitor.handle;
            move.facts.visible = IsWindowVisible(m.window) != FALSE;
            dockedMoves.push_back(move);
        }
        else if (dialogs.contains(m.window))
        {
            m.guarded = true; // Parked once its dialog is closed: alone, the dialog would float with nothing behind it
            guarded = true;
        }
        else
        {
            m.guarded = false;
            // Never resized: a parked window keeps the size it has (an app that does not paint off-screen would show nothing at a new
            // size, and so would its tile); promoting it docks it, and that resizes it
            RECT where{};
            const bool haveRect = GetWindowRect(m.window, &where) != FALSE;
            // Where the window waits behind the master when no sliver is possible (not on this monitor, or the sliver would show on another)
            const auto behind = StagePark::OuterRect(plan.Park, m.insets, stage.monitor.rect, monitors);
            const auto park = StagePark::Park(haveRect ? ToRect(where) : StageLayouts::LRect(), behind, stage.monitor.rect, monitors);
            if (!(m.parked && haveRect && ToRect(where).Same(park.outer)))
            {
                if (promoteAnimated && !m.parked && haveRect && IsWindowVisible(m.window))
                {
                    // It was docked: it may shrink into its tile, so it is parked once the sprites hold it
                    DemotedPark d;
                    d.window = &m;
                    d.outer = park.outer;
                    d.toBottom = !park.sliver;
                    d.oldRect = ToRect(where);
                    d.facts.visible = true;
                    d.facts.cloaked = IsCloaked(m.window);
                    d.facts.fullscreen = m.fullscreen;
                    d.facts.changesMonitor = MonitorFromWindow(m.window, MONITOR_DEFAULTTONEAREST) != stage.monitor.handle;
                    if (const auto shown = StageMotion::ScrolledTileOf(plan, m.id, scrollOffset))
                    {
                        d.tile = outerOfTile(*shown, m, where);
                    }

                    demoted.push_back(d);
                }
                else
                {
                    MoveTo(m.window, park.outer, !park.sliver, stage.monitor.handle); // Not again for a window that already sits there (HWND_BOTTOM is a z-order change)
                }
            }

            m.parked = true;
        }
    }

    if (!dockedMoves.empty() || !demoted.empty())
    {
        std::vector<StageMotion::Candidate> facts;
        for (const auto& move : dockedMoves)
        {
            facts.push_back(move.facts);
        }

        StageMotion::Context context;
        context.setting = FancyZonesSettings::settings().dockAnimation;
        context.dragging = m_drag.has_value();
        context.areaChanged = m_areaChanging;
        // The demoted windows are parked at the end, a throw or not (before, their park would have happened in the loop above)
        bool demotedParked = false;
        const auto parkDemoted = [&] {
            if (demotedParked)
            {
                return;
            }

            demotedParked = true;
            for (const auto& d : demoted)
            {
                try
                {
                    MoveTo(d.window->window, d.outer, d.toBottom, stage.monitor.handle);
                }
                catch (...)
                {
                }
            }
        };
        StageMotion::ScopeGuard parkGuard(parkDemoted);
        std::vector<DockMotion::Held> held;
        if (m_motion.release && m_motion.holdSpecs)
        {
            const bool fromParked = StageMotion::PromoteGlideAvailable && FancyZonesSettings::settings().stageAnimatePromote;
            const auto picked = StageMotion::Gliders(context, facts, fromParked);
            bool promoteGlides = false;
            for (const size_t i : picked)
            {
                const auto& move = dockedMoves[i];
                StageMotion::Spec spec;
                if (move.wasParked)
                {
                    promoteGlides = true;
                    spec = StageMotion::PromotedSpec(promoteStyle, move.tile, move.outer);
                }
                else
                {
                    spec = StageMotion::MovedSpec(moveStyle, move.oldRect, move.outer); // Slide: the default travel
                }

                held.push_back({ move.window->window, spec });
            }

            // A glider out from behind the master: hold the others too, so they are drawn above it and it does not flash over the master
            for (const size_t i : StageMotion::ExtraHolds(facts, picked))
            {
                held.push_back({ dockedMoves[i].window->window, {} });
            }

            // The window demoted into the strip glides along with a promotion: it shrinks into its tile, or fades out
            for (const auto& d : demoted)
            {
                if (StageMotion::DemoteAnimates(context, d.facts, promoteGlides))
                {
                    if (const auto spec = StageMotion::DemotedSpec(promoteStyle, d.tile, d.oldRect))
                    {
                        held.push_back({ d.window->window, *spec });
                    }
                }
            }
        }

        StageMotion::ScopeGuard releaseGuard; // minimized and maximized windows never get here (the loop above skipped them)
        if (!held.empty())
        {
            m_motion.holdSpecs(held); // Not before: the sprites start from where the windows are now
            releaseGuard.onExit = [&] { m_motion.release(true); };
        }

        parkDemoted();

        for (const auto& move : dockedMoves)
        {
            auto& m = *move.window;
            MoveTo(m.window, move.outer, false, stage.monitor.handle);
            if (move.wasParked)
            {
                SetWindowPos(m.window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS); // Back from behind everything
                m.parked = false;
            }
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
        if (stripLive)
        {
            SyncStrip(); // only the picture for the renderer thread
        }

        return; // A drag preview: no timers, no verification; the drop lands the whole stage
    }

    SyncStrip();
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

// The strips of every stage as the last Land planned them. Never waits for the renderer thread.
void StageMode::SyncStrip()
{
    if (!m_strip)
    {
        return;
    }

    m_strip->SetRadius(FancyZonesSettings::settings().stageStripRadius);
    std::vector<StageStripPlan::Strip> strips;
    for (const auto& stage : m_stages)
    {
        if (stage.strip && !stage.stripHidden)
        {
            strips.push_back(*stage.strip);
        }
    }

    m_strip->Publish(std::move(strips));
}

// A maximized or monitor-filling window of the stage: its strip is not shown (the tiles would sit over its caption buttons)
bool StageMode::StripCovered(const MonitorStage& stage) const
{
    bool zoomed = false, covers = false;
    for (const int id : stage.order)
    {
        const auto it = m_windows.find(id);
        if (it == m_windows.end() || !IsWindow(it->second.window) || !IsWindowVisible(it->second.window) || IsIconic(it->second.window))
        {
            continue;
        }

        RECT rect{};
        zoomed = zoomed || IsZoomed(it->second.window);
        covers = covers || (GetWindowRect(it->second.window, &rect) && StageStripPlan::CoversMonitor(ToRect(rect), stage.monitor.rect));
    }

    return StageStripPlan::Suppressed(zoomed, covers, ForeignFullscreenOn(stage));
}

// The slow poll: a window maximized or made fullscreen raises no event the stage hears
void StageMode::RefreshStripCover()
{
    bool changed = false;
    for (auto& stage : m_stages)
    {
        const bool hidden = stage.strip && StripCovered(stage);
        if (hidden != stage.stripHidden)
        {
            stage.stripHidden = hidden;
            changed = true;
        }
    }

    if (changed)
    {
        SyncStrip();
    }
}

// A tile of the strip was clicked: that window takes the main (pair: left) slot, as the promote key does. Docked windows and windows
// that are not tiles are left alone.
void StageMode::StripClick(HWND window) noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (!m_active || m_drag)
        {
            return;
        }

        auto* m = Find(window);
        MonitorStage* stage = m ? StageOfId(m->id) : nullptr;
        if (!m || !stage || !IsWindow(m->window) || IsIconic(m->window))
        {
            return;
        }

        const auto plan = ComputePlan(*stage);
        const auto item = std::find_if(plan.Items.begin(), plan.Items.end(), [&](const StageLayouts::PlanItem& i) { return i.Id == m->id; });
        if (item == plan.Items.end() || item->Role != StageLayouts::Role::StripTile || !stage->layout->PromoteHidden(stage->order, m->id))
        {
            return;
        }

        Logger::info(L"Stage: strip click hwnd={:x} class={}", reinterpret_cast<uintptr_t>(m->window), ClassOf(m->window));
        m_graceUntil = GetTickCount64() + ActivateGraceMillis; // the focus change below is ours
        m_clickedId = m->id;
        // The tile where the strip shows it now (not cut at the area): the promote glide starts there
        m_clickedTile = StageMotion::ScrolledTileOf(plan, m->id, m_strip ? m_strip->ScrollOffset(StageStripPlan::KeyOf(stage->monitor.rect)) : 0).value_or(StageLayouts::LRect());

        StageMotion::ScopeGuard clickDone([this] { m_clickedId = 0; });
        Land(*stage);
        if (m->window != GetForegroundWindow())
        {
            FancyZonesWindowUtils::SwitchToWindow(m->window);
        }
    }
    catch (...)
    {
        Logger::error(L"Stage: strip click failed");
    }
}

int StageMode::Capacity(const MonitorStage& stage) const
{
    return StageConfig::CapacityOf(*stage.layout, stage.monitor.work, CurrentSpacing());
}

void StageMode::LandAll()
{
    for (auto& stage : m_stages)
    {
        Land(stage);
    }
}

// Minimum-size cut: the windows go back where they were and stay out of the stage until something changes (room appears)
void StageMode::LooseCut(const std::vector<int>& ids)
{
    m_loosening++;
    for (const int id : ids)
    {
        const auto found = m_windows.find(id);
        if (found == m_windows.end())
        {
            continue; // An earlier release re-landed the stage and already took it
        }

        const HWND window = found->second.window;
        m_loose.insert(window);
        LogDecision(window, L"released: no room for it at its minimum size, it stays floating");
        Release(id, false, true);
    }

    m_loosening--;
}

// The minimum sizes an app reported (or that were learned) belong to a monitor scale the window is no longer on
void StageMode::ResetMin(Managed& m)
{
    m.minW = m.minH = 0;
    m.learned = 0;
    m.pending = {};
}

// A parked window (strip tile, hidden) that opens a modal dialog: the dialog would float over nothing. The window comes forward as if
// its tile had been clicked (it takes the master slot; the focus is not touched: the dialog has it or the user's window keeps it).
void StageMode::PromoteDialogOwners()
{
    if (!StageAdopt::MayPromoteOwners(m_drag.has_value()))
    {
        return; // The user has a window by the mouse: the order does not change under it
    }

    std::vector<StageAdopt::ParkedFacts> facts;
    for (const auto& [_, m] : m_windows)
    {
        if (m.parked && !m.guarded)
        {
            facts.push_back({ m.window, m.parked, m.guarded, IsIconic(m.window) != FALSE });
        }
    }

    if (facts.empty())
    {
        return;
    }

    const auto owners = StageAdopt::DialogOwners(DialogCandidates());
    for (const HWND window : StageAdopt::NeedsFront(facts, owners))
    {
        auto* m = Find(window);
        MonitorStage* stage = m ? StageOfId(m->id) : nullptr;
        if (!m || !stage || !stage->layout->PromoteHidden(stage->order, m->id))
        {
            continue;
        }

        Logger::info(L"Stage: a parked window opened a dialog, it comes forward hwnd={:x} class={}", reinterpret_cast<uintptr_t>(window), ClassOf(window));
        Land(*stage);
    }
}

// A held window that turned into a hard overlay (click-through, layered without a title bar) or whose app was excluded meanwhile goes
// back to where it was and is not taken again while it stays that way (and not more often than every few seconds). A window that merely
// went fullscreen (F11, a video, Alt+Enter) is NOT moved: the stage leaves it alone, and lands it in its slot again when it is a normal
// window again.
void StageMode::ReleaseOverlays()
{
    std::vector<int> release;
    std::vector<int> relandOf;
    for (auto& [id, m] : m_windows)
    {
        if (!IsWindow(m.window) || IsIconic(m.window))
        {
            continue;
        }

        const auto info = Describe(m.window);
        if (const auto* reason = StageAdopt::HardOverlayReason(info))
        {
            LogDecision(m.window, reason);
            m_overlayBackoff[m.window] = { info.style, info.exStyle, GetTickCount64() };
            release.push_back(id);
            continue;
        }

        bool fullscreen = StageAdopt::FullscreenLike(info);
        if (fullscreen)
        {
            // Not when it only sits in the slot the stage gave it and that slot fills the monitor (no margins, taskbar hidden)
            MonitorStage* stage = StageOfId(id);
            bool slotKnown = false, slotCovers = false;
            if (stage)
            {
                const auto plan = ComputePlan(*stage);
                const auto item = std::find_if(plan.Items.begin(), plan.Items.end(), [&](const StageLayouts::PlanItem& i) { return i.Id == id; });
                if (item != plan.Items.end() && (item->Role == StageLayouts::Role::Main || item->Role == StageLayouts::Role::Docked))
                {
                    slotKnown = true;
                    const auto& mr = stage->monitor.rect;
                    slotCovers = StageAdopt::CoversMonitor(item->Rect.L, item->Rect.T, item->Rect.R, item->Rect.B, mr.L, mr.T, mr.R, mr.B);
                }
            }

            fullscreen = !StageAdopt::IsOwnPlacement(slotKnown, slotCovers);
        }

        if (fullscreen && !m.fullscreen)
        {
            m.fullscreen = true;
            LogDecision(m.window, L"went fullscreen: left alone until it is a normal window again");
        }
        else if (!fullscreen && m.fullscreen)
        {
            m.fullscreen = false;
            LogDecision(m.window, L"a normal window again: back in its slot");
            relandOf.push_back(id);
        }
    }

    for (const int id : relandOf)
    {
        if (auto* stage = StageOfId(id))
        {
            Land(*stage);
        }
    }

    for (const int id : release)
    {
        Release(id, false, true);
    }
}

// A held window that is parked became the foreground window (the taskbar, Alt-Tab, an app): it comes forward in its own stage, exactly as
// a click on its tile does. The stage's own focus changes (grace) and a drag never trigger it.
void StageMode::PromoteActivated()
{
    const HWND window = m_activateWindow;
    m_activateWindow = nullptr;
    if (!m_active || !window || !IsWindow(window))
    {
        return;
    }

    auto* m = Find(window);
    MonitorStage* stage = m ? StageOfId(m->id) : nullptr;
    if (!m || !stage)
    {
        return;
    }

    StageAdopt::ActivateFacts facts;
    facts.held = true;
    facts.minimized = IsIconic(window) != FALSE;
    facts.guarded = m->guarded;
    facts.dragging = m_drag.has_value();
    facts.inGrace = m_activateInGrace;
    facts.stillForeground = ForegroundRoot() == window;
    const auto plan = ComputePlan(*stage);
    const auto item = std::find_if(plan.Items.begin(), plan.Items.end(), [&](const StageLayouts::PlanItem& i) { return i.Id == m->id; });
    facts.docked = item == plan.Items.end() || item->Role == StageLayouts::Role::Main || item->Role == StageLayouts::Role::Docked;
    if (!StageAdopt::ShouldPromoteOnActivate(facts) || !stage->layout->PromoteHidden(stage->order, m->id))
    {
        return;
    }

    m_graceUntil = GetTickCount64() + ActivateGraceMillis; // the Land below moves windows: its focus events are ours
    Logger::info(L"Stage: a parked window was activated, it comes forward hwnd={:x} class={}", reinterpret_cast<uintptr_t>(window), ClassOf(window));
    Land(*stage); // it is the foreground window already: nothing to hand the focus to
}

// A window of another process that fills the monitor (exclusive or borderless fullscreen, a video, a game) has the foreground: the
// strip must not float over it. The shell (desktop, taskbar) and our own windows do not count.
bool StageMode::ForeignFullscreenOn(const MonitorStage& stage) const
{
    const HWND foreground = ForegroundRoot();
    if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground) || IsCloaked(foreground))
    {
        return false;
    }

    DWORD pid{};
    GetWindowThreadProcessId(foreground, &pid);
    RECT rect{};
    if (pid == GetCurrentProcessId() || !GetWindowRect(foreground, &rect))
    {
        return false;
    }

    const auto cls = ClassOf(foreground);
    return StageStripPlan::FullscreenOver(ToRect(rect), stage.monitor.rect, cls == L"Progman" || cls == L"WorkerW" || cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd");
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
            if (m.learned >= MaxLearned || !IsWindow(m.window) || IsHungAppWindow(m.window) || !IsWindowVisible(m.window) || IsIconic(m.window) || IsZoomed(m.window) || m.guarded || m.fullscreen ||
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
        else if (!StageConfig::HasRoom(stage->order.size(), Capacity(*stage)))
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
    // The style, the extended style and the rect go in as well: what an overlay or an odd window looks like is read from here
    RECT rect{};
    GetWindowRect(window, &rect);
    Logger::info(L"Stage: hwnd={:x} class={} title={} chars style={:x} ex={:x} rect={},{},{},{}: {}", reinterpret_cast<uintptr_t>(window), ClassOf(window), (std::max)(titleLength, 0), static_cast<unsigned long long>(GetWindowLongPtrW(window, GWL_STYLE)), static_cast<unsigned long long>(GetWindowLongPtrW(window, GWL_EXSTYLE)), rect.left, rect.top, rect.right, rect.bottom, verdict);
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

    // A window that was sent away by a monitor that is back (it was cut meanwhile, or minimized) goes to that monitor's stage
    {
        std::vector<std::wstring> keys;
        for (const auto& s : m_stages)
        {
            keys.push_back(s.monitor.key);
        }

        const auto handle = reinterpret_cast<std::uintptr_t>(window);
        const auto record = m_exiled.find(handle);
        const std::wstring recorded = record != m_exiled.end() ? record->second : std::wstring();
        const int home = StageMonitors::ReturnTarget(m_exiled, handle, keys);
        if (home >= 0)
        {
            if (StageConfig::HasRoom(m_stages[static_cast<size_t>(home)].order.size(), Capacity(m_stages[static_cast<size_t>(home)])))
            {
                stage = &m_stages[static_cast<size_t>(home)];
            }
            else
            {
                m_exiled[handle] = recorded; // No room there yet
            }
        }
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

    if (!m_loosening)
    {
        m_loose.clear(); // Something left the stage: the windows that were cut for lack of room get another chance
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
        auto* held = m_active && window ? Find(window) : nullptr;
        if (!held)
        {
            return; // Only held windows: the debounce below is not armed by every move on the desktop
        }

        // A window that changed its shape (a parked one that resized itself, or one that was unmaximized): its strip tile has another
        // height now. Debounced; only when the aspect moved by more than 5%. Our own moves come through here too and end the same way.
        RECT rect{};
        // Armed during a drag too: OnTimer keeps it pending until the drag ends, so it fires after DragEnd.
        if (held->parked && held->tileAspect > 0 && IsWindow(window) && !IsIconic(window) && GetWindowRect(window, &rect))
        {
            const int w = rect.right - rect.left - held->insets.L - held->insets.R;
            const int h = rect.bottom - rect.top - held->insets.T - held->insets.B;
            if (w > 0 && h > 0 && StageStripPlan::AspectChanged(held->tileAspect, static_cast<double>(h) / w))
            {
                m_replanAt = GetTickCount64() + ReplanDelayMillis;
                Schedule(static_cast<UINT>(ReplanDelayMillis + 20));
            }
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
            facts.destinationHasRoom = to && StageConfig::HasRoom(to->order.size(), Capacity(*to));
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

    m_exiled.erase(reinterpret_cast<std::uintptr_t>(m.window)); // The user's move (or a window that is simply elsewhere): it no longer goes back to the monitor that was unplugged
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
    const auto now = GetTickCount64();
    for (auto it = m_pending.begin(); it != m_pending.end();)
    {
        const auto since = m_pendingSince.find(it->first);
        if (since != m_pendingSince.end() && now < since->second + NewWindowMinAgeMillis && IsWindow(it->first))
        {
            ++it; // too new: the next tick (the attempts are not used up)
            continue;
        }

        if (!IsWindow(it->first) || Adopt(it->first) || Find(it->first) || --it->second <= 0)
        {
            m_pendingSince.erase(it->first);
            it = m_pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void StageMode::WindowAppeared(HWND window, bool firstSeen) noexcept
{
    try
    {
        if (m_active && window && GetWindow(window, GW_OWNER))
        {
            m_dialogAt = GetTickCount64() + DialogLookMillis; // An owned window (a dialog) came up: does a parked window own it?
            Schedule(TickMillis);
        }

        if (!m_active || !window || GetAncestor(window, GA_ROOT) != window || Find(window) || m_pending.size() >= MaxPending)
        {
            return;
        }

        m_pending[window] = PendingAttempts;
        if (firstSeen)
        {
            m_pendingSince.try_emplace(window, GetTickCount64()); // only a new window waits: one that comes back from the tray or another desktop is adopted at once
        }
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
            WindowAppeared(window, false); // Restored from minimized: a window the stage knew
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
        m_pendingSince.erase(window);
        m_overlayBackoff.erase(window);
        m_stuck.erase(window);
        m_loose.erase(window);
        m_exiled.erase(reinterpret_cast<std::uintptr_t>(window)); // Closed: nothing to send back
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
                    facts.targetId = StageMouse::HitTest(ScrolledPlan(*stage), stage->order, drag.id, cursor.x, cursor.y);
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
            // Saved to settings.json under this monitor only; the other monitors' stages are not touched
            PersistMonitor(*stage);
            Land(*stage);
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
        m_loose.clear();
        m_exiled.clear();

        // What an earlier run that died left behind goes back first, and what can't (not responding) stays in the file
        m_orphans = RestoreEntries(StageState::Read(StateFilePath()));
        m_orphanTries = 0;
        m_orphanAt = GetTickCount64() + OrphanMillis; // Not stale: they were just tried
        m_values = StageConfig::FromSettings(FancyZonesSettings::settings());
        m_monitorMap = FancyZonesSettings::settings().stageMonitors;
        m_settingsPending = false;
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
        if (!m_strip && m_timerWindow && WM_PRIV_STAGE_STRIP_CLICK)
        {
            m_strip = std::make_unique<StageStrip>(m_timerWindow, WM_PRIV_STAGE_STRIP_CLICK);
        }

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

void StageMode::Leave(bool animate) noexcept
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
        if (m_strip)
        {
            m_strip->Publish({}, true); // The tiles are gone before the windows come back
        }

        m_keyTracker.Reset();
        StopDrag(false);
        if (m_timerWindow)
        {
            KillTimer(m_timerWindow, m_timerId);
        }

        m_pending.clear();
        m_pendingSince.clear();
        m_loose.clear();
        m_exiled.clear();
        m_remeasure = false;
        m_scanAt = m_relayoutAt = m_verifyAt = m_guardAt = m_rehomeAt = m_dialogAt = m_activateAt = m_replanAt = 0;
        m_activateWindow = nullptr;
        m_scansLeft = 0;

        // Everything goes back; what can't (not responding) stays in the file
        m_orphanTries = 0;
        std::vector<HWND> gliders;
        StageMotion::Context context;
        context.setting = FancyZonesSettings::settings().dockAnimation;
        context.shuttingDown = !animate;
        if (m_motion.hold && m_motion.release && StageMotion::ShouldGlideLeave(context))
        {
            for (const auto& [_, m] : m_windows)
            {
                RECT now{};
                const RECT back{ m.original.rect.L, m.original.rect.T, m.original.rect.R, m.original.rect.B };
                StageMotion::Candidate facts;
                facts.landsDocked = !m.parked; // a parked window stays out of sight: it does not glide
                facts.wasParked = m.parked;
                facts.moves = GetWindowRect(m.window, &now) && !ToRect(now).Same(m.original.rect);
                facts.minimized = IsIconic(m.window) != FALSE;
                facts.cloaked = IsCloaked(m.window);
                facts.maximized = IsZoomed(m.window) != FALSE || m.original.showCmd != SW_SHOWNORMAL; // it goes back by its placement, not by its rect
                facts.fullscreen = m.fullscreen;
                facts.changesMonitor = MonitorFromRect(&back, MONITOR_DEFAULTTONEAREST) != MonitorFromWindow(m.window, MONITOR_DEFAULTTONEAREST);
                facts.visible = IsWindowVisible(m.window) != FALSE;
                if (StageMotion::ShouldGlide(context, facts, false))
                {
                    gliders.push_back(m.window);
                }
            }
        }

        {
            StageMotion::ScopeGuard releaseGuard;
            if (!gliders.empty())
            {
                m_motion.hold(gliders);
                releaseGuard.onExit = [&] { m_motion.release(true); };
            }

            m_orphans = RestoreEntries(AllOriginals());
        }

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
        if (m_active && m_settingsPending && !m_drag)
        {
            m_settingsPending = false;
            SettingsChanged();
        }

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
            m_loose.clear(); // The monitors changed: the windows cut for lack of room may fit now
            if (m_remeasure)
            {
                // A monitor changed size or scale: what the windows said they need is in the old scale's pixels
                m_remeasure = false;
                for (auto& stage : m_stages)
                {
                    for (const int id : stage.order)
                    {
                        auto& m = m_windows.at(id);
                        ResetMin(m);
                        if (IsWindow(m.window) && !IsZoomed(m.window) && !IsIconic(m.window))
                        {
                            Measure(m, stage.monitor.work);
                        }
                    }
                }
            }

            for (auto& [_, m] : m_windows)
            {
                m.grace = now + GraceMillis; // The moves below are ours: VerifyLanding must not take their transit for a minimum size
            }

            m_areaChanging = true; // Every relayout (display change, unmaximize wait, monitor key retry) lands without a glide
            const StageMotion::ScopeGuard areaGuard{ [this] { m_areaChanging = false; } };
            LandAll();
            m_rehomeAt = now + TickMillis; // Windows of a monitor that went away or came: see where each one is
            m_scanAt = now + TickMillis; // Windows that were cut or waiting for a monitor are taken again (the cut list was cleared)
            m_scansLeft = (std::max)(m_scansLeft, 1);
        }

        if (!m_pending.empty())
        {
            ProcessPending();
        }

        if (!m_stages.empty() && now >= m_pollAt)
        {
            m_pollAt = now + RehomePollMillis;
            RefreshStripCover();
            ReleaseOverlays();
            PromoteDialogOwners(); // A modal dialog that came up with no event we heard
            if (m_stages.size() > 1 && !m_rehomeAt)
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

        if (m_dialogAt && now >= m_dialogAt)
        {
            m_dialogAt = 0;
            PromoteDialogOwners();
        }

        if (m_activateAt && now >= m_activateAt)
        {
            m_activateAt = 0;
            PromoteActivated();
        }

        if (m_replanAt && now >= m_replanAt)
        {
            if (m_drag)
            {
                m_replanAt = now + ReplanDelayMillis; // a drag is active: stay pending and fire after it ends
            }
            else
            {
                m_replanAt = 0;
                LandAll(); // the tile heights follow the windows' shapes (a plan that did not change moves nothing)
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

        if (m_active && !m_stages.empty() && !m_scanAt && !m_rehomeAt && m_pending.empty() && !m_relayoutAt && !m_guardAt && !m_verifyAt && !m_dialogAt && !m_activateAt && !m_replanAt)
        {
            Schedule(static_cast<UINT>(RehomePollMillis)); // Idle: the slow poll is all that is left
        }

        if (!m_pending.empty() || m_scanAt || m_rehomeAt || m_relayoutAt || m_guardAt || m_verifyAt || m_dialogAt || m_activateAt || m_replanAt || (m_active && m_settingsPending) || (!m_orphans.empty() && m_orphanTries < MaxOrphanTries))
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
            StageDpi::Scope dpi;
            RefreshStripCover(); // A fullscreen app in front hides the strip of its monitor (and the next foreground brings it back)

            // A parked window that gets the focus from the taskbar, Alt-Tab or an app: promoted once the foreground has settled
            const HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
            if (root && Find(root) && !IsIconic(root))
            {
                m_activateWindow = root;
                m_activateInGrace = GetTickCount64() < m_graceUntil; // judged when the event came, not when the debounce ends
                m_activateAt = GetTickCount64() + ActivateDebounceMillis;
                Schedule(static_cast<UINT>(ActivateDebounceMillis + 20));
            }
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
            SetLayoutName(name); // Saved to settings.json
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
            if (!StageConfig::HasRoom(destination.order.size(), Capacity(destination)))
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
            m_graceUntil = GetTickCount64() + ActivateGraceMillis; // the foreground event of this focus change is ours
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

void StageMode::ApplyValues(const StageConfig::Values& global, const std::map<std::wstring, StageMonitorValues>& monitors)
{
    m_values = global;
    m_monitorMap = monitors;
    m_loose.clear(); // Another layout or gap: the windows cut for lack of room may fit
    for (auto& stage : m_stages)
    {
        stage.values = StageConfig::Resolve(m_values, m_monitorMap, stage.monitor.key);
        stage.layout = StageConfig::MakeLayout(stage.values);
    }

    // A layout with a lower capacity (strip layout -> master): the windows past it go back where they were, they are not parked without a tile
    for (auto& stage : m_stages)
    {
        const auto excess = StageConfig::OverCapacity(stage.order, Capacity(stage));
        if (!excess.empty())
        {
            Logger::info(L"Stage: layout change releases {} window(s) over the capacity of the new layout", excess.size());
            for (const int id : excess)
            {
                Release(id, false, true);
            }
        }
    }

    LandAll();
}

void StageMode::SettingsChanged() noexcept
{
    try
    {
        StageDpi::Scope dpi;
        if (!m_active)
        {
            return;
        }

        if (m_strip)
        {
            m_strip->SetRadius(FancyZonesSettings::settings().stageStripRadius); // no relayout needed, also during a drag
        }

        if (m_drag)
        {
            // A reload must not replace the layout objects the drag is moving: it is applied once the drag ended
            m_settingsPending = true;
            Schedule(TickMillis);
            return;
        }

        const auto& settings = FancyZonesSettings::settings();
        ApplyValues(StageConfig::FromSettings(settings), settings.stageMonitors); // Also the gap and the margins
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

    ApplyValues(after, m_monitorMap);
}

// The cut or rows the user just dragged on one monitor: only its entry in settings.json changes, only its stage keeps the new values
void StageMode::PersistMonitor(MonitorStage& stage)
{
    const auto after = StageConfig::Read(*stage.layout, stage.values);
    const auto change = StageConfig::MonitorChange(stage.values, after);
    if (change.Empty())
    {
        return;
    }

    if (!FancyZonesSettings::SaveStageMonitor(stage.monitor.key, change))
    {
        Logger::warn(L"Stage: a monitor's cut could not be saved");
    }

    StageConfig::Merge(m_monitorMap, stage.monitor.key, change);
    stage.values = after;
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

#pragma endregion
