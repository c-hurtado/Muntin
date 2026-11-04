#include "pch.h"
#include "DesktopScroll.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include <dwmapi.h>

#include <common/SettingsAPI/settings_helpers.h>
#include <common/logger/logger.h>
#include <common/utils/elevation.h>

#include <FancyZonesLib/ModuleConstants.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/WindowUtils.h>

namespace
{
    constexpr wchar_t WindowClassName[] = L"MuntinDesktopScroll";
    constexpr wchar_t BorderClassName[] = L"MuntinDesktopScrollBorder";

    constexpr UINT WM_SCROLL_WHEEL = WM_APP + 1; // wparam: wheel delta, lparam: cursor
    constexpr UINT WM_SCROLL_FOLLOW = WM_APP + 2; // wparam: the window that became the foreground window
    constexpr UINT WM_SCROLL_RETURN_ALL = WM_APP + 3;
    constexpr UINT WM_SCROLL_STOP = WM_APP + 4;
    constexpr UINT WM_SCROLL_CARRY_DOWN = WM_APP + 6; // wparam: the monitor, lparam: the window
    constexpr UINT WM_SCROLL_CARRY_UP = WM_APP + 7;
    constexpr UINT WM_SCROLL_REST_ON_PAGE = WM_APP + 5; // wparam: the monitor, lparam: the window that docked

    constexpr ULONGLONG SettleMillis = 300; // The wheel stopped: the real windows take over from the thumbnails
    constexpr double EasePerFrame = 0.3; // Going to the page of a window
    constexpr double GlidePerFrame = 0.1; // Under the wheel: the windows keep gliding after the last notch
    constexpr double ZoomPerFrame = 0.2;
    constexpr double ArrivedPixels = 1.0;
    constexpr double ArrivedZoom = 0.002;
    constexpr LONG BorderPixels = 2;
    constexpr double BorderMargin = 16; // Between the border and the windows, with the overview fully open
    constexpr double BorderOpacity = 51; // Of 255, with the overview fully open
    constexpr LONG BorderRadius = 24;
    constexpr ULONGLONG LandMillis = 150; // A window that doesn't respond by then arrives when it can
    constexpr BYTE MaskKey = 0xE8; // Unassigned. A key between Win down and Win up stops Win from opening Start.

    struct Entry
    {
        HWND window{};
        LONG left{};
        LONG canvasTop{}; // Top of the window with the scroll at 0
        LONG width{};
        LONG height{};
        RECT normal{}; // The rect a maximized window restores to
        bool maximized{}; // Maximized when the monitor shows it in its place, plain and as large otherwise
        LONG maximizedTop{};
        LONG parkedTop{}; // Top of the window while it is parked
        bool parked{};
    };

    struct Screen
    {
        LONG top{};
        LONG height{};
        double offset{};
        int page{};
        std::vector<Entry> entries; // Bottom of the z-order first
    };

    struct Thumbnail
    {
        HTHUMBNAIL id{};
        const Entry* entry{};
        RECT drawn{}; // DWM is only told what changed: each update is a call into it
        bool inSight{};
    };

    // Read by the main thread, which must not move these windows
    std::mutex parkedLock;
    std::set<HWND> parkedWindows;

    std::wstring ParkedFileName()
    {
        return PTSettingsHelper::get_module_save_folder_location(NonLocalizable::ModuleKey) + L"\\desktop-scroll.txt";
    }

