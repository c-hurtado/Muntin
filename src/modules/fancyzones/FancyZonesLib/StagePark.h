#pragma once

// Stage mode parking and placement, pure. A window that has no place in the layout (master: the ones beyond the stack cap, or
// cut by minimum sizes) is parked like DesktopScroll parks its windows: as a one pixel sliver at the bottom edge of its monitor,
// so it stays on its monitor (its DPI does not change). A parked window is NEVER RESIZED: it keeps the size it has, so it keeps its
// last painted frame (an app that does not paint off-screen would show nothing at a new size, and so would its strip thumbnail).
// Only x moves, clamped so the window stays inside the horizontal bounds of its monitor. Where a sliver cannot be placed (it would
// reach another monitor, or the window is not on this monitor) the window waits behind the master in the master's place and
// size (the bottom of the z-order), as it always did. Either way Stage::IsHeld keeps Muntin's docking away from it (Arrange::HandsOff).

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

    // The x of a window of this width on a monitor: where it is, kept inside the monitor horizontally; a window wider than the monitor
    // starts at the monitor's left edge (and accepts spilling to the right)
    inline int ClampX(int x, int width, const LRect& monitor)
    {
        if (width >= monitor.W())
        {
            return monitor.L;
        }

        return (std::min)((std::max)(x, monitor.L), monitor.R - width);
    }

    // Whether a window is on the monitor: its horizontal middle is and it reaches into it vertically (a parked sliver has only its top row there).
    // A window on another monitor has the size of that monitor's DPI, which is no size to keep here.
    inline bool CenterOn(const LRect& r, const LRect& monitor)
    {
        const int cx = r.L + r.W() / 2;
        return r.W() > 0 && r.H() > 0 && cx >= monitor.L && cx < monitor.R && r.T < monitor.B && r.B > monitor.T;
    }

    // current: the window rect now (outer, with the invisible frame), empty when unknown. monitor: the monitor of its stage. fallback:
    // where the window waits behind the master when it cannot be a sliver (the master's place and size, on this monitor).
    // The sliver keeps the size of the window, whatever its height (SliverClear keeps it off other monitors); it is not used for a
    // window that is not on this monitor (it comes back the way it always did: master size, behind the master).
    inline Placement Park(const LRect& current, const LRect& fallback, const LRect& monitor, const std::vector<LRect>& monitors)
    {
        if (CenterOn(current, monitor))
        {
            const int w = current.W();
            const int h = current.H();
            const int x = ClampX(current.L, w, monitor);
            const int y = monitor.B - 1;
            if (SliverClear(x, y, w, h, monitor, monitors))
            {
                return { LRect(x, y, x + w, y + h), true };
            }
        }

        return { fallback, false };
    }

    // A window sitting where Park puts slivers (its top is the last pixel row of the monitor, whatever its size). Only the tests use it:
    // the stage knows its parked windows by its own flag, and adoption looks at DesktopScroll::IsParked and the marks of other apps.
    inline bool LooksParked(const LRect& windowRect, const LRect& monitor)
    {
        return windowRect.T == monitor.B - 1;
    }
}
