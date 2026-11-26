#include "pch.h"
#include "StageStrip.h"
#include <common/logger/logger.h>
#include "DockMotion.h"
#include "StageStripMotion.h"

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <dwmapi.h>

// No logging in the render path (Place, Tick, Apply): only the settle retry path logs, and only for tiles that are not in order.

namespace
{
    constexpr wchar_t ControlClass[] = L"MuntinStageStripControl";
    constexpr wchar_t OverlayClass[] = L"MuntinStageStrip";
    constexpr wchar_t WellClass[] = L"MuntinStageStripWell";
    constexpr UINT WM_STRIP_SYNC = WM_APP + 1;
    constexpr UINT WM_STRIP_STOP = WM_APP + 2;
    constexpr UINT WaitMillis = 500;
    constexpr COLORREF TileColor = RGB(0x1B, 0x1B, 0x20); // a tile without a thumbnail (and what a thumbnail's transparent corners show)
    constexpr BYTE WellAlpha = 1; // the well under the tiles: invisible, but a layered window with alpha 0 would let the mouse through
    constexpr UINT WM_STRIP_RADIUS = WM_APP + 3;
    constexpr UINT_PTR AnimTimer = 1;
    constexpr UINT_PTR SettleTimer = 2;
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
    bool timerOn{};
    std::thread thread;

    // Shared with the main thread
    std::mutex lock;
    std::vector<StageStripPlan::Strip> wanted;
    bool pending{};
    bool leaving{}; // the picture is the one of Leave: the scroll memory goes with it
    std::atomic<int> radius{ 12 }; // the corner radius of the tiles (settings().stageStripRadius), read by the render thread

    // Render thread only
    struct Shown
    {
        HWND overlay{}; // the thumbnails, shaped to them: everything else is see-through
        HWND well{}; // under the overlay, from the first drawn tile to the last: takes the wheel (and swallows clicks) between the tiles
        StageStripPlan::Strip strip; // what is applied; a tile whose thumbnail could not be registered is stored as not live, so the next picture retries it
        std::map<std::uintptr_t, HTHUMBNAIL> thumbs;
        std::map<std::uintptr_t, SIZE> lastSource; // the source size the crop of the thumbnail was computed from
        std::map<std::uintptr_t, RECT> lastFit; // where the thumbnail was last drawn (strip-local): the hover highlight hugs it
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
    mutable std::mutex offsetLock;
    std::map<std::wstring, int> offsetMirror; // the scroll offset of every strip as drawn, for the main thread (ScrollOffset)
    std::set<std::uintptr_t> liveSeen; // windows the renderer found live itself after the main thread said they were not
    std::map<std::uintptr_t, StageStripPlan::Tile> gaveUp; // tiles still not in order after a whole cycle: not retried until their tile changes
    bool scrollRefresh{}; // the Refresh in progress is a wheel step: the only picture whose tiles ease
    int settleStep{};
    bool settleTimerOn{};
    bool settleTroubled{}; // something was not in order during this settle cycle: it is logged
    StageStripMotion::OffsetMemory memory; // survives a strip that is hidden meanwhile

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

        WNDCLASSEXW wellClass{};
        wellClass.cbSize = sizeof(wellClass);
        wellClass.lpfnWndProc = WellProc;
        wellClass.hInstance = instance;
        wellClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wellClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wellClass.lpszClassName = WellClass;
        RegisterClassExW(&wellClass);

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
            KillTimer(this->control, SettleTimer);
            DestroyWindow(this->control);
        }

        UnregisterClassW(OverlayClass, instance);
        UnregisterClassW(WellClass, instance);
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
        case WM_STRIP_RADIUS:
            for (auto& s : self->shown)
            {
                if (s.overlay)
                {
                    ApplyRegion(s);
                    InvalidateRect(s.overlay, nullptr, TRUE);
                }
            }