    // The accent color of the Windows theme
    COLORREF AccentColor() noexcept
    {
        DWORD color{};
        DWORD size = sizeof(color);
        const auto result = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &color, &size);
        return result == ERROR_SUCCESS ? color & 0xFFFFFF : RGB(255, 255, 255);
    }

    bool IsDown(int key) noexcept
    {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    }

    // Puts the window at this height. Hidden: keeps the window out of sight, inside the width of the monitor,
    // so that no part of it reaches a monitor next to it.
    void Place(const Entry& entry, LONG top) noexcept
    {
        constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;

        if (IsIconic(entry.window))
        {
            // Minimized while it was parked: moving it wouldn't change where it comes back
            RECT rect{ entry.left, top, entry.left + entry.width, top + entry.height };
            MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
            WINDOWPLACEMENT placement{ .length = sizeof(WINDOWPLACEMENT) };
            if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info) && GetWindowPlacement(entry.window, &placement))
            {
                placement.rcNormalPosition = rect;
                OffsetRect(&placement.rcNormalPosition, info.rcMonitor.left - info.rcWork.left, info.rcMonitor.top - info.rcWork.top);
                placement.flags |= WPF_ASYNCWINDOWPLACEMENT;
                SetWindowPlacement(entry.window, &placement);
            }
        }
        else if (entry.maximized && top == entry.maximizedTop)
        {
            const auto& r = entry.normal;
            SetWindowPos(entry.window, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, flags);
            ShowWindowAsync(entry.window, SW_SHOWMAXIMIZED);
        }
        else if (entry.maximized)
        {
            if (IsZoomed(entry.window))
            {
                ShowWindowAsync(entry.window, SW_SHOWNOACTIVATE);
            }

            SetWindowPos(entry.window, nullptr, entry.left, top, entry.width, entry.height, flags);
        }
        else
        {
            SetWindowPos(entry.window, nullptr, entry.left, top, entry.width, entry.height, flags);
        }
    }

    void Hide(const Entry& entry, const RECT& monitor) noexcept
    {
        Entry parking = entry;
        parking.width = (std::min)(entry.width, monitor.right - monitor.left); // Maximized windows are wider than the monitor
        parking.left = (std::max)(monitor.left, (std::min)(entry.left, monitor.right - parking.width));
        parking.maximizedTop = entry.parkedTop - 1; // Never the maximized place
        Place(parking, entry.parkedTop);
    }

    // Back on the first page, or wherever a monitor is, if its monitor is gone
    void Return(const Entry& entry, const Screen& screen) noexcept
    {
        constexpr LONG reachable = 64; // Of the title bar, above the bottom of the work area

        Entry home = entry;
        home.canvasTop = entry.canvasTop - DesktopScroll::PageOf(entry.canvasTop - screen.top, entry.height, screen.height) * screen.height;
        RECT rect{ home.left, home.canvasTop, home.left + home.width, home.canvasTop + home.height };
        if (!MonitorFromRect(&rect, MONITOR_DEFAULTTONULL))
        {
            MONITORINFO primary{ .cbSize = sizeof(MONITORINFO) };
            GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &primary);
            OffsetRect(&home.normal, primary.rcWork.left - home.normal.left, primary.rcWork.top - home.normal.top);
            home.left = primary.rcWork.left;
            home.canvasTop = primary.rcWork.top;
            rect = { home.left, home.canvasTop, home.left + home.width, home.canvasTop + home.height };
        }

        MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
        if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info))
        {
            home.canvasTop = (std::max)(info.rcWork.top, (std::min)(home.canvasTop, info.rcWork.bottom - reachable));
        }

        home.maximizedTop = home.canvasTop; // Maximized is how it goes back, wherever it was scrolled to
        Place(home, home.canvasTop);
    }

    bool IsScrollable(HWND window, HMONITOR monitor) noexcept
    {
        constexpr LONG_PTR skippedStyles = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;

        if (!IsWindowVisible(window))
        {
            return false;
        }

        DWORD process{};
        GetWindowThreadProcessId(window, &process);
        int cloaked{};
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

        // InternalGetWindowText sends no message, so a hung app can't block this
        wchar_t title[2]{};
        wchar_t className[32]{};
        GetClassNameW(window, className, ARRAYSIZE(className));
        return process != GetCurrentProcessId() && !IsIconic(window) && !cloaked &&
               (GetWindowLongPtrW(window, GWL_EXSTYLE) & skippedStyles) == 0 &&
               InternalGetWindowText(window, title, ARRAYSIZE(title)) > 0 &&
               wcscmp(className, L"Progman") != 0 && wcscmp(className, L"WorkerW") != 0 &&
               MonitorFromWindow(window, MONITOR_DEFAULTTONULL) == monitor &&
               (is_process_elevated() || !IsProcessOfWindowElevated(window)); // These can't be moved from here
    }

    std::vector<HWND> TopLevelWindows()
    {
        std::vector<HWND> windows;
        EnumWindows([](HWND window, LPARAM param) {
            reinterpret_cast<std::vector<HWND>*>(param)->push_back(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));
        std::reverse(windows.begin(), windows.end());
        return windows;
    }
}

struct DesktopScroll::Impl
{
    static inline Impl* self{};

    HINSTANCE instance{};
    PageChanged pageChanged;
    HWND window{};
    std::thread thread;
    HHOOK mouseHook{};
    HWINEVENTHOOK foregroundHook{};
    std::map<HMONITOR, Screen> screens;

    bool scrolling{};
    bool returnPending{};
    bool settleNow{};
    bool steering{}; // The wheel drives this scroll: others have a destination
    HWND followed{}; // The scroll goes to the page of this window, which came to the foreground
    HWND pendingFollow{}; // Came to the foreground during a scroll: the next one goes there
    HWND docked{}; // Goes to the page the scroll rests on, instead of staying where it was dropped
    HWND carried{}; // Stays where it is on the monitor while the pages scroll past
    std::atomic<HWND> landedRider{}; // Reached its page, for the page change to pick up
    HMONITOR pendingRest{}; // Asked for during a scroll
    HWND pendingRestWindow{};
    HMONITOR scrolledMonitor{};
    double target{};
    ULONGLONG lastWheel{};

