#pragma once

// Stage mode layout engine: PURE data in, plan out. No Win32, DWM, HWND, timers or I/O in this file.
//
// Exact C++ port of the StageProto layout engine (C:\Users\carlo\Tools\StageProto\layouts.cs), ported from
// StageProto commit bafe4d9 ("Mouse: live feedback while resizing a shared edge, master stack heights").
// Keep it in step with layouts.cs: tools\stage-vectors\gen.ps1 regenerates the golden vectors from the C# engine and
// FancyZonesTests\UnitTests\StageLayouts.Spec.cpp (GoldenVectors) fails when this port drifts from them.
//
// Port rules: Round = std::lround (half away from zero, as DockTree), floor = static_cast<int>(std::floor), C# List<T>
// semantics are kept (Remove removes the first match, Insert at an index, IndexOf = -1 when absent), no mutable globals
// (the C# static Layouts registry becomes factory functions). A null C# double[] (Rows) is an empty vector here.
//
// Contract: a layout never touches windows. It is given the work area, the spacing and an ordered window list and
// returns a Plan (role + rect per window, strip rect or none). It also owns the ORDER POLICY (where a new window goes,
// what a promote / demote / rotate does) as plain operations on a list of window ids.
// Order semantics (all layouts): ids[0 .. Slots(n)-1] are the docked windows (slot 0 = main / left slot / master), the
// rest are the strip (stage, pair) or hidden (master), most recently used first.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// C26451 (int arithmetic widened for vector indexing) is noise for the index math ported verbatim from layouts.cs.
#pragma warning(push)
#pragma warning(disable : 26451)

namespace StageLayouts
{
    // Iterator to element i of an id list (keeps the iterator arithmetic in one place, in the wider type).
    inline std::vector<int>::iterator Nth(std::vector<int>& o, std::size_t i) { return o.begin() + static_cast<std::ptrdiff_t>(i); }

    struct LRect
    {
        int L = 0, T = 0, R = 0, B = 0;
        LRect() = default;
        LRect(int l, int t, int r, int b) :
            L(l), T(t), R(r), B(b) {}
        int W() const { return R - L; }
        int H() const { return B - T; }
        bool Same(const LRect& o) const { return L == o.L && T == o.T && R == o.R && B == o.B; }
        bool Overlaps(const LRect& o) const { return L < o.R && o.L < R && T < o.B && o.T < B; }
        bool Inside(const LRect& o) const { return L >= o.L && T >= o.T && R <= o.R && B <= o.B; }
    };

    enum class Role
    {
        Main,
        Docked,
        StripTile,
        Hidden
    };

    // Hidden: takes no slot and no tile (e.g. a window that is not to be shown)
    enum class WinFlags : int
    {
        None = 0,
        Hidden = 1
    };
    inline WinFlags operator|(WinFlags a, WinFlags b) { return static_cast<WinFlags>(static_cast<int>(a) | static_cast<int>(b)); }
    inline WinFlags operator&(WinFlags a, WinFlags b) { return static_cast<WinFlags>(static_cast<int>(a) & static_cast<int>(b)); }

    // MinW / MinH: the smallest VISIBLE size the window accepts (0 = none known). Layouts that use them (pair, master)
    // give a window at least that much, shrink the other windows and, if the minimums do not fit, hide windows (master)
    // or let the oversized window stick out at the edge of the usable area: docked rects never overlap each other.
    struct LWin
    {
        int Id = 0;
        WinFlags Flags = WinFlags::None;
        int MinW = 0, MinH = 0;
        LWin() = default;
        LWin(int id, WinFlags f = WinFlags::None, int minW = 0, int minH = 0) :
            Id(id), Flags(f), MinW(minW), MinH(minH) {}
    };