            return 0;
        case WM_TIMER:
            if (wparam == AnimTimer)
            {
                self->Tick();
            }
            else if (wparam == SettleTimer)
            {
                self->Settle();
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

    Shown* FindWell(HWND well) noexcept
    {
        for (auto& s : shown)
        {
            if (s.well == well)
            {
                return &s;
            }
        }

        return nullptr;
    }

    // Between the tiles: the wheel scrolls the strip as it does over a tile; a click does nothing (it would reach the desktop otherwise)
    static LRESULT CALLBACK WellProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEWHEEL:
            if (auto* s = self ? self->FindWell(hwnd) : nullptr)
            {
                self->Wheel(*s, GET_WHEEL_DELTA_WPARAM(wparam));
            }

            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
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
        case WM_DPICHANGED:
            // The corner radius scales with the DPI: rebuild the regions even when the strip's layout stays the same. Posted, since this
            // arrives inside Apply's SetWindowPos, before the tiles are updated; the suggested rect is ignored (Apply sets the exact one).
            if (self && self->control)
            {
                PostMessageW(self->control, WM_STRIP_RADIUS, 0, 0);
            }

            return 0;
        case WM_PAINT:
        {
            // No hover highlight: a hovered tile only grows (Place), so the strip stays see-through. The class brush erases what the region shows.
            PAINTSTRUCT paint;
            BeginPaint(hwnd, &paint);
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
            s.lastSource.erase(window);
            s.lastFit.erase(window);
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

        if (s.well)
        {
            DestroyWindow(s.well);
            s.well = nullptr;
        }
    }

    // The well spans the tiles where they are drawn now (their bounding box, cut at the strip's edges), so the empty rest of the strip
    // column stays clickable: the desktop, or a window over it. Hidden while there is no tile, or while the overlay is. reorder: put
    // it right under the overlay again (once it is shown); otherwise its place in the z-order is kept.
    static void PlaceWell(const Shown& s, bool reorder) noexcept
    {
        if (!s.well)
        {
            return;
        }

        RECT span{};
        for (const auto& tile : s.strip.tiles)
        {
            const auto it = s.motion.find(tile.window);
            const auto& r = it != s.motion.end() ? it->second.cur : tile.rect;
            const RECT rect{ r.L, r.T, r.R, r.B };
            UnionRect(&span, &span, &rect);
        }

        if (s.strip.clip.W() > 0 && s.strip.clip.H() > 0)
        {
            const RECT clip{ s.strip.clip.L, s.strip.clip.T, s.strip.clip.R, s.strip.clip.B };
            IntersectRect(&span, &span, &clip);
        }

        if (IsRectEmpty(&span) || !IsWindowVisible(s.overlay))
        {
            if (IsWindowVisible(s.well))
            {
                ShowWindow(s.well, SW_HIDE);
            }

            return;
        }

        SetWindowPos(s.well, reorder ? s.overlay : nullptr, span.left, span.top, span.right - span.left, span.bottom - span.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | (reorder ? 0 : SWP_NOZORDER));
    }

    static RECT Local(const StageLayouts::LRect& r, const StageLayouts::LRect& area) noexcept
    {
        return { r.L - area.L, r.T - area.T, r.R - area.L, r.B - area.T };
    }

    // The shape of a tile on the overlay (strip-local): the thumbnail where Place last fit it, with the highlight ring when hovered; a
    // tile without a fit (no thumbnail yet, or not live) is its drawn rect. ApplyRegion and WM_PAINT both use it.
    static StageStripMotion::TileShape ShapeFor(const Shown& s, std::uintptr_t window, bool live) noexcept
    {
        const auto it = s.motion.find(window);
        const auto* tile = StageStripPlan::FindTile(s.strip.tiles, window);
        RECT r = it != s.motion.end() ? Local(it->second.cur, s.strip.area) : tile ? Local(tile->rect, s.strip.area) : RECT{};
        if (const auto fit = s.lastFit.find(window); live && fit != s.lastFit.end())
        {
            r = fit->second; // set by Place, which runs before this wherever the tiles move
        }

        return StageStripMotion::ShapeOf(StageLayouts::LRect(r.left, r.top, r.right, r.bottom), StageStripMotion::ScaleRadius(self->radius.load(), s.overlay ? static_cast<int>(GetDpiForWindow(s.overlay)) : 0),
                                         false, 0); // no hover ring: the hovered thumbnail just grows
    }

    static HRGN RoundRegion(const StageLayouts::LRect& r, int radius) noexcept
    {
        HRGN rgn = radius > 0 ? CreateRoundRectRgn(r.L, r.T, r.R + 1, r.B + 1, radius * 2, radius * 2) : nullptr;
        return rgn ? rgn : CreateRectRgn(r.L, r.T, r.R, r.B); // a plain rect where the system cannot make the rounded one
    }

    // The region of a shape: the rounded outer rect; with a ring, its middle (the thumbnail's square rect) is swapped for the thumbnail
    // rounded at its own radius, so the thumbnail's corners are cut as they are at rest. The caller owns (deletes or hands off) the result.
    static HRGN RegionOf(const StageStripMotion::TileShape& shape) noexcept
    {
        HRGN rgn = RoundRegion(shape.outer, shape.outerRadius);
        if (!rgn || !shape.ring)
        {
            return rgn;
        }

        HRGN square = CreateRectRgn(shape.thumb.L, shape.thumb.T, shape.thumb.R, shape.thumb.B);
        HRGN round = RoundRegion(shape.thumb, shape.thumbRadius);
        if (square && round)
        {
            CombineRgn(rgn, rgn, square, RGN_DIFF);
            CombineRgn(rgn, rgn, round, RGN_OR);
        }

        if (square)
        {
            DeleteObject(square);
        }

        if (round)
        {
            DeleteObject(round);
        }

        return rgn;
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
        const auto box = StageStripMotion::Deflate(rect, StageStripMotion::PadOf(tile.window == s.hover));
        properties.rcDestination = Local(box, s.strip.area);
        // The window without its invisible resize frame: cropped by the frame insets (the title bar stays, as it does not with client-area-only)
        SIZE source{};
        if (FAILED(DwmQueryThumbnailSourceSize(it->second, &source)))
        {
            source = {};
        }

        s.lastSource[tile.window] = source;
        // Known size: a valid crop always replaces the old rcSource. Unknown size: no rcSource is sent (DWM keeps the last one; the
        // settle timer asks again)
        int shownW = source.cx, shownH = source.cy; // the part of the source that is shown: the crop, or the whole of it
        if (const auto crop = StageStripPlan::CropFor(source.cx, source.cy, tile.insets))
        {
            properties.dwFlags |= DWM_TNP_RECTSOURCE;
            properties.rcSource = { crop->L, crop->T, crop->R, crop->B };
            shownW = crop->W();
            shownH = crop->H();
        }

        // A parked window keeps its own shape: shown as large as fits the tile, centered (not stretched to it). With the size unknown the
        // whole tile (FitRect). The rounded tile behind it is letterboxed: the thumbnail itself stays square-cornered.
        properties.rcDestination = Local(StageStripMotion::FitRect(box, shownW, shownH), s.strip.area);
        s.lastFit[tile.window] = properties.rcDestination;

        properties.opacity = 255;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(it->second, &properties);
    }

    // The overlay's shape: the thumbnails where they are drawn now (with the highlight around the hovered one), with rounded corners
    // (a plain rect where the system cannot make the region). A tile without a thumbnail keeps its dark tile, or it would vanish.
    // Everything else is transparent, and the mouse goes through to the well below.
    static void ApplyRegion(Shown& s) noexcept
    {
        HRGN all = CreateRectRgn(0, 0, 0, 0);
        const RECT view = Local(s.strip.clip, s.strip.area);
        HRGN viewRgn = CreateRectRgn(view.left, view.top, view.right, view.bottom); // tiles on their way in or out are cut at the strip's edges
        for (const auto& tile : s.strip.tiles)
        {
            HRGN part = RegionOf(ShapeFor(s, tile.window, tile.live));
            if (!part)
            {
                continue;
            }

            if (s.strip.clip.W() > 0 && s.strip.clip.H() > 0)
            {
                CombineRgn(part, part, viewRgn, RGN_AND);
            }

            CombineRgn(all, all, part, RGN_OR);
            DeleteObject(part);
        }

        DeleteObject(viewRgn);
        SetWindowRgn(s.overlay, all, TRUE); // the system owns the region from here
        PlaceWell(s, false); // every relayout, scroll step and settle comes through here
        if (s.hover)
        {
            InvalidateRect(s.overlay, nullptr, TRUE);
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

        ApplyRegion(s); // the highlight is part of the shape
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
                const int steps = StageStripMotion::WheelSteps(s.wheel, delta);
                const int offset = StageStripMotion::ClampOffsetFor(view.full, view.offset + steps);
                if (offset != view.offset)
                {
                    view.offset = offset;
                    memory.Remember(view.full.key, view.full.layout, offset);
                    scrollRefresh = true; // only this picture slides
                    Refresh();
                    scrollRefresh = false;
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

    // A tile that slid out of view and stopped is taken out: its thumbnail goes, it is no longer drawn (the clip cut it already)
    void PruneSlidOff(Shown& s) noexcept
    {
        const auto view = std::find_if(views.begin(), views.end(), [&](const View& v) { return v.full.key == s.strip.key; });
        if (view == views.end())
        {
            return;
        }

        std::set<std::uintptr_t> needed;
        for (const auto& tile : StageStripMotion::ProjectSlide(view->full, view->offset).tiles)
        {
            needed.insert(tile.window);
        }

        bool removed = false;
        for (auto it = s.strip.tiles.begin(); it != s.strip.tiles.end();)
        {
            const auto motion = s.motion.find(it->window);
            const bool idle = motion != s.motion.end() && !motion->second.Active();
            if (idle && !needed.contains(it->window) && StageStripMotion::ClipRect(motion->second.cur, s.strip.clip).H() <= 0)
            {
                Unregister(s, it->window);
                s.motion.erase(it->window);
                if (s.hover == it->window)
                {
                    s.hover = 0;
                }

                it = s.strip.tiles.erase(it);
                removed = true;
            }
            else
            {
                ++it;
            }
        }

        if (removed)
        {
            ApplyRegion(s);
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

            PruneSlidOff(s);
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
            // The well first: the overlay, made after it, is above it
            fresh.well = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, WellClass, L"", WS_POPUP, change.next.area.L, change.next.area.T, change.next.area.W(), change.next.area.H(), nullptr, nullptr, instance, nullptr);
            if (fresh.well && !SetLayeredWindowAttributes(fresh.well, 0, WellAlpha, LWA_ALPHA))
            {
                DestroyWindow(fresh.well); // without it the wheel between the tiles reaches what is behind
                fresh.well = nullptr;
            }

            fresh.overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, OverlayClass, L"", WS_POPUP, change.next.area.L, change.next.area.T, change.next.area.W(), change.next.area.H(), nullptr, nullptr, instance, nullptr);
            if (!fresh.overlay)
            {
                Destroy(fresh);
                return; // The next picture tries again (the strip is not in 'shown', so it is a Create again)
            }

            fresh.strip.key = change.next.key;
            fresh.strip.area = change.next.area;
            fresh.strip.clip = change.next.clip;
            shown.push_back(std::move(fresh));
            s = &shown.back();
        }
        else
        {
            if (change.areaChanged)
            {
                s->strip.area = change.next.area;
            }

            s->strip.clip = change.next.clip; // the margins or the work area changed: the tiles are cut at the new edges
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

        // Tiles ease to their new places from where they are drawn (a wheel step only); the others are at their rect at once
        const auto now = GetTickCount64();
        const bool animate = StageStripMotion::ShouldAnimate(scrollRefresh, change.kind == StageStripPlan::Kind::Update, change.areaChanged);
        StageStripMotion::ApplyMotions(s->motion, s->strip.tiles, now, animate);

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
            PlaceWell(*s, true); // shown with the overlay, right under it
        }

        StartSettle();
    }

    // After a plan change the windows are still being parked, resized and painted: look again a few times (bounded, no polling after)
    void StartSettle() noexcept
    {
        settleStep = 0;
        settleTroubled = false;
        settleTimerOn = SetTimer(control, SettleTimer, StageStripMotion::SettleMillis, nullptr) != 0;
    }

    void StopSettle() noexcept
    {
        if (settleTimerOn)
        {
            KillTimer(control, SettleTimer);
            settleTimerOn = false;
        }
    }

    static bool WindowLooksLive(HWND window) noexcept
    {
        if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window))
        {
            return false;
        }

        BOOL cloaked = FALSE;
        return !(SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked);
    }

