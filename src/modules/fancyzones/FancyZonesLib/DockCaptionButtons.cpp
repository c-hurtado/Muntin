#include "pch.h"
#include "DockCaptionButtons.h"

#include <algorithm>
#include <cmath>

namespace
{
    const wchar_t WindowClassName[] = L"FanciestZones_DockCaptionButtons";

    // Caption button size of a window at 100% scale
    constexpr int ButtonWidth = 46;
    constexpr int ButtonHeight = 32;
    constexpr int TabRowHeight = 40; // Windows Terminal
    constexpr int GlyphShift = 3; // The triangles sit this far right of the middle of their cells, closer to the minimize button
    constexpr float TriangleWidth = 5; // At 100% scale
    constexpr float TriangleHeight = 9;

    constexpr BYTE ActiveGlyphAlpha = 255;
    constexpr BYTE InactiveGlyphAlpha = 150; // The caption buttons of a window that isn't active are dimmed too
    constexpr BYTE HoverAlpha = 24;
    constexpr BYTE IdleAlpha = 1; // Not 0: the pixels with no alpha let the mouse through

    constexpr ULONGLONG SampleIntervalMillis = 2000; // How often the title bar color is looked at again

    // The scale of the monitor the window is on. The DPI of the window itself is 96 for an app that isn't DPI aware.
    int DpiOf(HWND window) noexcept
    {
        UINT x = 96, y = 96;
        if (const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST); monitor && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y)))
        {
            return static_cast<int>(x);
        }

        return 96;
    }

    bool IsShown(HWND window) noexcept
    {
        DWORD cloaked = 0;
        DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (!IsWindow(window) || !IsWindowVisible(window) || IsIconic(window) || cloaked)
        {
            return false;
        }

        // Full screen: no title bar. A maximized window can reach past the monitor too, it has one.
        RECT rect{};
        MONITORINFO monitor{ sizeof(monitor) };
        if (!IsZoomed(window) && GetWindowRect(window, &rect) && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) && DockCaptionButtons::IsInside(monitor.rcMonitor, rect))
        {
            return false;
        }

        return true;
    }

    struct CaptionButtons
    {
        RECT rect; // Screen rect of the minimize, maximize and close buttons
        int count;
        RECT window;
    };

    std::optional<CaptionButtons> CaptionButtonsOf(HWND window, int dpi) noexcept
    {
        // No caption, no buttons: the frame of a borderless window, or one that draws its own, is the app's
        const auto style = static_cast<LONG>(GetWindowLongPtrW(window, GWL_STYLE));
        const auto exStyle = static_cast<LONG>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        if ((style & WS_CAPTION) != WS_CAPTION || !(style & WS_SYSMENU))
        {
            return std::nullopt;
        }

        RECT windowRect{};
        if (!GetWindowRect(window, &windowRect))
        {
            return std::nullopt;
        }

        const int count = DockCaptionButtons::ButtonCount(style, exStyle);

        // Windows Terminal makes its caption as tall as the tab row, and the system reports the buttons as if it had not
        wchar_t className[48]{};
        GetClassNameW(window, className, ARRAYSIZE(className));
        const bool tabRowCaption = wcscmp(className, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0;

        RECT relative{};
        if (!tabRowCaption && SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CAPTION_BUTTON_BOUNDS, &relative, sizeof(relative))) && relative.right > relative.left && relative.bottom > relative.top)
        {
            OffsetRect(&relative, windowRect.left, windowRect.top);
            return CaptionButtons{ relative, count, windowRect };
        }

        // The buttons are where the system would put them
        RECT frame = windowRect;
        DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
        return CaptionButtons{ RECT{ frame.right - count * MulDiv(ButtonWidth, dpi, 96), frame.top, frame.right, frame.top + MulDiv(tabRowCaption ? TabRowHeight : ButtonHeight, dpi, 96) }, count, windowRect };
    }
}

namespace DockCaptionButtons
{
    RECT PairRect(const RECT& captionButtons, int buttonCount) noexcept
    {
        const LONG button = (captionButtons.right - captionButtons.left) / (std::max)(buttonCount, 1);
        const LONG cell = button * 3 / 5;
        return RECT{ captionButtons.left - 2 * cell, captionButtons.top, captionButtons.left, captionButtons.bottom };
    }