    // Spacing exactly as Muntin's docking applies it (settings fancyzones_dockGap and fancyzones_dockMargin*): raw pixels,
    // the same for every monitor, no DPI scaling. Margins are kept free between the windows and each edge of the work
    // area; margins that leave no room are ignored (all four together). The gap is the space between two neighbouring windows.
    struct Spacing
    {
        int Gap = 8, MarginL = 0, MarginT = 0, MarginR = 0, MarginB = 0;
        Spacing() = default;
        Spacing(int gap, int l, int t, int r, int b) :
            Gap(gap), MarginL(l), MarginT(t), MarginR(r), MarginB(b) {}
        static Spacing MuntinDefault() { return Spacing(8, 0, 0, 0, 0); }
        Spacing Clamped() const
        {
            auto c = [](int v, int lo, int hi) { return (std::max)(lo, (std::min)(hi, v)); };
            return Spacing(c(Gap, 0, 100), c(MarginL, 0, 500), c(MarginT, 0, 500), c(MarginR, 0, 500), c(MarginB, 0, 500));
        }
        LRect Inset(const LRect& work) const
        {
            LRect r(work.L + MarginL, work.T + MarginT, work.R - MarginR, work.B - MarginB);
            return r.R > r.L && r.B > r.T ? r : work;
        }
    };

    struct LayoutInput
    {
        LRect Work;                                  // work area of the monitor, physical pixels
        Spacing Spacing = Spacing::MuntinDefault();
        std::vector<LWin> Wins;                      // in order: docked windows first, then the strip / hidden ones (most recently used first)
    };

    // Slot: docked slot number, or the tile index in the strip (unscrolled position); Hidden items have an empty Rect
    struct PlanItem
    {
        int Id = 0;
        Role Role = Role::Hidden;
        int Slot = 0;
        LRect Rect;
    };

    struct Plan
    {
        std::vector<PlanItem> Items;                 // same order as the input list
        bool HasStrip = false;                       // strip shown? (Strip is always the zone it would occupy)
        LRect Area;                                  // the work area less the margins (the usable area every rect lies in)
        LRect Strip;
        LRect Park;                                  // where windows that are not docked are physically parked, and the size every tile is scaled from
        int TileW = 0, TileH = 0, Gap = 0, Docked = 0; // tile size in the strip, gap in px, number of docked windows
    };

    class ILayout
    {
    public:
        virtual ~ILayout() = default;
        virtual std::string Name() const = 0;
        virtual int Slots(int n) const = 0;          // docked windows when n windows are visible
        virtual bool HasStrip(int n) const = 0;
        virtual bool UsesMinSizes() const = 0;       // the layout honours LWin.MinW / MinH
        virtual Plan Compute(const LayoutInput& inp) const = 0;
        virtual void Add(std::vector<int>& order, int id, bool focused) const = 0;  // a window appeared (stage: only a focused one takes main; pair and master: a new window always takes the left slot / master)
        virtual void Join(std::vector<int>& order, int id, bool docked) const = 0;  // a window arrived from another monitor (docked: it was docked there) or was adopted without being in front
        virtual bool Promote(std::vector<int>& order, int id) const = 0;            // strip tile clicked, or "promote to main / left / master": true if the order changed
        virtual bool PromoteHidden(std::vector<int>& order, int id) const = 0;      // the same for a window that is NOT docked right now (strip tile, hidden, cut by minimum sizes)
        virtual bool Demote(std::vector<int>& order, int id) const = 0;             // "send to strip" (master: to the bottom of the stack / hidden end); false when there is nothing to do
        virtual void Remove(std::vector<int>& order, int id) const = 0;             // a window went away
        virtual bool Rotate(std::vector<int>& order, int dir) const = 0;            // carousel: the whole list rotates one step (dir < 0: first to the end, dir > 0: last to the front)
        virtual bool HasCut() const = 0;             // the layout has one movable vertical edge between its left window and the rest (pair: left | right, master: master | stack)
        virtual double Cut() const = 0;              // where that edge sits, as a ratio of CutSpan
        virtual void SetCut(double value) = 0;       // keeps it in a sane range
        virtual LRect CutSpan(const LRect& area, const LRect& zone, int n) const = 0; // the span the ratio refers to, for n visible windows (area = usable area, zone = strip zone)
        virtual std::vector<double> Rows() const = 0;              // master: the stack's height shares by position (empty = equal); other layouts: always empty
        virtual void SetRows(const std::vector<double>& value) = 0;
        virtual void RowsInto(std::vector<double>& out) const { out = Rows(); } // Rows() without a new vector: reuses out's capacity (drag ticks)
        virtual bool MoveRow(const std::vector<int>& heights, int upper, int deltaPx) = 0; // master: the edge between stack windows upper and upper + 1 moves by deltaPx (positive = down); false = nothing changed
    };

