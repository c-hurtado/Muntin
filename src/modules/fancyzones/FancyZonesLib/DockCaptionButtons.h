#pragma once

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <UIAutomation.h>
#include <optional>
#include <winrt/base.h>

#include <FancyZonesLib/DockTree.h>

// The < > buttons next to the minimize, maximize and close buttons of the docked windows:
// they move the window to the neighbor on that side, like Shift+Win+Left and Shift+Win+Right.
namespace DockCaptionButtons
{
    // How many caption buttons a window has: close alone, or with minimize and maximize (a help button replaces them)
    constexpr int ButtonCount(LONG style, LONG exStyle) noexcept
    {
        if (style & (WS_MINIMIZEBOX | WS_MAXIMIZEBOX))
        {
            return 3;
        }

        return (exStyle & WS_EX_CONTEXTHELP) ? 2 : 1;
    }

    // Screen rect of the pair, left of the caption buttons of a window: as tall as they are, three fifths of the width
    // of one of them per button, so the chevrons stand closer than the caption buttons do.
    RECT PairRect(const RECT& captionButtons, int buttonCount = 3) noexcept;

    // Which half of a pair of the given width the x coordinate is in: 0 (back), 1 (next), -1 outside
    constexpr int ButtonAt(int x, int width) noexcept
    {
        return x < 0 || x >= width ? -1 : (x < width / 2 ? 0 : 1);
    }

    constexpr bool IsInside(const RECT& inner, const RECT& outer) noexcept
    {
        return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
    }

    // How much of the pixel (0..255) a solid triangle covers: it points left or right, is width wide and height tall,
    // and its bounding box is centered on (centerX, centerY).
    BYTE TriangleCoverage(int x, int y, float centerX, float centerY, float width, float height, bool pointsLeft) noexcept;

    // Premultiplied BGRA pixel of the pair. coverage: share of the pixel (0..255) covered by the glyph.
    // lightGlyph: glyph and hover shade are white, black otherwise.
    DWORD Pixel(BYTE coverage, bool lightGlyph, BYTE glyphAlpha, BYTE hoverAlpha) noexcept;

    class Overlays
    {
    public:
        using Click = std::function<void(HWND window, Dock::Edge edge)>;

        Overlays(HINSTANCE hinstance, Click onClick);
        ~Overlays();

        // Shows the buttons on these windows and removes them from all others. Meant to be called often.
        void Update(const std::vector<HWND>& windows) noexcept;
        // Hides the buttons without giving them up
        void Hide() noexcept;
        // Gives all buttons up. Needed when the virtual desktop changes: a window stays on the desktop it was created on.
        void Clear() noexcept;

    private:
        struct Overlay;

        static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM) noexcept;
        void Render(Overlay& overlay) noexcept;
        std::optional<RECT> FindOwnMinimizeButton(HWND window) noexcept;

        HINSTANCE m_hinstance{};
        winrt::com_ptr<IUIAutomation> m_automation; // Created when first needed
        Click m_onClick;
        std::map<HWND, std::unique_ptr<Overlay>> m_overlays; // by docked window
    };
}
