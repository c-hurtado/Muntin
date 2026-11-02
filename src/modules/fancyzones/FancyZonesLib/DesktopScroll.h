#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

// Shift+Win+mouse wheel scrolls the windows of a monitor up and down a tall canvas, and they stay wherever
// the scroll stops, cut off by the edge of the monitor if need be. While it scrolls, live DWM thumbnails
// slide over the desktop and the real windows wait out of sight. A window that is fully off the monitor
// waits as a one pixel sliver under the taskbar: that keeps it on its monitor, so its DPI doesn't change.
// Every stretch of the canvas as tall as the monitor is a page with its own dock layout, in force while
// the monitor shows exactly that page.
class DesktopScroll
{
public:
    // Runs on the scroll thread. monitor: the monitor that now shows mostly that page. Null: everything is back on page 0.
    using PageChanged = std::function<void(HMONITOR monitor, int page)>;

    DesktopScroll(HINSTANCE instance, PageChanged pageChanged);
    // Brings every window back to its place
    ~DesktopScroll();

    // Brings every window back to its place on the first page, and every monitor to the top of its canvas
    void ReturnAll() noexcept;

    // The scroll holds the window: nothing else may move it
    static bool IsParked(HWND window) noexcept;

    // How far a turn of the wheel scrolls. Positive: down the canvas.
    static constexpr double WheelPixels(int delta, LONG pageHeight) noexcept
    {
        constexpr double pagesPerNotch = 1.0 / 6;

        return -delta / static_cast<double>(WHEEL_DELTA) * pagesPerNotch * pageHeight;
    }

    // Where a scroll comes to rest: where it stopped, or on a page when that is very close
    static double RestingOffset(double offset, double pageHeight) noexcept
    {
        constexpr double closeEnough = 0.03;

        const double page = std::round(offset / pageHeight) * pageHeight;
        return std::abs(offset - page) < closeEnough * pageHeight ? page : offset;
    }

    // The page a window belongs to: the one that holds its middle
    static LONG PageOf(LONG canvasTop, LONG windowHeight, LONG pageHeight) noexcept
    {
        return static_cast<LONG>(std::floor((static_cast<double>(canvasTop) + windowHeight / 2.0) / pageHeight));
    }

    // How far a monitor scrolls: to the first page past its lowest window, which is empty
    static double ScrollLimit(LONG lowestBottom, LONG pageHeight) noexcept
    {
        return (std::max)(1.0, std::ceil(static_cast<double>(lowestBottom) / pageHeight)) * pageHeight;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