    struct Metrics
    {
        static constexpr double StripShare = 0.15, DefaultMasterShare = 0.60;
        static constexpr int DefaultMaxStack = 4;
        // Width of a strip tile: 15% of the usable width. The strip zone is the tile column plus one gap on its left, from
        // the top to the bottom of the work area; it only depends on work area and spacing, never on the window count.
        static int Round(double x) { return static_cast<int>(std::lround(x)); } // half away from zero, as Muntin's DockTree uses
        static int TileWidth(const LRect& area) { return (std::max)(1, Round(area.W() * StripShare)); }
        static LRect StripZone(const LRect& work, Spacing sp)
        {
            sp = sp.Clamped();
            LRect a = sp.Inset(work);
            return LRect(a.R - TileWidth(a) - sp.Gap, work.T, work.R, work.B);
        }
        // Muntin's split: the cut sits at ratio of the width, the gap is shared around it (first side loses gap/2, the
        // second gap - gap/2), so the space between the two is exactly gap.
        static void Split(const LRect& r, double ratio, int gap, LRect& first, LRect& second)
        {
            int cut = r.L + Round(r.W() * ratio);
            first = LRect(r.L, r.T, cut - gap / 2, r.B);
            second = LRect(cut + (gap - gap / 2), r.T, r.R, r.B);
        }
    };

    class LayoutBase : public ILayout
    {
    public:
        bool HasCut() const override { return false; }
        double Cut() const override { return 0.5; }
        void SetCut(double) override {}
        LRect CutSpan(const LRect& area, const LRect&, int) const override { return area; }
        std::vector<double> Rows() const override { return {}; }
        void SetRows(const std::vector<double>&) override {}
        bool MoveRow(const std::vector<int>&, int, int) override { return false; }
        bool HasStrip(int n) const override { return n > Slots(n); }
        bool UsesMinSizes() const override { return false; }

        Plan Compute(const LayoutInput& inp) const override
        {
            Plan p;
            LRect w = inp.Work;
            Spacing sp = inp.Spacing.Clamped();
            LRect a = sp.Inset(w);
            std::vector<int> vis;
            std::vector<LWin> vw;
            for (int i = 0; i < static_cast<int>(inp.Wins.size()); i++)
            {
                if ((inp.Wins[i].Flags & WinFlags::Hidden) == WinFlags::None)
                {
                    vis.push_back(i);
                    vw.push_back(inp.Wins[i]);
                }
            }
            int n = static_cast<int>(vis.size()), gap = sp.Gap;
            LRect zone = Metrics::StripZone(w, sp);
            std::vector<LRect> docks = n > 0 ? Docks(a, zone, gap, vw) : std::vector<LRect>();
            int k = static_cast<int>(docks.size());
            p.Gap = gap;
            p.Area = a;
            p.Docked = k;
            p.HasStrip = HasStrip(n);
            p.Strip = zone;
            p.Park = !docks.empty() ? docks[0] : a;
            if (p.HasStrip)
            {
                p.TileW = Metrics::TileWidth(a);
                p.TileH = (std::max)(1, Metrics::Round(p.TileW * static_cast<double>(p.Park.H()) / p.Park.W()));
            }
            int v = 0;
            for (int i = 0; i < static_cast<int>(inp.Wins.size()); i++)
            {
                PlanItem it;
                it.Id = inp.Wins[i].Id;
                it.Role = Role::Hidden;
                if ((inp.Wins[i].Flags & WinFlags::Hidden) == WinFlags::None)
                {
                    if (v < k)
                    {
                        it.Role = (k == 1 || (FirstAlwaysMain() && v == 0)) ? Role::Main : Role::Docked;
                        it.Slot = v;
                        it.Rect = docks[v];
                    }
                    else if (p.HasStrip)
                    {
                        int j = v - k;
                        it.Role = Role::StripTile;
                        it.Slot = j;
                        int x = zone.L + gap, y = a.T + j * (p.TileH + gap);
                        it.Rect = LRect(x, y, x + p.TileW, y + p.TileH);
                    }
                    else
                    {
                        it.Slot = v - k; // no strip: the window is simply not shown (parked), Role::Hidden with an empty rect
                    }
                    v++;
                }
                p.Items.push_back(it);
            }
            return p;
        }