    void Settle() noexcept
    {
        if (shown.empty())
        {
            gaveUp.clear();
            StopSettle();
            return;
        }

        settleStep++;
        const int step = settleStep;
        bool all = true;
        bool refresh = false;
        std::vector<StageStripPlan::Tile> notInOrder;
        for (auto& s : shown)
        {
            bool placed = false;
            for (const auto& tile : s.strip.tiles)
            {
                const auto given = gaveUp.find(tile.window);
                if (given != gaveUp.end())
                {
                    if (given->second == tile)
                    {
                        continue; // given up on with this very tile (rect, liveness, insets): not looked at again
                    }

                    gaveUp.erase(given); // the plan changed it: another chance
                }

                const HWND window = reinterpret_cast<HWND>(tile.window);
                const auto thumb = s.thumbs.find(tile.window);
                const bool registered = thumb != s.thumbs.end();
                SIZE now{};
                HRESULT hr = S_OK;
                if (registered)
                {
                    hr = DwmQueryThumbnailSourceSize(thumb->second, &now);
                    if (FAILED(hr))
                    {
                        now = {};
                    }
                }

                const auto lastIt = s.lastSource.find(tile.window);
                const SIZE last = lastIt != s.lastSource.end() ? lastIt->second : SIZE{};
                const bool ok = StageStripMotion::TileSettled(registered, tile.live, now.cx, now.cy, last.cx, last.cy);
                all = all && ok;
                const bool looksLive = WindowLooksLive(window);
                if (!tile.live && looksLive && liveSeen.insert(tile.window).second)
                {
                    refresh = true; // live after all (or its registration failed): the next picture registers it
                }

                if (!ok)
                {
                    settleTroubled = true;
                    notInOrder.push_back(tile);
                }

                if ((!ok && StageStripMotion::ShouldLogStep(step)) || (step == StageStripMotion::ForcedStep && settleTroubled))
                {
                    const auto crop = StageStripPlan::CropFor(now.cx, now.cy, tile.insets);
                    const auto inEffect = StageStripPlan::CropFor(last.cx, last.cy, tile.insets); // what the last Place sent
                    wchar_t cls[64]{};
                    GetClassNameW(window, cls, 64);
                    Logger::info(L"Stage strip settle step={} hwnd={:x} class={} registered={} planLive={} looksLive={} hr={:x} source={}x{} last={}x{} crop={},{},{},{} inEffect={},{},{},{}", step, tile.window, cls, registered, tile.live, looksLive, static_cast<unsigned>(hr), now.cx, now.cy, last.cx, last.cy, crop ? crop->L : -1, crop ? crop->T : -1, crop ? crop->R : -1, crop ? crop->B : -1, inEffect ? inEffect->L : -1, inEffect ? inEffect->T : -1, inEffect ? inEffect->R : -1, inEffect ? inEffect->B : -1);
                }

                if (registered && StageStripMotion::ShouldReplace(now.cx, now.cy, last.cx, last.cy, step))
                {
                    Place(s, tile);
                    placed = true;
                }
            }

            if (placed)
            {
                ApplyRegion(s); // the outline follows the thumbnail's new fit
            }
        }

        if (refresh)
        {
            Refresh(); // may restart the cycle (Apply)
            return;
        }

        if (StageStripMotion::SettleDone(step, all))
        {
            if (!all && step >= StageStripMotion::MaxSteps)
            {
                for (const auto& tile : notInOrder)
                {
                    gaveUp[tile.window] = tile; // the next Apply does not run a whole cycle again for it
                }
            }

            StopSettle();
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
                gaveUp.clear();
                memory.offsets.clear();
            }
        }