    void Run(std::promise<void>& ready) noexcept
    {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        self = this;

        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.lpfnWndProc = WndProc;
        wcex.hInstance = instance;
        wcex.lpszClassName = WindowClassName;
        RegisterClassExW(&wcex);

        WNDCLASSEXW borderClass{};
        borderClass.cbSize = sizeof(WNDCLASSEX);
        borderClass.lpfnWndProc = DefWindowProcW;
        borderClass.hInstance = instance;
        borderClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        borderClass.lpszClassName = BorderClassName;
        RegisterClassExW(&borderClass);

        // Top level and never shown: it receives WM_DISPLAYCHANGE
        window = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        ready.set_value();
        if (!window)
        {
            Logger::error(L"Failed to create the desktop scroll window");
            return;
        }

        RecoverParked();
        mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
        if (!mouseHook)
        {
            Logger::error(L"Failed to hook the mouse for desktop scroll");
        }

        foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            DispatchMessageW(&message);
        }

        UnhookWinEvent(foregroundHook);
        UnhookWindowsHookEx(mouseHook);
        ReturnAllNow(false);
        DestroyWindow(window);
        self = nullptr;
    }

    static LRESULT CALLBACK MouseProc(int code, WPARAM wparam, LPARAM lparam) noexcept
    {
        if (code == HC_ACTION && self)
        {
            const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
            if (wparam == WM_MOUSEWHEEL && (IsDown(VK_LWIN) || IsDown(VK_RWIN)) && IsDown(VK_SHIFT) && !IsDown(VK_CONTROL) && !IsDown(VK_MENU) && !IsDown(VK_LBUTTON))
            {
                PostMessageW(self->window, WM_SCROLL_WHEEL, static_cast<WPARAM>(static_cast<short>(HIWORD(info->mouseData))), MAKELPARAM(info->pt.x, info->pt.y));
                return 1;
            }

            if (self->scrolling && (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN))
            {
                self->settleNow = true;
            }
        }

        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    static void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD) noexcept
    {
        if (self && window && object == OBJID_WINDOW)
        {
            PostMessageW(self->window, WM_SCROLL_FOLLOW, reinterpret_cast<WPARAM>(GetAncestor(window, GA_ROOT)), 0);
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        switch (message)
        {
        case WM_SCROLL_WHEEL:
            self->Wheel(static_cast<int>(static_cast<short>(wparam)), { static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam)) });
            return 0;
        case WM_SCROLL_FOLLOW:
            self->Follow(reinterpret_cast<HWND>(wparam));
            return 0;
        case WM_SCROLL_CARRY_DOWN:
        case WM_SCROLL_CARRY_UP:
            self->Carry(reinterpret_cast<HMONITOR>(wparam), reinterpret_cast<HWND>(lparam), message == WM_SCROLL_CARRY_DOWN ? 1 : -1);
            return 0;
        case WM_SCROLL_REST_ON_PAGE:
            self->RestOnPage(reinterpret_cast<HMONITOR>(wparam), reinterpret_cast<HWND>(lparam));
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SCROLL_RETURN_ALL:
            self->RequestReturnAll();
            return 0;
        case WM_SCROLL_STOP:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void Wheel(int delta, POINT cursor) noexcept
    {
        // Not from the mouse hook: SendInput waits for the keyboard hook of the main thread
        INPUT inputs[2]{};
        inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = inputs[1].ki.wVk = MaskKey;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));

        lastWheel = GetTickCount64();
        const auto monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(monitor, &info))
        {
            return;
        }

        const double pixels = DesktopScroll::WheelPixels(delta, info.rcMonitor.bottom - info.rcMonitor.top);
        if (scrolling)
        {
            if (steering && monitor == scrolledMonitor)
            {
                target += pixels;
            }

            return;
        }

        Scroll(monitor, std::nullopt, pixels);
    }

    void Follow(HWND foreground) noexcept
    {
        if (scrolling)
        {
            if (!followed)
            {
                pendingFollow = foreground;
            }

            return;
        }

        for (auto& [monitor, screen] : screens)
        {
            // A dialog that just opened isn't on any page yet, its owner is
            const auto owner = GetAncestor(foreground, GA_ROOTOWNER);
            const auto entry = std::find_if(screen.entries.begin(), screen.entries.end(), [&](const Entry& candidate) { return candidate.window == foreground || candidate.window == owner; });
            if (entry != screen.entries.end() && entry->parked)
            {
                const LONG page = DesktopScroll::PageOf(entry->canvasTop - screen.top, entry->height, screen.height);
                const auto reached = monitor;
                followed = foreground;
                Scroll(reached, static_cast<double>(page) * screen.height, 0);
                followed = nullptr;
                return;
            }
        }
    }

    void Carry(HMONITOR monitor, HWND rider, int pages) noexcept
    {
        MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
        if (scrolling || !GetMonitorInfoW(monitor, &info))
        {
            return;
        }

        const double height = static_cast<double>(info.rcMonitor.bottom) - info.rcMonitor.top;
        const auto found = screens.find(monitor);
        const double page = std::round((found != screens.end() ? found->second.offset : 0.0) / height) + pages;
        if (page >= 0)
        {
            carried = rider;
            Scroll(monitor, page * height, 0);
        }
    }

    void RestOnPage(HMONITOR monitor, HWND dockedWindow) noexcept
    {
        if (scrolling)
        {
            pendingRest = monitor;
            pendingRestWindow = dockedWindow;
            return;
        }

        const auto found = screens.find(monitor);
        if (found != screens.end() && std::fmod(found->second.offset, found->second.height) != 0)
        {
            const auto& screen = found->second;
            docked = dockedWindow;
            Scroll(monitor, std::round(screen.offset / screen.height) * screen.height, 0);
        }
    }

    void ForgetOnOtherScreens(HWND candidate, const Screen& kept) noexcept
    {
        for (auto& [_, screen] : screens)
        {
            if (&screen != &kept)
            {
                std::erase_if(screen.entries, [&](const Entry& entry) { return entry.window == candidate; });
            }
        }
    }

    void PublishParked() const
    {
        std::lock_guard lock(parkedLock);
        parkedWindows.clear();
        for (const auto& [_, screen] : screens)
        {
            const bool onPage = std::fmod(screen.offset, screen.height) == 0;
            for (const auto& entry : screen.entries)
            {
                if (entry.parked || !onPage)
                {
                    parkedWindows.insert(entry.window);
                }
            }
        }
    }

    // Takes in the windows as they are now: whatever the user moved stays there, new windows join
    void Collect(Screen& screen, HMONITOR monitor, const MONITORINFO& info) noexcept
    {
        const LONG parkedTop = info.rcMonitor.bottom - 1;
        const LONG offset = std::lround(screen.offset);
        auto old = std::move(screen.entries);
        screen.entries.clear();
        for (const auto candidate : TopLevelWindows())
        {
            if (!IsScrollable(candidate, monitor))
            {
                continue;
            }

            RECT rect{};
            GetWindowRect(candidate, &rect);
            ForgetOnOtherScreens(candidate, screen);

            const auto known = std::find_if(old.begin(), old.end(), [&](const Entry& entry) { return entry.window == candidate; });
            // Still waiting, or a move the app hasn't got to yet: its place on the canvas stands
            if (known != old.end() && (rect.top == known->parkedTop || IsHungAppWindow(candidate)))
            {
                screen.entries.push_back(*known);
                screen.entries.back().parked = rect.top == known->parkedTop;
                old.erase(known);
                continue;
            }

            Entry entry{ .window = candidate };
            const LONG width = rect.right - rect.left;
            const LONG height = rect.bottom - rect.top;
            if (known != old.end() && known->maximized && known->width == width && known->height == height)
            {
                entry = *known; // Maximized on its page, cut off here
            }

            if (known != old.end())
            {
                old.erase(known);
            }

            entry.left = rect.left;
            entry.canvasTop = rect.top + offset;
            entry.width = width;
            entry.height = height;
            entry.parkedTop = parkedTop;
            entry.parked = false;
            if (IsZoomed(candidate))
            {
                WINDOWPLACEMENT placement{ .length = sizeof(WINDOWPLACEMENT) };
                GetWindowPlacement(candidate, &placement);
                entry.normal = placement.rcNormalPosition; // In work area coordinates
                OffsetRect(&entry.normal, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
                entry.maximized = true;
                entry.maximizedTop = rect.top;
            }

            screen.entries.push_back(entry);
        }

        // Not in the list of windows: minimized meanwhile, it keeps its place on the canvas
        for (const auto& entry : old)
        {
            if (IsWindow(entry.window) && (entry.parked || IsIconic(entry.window)))
            {
                screen.entries.insert(screen.entries.begin(), entry);
            }
        }
    }

    // False once the thread has to stop. Keeps the mouse hook answering during a scroll.
    static bool Pump() noexcept
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                PostQuitMessage(0);
                return false;
            }

            DispatchMessageW(&message);
        }

        return true;
    }

    void Scroll(HMONITOR monitor, std::optional<double> destination, double pixels) noexcept
    {
        MONITORINFO info{ .cbSize = sizeof(MONITORINFO) };
        if (!GetMonitorInfoW(monitor, &info))
        {
            return;
        }

        // ponytail: no scrolling with a monitor right below this one, a parked window would be on it. Park on another edge if that layout ever comes up.
        const RECT below{ info.rcMonitor.left, info.rcMonitor.bottom, info.rcMonitor.right, info.rcMonitor.bottom + 1 };
        if (MonitorFromRect(&below, MONITOR_DEFAULTTONULL))
        {
            return;
        }

        auto& screen = screens[monitor];
        screen.top = info.rcMonitor.top;
        screen.height = info.rcMonitor.bottom - info.rcMonitor.top;

        scrolling = true;
        settleNow = false;
        steering = !destination;
        scrolledMonitor = monitor;
        Collect(screen, monitor, info);
        const HWND seated = std::exchange(docked, nullptr);
        const HWND riding = std::exchange(carried, nullptr);
        if (destination)
        {
            const auto dropped = std::find_if(screen.entries.begin(), screen.entries.end(), [&](const Entry& entry) { return entry.window == seated; });
            if (dropped != screen.entries.end())
            {
                const LONG page = std::lround(*destination / screen.height);
                dropped->canvasTop = screen.top + DesktopScroll::IntoPage(dropped->canvasTop - screen.top, dropped->height, page, screen.height);
            }
        }

        LONG lowest = 0;
        for (const auto& entry : screen.entries)
        {
            if (entry.window != riding) // A carried window doesn't make the canvas longer, so it can't be carried past the last page
            {
                lowest = (std::max)(lowest, entry.canvasTop + entry.height - screen.top);
            }
        }

        const double limit = (std::max)(DesktopScroll::ScrollLimit(lowest, screen.height, FancyZonesSettings::settings().desktopScrollPages), std::ceil(screen.offset / screen.height) * screen.height);
        const auto clampTarget = [&] { target = std::clamp(target, 0.0, limit); };
        target = destination ? *destination : screen.offset + pixels;
        clampTarget();
        if (target == screen.offset)
        {
            PublishParked();
            scrolling = false;
            FinishReturn();
            return;
        }

        const auto rider = std::find_if(screen.entries.begin(), screen.entries.end(), [&](const Entry& entry) { return entry.window == riding; });
        if (destination && rider != screen.entries.end())
        {
            rider->canvasTop += std::lround(target - screen.offset);
            landedRider = riding;
        }

        const auto& work = info.rcWork;
        const HWND overlay = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, WindowClassName, L"", WS_POPUP, work.left, work.top, work.right - work.left, work.bottom - work.top, nullptr, nullptr, instance, nullptr);
        std::vector<Thumbnail> thumbnails;
        if (overlay)
        {
            for (const auto& entry : screen.entries)
            {
                HTHUMBNAIL id{};
                if (!IsIconic(entry.window) && SUCCEEDED(DwmRegisterThumbnail(overlay, entry.window, &id)))
                {
                    thumbnails.push_back({ id, &entry });
                }
            }
        }

        // The overview stays open while the keys are down, and always ends on a page
        const bool overview = !destination && overlay && FancyZonesSettings::settings().desktopScrollOverview;
        // Its own window, as the overlay can't draw anything but thumbnails. Painted whole once: from then on its region shows the border.
        const HWND border = overview ? CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, BorderClassName, L"", WS_POPUP, work.left, work.top, work.right - work.left, work.bottom - work.top, nullptr, nullptr, instance, nullptr) : nullptr;
        if (border)
        {
            // The class keeps a brush of the color of the theme as it was at the last scroll
            const auto brush = reinterpret_cast<LONG_PTR>(CreateSolidBrush(AccentColor()));
            DeleteObject(reinterpret_cast<HGDIOBJ>(SetClassLongPtrW(border, GCLP_HBRBACKGROUND, brush)));
            SetLayeredWindowAttributes(border, 0, 0, LWA_ALPHA);
            ShowWindow(border, SW_SHOWNOACTIVATE);
            UpdateWindow(border);
        }

        RECT borderDrawn{};
        BYTE borderOpacityDrawn{};
        const double middle = (static_cast<double>(info.rcMonitor.left) + info.rcMonitor.right) / 2;
        const double monitorWidth = static_cast<double>(info.rcMonitor.right) - info.rcMonitor.left;
        const auto draw = [&](double offset, double zoom) {
            for (auto& thumbnail : thumbnails)
            {
                const auto& entry = *thumbnail.entry;
                const LONG canvasY = entry.canvasTop - screen.top;
                const double page = DesktopScroll::PageOf(canvasY, entry.height, screen.height);
                const LONG top = std::lround(screen.top + DesktopScroll::OverviewY(canvasY, entry.window == riding ? target : offset, screen.height, page, zoom)) - work.top;
                const LONG left = std::lround(middle + (entry.left - middle) * zoom) - work.left;
                const RECT place{ left, top, left + std::lround(entry.width * zoom), top + std::lround(entry.height * zoom) };
                if (EqualRect(&place, &thumbnail.drawn))
                {
                    continue;
                }

                thumbnail.drawn = place;
                const bool inSight = place.bottom > 0 && place.top < work.bottom - work.top && place.right > 0 && place.left < work.right - work.left;
                if (!inSight && !thumbnail.inSight)
                {
                    continue;
                }

                thumbnail.inSight = inSight;
                DWM_THUMBNAIL_PROPERTIES properties{ .dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE, .opacity = 255, .fVisible = inSight };
                properties.rcDestination = place;
                DwmUpdateThumbnailProperties(thumbnail.id, &properties);
            }

            const double opened = std::clamp((1.0 - zoom) / (1.0 - DesktopScroll::OverviewZoom), 0.0, 1.0);
            const LONG margin = std::lround(BorderMargin * opened);
            const LONG pageLeft = std::lround(middle + (info.rcMonitor.left - middle) * zoom) - work.left;
            const LONG left = pageLeft - margin;
            const LONG right = pageLeft + std::lround(monitorWidth * zoom) + margin;
            const double lastPage = std::round(limit / screen.height);
            const LONG top = std::lround(screen.top + DesktopScroll::OverviewY(0, offset, screen.height, 0, zoom)) - work.top - margin;
            const LONG bottom = std::lround(screen.top + DesktopScroll::OverviewY((lastPage + 1) * screen.height, offset, screen.height, lastPage, zoom)) - work.top + margin;
            const RECT borderPlace{ left, top, right, bottom };
            const auto borderOpacity = static_cast<BYTE>(std::lround(BorderOpacity * opened));
            if (border && (!EqualRect(&borderPlace, &borderDrawn) || borderOpacity != borderOpacityDrawn))
            {
                borderDrawn = borderPlace;
                borderOpacityDrawn = borderOpacity;
                const HRGN ring = CreateRoundRectRgn(left, top, right + 1, bottom + 1, BorderRadius * 2, BorderRadius * 2);
                const HRGN inside = CreateRoundRectRgn(left + BorderPixels, top + BorderPixels, right - BorderPixels + 1, bottom - BorderPixels + 1, (BorderRadius - BorderPixels) * 2, (BorderRadius - BorderPixels) * 2);
                CombineRgn(ring, ring, inside, RGN_DIFF);
                DeleteObject(inside);
                SetWindowRgn(border, ring, FALSE); // The window owns it now
                SetLayeredWindowAttributes(border, 0, borderOpacity, LWA_ALPHA);
                SetWindowPos(border, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        };

        double position = screen.offset;
        double zoom = 1.0;
        draw(position, zoom);
        DwmFlush();
        if (overlay)
        {
            SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            DwmFlush();
        }

        for (auto& entry : screen.entries)
        {
            entry.parked = true;
        }

        // First the record and the claim on the windows, then the move
        SaveParked();
        PublishParked();
        for (const auto& entry : screen.entries)
        {
            Hide(entry, info.rcMonitor);
        }

        const auto restingOffset = [&](double offset) {
            const double resting = overview ? std::round(offset / screen.height) * screen.height : DesktopScroll::RestingOffset(offset, screen.height);
            return std::clamp(resting, 0.0, limit);
        };

        bool settling = destination.has_value() || !overlay;
        bool quit = false;
        while (!quit)
        {
            if (!Pump())
            {
                quit = true;
                break;
            }

            clampTarget();
            const bool wheeling = overview ? (IsDown(VK_LWIN) || IsDown(VK_RWIN)) && IsDown(VK_SHIFT) : GetTickCount64() - lastWheel < SettleMillis;
            if (settling && !destination && !settleNow && wheeling)
            {
                settling = false; // The wheel moved again on its way to rest
            }

            if (settleNow)
            {
                // A click stops the glide: only the real windows can take the next one
                if (!destination)
                {
                    target = restingOffset(position);
                }

                position = target;
                zoom = 1.0;
                settling = true;
            }
            else if (!settling && !wheeling)
            {
                target = restingOffset(target);
                settling = true;
            }

            position += (target - position) * (destination || (overview && settling) ? EasePerFrame : GlidePerFrame);
            if (overview)
            {
                zoom += ((settling ? 1.0 : DesktopScroll::OverviewZoom) - zoom) * ZoomPerFrame;
            }

            if (settling && std::abs(target - position) < ArrivedPixels && std::abs(1.0 - zoom) < ArrivedZoom)
            {
                position = target;
                draw(position, 1.0);
                DwmFlush();
                break;
            }

            draw(position, zoom);
            DwmFlush();
        }

        const HWND foregroundBefore = GetForegroundWindow();
        const double previousOffset = screen.offset;
        screen.offset = std::round(quit ? position : target);
        const LONG offset = std::lround(screen.offset);
        std::vector<Entry*> shown;
        for (auto& entry : screen.entries)
        {
            const LONG top = entry.canvasTop - offset;
            if (top < work.bottom && top + entry.height > work.top)
            {
                // Mostly below a shorter monitor, a window that crosses into the next one would move to that one
                Entry resting = entry;
                const RECT rect{ entry.left, top, entry.left + entry.width, top + entry.height };
                if (!entry.maximized && MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST) != monitor)
                {
                    resting.left = (std::max)(info.rcMonitor.left, info.rcMonitor.right - entry.width);
                }

                Place(resting, top);
                entry.parked = false;
                shown.push_back(&entry);
            }
        }

        WaitForLanding(shown, offset);
        DwmFlush();
        DwmFlush();
        for (const auto& thumbnail : thumbnails)
        {
            DwmUnregisterThumbnail(thumbnail.id);
        }

        if (border)
        {
            DestroyWindow(border);
        }

        if (overlay)
        {
            DestroyWindow(overlay);
        }

        // Maximizing a window that lands activates it
        const auto wasForeground = std::find_if(screen.entries.begin(), screen.entries.end(), [&](const Entry& entry) { return entry.window == foregroundBefore; });
        if (!followed && !pendingFollow && GetForegroundWindow() != foregroundBefore && IsWindow(foregroundBefore) && (wasForeground == screen.entries.end() || !wasForeground->parked))
        {
            FancyZonesWindowUtils::SwitchToWindow(foregroundBefore);
        }

        if (!pendingFollow)
        {
            Refocus(screen, monitor, shown);
        }

        const int page = static_cast<int>(std::lround(screen.offset / screen.height));
        const bool wasOnPage = previousOffset == std::floor(previousOffset / screen.height) * screen.height;
        const bool onPage = screen.offset == page * static_cast<double>(screen.height);
        const bool changed = page != screen.page || (onPage && !wasOnPage); // The layout comes back in force
        screen.page = page;
        PublishParked();
        SaveParked();
        scrolling = false;
        if (changed)
        {
            pageChanged(monitor, page);
        }

        FinishReturn();
        if (const auto next = std::exchange(pendingFollow, nullptr))
        {
            Follow(next);
        }

        if (const auto rest = std::exchange(pendingRest, nullptr))
        {
            RestOnPage(rest, pendingRestWindow);
        }
    }

    // The focus can't stay on a window that went out of sight
    void Refocus(const Screen& screen, HMONITOR monitor, const std::vector<Entry*>& shown) const noexcept
    {
        if (followed)
        {
            FancyZonesWindowUtils::SwitchToWindow(followed);
            return;
        }

        const auto foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        const auto entry = std::find_if(screen.entries.begin(), screen.entries.end(), [&](const Entry& candidate) { return candidate.window == foreground; });
        if (entry != screen.entries.end() && entry->parked && MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) == monitor)
        {
            const auto top = std::find_if(shown.rbegin(), shown.rend(), [](const Entry* candidate) { return IsWindow(candidate->window) && !IsIconic(candidate->window); });
            FancyZonesWindowUtils::SwitchToWindow(top != shown.rend() ? (*top)->window : GetShellWindow());
        }
    }

    static void WaitForLanding(const std::vector<Entry*>& entries, LONG offset) noexcept
    {
        const auto start = GetTickCount64();
        while (GetTickCount64() - start < LandMillis)
        {
            const bool landed = std::all_of(entries.begin(), entries.end(), [&](const Entry* entry) {
                const LONG top = entry->canvasTop - offset;
                if (entry->maximized && top == entry->maximizedTop)
                {
                    return IsZoomed(entry->window) != FALSE;
                }

                RECT rect{};
                return IsIconic(entry->window) || !GetWindowRect(entry->window, &rect) || rect.top == top;
            });
            if (landed || !Pump())
            {
                return;
            }

            MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
        }
    }

    void RequestReturnAll() noexcept
    {
        if (scrolling)
        {
            returnPending = true;
        }
        else
        {
            ReturnAllNow(true);
        }
    }

    void FinishReturn() noexcept
    {
        if (returnPending)
        {
            returnPending = false;
            ReturnAllNow(true);
        }
    }

    void ReturnAllNow(bool notify) noexcept
    {
        bool any = false;
        for (const auto& [_, screen] : screens)
        {
            any |= screen.offset != 0 || screen.page != 0;
            const LONG offset = std::lround(screen.offset);
            for (const auto& entry : screen.entries)
            {
                // A window the user took elsewhere since the last scroll isn't the scroll's to bring back
                RECT rect{};
                const bool whereLeft = GetWindowRect(entry.window, &rect) && rect.top == entry.canvasTop - offset && rect.left == entry.left;
                if (entry.parked || rect.top == entry.parkedTop || (offset != 0 && (whereLeft || IsIconic(entry.window))))
                {
                    Return(entry, screen);
                }
            }
        }

        screens.clear();
        SaveParked();
        PublishParked();
        if (notify && any)
        {
            pageChanged(nullptr, 0);
        }
    }

    // A crash leaves the windows parked: the file tells the next run where they go
    void SaveParked() const noexcept
    {
        try
        {
            const auto fileName = ParkedFileName();
            const auto tempFileName = fileName + L".tmp";
            std::ofstream file(tempFileName, std::ios::trunc);
            bool any = false;
            for (const auto& [_, screen] : screens)
            {
                const bool held = screen.offset != 0 || std::any_of(screen.entries.begin(), screen.entries.end(), [](const Entry& entry) { return entry.parked; });
                for (const auto& entry : held ? screen.entries : std::vector<Entry>{})
                {
                    const auto& n = entry.normal;
                    DWORD process{};
                    GetWindowThreadProcessId(entry.window, &process);
                    file << reinterpret_cast<intptr_t>(entry.window) << ' ' << process << ' ' << entry.left << ' ' << entry.canvasTop << ' ' << entry.width << ' ' << entry.height << ' '
                         << n.left << ' ' << n.top << ' ' << n.right << ' ' << n.bottom << ' ' << entry.maximized << ' ' << entry.maximizedTop << ' ' << entry.parkedTop << ' '
                         << screen.top << ' ' << screen.height << ' ' << screen.offset << '\n';
                    any = true;
                }
            }

            file.close();
            if (!any)
            {
                DeleteFileW(tempFileName.c_str());
                DeleteFileW(fileName.c_str());
            }
            else if (file.fail() || !MoveFileExW(tempFileName.c_str(), fileName.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                Logger::error(L"Saving the parked windows failed");
            }
        }
        catch (const std::exception&)
        {
            Logger::error(L"Saving the parked windows failed");
        }
    }

    void RecoverParked() noexcept
    {
        try
        {
            const auto fileName = ParkedFileName();
            std::ifstream file(fileName);
            intptr_t handle{};
            DWORD process{};
            Entry entry;
            Screen screen;
            while (file >> handle >> process >> entry.left >> entry.canvasTop >> entry.width >> entry.height >> entry.normal.left >> entry.normal.top >> entry.normal.right >> entry.normal.bottom >> entry.maximized >> entry.maximizedTop >> entry.parkedTop >> screen.top >> screen.height >> screen.offset)
            {
                entry.window = reinterpret_cast<HWND>(handle);
                RECT rect{};
                DWORD owner{};
                // Handles get reused: only a window of that process where the scroll left it is one of ours
                const LONG top = entry.canvasTop - std::lround(screen.offset);
                if (GetWindowThreadProcessId(entry.window, &owner) && owner == process && GetWindowRect(entry.window, &rect) && (rect.top == entry.parkedTop || rect.top == top || IsIconic(entry.window)))
                {
                    Return(entry, screen);
                }
            }

            file.close();
            DeleteFileW(fileName.c_str());
        }
        catch (const std::exception&)
        {
            Logger::error(L"Recovering the parked windows failed");
        }
    }
};