        bool PromoteHidden(std::vector<int>& o, int id) const override { return Promote(o, id); }
        bool Promote(std::vector<int>& o, int id) const override
        {
            int i = IndexOf(o, id);
            if (i <= 0)
                return false;
            int k = Slots(static_cast<int>(o.size()));
            if (i < k) // docked, not first: swap with slot 0
            {
                o[i] = o[0];
                o[0] = id;
                return true;
            }
            o.erase(Nth(o, i)); // strip tile: takes slot 0, the old slot 0 window goes to the front of the strip
            o.insert(o.begin(), id);
            if (k > 1)
            {
                int old = o[1];
                o.erase(Nth(o, 1));
                o.insert(Nth(o, k), old);
            }
            return true;
        }
        bool Demote(std::vector<int>& o, int id) const override
        {
            int i = IndexOf(o, id), k = Slots(static_cast<int>(o.size()));
            if (i < 0 || static_cast<int>(o.size()) <= k || i >= k)
                return false;
            int t = o[k];
            o[k] = id;
            o[i] = t;
            return true;
        }
        void Remove(std::vector<int>& o, int id) const override
        {
            int i = IndexOf(o, id);
            if (i < 0)
                return;
            int k = Slots(static_cast<int>(o.size()));
            if (i < k && static_cast<int>(o.size()) > k)
            {
                o[i] = o[k];
                o.erase(Nth(o, k));
            }
            else
            {
                o.erase(Nth(o, i));
            }
        }
        // Carousel over the whole ordered list O = [slot 0, slot 1 (pair), strip...]: dir < 0 gives [O1..On, O0] (the old
        // main / left goes to the end of the strip, the next window takes its place), dir > 0 the reverse [On, O0..On-1].
        // Two windows swap, one is a no-op. The same for every layout, since the order list is what the layouts interpret.
        bool Rotate(std::vector<int>& o, int dir) const override
        {
            int n = static_cast<int>(o.size());
            if (n < 2)
                return false;
            if (dir < 0)
            {
                int first = o[0];
                o.erase(o.begin());
                o.push_back(first);
            }
            else
            {
                int last = o[n - 1];
                o.erase(Nth(o, (n - 1)));
                o.insert(o.begin(), last);
            }
            return true;
        }

    protected:
        virtual bool FirstAlwaysMain() const { return false; } // slot 0 is Role::Main even when there are other docked slots (master)
        // rects of the docked windows for the visible windows w (w.size() >= 1); may be fewer than Slots (master hides what does not fit)
        virtual std::vector<LRect> Docks(const LRect& area, const LRect& zone, int gap, const std::vector<LWin>& w) const = 0;
        static int IndexOf(const std::vector<int>& o, int id)
        {
            auto it = std::find(o.begin(), o.end(), id);
            return it == o.end() ? -1 : static_cast<int>(it - o.begin());
        }
    };

    // One main window (the whole usable area when alone), plus a strip of tiles on the right once there are two or more windows.
    class StageLayout final : public LayoutBase
    {
    public:
        std::string Name() const override { return "stage"; }
        int Slots(int n) const override { return n >= 1 ? 1 : 0; }
        void Add(std::vector<int>& o, int id, bool focused) const override { o.insert(Nth(o, ((focused || o.empty()) ? 0 : 1)), id); }
        void Join(std::vector<int>& o, int id, bool docked) const override { Add(o, id, docked); }

    protected:
        // minimum sizes are not honoured here: a main window that will not shrink is covered by the strip tiles
        std::vector<LRect> Docks(const LRect& a, const LRect& zone, int, const std::vector<LWin>& w) const override
        {
            return w.size() == 1 ? std::vector<LRect>{ a } : std::vector<LRect>{ LRect(a.L, a.T, zone.L, a.B) };
        }
    };

