#include "pch.h"
#include "StageStrip.h"
#include "DockMotion.h"
#include "StageStripMotion.h"

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
    constexpr COLORREF HoverColor = RGB(0x4A, 0x4A, 0x5C); // the tile under the cursor
    constexpr int CornerRadius = 6; // StageProto's default
    constexpr UINT_PTR AnimTimer = 1;
    constexpr UINT AnimMillis = 16; // ~60 Hz, only while a tile moves

    double EaseOut(double progress) noexcept
    {
        return DockMotion::Ease(progress, false);
    }
}

struct StageStrip::Impl
{
    static inline Impl* self{};

    HWND target{};
    UINT clickMessage{};
    HINSTANCE instance{};
    HWND control{};
    HBRUSH brush{};
    HBRUSH hoverBrush{};
    bool timerOn{};
    std::thread thread;

    // Shared with the main thread
    std::mutex lock;
    std::vector<StageStripPlan::Strip> wanted;
    bool pending{};
    bool leaving{}; // the picture is the one of Leave: the scroll memory goes with it

    // Render thread only
    struct Shown
    {
        HWND overlay{};
        StageStripPlan::Strip strip; // what is applied; a tile whose thumbnail could not be registered is stored as not live, so the next picture retries it
        std::map<std::uintptr_t, HTHUMBNAIL> thumbs;
        std::map<std::uintptr_t, StageStripMotion::Motion> motion; // where each tile of 'strip' is drawn, and on its way to where
        std::uintptr_t pressed{};
        std::uintptr_t hover{};
        bool tracking{};
        int wheel{}; // what is left of a wheel notch
    };

    std::vector<Shown> shown;

    // The strips as the main thread published them, and how far each one is scrolled. 'shown' holds what that projects to.
    struct View
    {
        StageStripPlan::Strip full;
        int offset{};
    };

    std::vector<View> views;
    StageStripMotion::OffsetMemory memory; // survives a strip that is hidden meanwhile

    void Run(std::promise<void>& ready) noexcept
    {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // physical pixels, as the stage computes them
        self = this;
        brush = CreateSolidBrush(TileColor);
        hoverBrush = CreateSolidBrush(HoverColor);

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
            KillTimer(this->control, AnimTimer);
            DestroyWindow(this->control);
        }

        UnregisterClassW(OverlayClass, instance);
        UnregisterClassW(ControlClass, instance);
        DeleteObject(brush);
        DeleteObject(hoverBrush);
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
        case WM_TIMER:
            if (wparam == AnimTimer)
            {
                self->Tick();
            }

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
        return StageStripMotion::HitDrawn(s.strip.tiles, s.motion, x, y); // where the tile is drawn now, not where it is going
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
        case WM_MOUSEMOVE:
            if (auto* s = self ? self->Find(hwnd) : nullptr)
            {
                if (!s->tracking)
                {
                    TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd, 0 };
                    s->tracking = TrackMouseEvent(&track) != FALSE;
                }

