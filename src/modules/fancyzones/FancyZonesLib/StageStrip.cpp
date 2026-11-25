#include "pch.h"
#include "StageStrip.h"

#include <future>
#include <map>
#include <mutex>
#include <thread>

#include <dwmapi.h>

// No logging in this file: it is the render path.

namespace
{
    constexpr wchar_t ControlClass[] = L"MuntinStageStripControl";
    constexpr wchar_t OverlayClass[] = L"MuntinStageStrip";
    constexpr UINT WM_STRIP_SYNC = WM_APP + 1;
    constexpr UINT WM_STRIP_STOP = WM_APP + 2;
    constexpr UINT WaitMillis = 500;
    constexpr COLORREF TileColor = RGB(0x1B, 0x1B, 0x20); // as StageProto's strip
}

struct StageStrip::Impl
{
    static inline Impl* self{};

    HWND target{};
    UINT clickMessage{};
    HINSTANCE instance{};
    HWND control{};
    HBRUSH brush{};
    std::thread thread;

    // Shared with the main thread
    std::mutex lock;
    std::vector<StageStripPlan::Strip> wanted;
    bool pending{};

    // Render thread only
    struct Shown
    {
        HWND overlay{};
        StageStripPlan::Strip strip; // what is applied; a tile whose thumbnail could not be registered is stored as not live, so the next picture retries it
        std::map<std::uintptr_t, HTHUMBNAIL> thumbs;
        std::uintptr_t pressed{};
    };

    std::vector<Shown> shown;

    void Run(std::promise<void>& ready) noexcept
    {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // physical pixels, as the stage computes them
        self = this;
        brush = CreateSolidBrush(TileColor);

        WNDCLASSEXW controlClass{};
        controlClass.cbSize = sizeof(controlClass);
        controlClass.lpfnWndProc = ControlProc;
        controlClass.hInstance = instance;
        controlClass.lpszClassName = ControlClass;
        RegisterClassExW(&controlClass);

        WNDCLASSEXW overlayClass{};
        overlayClass.cbSize = sizeof(overlayClass);
        overlayClass.lpfnWndProc = OverlayProc;
        overlayClass.hInstance = instance;
        overlayClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        overlayClass.hbrBackground = brush;
        overlayClass.lpszClassName = OverlayClass;
        RegisterClassExW(&overlayClass);

        this->control = CreateWindowExW(0, ControlClass, L"", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        ready.set_value();
        if (this->control)
        {
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                DispatchMessageW(&message);
            }
        }

        for (auto& s : shown)
        {
            Destroy(s);
        }

        shown.clear();
        if (this->control)
        {
            DestroyWindow(this->control);
        }

        UnregisterClassW(OverlayClass, instance);
        UnregisterClassW(ControlClass, instance);
        DeleteObject(brush);
        self = nullptr;
    }