    // Two docked windows side by side (50/50 when only two, about 42% each with a strip), strip on the right from the third window on.
    class PairLayout final : public LayoutBase
    {
    public:
        double Ratio = 0.5;
        std::string Name() const override { return "pair"; }
        bool HasCut() const override { return true; }
        double Cut() const override { return Ratio; }
        void SetCut(double value) override { Ratio = (std::max)(0.15, (std::min)(0.85, value)); }
        LRect CutSpan(const LRect& a, const LRect& zone, int n) const override { return n == 2 ? a : LRect(a.L, a.T, zone.L, a.B); }
        int Slots(int n) const override { return (std::min)(n, 2); }
        bool UsesMinSizes() const override { return true; }
        // A new window always takes the left slot. The window that was on the left takes the right slot (second window)
        // or goes to the front of the strip (third window on); the right slot stays.
        void Add(std::vector<int>& o, int id, bool) const override
        {
            int n = static_cast<int>(o.size());
            o.insert(o.begin(), id);
            if (n >= 2)
            {
                int old = o[1];
                o.erase(Nth(o, 1));
                o.insert(Nth(o, 2), old);
            }
        }
        void Join(std::vector<int>& o, int id, bool docked) const override
        {
            if (docked)
                Add(o, id, true);
            else
                o.insert(Nth(o, (o.size() < 2 ? o.size() : 2)), id);
        }

    protected:
        std::vector<LRect> Docks(const LRect& a, const LRect& zone, int gap, const std::vector<LWin>& w) const override
        {
            if (w.size() == 1)
                return { a };
            LRect d = w.size() == 2 ? a : LRect(a.L, a.T, zone.L, a.B);
            LRect l, r;
            Metrics::Split(d, Ratio, gap, l, r);
            int minL = w[0].MinW, minR = w[1].MinW;
            if (l.W() < minL || r.W() < minR) // move the cut; if both minimums do not fit, the right window sticks out at the right edge, never over the left one
            {
                int total = d.W() - gap, wl = (std::max)(minL, (std::min)(l.W(), total - minR)), wr = (std::max)(minR, total - wl);
                l = LRect(d.L, d.T, d.L + wl, d.B);
                r = LRect(l.R + gap, d.T, l.R + gap + wr, d.B);
            }
            return { LRect(l.L, l.T, l.R, l.T + (std::max)(l.H(), w[0].MinH)), LRect(r.L, r.T, r.R, r.T + (std::max)(r.H(), w[1].MinH)) };
        }
    };

