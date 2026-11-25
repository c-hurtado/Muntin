#pragma once

// Stage mode mouse handling, pure parts (port of StageProto's drag code): which edge of a held window the user is dragging, what
// that does to the layout's cut or stack heights, and which slot a window was dropped on. No window is touched here; the rects,
// the pointer and the layout are passed in, so tests need no windows.

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <vector>

#include <FancyZonesLib/StageLayouts.h>

namespace StageMouse
{
    using StageLayouts::ILayout;
    using StageLayouts::LRect;
    using StageLayouts::Plan;
    using StageLayouts::Role;

    // An edge only counts as dragged when the opposite edge stayed put (a move that changes the size on the way, a DPI change or
    // an un-maximize is not an edge drag).
    constexpr int EdgeTolerance = 2;

    // Was the drag started on a window border (a resize) or anywhere else (a move: title bar, Alt-drag, unknown)? From WM_NCHITTEST at the
    // pointer. Only a border drag is ever an edge drag: a title-bar drag that Aero Snap / Snap Layouts turns into a resize is not.
    enum class Intent
    {
        Move,
        Size
    };

    inline Intent IntentFromHitTest(long long hit)
    {
        constexpr int firstBorder = 10, lastBorder = 17; // HTLEFT .. HTBOTTOMRIGHT: the eight sizing borders
        return hit >= firstBorder && hit <= lastBorder ? Intent::Size : Intent::Move;
    }

    // What the layout looked like when a drag started. Every preview and the final drop are computed from it (never from the
    // previous preview), so moving the mouse back restores things exactly.
    struct DragCtx
    {
        int slot{}; // docked slot of the dragged window (0 = master / left)
        int n{}; // windows in the stage
        int cutPx{}; // where the vertical cut is, in pixels (the left window's right edge plus half a gap)
        int spanL{}, spanW{}; // the span the cut ratio refers to
        double cut0{};
        std::vector<double> rows0;
        std::vector<int> rowH; // heights of the docked windows after the first
    };

    // What a drag changes in a layout: compared tick to tick, so a tick that changes nothing moves nothing.
    struct Snapshot
    {
        double cut{};
        std::vector<double> rows;

        bool operator==(const Snapshot& o) const { return std::abs(cut - o.cut) <= 1e-12 && rows == o.rows; }
    };

    inline void Take(const ILayout& layout, Snapshot& into)
    {
        into.cut = layout.Cut();
        layout.RowsInto(into.rows); // reuses the capacity after the first call
    }

    // The window at docked slot `slot` of a stage with n windows and the given plan; nullopt when there is nothing to resize (the layout
    // has no cut, fewer than two docked windows, or the window is not docked).
    inline std::optional<DragCtx> Capture(const ILayout& layout, const Plan& plan, int slot, int n)
    {
        if (!layout.HasCut() || plan.Docked < 2 || slot < 0 || slot >= plan.Docked || slot >= static_cast<int>(plan.Items.size()) || plan.Items[slot].Role == Role::Hidden)
        {
            return std::nullopt;
        }

        const LRect span = layout.CutSpan(plan.Area, plan.Strip, n);
        if (span.W() <= 0)
        {
            return std::nullopt;
        }

        DragCtx c;
        c.slot = slot;
        c.n = n;
        c.cutPx = plan.Items[0].Rect.R + plan.Gap / 2; // Metrics::Split: the cut sits half a gap right of the left window
        c.spanL = span.L;
        c.spanW = span.W();
        c.cut0 = layout.Cut();
        c.rows0 = layout.Rows();
        for (int i = 1; i < plan.Docked; i++)
        {
            c.rowH.push_back(plan.Items[i].Rect.H());
        }

        return c;
    }

    // The stage changed during the drag (a window came or went, the dragged one moved in the order): the numbers taken at the start
    // no longer fit.
    inline bool StillValid(const DragCtx& c, int orderSize, int indexOfDragged)
    {
        return orderSize == c.n && indexOfDragged == c.slot;
    }

    inline void Restore(ILayout& layout, const DragCtx& c)
    {
        layout.SetCut(c.cut0);
        layout.SetRows(c.rows0);
    }

    // A resize (the size changed), as opposed to a move
    inline bool IsResize(const LRect& from, const LRect& to)
    {
        return from.W() != to.W() || from.H() != to.H();
    }

    // The edge pixels moved by an edge drag: how far the vertical cut moved (dx), and the stack row edges (dt: top edge, db: bottom edge),
    // each 0 when it is not an edge drag of that edge.
    struct EdgeMove
    {
        int dx{}, dt{}, db{};
        bool upperRow{}, lowerRow{}; // dt / db count as a row edge move (the window has a stack neighbour above / below)
    };