    BYTE TriangleCoverage(int x, int y, float centerX, float centerY, float width, float height, bool pointsLeft) noexcept
    {
        // 4x4 samples per pixel
        constexpr int Samples = 4;
        int inside = 0;
        for (int i = 0; i < Samples; i++)
        {
            for (int j = 0; j < Samples; j++)
            {
                const float dx = x + (i + 0.5f) / Samples - centerX;
                const float dy = y + (j + 0.5f) / Samples - centerY;
                // From the apex (0) to the base (1)
                const float along = pointsLeft ? (dx + width / 2) / width : (width / 2 - dx) / width;
                if (along >= 0 && along <= 1 && std::abs(dy) <= height / 2 * along)
                {
                    inside++;
                }
            }
        }

        return static_cast<BYTE>(inside * 255 / (Samples * Samples));
    }

    DWORD Pixel(BYTE coverage, bool lightGlyph, BYTE glyphAlpha, BYTE hoverAlpha) noexcept
    {
        const UINT glyph = coverage * glyphAlpha / 255;
        const UINT alpha = glyph + hoverAlpha * (255 - glyph) / 255;
        const UINT color = lightGlyph ? alpha : 0;
        return (alpha << 24) | (color << 16) | (color << 8) | color;
    }

    struct Overlays::Overlay
    {
        Overlays* owner{};
        HWND window{};
        HWND target{};
        RECT rect{};
        bool light{};
        bool active{};
        bool drawn{};
        ULONGLONG sampledAt{}; // Tick count of the last look at the title bar color
        bool ownChecked{}; // The minimize button that the app draws itself was looked for, in a window of the size below
        SIZE ownCheckedFor{};
        std::optional<RECT> own; // That button: left is the offset from the right edge of the window, top from its top edge, right and bottom its size
        int hover{ -1 }; // 0: back, 1: next
        int pressed{ -1 };
        bool tracking{};

        ~Overlay()
        {
            if (window)
            {
                DestroyWindow(window);
            }
        }
    };

    Overlays::Overlays(HINSTANCE hinstance, Click onClick) :
        m_hinstance(hinstance),
        m_onClick(std::move(onClick))
    {
        WNDCLASSEXW wcex{};
        wcex.cbSize = sizeof(wcex);
        wcex.lpfnWndProc = WndProc;
        wcex.hInstance = hinstance;
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.lpszClassName = WindowClassName;
        RegisterClassExW(&wcex);
    }

    Overlays::~Overlays()
    {
        m_overlays.clear();
        UnregisterClassW(WindowClassName, m_hinstance);
    }

    void Overlays::Hide() noexcept
    {
        for (const auto& [_, overlay] : m_overlays)
        {
            ShowWindow(overlay->window, SW_HIDE);
        }
    }

    void Overlays::Clear() noexcept
    {
        m_overlays.clear();
    }