                self->SetHover(*s, TileAt(*s, lparam));
            }

            return 0;
        case WM_MOUSELEAVE:
            if (auto* s = self ? self->Find(hwnd) : nullptr)
            {
                s->tracking = false;
                self->SetHover(*s, 0);
            }

            return 0;
        case WM_MOUSEWHEEL:
            if (auto* s = self ? self->Find(hwnd) : nullptr)
            {
                self->Wheel(*s, GET_WHEEL_DELTA_WPARAM(wparam));
            }

            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint;
            const HDC dc = BeginPaint(hwnd, &paint);
            if (auto* s = self ? self->Find(hwnd) : nullptr)
            {
                const auto it = s->hover ? s->motion.find(s->hover) : s->motion.end();
                if (it != s->motion.end())
                {
                    const RECT r = Local(it->second.cur, s->strip.area);
                    FillRect(dc, &r, self->hoverBrush); // the pad around the (raised) thumbnail
                }
            }

            EndPaint(hwnd, &paint);
            return 0;
        }
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

        const auto motion = s.motion.find(tile.window);
        const auto& rect = motion != s.motion.end() ? motion->second.cur : tile.rect;
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        properties.rcDestination = Local(StageStripMotion::Deflate(rect, StageStripMotion::PadOf(tile.window == s.hover)), s.strip.area);
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

    // The overlay's shape: the union of the tiles where they are drawn now, with rounded corners (a plain rect where the system
    // cannot make the region). Everything outside is transparent and click-through.
    static void ApplyRegion(Shown& s) noexcept
    {
        HRGN all = CreateRectRgn(0, 0, 0, 0);
        for (const auto& tile : s.strip.tiles)
        {
            const auto it = s.motion.find(tile.window);
            const RECT r = Local(it != s.motion.end() ? it->second.cur : tile.rect, s.strip.area);
            const int radius = StageStripMotion::ClampRadius(CornerRadius, r.right - r.left, r.bottom - r.top);
            HRGN part = radius > 0 ? CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius * 2, radius * 2) : nullptr;
            if (!part)
            {
                part = CreateRectRgn(r.left, r.top, r.right, r.bottom);
            }

            CombineRgn(all, all, part, RGN_OR);
            DeleteObject(part);
        }

        SetWindowRgn(s.overlay, all, TRUE); // the system owns the region from here
        if (s.hover)
        {
            InvalidateRect(s.overlay, nullptr, TRUE); // the hover pad follows the tile
        }
    }

    void SetHover(Shown& s, std::uintptr_t window) noexcept
    {
        if (s.hover == window)
        {
            return;
        }

        const auto before = s.hover;
        s.hover = window;
        for (const auto id : { before, window })
        {
            if (const auto* tile = StageStripPlan::FindTile(s.strip.tiles, id); tile && tile->live)
            {
                Place(s, *tile);
            }
        }

        InvalidateRect(s.overlay, nullptr, TRUE);
    }

    // The hover after the tiles moved under a cursor that did not: what is drawn under it now (only where the overlay is on top)
    void RefreshHover(Shown& s) noexcept
    {
        POINT pt{};
        std::uintptr_t tile = 0;
        if (GetCursorPos(&pt) && WindowFromPoint(pt) == s.overlay)
        {
            tile = StageStripMotion::HitDrawn(s.strip.tiles, s.motion, pt.x, pt.y);
            if (tile && !s.tracking)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, s.overlay, 0 };
                s.tracking = TrackMouseEvent(&track) != FALSE;
            }
        }

        SetHover(s, tile);
    }

    void Wheel(Shown& s, int delta) noexcept
    {
        const HWND overlay = s.overlay; // 's' may be gone (or moved) after the refresh
        for (auto& view : views)
        {
            if (view.full.key == s.strip.key)
            {
                const int total = static_cast<int>(view.full.tiles.size());
                const int steps = StageStripMotion::WheelSteps(s.wheel, delta);
                const int offset = StageStripMotion::ClampOffset(view.offset + steps, total, view.full.fit);
                if (offset != view.offset)
                {
                    view.offset = offset;
                    memory.Remember(view.full.key, view.full.layout, offset);
                    Refresh();
                    if (auto* again = Find(overlay))
                    {
                        RefreshHover(*again);
                    }
                }

                return;
            }
        }
    }

    bool AnyActive() const noexcept
    {
        return std::any_of(shown.begin(), shown.end(), [](const Shown& s) {
            return std::any_of(s.motion.begin(), s.motion.end(), [](const auto& m) { return m.second.Active(); });
        });
    }

    // The timer runs only while a tile is on its way
    void UpdateTimer() noexcept
    {
        const bool need = AnyActive();
        if (need && !timerOn)
        {
            timerOn = SetTimer(control, AnimTimer, AnimMillis, nullptr) != 0;
        }
        else if (!need && timerOn)
        {
            KillTimer(control, AnimTimer);
            timerOn = false;
        }
    }

    void Tick() noexcept
    {
        const auto now = GetTickCount64();
        const auto duration = static_cast<std::uint64_t>(DockMotion::EaseSeconds * 1000);
        for (auto& s : shown)
        {
            bool moved = false;
            for (auto& entry : s.motion)
            {
                moved = StageStripMotion::Step(entry.second, now, duration, EaseOut) || moved;
            }

            if (moved)
            {
                for (const auto& tile : s.strip.tiles)
                {
                    if (tile.live)
                    {
                        Place(s, tile);
                    }
                }

                ApplyRegion(s);
                RefreshHover(s);
            }
        }

        UpdateTimer();
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

        // Tiles ease to their new places from where they are drawn; new ones appear in place, and a moved overlay snaps
        const auto now = GetTickCount64();
        const bool animate = change.kind == StageStripPlan::Kind::Update && !change.areaChanged;
        std::erase_if(s->motion, [&](const auto& m) { return !StageStripPlan::FindTile(s->strip.tiles, m.first); });
        for (const auto& tile : s->strip.tiles)
        {
            const auto it = s->motion.find(tile.window);
            if (it == s->motion.end() || !animate)
            {
                s->motion[tile.window] = StageStripMotion::Motion::At(tile.rect, now);
            }
            else
            {
                StageStripMotion::Retarget(it->second, tile.rect, now);
            }
        }

        if (s->hover && !StageStripPlan::FindTile(s->strip.tiles, s->hover))
        {
            s->hover = 0;
        }

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

        ApplyRegion(*s); // the tiles' places (and the corners) may have changed even where the set of rects did not

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
            if (leaving)
            {
                memory.offsets.clear();
            }
        }

        std::vector<View> kept;
        for (auto& strip : next)
        {
            View view;
            // Remembered per monitor and layout (also while the strip was hidden); windows gone: back in range
            view.offset = memory.Recall(strip.key, strip.layout, static_cast<int>(strip.tiles.size()), strip.fit);
            memory.Remember(strip.key, strip.layout, view.offset);
            view.full = std::move(strip);
            kept.push_back(std::move(view));
        }

        views = std::move(kept);
        Refresh();
    }

    // Applies what the views project to (their scroll offsets) over what is shown
    void Refresh() noexcept
    {
        std::vector<StageStripPlan::Strip> now, projected;
        for (const auto& s : shown)
        {
            now.push_back(s.strip);
        }

        for (const auto& view : views)
        {
            projected.push_back(StageStripMotion::Project(view.full, view.offset));
        }

        for (const auto& change : StageStripPlan::DiffStrips(now, projected))
        {
            Apply(change);
        }

        UpdateTimer();
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
            m_impl->leaving = wait;
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