    // Master-stack (dwm / i3): the master on the left (Share of the usable width), the other windows stacked on the right
    // with equal heights, up to MaxStack of them. Every window is real and visible, there is no strip and no tile. Windows
    // beyond the cap are Hidden (parked) and reached with Rotate.
    class MasterLayout final : public LayoutBase
    {
    public:
        double Share = Metrics::DefaultMasterShare;
        int MaxStack = Metrics::DefaultMaxStack;
        std::string Name() const override { return "master"; }
        bool HasCut() const override { return true; }
        double Cut() const override { return Share; }
        void SetCut(double value) override { Share = (std::max)(0.15, (std::min)(0.85, value)); }
        std::vector<double> Rows() const override { return rows; }
        void RowsInto(std::vector<double>& out) const override { out = rows; }
        void SetRows(const std::vector<double>& value) override { rows = value; }
        // The edge between stack windows upper and upper + 1 moves: the two share what they had, in the same total, each
        // keeping at least 8% of the stack. The shares start from the heights the windows really have (minimum sizes included).
        bool MoveRow(const std::vector<int>& heights, int upper, int deltaPx) override
        {
            int kk = static_cast<int>(heights.size());
            if (upper < 0 || upper + 1 >= kk || deltaPx == 0)
                return false;
            double tot = 0;
            for (int hh : heights)
                tot += (std::max)(1, hh);
            double d = deltaPx / tot, lo = 0.08;
            double ru = (std::max)(1, heights[upper]) / tot, rl = (std::max)(1, heights[upper + 1]) / tot, pair = ru + rl;
            double nu = (std::max)(lo, (std::min)(pair - lo, ru + d));
            if (std::abs(nu - ru) < 1e-9)
                return false;
            rows.resize(kk); // no new vector per call once it has its size
            for (int i = 0; i < kk; i++)
                rows[i] = (std::max)(1, heights[i]) / tot;
            rows[upper] = nu;
            rows[upper + 1] = pair - nu;
            return true;
        }
        int Slots(int n) const override { return n <= 1 ? (std::max)(0, n) : 1 + (std::min)(n - 1, (std::max)(1, MaxStack)); }
        bool HasStrip(int) const override { return false; }
        bool UsesMinSizes() const override { return true; }
        // A new window becomes master; the old master goes to the top of the stack (and the bottom of the stack to the hidden end once the cap is reached).
        void Add(std::vector<int>& o, int id, bool) const override { o.insert(o.begin(), id); }
        void Join(std::vector<int>& o, int id, bool docked) const override { o.insert(Nth(o, ((docked || o.empty()) ? 0 : 1)), id); }
        // A window that is not docked (hidden by the cap, or cut by minimum sizes): it becomes master and the old master heads the stack, whatever the cap says.
        bool PromoteHidden(std::vector<int>& o, int id) const override
        {
            int i = IndexOf(o, id);
            if (i <= 0)
                return false;
            o.erase(Nth(o, i));
            o.insert(o.begin(), id);
            return true;
        }
        // A stack window swaps places with the master; a hidden window becomes master and the old master goes to the top of the stack.
        bool Promote(std::vector<int>& o, int id) const override
        {
            int i = IndexOf(o, id);
            if (i <= 0)
                return false;
            int k = Slots(static_cast<int>(o.size()));
            if (i < k)
            {
                o[i] = o[0];
                o[0] = id;
                return true;
            }
            o.erase(Nth(o, i));
            o.insert(o.begin(), id);
            return true;
        }
        // Send to the bottom of the stack / hidden end: the end of the list. The next window becomes master when the master is sent.
        bool Demote(std::vector<int>& o, int id) const override
        {
            int i = IndexOf(o, id);
            if (i < 0 || o.size() < 2 || i == static_cast<int>(o.size()) - 1)
                return false;
            o.erase(Nth(o, i));
            o.push_back(id);
            return true;
        }
        // A window goes away: everything behind it moves up (the first hidden window enters the stack, the next window becomes master).
        void Remove(std::vector<int>& o, int id) const override
        {
            int i = IndexOf(o, id);
            if (i >= 0)
                o.erase(Nth(o, i));
        }