    static LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        switch (message)
        {
        case WM_STRIP_SYNC:
            self->Sync();
            return 0;
        case WM_STRIP_STOP:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    Shown* Find(HWND overlay) noexcept
    {
        for (auto& s : shown)
        {
            if (s.overlay == overlay)
            {
                return &s;
            }
        }

        return nullptr;
    }

    // The tile (window) under a client point of an overlay
    static std::uintptr_t TileAt(const Shown& s, LPARAM lparam) noexcept
    {
        const int x = static_cast<short>(LOWORD(lparam)) + s.strip.area.L;
        const int y = static_cast<short>(HIWORD(lparam)) + s.strip.area.T;
        return StageStripPlan::HitTile(s.strip.tiles, x, y);
    }

    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            if (auto* s = self ? self->Find(hwnd) : nullptr)
            {
                s->pressed = TileAt(*s, lparam);
            }

            return 0;
        case WM_LBUTTONUP:
            if (auto* s = self ? self->Find(hwnd) : nullptr)
            {
                const auto tile = TileAt(*s, lparam);
                const auto pressed = s->pressed;
                s->pressed = 0;
                if (tile && tile == pressed)
                {
                    PostMessageW(self->target, self->clickMessage, static_cast<WPARAM>(tile), 0);
                }
            }

            return 0;
        case WM_CAPTURECHANGED:
            if (auto* s = self ? self->Find(hwnd) : nullptr)
            {
                s->pressed = 0;
            }

            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    static void Unregister(Shown& s, std::uintptr_t window) noexcept
    {
        const auto it = s.thumbs.find(window);
        if (it != s.thumbs.end())
        {
            DwmUnregisterThumbnail(it->second);
            s.thumbs.erase(it);
        }
    }

    static void Destroy(Shown& s) noexcept
    {
        for (const auto& [_, id] : s.thumbs)
        {
            DwmUnregisterThumbnail(id);
        }

        s.thumbs.clear();
        if (s.overlay)
        {
            DestroyWindow(s.overlay);
            s.overlay = nullptr;
        }
    }

    static RECT Local(const StageLayouts::LRect& r, const StageLayouts::LRect& area) noexcept
    {
        return { r.L - area.L, r.T - area.T, r.R - area.L, r.B - area.T };
    }

    static void Place(Shown& s, const StageStripPlan::Tile& tile) noexcept
    {
        const auto it = s.thumbs.find(tile.window);
        if (it == s.thumbs.end())
        {
            return;
        }

        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = Local(tile.rect, s.strip.area);
        // The window without its invisible resize frame: cropped by the frame insets (the title bar stays, as it does not with client-area-only)
        SIZE source{};
        if (SUCCEEDED(DwmQueryThumbnailSourceSize(it->second, &source)) && source.cx > 0 && source.cy > 0)
        {
            const auto crop = StageStripPlan::SourceCrop(source.cx, source.cy, tile.insets);
            properties.dwFlags |= DWM_TNP_RECTSOURCE;
            properties.rcSource = { crop.L, crop.T, crop.R, crop.B };
        }

        properties.opacity = 255;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(it->second, &properties);
    }

    static void ApplyRegion(Shown& s) noexcept
    {
        HRGN all = CreateRectRgn(0, 0, 0, 0);
        for (const auto& tile : s.strip.tiles)
        {
            const RECT r = Local(tile.rect, s.strip.area);
            HRGN part = CreateRectRgn(r.left, r.top, r.right, r.bottom);
            CombineRgn(all, all, part, RGN_OR);
            DeleteObject(part);
        }

        SetWindowRgn(s.overlay, all, TRUE); // the system owns the region from here
    }

    void Apply(const StageStripPlan::Change& change) noexcept
    {
        if (change.kind == StageStripPlan::Kind::Destroy)
        {
            for (auto it = shown.begin(); it != shown.end(); ++it)
            {
                if (it->strip.key == change.key)
                {
                    Destroy(*it);
                    shown.erase(it);
                    break;
                }
            }

            return;
        }

        Shown* s = nullptr;
        for (auto& candidate : shown)
        {
            if (candidate.strip.key == change.key)
            {
                s = &candidate;
            }
        }

        if (change.kind == StageStripPlan::Kind::Create || !s)
        {
            Shown fresh;
            fresh.overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, OverlayClass, L"", WS_POPUP, change.next.area.L, change.next.area.T, change.next.area.W(), change.next.area.H(), nullptr, nullptr, instance, nullptr);
            if (!fresh.overlay)
            {
                return; // The next picture tries again (the strip is not in 'shown', so it is a Create again)
            }

            fresh.strip.key = change.next.key;
            fresh.strip.area = change.next.area;
            shown.push_back(std::move(fresh));
            s = &shown.back();
        }
        else if (change.areaChanged)
        {
            s->strip.area = change.next.area;
        }

        // The overlay's rect first, so that the destinations are in the right coordinates
        const auto& area = change.next.area;
        SetWindowPos(s->overlay, nullptr, area.L, area.T, area.W(), area.H(), SWP_NOACTIVATE | SWP_NOZORDER); // topmost since it was created: not raised again over fullscreen or other topmost windows

        for (const auto window : change.tiles.remove)
        {
            Unregister(*s, window);
        }

        // From now on the tiles are the new ones; a thumbnail that could not be registered is stored as not live (retried next time)
        s->strip.tiles = change.next.tiles;
        for (auto& tile : s->strip.tiles)
        {
            if (tile.live && s->thumbs.find(tile.window) == s->thumbs.end())
            {
                const bool isNew = std::any_of(change.tiles.add.begin(), change.tiles.add.end(), [&](const StageStripPlan::Tile& a) { return a.window == tile.window; });
                if (!isNew)
                {
                    continue;
                }

                HTHUMBNAIL id{};
                if (SUCCEEDED(DwmRegisterThumbnail(s->overlay, reinterpret_cast<HWND>(tile.window), &id)))
                {
                    s->thumbs[tile.window] = id;
                }
                else
                {
                    tile.live = false;
                }
            }
        }

        for (const auto& tile : s->strip.tiles)
        {
            if (tile.live)
            {
                Place(*s, tile); // new, moved, and the strip's area may have moved all of them
            }
        }

        if (change.tiles.regionChanged || change.areaChanged || change.kind == StageStripPlan::Kind::Create)
        {
            ApplyRegion(*s);
        }

        if (!IsWindowVisible(s->overlay))
        {
            ShowWindow(s->overlay, SW_SHOWNOACTIVATE);
        }
    }

    void Sync() noexcept
    {
        std::vector<StageStripPlan::Strip> next;
        {
            std::lock_guard guard(lock);
            if (!pending)
            {
                return; // A second wake for a picture that is already applied
            }

            next = std::move(wanted);
            wanted.clear();
            pending = false;
        }

        std::vector<StageStripPlan::Strip> now;
        for (const auto& s : shown)
        {
            now.push_back(s.strip);
        }

        for (const auto& change : StageStripPlan::DiffStrips(now, next))
        {
            Apply(change);
        }
    }
};

StageStrip::StageStrip(HWND target, UINT clickMessage) :
    m_impl(std::make_unique<Impl>())
{
    m_impl->target = target;
    m_impl->clickMessage = clickMessage;
    m_impl->instance = GetModuleHandleW(nullptr);

    std::promise<void> ready;
    m_impl->thread = std::thread([this, &ready] { m_impl->Run(ready); });
    ready.get_future().wait();
}

StageStrip::~StageStrip()
{
    if (m_impl->control)
    {
        PostMessageW(m_impl->control, WM_STRIP_STOP, 0, 0);
    }

    m_impl->thread.join();
}

void StageStrip::Publish(std::vector<StageStripPlan::Strip> strips, bool wait) noexcept
{
    try
    {
        bool post = false;
        {
            std::lock_guard guard(m_impl->lock);
            m_impl->wanted = std::move(strips);
            post = !m_impl->pending;
            m_impl->pending = true;
        }

        if (post && m_impl->control && !PostMessageW(m_impl->control, WM_STRIP_SYNC, 0, 0) && !wait)
        {
            // The queue is full: the next Publish must post again (a waiting one sends below instead, and needs the flag)
            std::lock_guard guard(m_impl->lock);
            m_impl->pending = false;
        }

        if (wait && m_impl->control)
        {
            // Leave: the overlays are gone when this returns (bounded: a stuck renderer must not hang the main thread)
            DWORD_PTR ignored{};
            SendMessageTimeoutW(m_impl->control, WM_STRIP_SYNC, 0, 0, SMTO_ABORTIFHUNG, WaitMillis, &ignored);
        }
    }
    catch (...)
    {
    }
}
