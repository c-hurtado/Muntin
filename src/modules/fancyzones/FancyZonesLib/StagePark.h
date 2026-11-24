#pragma once

// Stage mode parking and placement, pure. A window that has no place in the layout (master: the ones beyond the stack cap, or
// cut by minimum sizes) is parked like DesktopScroll parks its windows: as a one pixel sliver at the bottom edge of its monitor,
// same x, same size, so it stays on its monitor (its DPI does not change). Where such a sliver would reach another monitor, the
// window waits behind the master instead. Either way Stage::IsHeld keeps Muntin's docking away from it (Arrange::HandsOff).

#include <vector>

#include <FancyZonesLib/StageLayouts.h>

namespace StagePark
{
    using StageLayouts::LRect;

    // Invisible frame around a window (DWM extended frame bounds against GetWindowRect)
    struct Insets
    {
        int L = 0, T = 0, R = 0, B = 0;
    };

    inline bool Overlaps(int x, int y, int w, int h, const LRect& m)
    {
        return x < m.R && x + w > m.L && y < m.B && y + h > m.T;
    }

    // A sliver parked at the bottom edge of the monitor extends below it: it must not reach any other monitor
    inline bool SliverClear(int x, int y, int w, int h, const LRect& monitor, const std::vector<LRect>& monitors)
    {
        for (const auto& m : monitors)
        {
            if (m.Same(monitor))
            {
                continue;
            }

            if (Overlaps(x, y, w, h, m))
            {
                return false;
            }
        }

        return true;
    }

    // The window rect (outer, with the invisible frame) that puts the VISIBLE bounds at vis. The invisible resize border must not
    // spill onto an adjacent monitor: Windows moves and resizes a window that does. On an edge shared with another monitor the outer rect stops at
    // the monitor edge and the visible frame ends up that many px short there.
    inline LRect OuterRect(const LRect& vis, const Insets& ins, const LRect& monitor, const std::vector<LRect>& monitors)
    {
        int l = vis.L - ins.L, t = vis.T - ins.T, r = vis.R + ins.R, b = vis.B + ins.B;
        for (const auto& o : monitors)
        {
            if (o.Same(monitor))
            {
                continue;
            }

            const bool vo = o.T < monitor.B && o.B > monitor.T;
            const bool ho = o.L < monitor.R && o.R > monitor.L;
            if (vo && o.L == monitor.R)
            {
                r = (std::min)(r, monitor.R);
            }

            if (vo && o.R == monitor.L)
            {
                l = (std::max)(l, monitor.L);
            }

            if (ho && o.T == monitor.B)
            {
                b = (std::min)(b, monitor.B);
            }

            if (ho && o.B == monitor.T)
            {
                t = (std::max)(t, monitor.T);
            }
        }

        return LRect(l, t, r, b);
    }

    struct Placement
    {
        LRect outer; // for SetWindowPos
        bool sliver{}; // at the bottom edge of the monitor (else behind the master: send to the bottom of the z-order)
    };

    // vis: where the window would sit if it were shown (the master rect: parked windows are as large as the master, so the app
    // does not have to relayout twice). monitor: its monitor rect.
    inline Placement Park(const LRect& vis, const Insets& ins, const LRect& monitor, const std::vector<LRect>& monitors)
    {
        const int w = vis.W() + ins.L + ins.R;
        const int h = vis.H() + ins.T + ins.B;
        const int x = vis.L - ins.L;
        const int y = monitor.B - 1;
        if (SliverClear(x, y, w, h, monitor, monitors))
        {
            return { LRect(x, y, x + w, y + h), true };
        }

        return { OuterRect(vis, ins, monitor, monitors), false };
    }

    // A window sitting where Park puts slivers: used to tell a parked window from one the user moved
    inline bool LooksParked(const LRect& windowRect, const LRect& monitor)
    {
        return windowRect.T == monitor.B - 1;
    }
}