    protected:
        bool FirstAlwaysMain() const override { return true; }
        std::vector<LRect> Docks(const LRect& a, const LRect&, int gap, const std::vector<LWin>& w) const override
        {
            int n = static_cast<int>(w.size());
            if (n == 1)
                return { a };
            int k0 = Slots(n) - 1, mmw = w[0].MinW, kk = k0;
            for (; kk > 1; kk--) // the largest stack that fits: heights (minimums + gaps) in the area, widths (master min + gap + widest stack min) in the area
            {
                int sumH = (kk - 1) * gap, maxW = 0;
                for (int i = 1; i <= kk; i++)
                {
                    sumH += w[i].MinH;
                    maxW = (std::max)(maxW, w[i].MinW);
                }
                if (sumH <= a.H() && mmw + gap + maxW <= a.W())
                    break;
            }
            int smw = 0;
            for (int i = 1; i <= kk; i++)
                smw = (std::max)(smw, w[i].MinW);
            LRect m, s;
            Metrics::Split(a, (std::max)(0.1, (std::min)(0.9, Share)), gap, m, s);
            int masterW = (std::max)(m.W(), mmw);                                  // at least the master's minimum ...
            masterW = (std::max)(mmw, (std::min)(masterW, a.W() - gap - smw));     // ... but leave the stack its minimum width when there is room
            if (masterW != m.W())
            {
                m = LRect(a.L, a.T, a.L + masterW, a.B);
                s = LRect(m.R + gap, a.T, a.R, a.B);
            }
            if (s.W() < smw) // nothing fits: the stack sticks out at the right edge, never over the master
                s = LRect(s.L, s.T, s.L + smw, s.B);
            m = LRect(m.L, m.T, m.R, m.T + (std::max)(m.H(), w[0].MinH));
            std::vector<LRect> r(kk + 1);
            r[0] = m;
            int avail = (std::max)(kk, s.H() - (kk - 1) * gap);
            std::vector<int> fixedH(kk, 0);
            std::vector<char> isFixed(kk, 0);
            int freeN = kk, rest = avail;
            const std::vector<double>* wt = (!rows.empty() && static_cast<int>(rows.size()) == kk) ? &rows : nullptr;
            auto W = [&](int i) { return wt == nullptr ? 1.0 : (std::max)(0.01, (*wt)[i]); };
            for (bool again = true; again && freeN > 0;) // water filling: a window whose minimum is above its share keeps its minimum, the others share what is left (by their weights; equal when there are none)
            {
                again = false;
                double sw = 0;
                for (int i = 0; i < kk; i++)
                    if (!isFixed[i])
                        sw += W(i);
                for (int i = 0; i < kk; i++)
                {
                    if (!isFixed[i] && w[i + 1].MinH > 0 && w[i + 1].MinH > rest * W(i) / sw)
                    {
                        isFixed[i] = 1;
                        fixedH[i] = w[i + 1].MinH;
                        rest -= fixedH[i];
                        freeN--;
                        again = true;
                        break;
                    }
                }
            }
            int fl = (std::max)(1, (std::min)(60, avail / (std::max)(1, kk)));
            rest = (std::max)(rest, freeN * fl); // minimums that crowd the others out: a free window keeps a usable height (the stack sticks out at the bottom)
            std::vector<int> fh(kk, 0);
            {
                double sw = 0;
                for (int i = 0; i < kk; i++)
                    if (!isFixed[i])
                        sw += W(i);
                int used = 0;
                for (int i = 0; i < kk; i++)
                {
                    if (!isFixed[i])
                    {
                        fh[i] = static_cast<int>(std::floor(rest * W(i) / sw));
                        used += fh[i];
                    }
                }
                int rem2 = rest - used;
                for (int i = 0; i < kk && rem2 > 0; i++)
                {
                    if (!isFixed[i])
                    {
                        fh[i]++;
                        rem2--;
                    }
                }
            }
            int y = s.T;
            for (int i = 0; i < kk; i++)
            {
                int hi = isFixed[i] ? fixedH[i] : (std::max)(wt != nullptr ? 1 : 0, fh[i]);
                if (kk == 1)
                    hi = (std::max)(avail, w[1].MinH);
                r[i + 1] = LRect(s.L, y, s.R, y + hi);
                y += hi + gap;
            }
            return r;
        }

    private:
        std::vector<double> rows; // height share per stack position; used only while its length equals the stack size (another size = equal heights again)
    };

    // Factories (replace the C# static Layouts registry; fresh instance each call, nothing shared).
    inline std::unique_ptr<ILayout> MakeStage() { return std::make_unique<StageLayout>(); }
    inline std::unique_ptr<ILayout> MakePair() { return std::make_unique<PairLayout>(); }
    inline std::unique_ptr<ILayout> MakeMaster() { return std::make_unique<MasterLayout>(); }
    // Layout names in cycle order: stage -> pair -> master -> stage.
    inline const std::vector<std::string>& LayoutNames()
    {
        static const std::vector<std::string> names{ "stage", "pair", "master" };
        return names;
    }
    // Like Layouts.ByName: an unknown name gives the stage layout.
    inline std::unique_ptr<ILayout> MakeLayout(const std::string& name)
    {
        if (name == "pair")
            return MakePair();
        if (name == "master")
            return MakeMaster();
        return MakeStage();
    }
    // Like Layouts.Next: the name after cur in the cycle (an unknown name gives the first one).
    inline std::string NextLayoutName(const std::string& cur)
    {
        const auto& n = LayoutNames();
        for (size_t i = 0; i < n.size(); i++)
            if (n[i] == cur)
                return n[(i + 1) % n.size()];
        return n[0];
    }
}

#pragma warning(pop)