    // Screen rect of the minimize button that an app draws itself, found through UI Automation: nothing else tells where it is
    std::optional<RECT> Overlays::FindOwnMinimizeButton(HWND window) noexcept
    {
        // A window that doesn't answer would hold up the thread of the keyboard hook
        if (IsHungAppWindow(window))
        {
            return std::nullopt;
        }

        if (!m_automation && FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(m_automation.put()))))
        {
            return std::nullopt;
        }

        winrt::com_ptr<IUIAutomationElement> root;
        winrt::com_ptr<IUIAutomationCondition> condition;
        winrt::com_ptr<IUIAutomationElement> minimize;
        VARIANT name{};
        name.vt = VT_BSTR;
        name.bstrVal = SysAllocString(L"Minimize");
        const bool found = SUCCEEDED(m_automation->ElementFromHandle(window, root.put())) &&
                           SUCCEEDED(m_automation->CreatePropertyCondition(UIA_NamePropertyId, name, condition.put())) &&
                           SUCCEEDED(root->FindFirst(TreeScope_Descendants, condition.get(), minimize.put())) && minimize;
        VariantClear(&name);

        RECT rect{};
        if (!found || FAILED(minimize->get_CurrentBoundingRectangle(&rect)) || rect.right <= rect.left || rect.bottom <= rect.top)
        {
            return std::nullopt;
        }

        return rect;
    }

    void Overlays::Update(const std::vector<HWND>& windows) noexcept
    {
        std::erase_if(m_overlays, [&](const auto& entry) {
            return std::find(windows.begin(), windows.end(), entry.first) == windows.end();
        });

        const HWND foreground = GetForegroundWindow();
        const ULONGLONG now = GetTickCount64();
        for (const HWND target : windows)
        {
            auto found = m_overlays.find(target);
            const int dpi = DpiOf(target);
            auto buttons = IsShown(target) ? CaptionButtonsOf(target, dpi) : std::nullopt;
            if (!buttons)
            {
                if (found != m_overlays.end())
                {
                    ShowWindow(found->second->window, SW_HIDE);
                }

                continue;
            }

            if (found == m_overlays.end())
            {
                auto overlay = std::make_unique<Overlay>();
                overlay->owner = this;
                overlay->target = target;
                overlay->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WindowClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, m_hinstance, overlay.get());
                if (!overlay->window)
                {
                    continue;
                }

                found = m_overlays.emplace(target, std::move(overlay)).first;
            }

            Overlay& overlay = *found->second;

            // An app that draws its own caption buttons (Chromium) puts them where it likes
            wchar_t className[32]{};
            GetClassNameW(target, className, ARRAYSIZE(className));
            if (_wcsnicmp(className, L"Chrome_WidgetWin_", 17) == 0)
            {
                const SIZE size{ buttons->window.right - buttons->window.left, buttons->window.bottom - buttons->window.top };
                if (!overlay.ownChecked || overlay.ownCheckedFor.cx != size.cx || overlay.ownCheckedFor.cy != size.cy)
                {
                    overlay.ownChecked = true;
                    overlay.ownCheckedFor = size;
                    overlay.own.reset();
                    if (const auto minimize = FindOwnMinimizeButton(target))
                    {
                        // Relative to the window, so that a move of the window needs no new look
                        overlay.own = RECT{ minimize->left - buttons->window.right, minimize->top - buttons->window.top, minimize->right - minimize->left, minimize->bottom - minimize->top };
                    }
                }

                if (overlay.own)
                {
                    const int left = buttons->window.right + overlay.own->left;
                    const int top = buttons->window.top + overlay.own->top;
                    buttons->rect = RECT{ left, top, left + 3 * overlay.own->right, top + overlay.own->bottom };
                    buttons->count = 3;
                }
            }

            const RECT rect = PairRect(buttons->rect, buttons->count);

            // Not over the content of the neighbor
            if (!IsInside(rect, buttons->window))
            {
                ShowWindow(overlay.window, SW_HIDE);
                continue;
            }

            // A title bar that is dark tells which glyph color is readable. An app that draws its own
            // title bar says nothing, so the color next to the buttons is looked at now and then.
            bool light = overlay.light;
            BOOL dark = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(target, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark))) && dark)
            {
                light = true;
            }
            else if (!overlay.drawn || now - overlay.sampledAt > SampleIntervalMillis)
            {
                overlay.sampledAt = now;
                HDC screen = GetDC(nullptr);
                const COLORREF behind = GetPixel(screen, rect.left - 4, (rect.top + rect.bottom) / 2);
                ReleaseDC(nullptr, screen);
                if (behind != CLR_INVALID)
                {
                    light = (GetRValue(behind) * 299 + GetGValue(behind) * 587 + GetBValue(behind) * 114) / 1000 < 128;
                }
            }

            const bool active = target == foreground;
            const bool redraw = !overlay.drawn || light != overlay.light || active != overlay.active || rect.right - rect.left != overlay.rect.right - overlay.rect.left || rect.bottom - rect.top != overlay.rect.bottom - overlay.rect.top;
            overlay.rect = rect;
            overlay.light = light;
            overlay.active = active;
            if (redraw)
            {
                Render(overlay);
            }

            // Nothing to show when the window could not be drawn
            if (!overlay.drawn)
            {
                ShowWindow(overlay.window, SW_HIDE);
                continue;
            }

            // Right above the window and not above the windows that cover it. A window that is always on top
            // is only passed by a window of that band.
            const bool topmost = (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            const HWND above = GetWindow(target, GW_HWNDPREV);
            UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
            if (above == overlay.window)
            {
                flags |= SWP_NOZORDER;
            }

            SetWindowPos(overlay.window, above ? above : (topmost ? HWND_TOPMOST : HWND_TOP), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, flags);
        }
    }

    void Overlays::Render(Overlay& overlay) noexcept
    {
        overlay.drawn = false;

        const int width = overlay.rect.right - overlay.rect.left;
        const int height = overlay.rect.bottom - overlay.rect.top;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        BITMAPINFO info{};
        info.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };

        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dc && bitmap && bits)
        {
            HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
            memset(bits, 0, static_cast<size_t>(width) * height * 4);

            // Pixels hold the coverage of the triangles: the color is chosen when they are composed
            const float scale = DpiOf(overlay.target) / 96.0f;
            const float shift = GlyphShift * scale;
            const float centerY = height / 2.0f;
            const float centerX[2]{ width / 4.0f + shift, width * 3 / 4.0f + shift };
            auto pixels = static_cast<DWORD*>(bits);
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    const int button = ButtonAt(x, width);
                    pixels[static_cast<ptrdiff_t>(y) * width + x] = TriangleCoverage(x, y, centerX[button], centerY, TriangleWidth * scale, TriangleHeight * scale, button == 0);
                }
            }

            const BYTE glyphAlpha = overlay.active ? ActiveGlyphAlpha : InactiveGlyphAlpha;
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    DWORD& pixel = pixels[y * width + x];
                    pixel = Pixel(static_cast<BYTE>(pixel & 0xFF), overlay.light, glyphAlpha, ButtonAt(x, width) == overlay.hover ? HoverAlpha : IdleAlpha);
                }
            }

            SIZE size{ width, height };
            POINT origin{};
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            overlay.drawn = UpdateLayeredWindow(overlay.window, screen, nullptr, &size, dc, &origin, 0, &blend, ULW_ALPHA) != FALSE;
            SelectObject(dc, oldBitmap);
        }

        if (bitmap)
        {
            DeleteObject(bitmap);
        }

        if (dc)
        {
            DeleteDC(dc);
        }

        ReleaseDC(nullptr, screen);
    }

    LRESULT CALLBACK Overlays::WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        if (message == WM_NCCREATE)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        }

        auto overlay = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!overlay)
        {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        const int width = overlay->rect.right - overlay->rect.left;
        switch (message)
        {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE:
        {
            if (!overlay->tracking)
            {
                TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
                overlay->tracking = TrackMouseEvent(&track) != FALSE;
            }

            const int hover = ButtonAt(GET_X_LPARAM(lparam), width);
            if (hover != overlay->hover)
            {
                overlay->hover = hover;
                overlay->owner->Render(*overlay);
            }

            return 0;
        }

        case WM_MOUSELEAVE:
            overlay->tracking = false;
            if (overlay->hover != -1)
            {
                overlay->hover = -1;
                overlay->owner->Render(*overlay);
            }

            return 0;

        case WM_LBUTTONDOWN:
            overlay->pressed = ButtonAt(GET_X_LPARAM(lparam), width);
            SetCapture(window);
            return 0;

        case WM_LBUTTONUP:
        {
            // Only a press and a release on the same button is a click
            const int pressed = std::exchange(overlay->pressed, -1);
            ReleaseCapture();
            if (pressed != -1 && ButtonAt(GET_X_LPARAM(lparam), width) == pressed)
            {
                overlay->owner->m_onClick(overlay->target, pressed == 0 ? Dock::Edge::Left : Dock::Edge::Right);
            }

            return 0;
        }

        case WM_CAPTURECHANGED:
            overlay->pressed = -1;
            return 0;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    }
}