        std::vector<View> kept;
        for (auto& strip : next)
        {
            View view;
            // Remembered per monitor and layout (also while the strip was hidden); windows gone: back in range
            view.offset = StageStripMotion::ClampOffsetFor(strip, memory.Raw(strip.key, strip.layout));
            memory.Remember(strip.key, strip.layout, view.offset);
            view.full = std::move(strip);
            kept.push_back(std::move(view));
        }

        views = std::move(kept);
        liveSeen.clear(); // the picture is fresh: the word of the main thread counts again
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
            // A wheel step: tiles that are shown and move out of view stay until they have slid off (PruneSlidOff takes them out then)
            std::set<std::uintptr_t> keep;
            if (scrollRefresh)
            {
                for (const auto& s : shown)
                {
                    if (s.strip.key == view.full.key)
                    {
                        for (const auto& tile : s.strip.tiles)
                        {
                            keep.insert(tile.window);
                        }
                    }
                }
            }

            auto strip = StageStripMotion::ProjectSlide(view.full, view.offset, scrollRefresh ? &keep : nullptr);
            for (auto& tile : strip.tiles)
            {
                tile.live = tile.live || liveSeen.contains(tile.window);
            }

            projected.push_back(std::move(strip));
        }

        for (const auto& change : StageStripPlan::DiffStrips(now, projected))
        {
            Apply(change);
        }

        {
            std::lock_guard guard(offsetLock);
            offsetMirror.clear();
            for (const auto& view : views)
            {
                offsetMirror[view.full.key] = view.offset;
            }
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

int StageStrip::ScrollOffset(const std::wstring& key) const noexcept
{
    try
    {
        std::lock_guard guard(m_impl->offsetLock);
        const auto it = m_impl->offsetMirror.find(key);
        return it == m_impl->offsetMirror.end() ? 0 : it->second;
    }
    catch (...)
    {
        return 0;
    }
}

void StageStrip::SetRadius(int pixels) noexcept
{
    const int radius = StageStripMotion::RadiusOf(pixels);
    if (m_impl->radius.exchange(radius) != radius && m_impl->control)
    {
        PostMessageW(m_impl->control, WM_STRIP_RADIUS, 0, 0); // the regions are rebuilt on the render thread
    }
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