bool DesktopScroll::IsParked(HWND window) noexcept
{
    std::lock_guard lock(parkedLock);
    return parkedWindows.contains(window);
}

HWND DesktopScroll::TakeLandedWindow() noexcept
{
    return m_impl->landedRider.exchange(nullptr);
}

void DesktopScroll::CarryToPage(HMONITOR monitor, HWND window, int pages) noexcept
{
    if (m_impl->window)
    {
        PostMessageW(m_impl->window, pages > 0 ? WM_SCROLL_CARRY_DOWN : WM_SCROLL_CARRY_UP, reinterpret_cast<WPARAM>(monitor), reinterpret_cast<LPARAM>(window));
    }
}

void DesktopScroll::RestOnPage(HMONITOR monitor, HWND window) noexcept
{
    if (Impl::self)
    {
        PostMessageW(Impl::self->window, WM_SCROLL_REST_ON_PAGE, reinterpret_cast<WPARAM>(monitor), reinterpret_cast<LPARAM>(window));
    }
}

DesktopScroll::DesktopScroll(HINSTANCE instance, PageChanged pageChanged) :
    m_impl(std::make_unique<Impl>())
{
    m_impl->instance = instance;
    m_impl->pageChanged = std::move(pageChanged);

    std::promise<void> ready;
    m_impl->thread = std::thread([this, &ready] { m_impl->Run(ready); });
    ready.get_future().wait();
}

DesktopScroll::~DesktopScroll()
{
    if (m_impl->window)
    {
        PostMessageW(m_impl->window, WM_SCROLL_STOP, 0, 0);
    }

    m_impl->thread.join();
}

void DesktopScroll::ReturnAll() noexcept
{
    if (m_impl->window)
    {
        PostMessageW(m_impl->window, WM_SCROLL_RETURN_ALL, 0, 0);
    }
}