    inline EdgeMove DetectEdge(const DragCtx& c, const LRect& from, const LRect& to)
    {
        const bool lFix = std::abs(to.L - from.L) <= EdgeTolerance, rFix = std::abs(to.R - from.R) <= EdgeTolerance;
        const bool tFix = std::abs(to.T - from.T) <= EdgeTolerance, bFix = std::abs(to.B - from.B) <= EdgeTolerance;
        EdgeMove e;
        e.dx = c.slot == 0 ? (lFix ? to.R - from.R : 0) : (rFix ? to.L - from.L : 0);
        if (c.slot >= 1)
        {
            const int si = c.slot - 1;
            const int dt = to.T - from.T, db = to.B - from.B;
            if (dt != 0 && si >= 1 && bFix)
            {
                e.dt = dt;
                e.upperRow = true;
            }
            else if (db != 0 && si + 1 < static_cast<int>(c.rowH.size()) && tFix)
            {
                e.db = db;
                e.lowerRow = true;
            }
        }

        return e;
    }

    // Applies the drag of the window from rect `from` (start of the drag) to `to` (now) to the layout, always from the layout as it was
    // at the start (so it can be called again and again while the mouse moves). true = the layout changed. The layout clamps (cut
    // 15..85%, every stack window at least 8% of the stack).
    inline bool ResizeEdge(ILayout& layout, const DragCtx& c, const LRect& from, const LRect& to)
    {
        Restore(layout, c);
        const EdgeMove e = DetectEdge(c, from, to);
        bool changed = false;
        if (e.dx != 0)
        {
            layout.SetCut((c.cutPx + e.dx - c.spanL) / static_cast<double>(c.spanW));
            changed = std::abs(layout.Cut() - c.cut0) > 1e-6;
        }

        const int si = c.slot - 1;
        if (e.upperRow)
        {
            changed |= layout.MoveRow(c.rowH, si - 1, e.dt);
        }
        else if (e.lowerRow)
        {
            changed |= layout.MoveRow(c.rowH, si, e.db);
        }

        return changed;
    }

    // The window (by id) whose slot holds the point, other than the dragged one; 0 = none. Hidden items have no slot. A point in a gap
    // between slots is no slot. order is the stage's order list (ids), the same order as plan.Items.
    inline int HitTest(const Plan& plan, const std::vector<int>& order, int draggedId, int x, int y)
    {
        const size_t count = (std::min)(plan.Items.size(), order.size());
        for (size_t i = 0; i < count; i++)
        {
            const auto& item = plan.Items[i];
            if (order[i] == draggedId || item.Role == Role::Hidden)
            {
                continue;
            }

            if (x >= item.Rect.L && x < item.Rect.R && y >= item.Rect.T && y < item.Rect.B)
            {
                return order[i];
            }
        }

        return 0;
    }

    // The two windows swap places in the order list. false when one is not in it or they are the same.
    inline bool Swap(std::vector<int>& order, int a, int b)
    {
        const auto ia = std::find(order.begin(), order.end(), a);
        const auto ib = std::find(order.begin(), order.end(), b);
        if (a == b || ia == order.end() || ib == order.end())
        {
            return false;
        }

        std::iter_swap(ia, ib);
        return true;
    }

    // What a drag that has ended turns into
    enum class Drop
    {
        SnapBack, // back into the slot: nothing moved (Esc), a snap / outer edge, or dropped where nothing is
        ResizeEdge, // a shared edge was dragged: the layout changed
        Swap, // dropped on another slot
        Rehome, // most of it is on another monitor: the rehome takes over
        Unmaximize // dropped maximized (Aero Snap on the top edge): restored first, then placed
    };

    struct DropFacts
    {
        bool rectSameAsStart{}; // Esc or a click
        bool onOtherMonitor{};
        bool zoomed{};
        Intent intent{ Intent::Move };
        bool resized{}; // the size changed
        bool edgeChanged{}; // ResizeEdge() said so (only asked for a Size drag that resized)
        int targetId{}; // HitTest result (only asked for a drag that did not resize)
    };

    struct DropPlan
    {
        Drop action{ Drop::SnapBack };
        bool skipDragged{}; // the stage lands the other windows around the dragged one and leaves it where the user put it (rehome follows)
    };

    inline DropPlan PlanDrop(const DropFacts& f)
    {
        if (f.rectSameAsStart)
        {
            return { Drop::SnapBack, false };
        }

        if (f.onOtherMonitor)
        {
            return { Drop::Rehome, true }; // landing the dragged window here would pull it back from the other monitor
        }

        if (f.zoomed)
        {
            return { Drop::Unmaximize, false };
        }

        if (f.resized)
        {
            return { f.intent == Intent::Size && f.edgeChanged ? Drop::ResizeEdge : Drop::SnapBack, false }; // a snap drop of a title-bar drag, or an outer edge
        }

        return { f.targetId != 0 ? Drop::Swap : Drop::SnapBack, false };
    }
}
