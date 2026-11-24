#include "pch.h"
#include "FancyZonesLib\StageLayouts.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace StageLayouts;

#pragma warning(disable : 26451) // int index arithmetic ported verbatim from layouts.cs

// Faithful port of StageProto layouts.cs LayoutTests (StageProto commit bafe4d9), one TEST_METHOD per group, plus a
// GoldenVectors test against tools\stage-vectors\StageLayouts.vectors.txt (generated from the C# engine by gen.ps1).
// The case counts below are what the C# LayoutTests.Run reports for the same groups (gen.ps1 prints them).

namespace
{
    constexpr int CsGeometryCases = 28370;
    constexpr int CsScenarioCases = 85;
    constexpr int CsMinSizeCases = 5777;
    constexpr int CsOrderCases = 227;

    // ---- check harness (C#: fails / log / cases) ----
    int fails = 0;
    int cases = 0;
    std::vector<std::string> failLog;

    void Check(const std::string& what, bool ok)
    {
        cases++;
        if (!ok)
        {
            fails++;
            if (failLog.size() < 40)
                failLog.push_back("  FAIL " + what);
        }
    }

    std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

    void Begin()
    {
        fails = 0;
        cases = 0;
        failLog.clear();
    }

    void Finish(const char* group, int expectedCases)
    {
        std::string msg = std::string(group) + ": " + std::to_string(fails) + " failed of " + std::to_string(cases) + " checks\n";
        for (const auto& l : failLog)
            msg += l + "\n";
        Assert::AreEqual(0, fails, Widen(msg).c_str());
        Assert::AreEqual(expectedCases, cases, Widen(std::string(group) + ": case count differs from the C# LayoutTests").c_str());
    }

    std::string Str(const LRect& r) { return "(" + std::to_string(r.L) + "," + std::to_string(r.T) + ") " + std::to_string(r.W()) + "x" + std::to_string(r.H()); }
    std::string Str(const Spacing& s)
    {
        return "gap " + std::to_string(s.Gap) + ", margins " + std::to_string(s.MarginL) + "/" + std::to_string(s.MarginT) + "/" + std::to_string(s.MarginR) + "/" + std::to_string(s.MarginB);
    }

    std::string S(const std::vector<int>& o)
    {
        std::string s;
        for (size_t i = 0; i < o.size(); i++)
            s += (i ? "," : "") + std::to_string(o[i]);
        return s;
    }
    bool Eq(const std::vector<int>& o, std::initializer_list<int> want) { return S(o) == S(std::vector<int>(want)); }
    std::vector<int> Ids(std::initializer_list<int> ids = {}) { return std::vector<int>(ids); }

    LayoutInput In(const LRect& w, const Spacing& sp, int n, int hiddenIdx = -1)
    {
        LayoutInput i;
        i.Work = w;
        i.Spacing = sp;
        for (int k = 0; k < n; k++)
            i.Wins.push_back(LWin(k + 1, k == hiddenIdx ? WinFlags::Hidden : WinFlags::None));
        return i;
    }

    using MS = std::pair<int, int>; // MinW, MinH
    LayoutInput InM(const LRect& w, const Spacing& sp, std::initializer_list<MS> ws)
    {
        LayoutInput i;
        i.Work = w;
        i.Spacing = sp;
        int k = 0;
        for (const auto& m : ws)
        {
            i.Wins.push_back(LWin(k + 1, WinFlags::None, m.first, m.second));
            k++;
        }
        return i;
    }

    std::vector<LRect> Docked(const Plan& p)
    {
        std::vector<LRect> l;
        for (const auto& it : p.Items)
            if (it.Role == Role::Main || it.Role == Role::Docked)
                l.push_back(it.Rect);
        return l;
    }
    void NoOverlap(const std::string& tag, const Plan& p)
    {
        auto d = Docked(p);
        for (size_t a = 0; a < d.size(); a++)
            for (size_t b = a + 1; b < d.size(); b++)
                Check(tag + "docked rects never overlap", !d[a].Overlaps(d[b]));
    }

    const Spacing S0(0, 0, 0, 0, 0), S8 = Spacing::MuntinDefault(), S10(10, 10, 10, 10, 10);

    std::vector<LRect> Works()
    {
        return { LRect(0, 24, 2560, 1392), LRect(2560, 0, 5120, 1392), LRect(-1920, 31, -1, 1048), LRect(0, 0, 1919, 1077), LRect(100, 50, 1437, 903) }; // odd sizes and negative origins
    }
    std::vector<Spacing> Spacings()
    {
        return { S0, S8, S10, Spacing(25, 5, 10, 15, 20), Spacing(7, 0, 30, 0, 0), Spacing(100, 500, 500, 500, 500), Spacing(8, 3000, 3000, 3000, 3000), Spacing(-5, -5, -5, -5, -5) }; // the last two: no room for the margins (ignored) and out-of-range values (clamped)
    }

    std::vector<std::unique_ptr<ILayout>> FreshAll()
    {
        std::vector<std::unique_ptr<ILayout>> all;
        all.push_back(MakeStage());
        all.push_back(MakePair());
        all.push_back(MakeMaster());
        return all;
    }

    // ---- LayoutTests.Geometry ----
    void Geometry(const ILayout& lay, const LRect& w, const Spacing& spRaw, int n)
    {
        Spacing sp = spRaw.Clamped();
        std::string name = lay.Name();
        std::string tag = name + " n=" + std::to_string(n) + " " + Str(spRaw) + " work " + Str(w) + ": ";
        // expected usable area, computed here independently of Spacing.Inset
        bool room = w.R - sp.MarginR > w.L + sp.MarginL && w.B - sp.MarginB > w.T + sp.MarginT;
        LRect area = room ? LRect(w.L + sp.MarginL, w.T + sp.MarginT, w.R - sp.MarginR, w.B - sp.MarginB) : w;
        Plan p = lay.Compute(In(w, spRaw, n));
        int k = lay.Slots(n);
        int gap = sp.Gap;
        int dk = name == "stage" ? (std::min)(n, 1) : name == "pair" ? (std::min)(n, 2) : n <= 1 ? n : 1 + (std::min)(n - 1, Metrics::DefaultMaxStack);
        bool strip = name == "stage" ? n >= 2 : name == "pair" ? n >= 3 : false;
        bool tiny = name == "master" && n >= 2 && area.H() - (dk - 2) * gap < (dk - 1) * 4; // more windows than pixels: the stack cannot keep its gaps, not a layout case
        Check(tag + "item count", static_cast<int>(p.Items.size()) == n);
        Check(tag + "usable area = work less margins (ignored when they leave no room)", p.Area.Same(area) && area.Inside(w));
        Check(tag + "strip presence", p.HasStrip == strip);
        if (!tiny)
            Check(tag + "docked count", p.Docked == dk && k == dk);
        Check(tag + "strip zone constant for the work area and spacing", p.Strip.Same(Metrics::StripZone(w, spRaw)) && p.Strip.R == w.R && p.Strip.T == w.T && p.Strip.B == w.B);
        std::vector<LRect> docked, tiles;
        int hidden = 0;
        for (const auto& it : p.Items)
        {
            if (it.Role == Role::Main || it.Role == Role::Docked)
            {
                docked.push_back(it.Rect);
                if (!tiny)
                    Check(tag + "docked inside the usable area", it.Rect.Inside(area) && it.Rect.W() > 0 && it.Rect.H() > 0);
                Check(tag + "docked not in strip zone", !p.HasStrip || it.Rect.R <= p.Strip.L);
            }
            else if (it.Role == Role::StripTile)
            {
                tiles.push_back(it.Rect);
                Check(tag + "tile in the strip column", it.Rect.L == p.Strip.L + gap && it.Rect.R == area.R && it.Rect.W() == p.TileW && it.Rect.H() == p.TileH);
            }
            else
            {
                hidden++;
                Check(tag + "hidden has no rect", it.Rect.W() == 0 && it.Rect.H() == 0);
            }
        }
        if (!tiny)
            Check(tag + "docked / tile / hidden counts", static_cast<int>(docked.size()) == dk && static_cast<int>(tiles.size()) == (strip ? n - dk : 0) && hidden == (strip ? 0 : n - dk));
        for (size_t a = 0; a < docked.size(); a++)
            for (size_t b = a + 1; b < docked.size(); b++)
                Check(tag + "docked slots do not overlap", !docked[a].Overlaps(docked[b]));
        for (const auto& t : tiles)
            for (const auto& d : docked)
                Check(tag + "tiles and docked windows do not overlap", !t.Overlaps(d));
        for (size_t a = 0; a + 1 < tiles.size(); a++)
            Check(tag + "tiles do not overlap, exactly gap apart", !tiles[a].Overlaps(tiles[a + 1]) && tiles[a + 1].T - tiles[a].B == gap);
        if (!tiles.empty())
            Check(tag + "first tile at the top margin", tiles[0].T == area.T);
        if (n == 1)
            Check(tag + "single window == usable area", docked[0].Same(area));
        if (n >= 1)
            Check(tag + "main / master / left slot starts at the left margin and the top margin", docked[0].L == area.L && docked[0].T == area.T);
        if (name == "stage" && n >= 2)
            Check(tag + "main and tiles exactly gap apart, main full height", tiles[0].L - docked[0].R == gap && docked[0].T == area.T && docked[0].B == area.B);
        if (name == "pair" && n >= 2)
            Check(tag + "left/right exactly gap apart, same top and bottom", docked[1].L - docked[0].R == gap && docked[0].T == area.T && docked[0].B == area.B && docked[1].T == docked[0].T && docked[1].B == docked[0].B);
        if (name == "pair" && n == 2)
            Check(tag + "pair of two spans the usable area, halves differ by at most gap+1", docked[1].R == area.R && std::abs(docked[0].W() - docked[1].W()) <= 1 + gap % 2);
        if (name == "pair" && n >= 3)
            Check(tag + "right slot ends at the strip zone", docked[1].R == p.Strip.L);
        if (name == "master" && n >= 2 && !tiny)
        {
            Check(tag + "master and stack exactly gap apart, master full height", docked[1].L - docked[0].R == gap && docked[0].T == area.T && docked[0].B == area.B);
            Check(tag + "stack reaches the right margin", docked[1].R == area.R && docked.back().B == area.B && docked[1].T == area.T);
            for (size_t a = 1; a + 1 < docked.size(); a++)
                Check(tag + "stack windows exactly gap apart, same width", docked[a + 1].T - docked[a].B == gap && docked[a].L == docked[1].L && docked[a].R == docked[1].R);
            int hmin = INT_MAX, hmax = 0;
            for (size_t a = 1; a < docked.size(); a++)
            {
                hmin = (std::min)(hmin, docked[a].H());
                hmax = (std::max)(hmax, docked[a].H());
            }
            Check(tag + "stack heights equal within 1 px", hmax - hmin <= 1);
            Check(tag + "master takes about the share of the usable width", std::abs((docked[0].W() + gap / 2.0) / area.W() - Metrics::DefaultMasterShare) < 0.01);
        }
        if (p.HasStrip)
            Check(tag + "tile aspect follows park", std::abs(p.TileH - p.TileW * static_cast<double>(p.Park.H()) / p.Park.W()) <= 0.5);
    }

    // ---- LayoutTests.Scenarios ----
    void Scenarios()
    {
        LRect w(0, 24, 2560, 1392), odd(0, 0, 1919, 1077); // 1919 wide: the 50/50 split cannot be exact
        PairLayout pair;
        StageLayout stage;
        MasterLayout master;
        auto p1 = pair.Compute(In(w, S0, 1));
        Check("pair n=1 fills the work area (no margins), no strip", p1.Items[0].Rect.Same(w) && !p1.HasStrip && p1.Items[0].Role == Role::Main);
        auto s1 = stage.Compute(In(w, S10, 1));
        Check("stage n=1 fills the work area minus 10 px margins on every side, no strip", s1.Items[0].Rect.Same(LRect(10, 34, 2550, 1382)) && !s1.HasStrip);
        auto m1 = master.Compute(In(w, Spacing(8, 10, 20, 30, 40), 1));
        Check("master n=1 fills the work area minus the four different margins", m1.Items[0].Rect.Same(LRect(10, 44, 2530, 1352)));
        auto p2 = pair.Compute(In(w, S0, 2));
        Check("pair n=2, gap 0, no margins: 50/50 of the whole work area", p2.Items[0].Rect.Same(LRect(0, 24, 1280, 1392)) && p2.Items[1].Rect.Same(LRect(1280, 24, 2560, 1392)) && !p2.HasStrip && p2.Items[1].Role == Role::Docked && p2.Items[1].Slot == 1);
        auto p2g = pair.Compute(In(w, S10, 2));
        Check("pair n=2, gap 10, margins 10: halves exactly 10 px apart inside the margins", p2g.Items[0].Rect.Same(LRect(10, 34, 1275, 1382)) && p2g.Items[1].Rect.Same(LRect(1285, 34, 2550, 1382)));
        auto p2o = pair.Compute(In(odd, S0, 2));
        Check("pair n=2 at an odd width: left+right tile it exactly, halves differ by at most 1 px", p2o.Items[0].Rect.R == p2o.Items[1].Rect.L && p2o.Items[0].Rect.L == 0 && p2o.Items[1].Rect.R == 1919 && std::abs(p2o.Items[0].Rect.W() - p2o.Items[1].Rect.W()) <= 1);
        auto p2odd = pair.Compute(In(w, Spacing(7, 0, 0, 0, 0), 2));
        Check("odd gap 7 is split like Muntin (gap/2 and gap - gap/2): exactly 7 px between", p2odd.Items[1].Rect.L - p2odd.Items[0].Rect.R == 7 && p2odd.Items[0].Rect.R == 1280 - 3 && p2odd.Items[1].Rect.L == 1280 + 4);
        auto p3 = pair.Compute(In(w, S8, 3));
        Check("pair n=3: two docked slots near 42% each plus a 15% strip column holding the rest", p3.HasStrip && std::abs(p3.Items[0].Rect.W() / 2560.0 - 0.42) < 0.01 && std::abs(p3.Items[1].Rect.W() / 2560.0 - 0.42) < 0.01 && std::abs(p3.TileW / 2560.0 - 0.15) < 0.001 && p3.Items[2].Role == Role::StripTile);
        Check("pair n=3: right slot ends where the strip zone starts, left starts at the margin, slots gap apart", p3.Items[1].Rect.R == p3.Strip.L && p3.Items[0].Rect.L == 0 && p3.Items[1].Rect.L - p3.Items[0].Rect.R == 8 && p3.Items[2].Rect.L - p3.Items[1].Rect.R == 8);
        auto st3 = stage.Compute(In(w, S10, 3));
        Check("stage n=3, gap 10 margins 10: main inside the margins up to the strip zone, tiles 10 px right of main", st3.HasStrip && st3.Items[0].Rect.L == 10 && st3.Items[0].Rect.T == 34 && st3.Items[0].Rect.B == 1382 && st3.Items[1].Role == Role::StripTile && st3.Items[1].Rect.L - st3.Items[0].Rect.R == 10 && st3.Items[1].Rect.R == 2550 && st3.Items[1].Rect.T == 34);
        LRect smallArea(0, 0, 900, 900);
        auto big = stage.Compute(In(smallArea, Spacing(8, 500, 500, 500, 500), 1));
        Check("margins that leave no room (500 + 500 on 900 px) are ignored, all four together like Muntin", big.Items[0].Rect.Same(smallArea) && big.Area.Same(smallArea));
        auto one = stage.Compute(In(smallArea, Spacing(8, 500, 0, 0, 0), 1));
        Check("margins that do leave room are applied", one.Items[0].Rect.Same(LRect(500, 0, 900, 900)));
        auto clamp = stage.Compute(In(w, Spacing(1000, 9999, 0, 0, 0), 2));
        Check("gap and margins are clamped to Muntin's ranges (gap 100, margin 500)", clamp.Gap == 100 && clamp.Area.L == 500);
        // cuts round half away from zero like Muntin's std::lround (banker's rounding would give 958 here): usable width 1917, cut = lround(958.5) = 959, first side loses gap/2 = 4, second starts 959 + 4
        LRect w1917(0, 0, 1917, 1000);
        auto r2 = pair.Compute(In(w1917, S8, 2));
        Check("pair n=2 on a 1917 px wide area, gap 8: first.R = 955, second.L = 963 (cut 959)", r2.Items[0].Rect.R == 955 && r2.Items[1].Rect.L == 963);
        MasterLayout mh;
        mh.Share = 0.5;
        auto rm = mh.Compute(In(w1917, S8, 2));
        Check("master at 50% on 1917 px, gap 8: master.R = 955, stack.L = 963", rm.Items[0].Rect.R == 955 && rm.Items[1].Rect.L == 963);
        for (int ww = 1500; ww <= 1530; ww++) // reference: cut = floor(W * ratio + 0.5), the lround of a positive number
        {
            LRect wk(0, 0, ww, 900);
            auto rp3 = pair.Compute(In(wk, S8, 3));
            int dW = rp3.Strip.L, cutP = static_cast<int>(std::floor(dW * 0.5 + 0.5));
            Check("pair n=3 cut at width " + std::to_string(ww) + " rounds half away from zero", rp3.Items[0].Rect.R == cutP - 4 && rp3.Items[1].Rect.L == cutP + 4);
            auto rm6 = MasterLayout().Compute(In(wk, S8, 2));
            int cutM = static_cast<int>(std::floor(ww * 0.6 + 0.5));
            Check("master cut at width " + std::to_string(ww) + " rounds half away from zero", rm6.Items[0].Rect.R == cutM - 4 && rm6.Items[1].Rect.L == cutM + 4);
        }
        auto hd = pair.Compute(In(w, S8, 4, 1));
        Check("hidden flag: no slot, no tile, others laid out as if it were absent", hd.Items[1].Role == Role::Hidden && hd.Items[1].Rect.W() == 0 && hd.Items[0].Role == Role::Docked && hd.Items[2].Role == Role::Docked && hd.Items[2].Slot == 1 && hd.Items[3].Role == Role::StripTile && hd.Items[3].Slot == 0);
        auto h2 = pair.Compute(In(w, S0, 3, 2));
        Check("hidden flag: 3 windows with one hidden is a 50/50 pair", !h2.HasStrip && h2.Items[1].Rect.Same(LRect(1280, 24, 2560, 1392)));
        auto e0 = stage.Compute(In(w, S8, 0));
        Check("no windows: empty plan, no strip", e0.Items.empty() && !e0.HasStrip);
        // master
        auto m2 = master.Compute(In(w, S0, 2));
        Check("master n=2, no spacing: master 60% on the left, the other window fills the right", m2.Items[0].Rect.Same(LRect(0, 24, 1536, 1392)) && m2.Items[1].Rect.Same(LRect(1536, 24, 2560, 1392)) && !m2.HasStrip && m2.Items[0].Role == Role::Main && m2.Items[1].Role == Role::Docked);
        auto m5 = master.Compute(In(w, S10, 5));
        Check("master n=5: master + 4 stacked windows of equal height with gap 10, margins 10, no strip, nothing hidden", m5.Docked == 5 && !m5.HasStrip && m5.Items[4].Rect.B == 1382 && m5.Items[1].Rect.T == 34 && m5.Items[2].Rect.T - m5.Items[1].Rect.B == 10 && m5.Items[1].Rect.L - m5.Items[0].Rect.R == 10);
        auto m6 = master.Compute(In(w, S10, 6));
        Check("master n=6: cap of 4 in the stack, the 6th window is Hidden (no rect, no tile)", m6.Docked == 5 && m6.Items[5].Role == Role::Hidden && m6.Items[5].Rect.W() == 0 && !m6.HasStrip && m6.TileW == 0);
        MasterLayout mc;
        mc.MaxStack = 2;
        mc.Share = 0.5;
        auto mcp = mc.Compute(In(w, S0, 5));
        Check("master with MaxStack 2 and share 50%: 3 docked, 2 hidden, master is half", mcp.Docked == 3 && mcp.Items[3].Role == Role::Hidden && mcp.Items[4].Role == Role::Hidden && mcp.Items[0].Rect.W() == 1280);
        Check("layout registry cycles stage -> pair -> master -> stage", NextLayoutName("stage") == "pair" && NextLayoutName("pair") == "master" && NextLayoutName("master") == "stage" && MakeLayout("master")->Name() == "master" && MakeLayout("nope")->Name() == "stage");
    }

    // ---- LayoutTests.MinSizes ----
    // Pseudo-random generator shared by the property test and the vectors (LCG, uint32_t wraparound).
    uint32_t lcgSeed = 0;
    int Rnd(int m)
    {
        lcgSeed = lcgSeed * 1664525u + 1013904223u;
        return static_cast<int>((lcgSeed >> 8) % static_cast<uint32_t>(m));
    }

    void MinSizes()
    {
        LRect w(0, 24, 2560, 1392);
        Spacing sp = S8;
        MasterLayout master;
        PairLayout pair;
        MS z{ 0, 0 };
        // heights: 4 stack windows of 500 px minimum cannot fit in 1368 px (2 fit: 2*500 + 8), so the stack is cut to 2 and the other two are hidden
        auto pm = master.Compute(InM(w, sp, { z, { 0, 500 }, { 0, 500 }, { 0, 500 }, { 0, 500 }, { 0, 500 } }));
        Check("master: 5 stack windows with 500 px minimum height: only 2 are shown, 3 hidden, master + 2 docked", pm.Docked == 3 && pm.Items[3].Role == Role::Hidden && pm.Items[4].Role == Role::Hidden && pm.Items[5].Role == Role::Hidden);
        Check("master: the shown stack windows keep at least 500 px and fill the stack height", pm.Items[1].Rect.H() >= 500 && pm.Items[2].Rect.H() >= 500 && pm.Items[2].Rect.B == pm.Area.B && pm.Items[2].Rect.T - pm.Items[1].Rect.B == 8);
        NoOverlap("master 500s: ", pm);
        // one window needs more than its equal share: it keeps its minimum, the others share the rest
        auto pw = master.Compute(InM(w, sp, { z, { 0, 100 }, { 0, 700 }, { 0, 100 } }));
        int tot = pw.Items[1].Rect.H() + pw.Items[2].Rect.H() + pw.Items[3].Rect.H() + 16;
        Check("master: a 700 px minimum in the middle keeps 700, the other two share the rest equally (within 1 px), total fills the area", pw.Items[2].Rect.H() == 700 && std::abs(pw.Items[1].Rect.H() - pw.Items[3].Rect.H()) <= 1 && tot == pw.Area.H() && pw.Docked == 4);
        NoOverlap("master 700: ", pw);
        auto pe = master.Compute(InM(w, sp, { z, z, z, z }));
        Check("master: no minimums behaves as before (equal heights)", std::abs(pe.Items[1].Rect.H() - pe.Items[3].Rect.H()) <= 1);
        // a stack window taller than the whole area: shown alone, sticks out at the bottom, never over the master
        auto pt = master.Compute(InM(w, sp, { z, { 0, 2000 }, z }));
        Check("master: a stack window that needs 2000 px is shown alone with its height, the next is hidden, no overlap", pt.Docked == 2 && pt.Items[1].Rect.H() == 2000 && pt.Items[2].Role == Role::Hidden);
        NoOverlap("master 2000: ", pt);
        // widths: a stack window that needs 1800 px pushes the master narrower than its 60% share
        auto pwd = master.Compute(InM(w, sp, { z, { 1800, 0 } }));
        Check("master: stack minimum width 1800 shrinks the master below the share, stack keeps >= 1800", pwd.Items[1].Rect.W() >= 1800 && pwd.Items[0].Rect.W() < 1536 && pwd.Items[1].Rect.L - pwd.Items[0].Rect.R == 8 && pwd.Items[1].Rect.R <= pwd.Area.R);
        NoOverlap("master 1800 wide: ", pwd);
        // master minimum above the share
        auto pmw = master.Compute(InM(w, sp, { { 1900, 0 }, { 400, 0 } }));
        Check("master: master minimum width 1900 (share is 1536) is respected, the stack gets the rest", pmw.Items[0].Rect.W() >= 1900 && pmw.Items[1].Rect.W() >= 400 && pmw.Items[1].Rect.R <= pmw.Area.R);
        NoOverlap("master min 1900: ", pmw);
        // minimums that cannot all fit: the wide one is hidden (stack cut to the prefix that fits), master min wider than any stack could allow: stack sticks out, no overlap
        auto pcut = master.Compute(InM(w, sp, { { 500, 0 }, { 300, 0 }, { 2400, 0 } }));
        Check("master: a stack window that cannot fit next to the master (2400 + 500 > 2560) is hidden", pcut.Docked == 2 && pcut.Items[2].Role == Role::Hidden);
        NoOverlap("master cut: ", pcut);
        auto pbig = master.Compute(InM(w, sp, { { 2000, 0 }, { 800, 0 } }));
        Check("master: master 2000 + stack 800 do not fit 2560: the master keeps 2000, the stack starts a gap later and sticks out at the right edge", pbig.Items[0].Rect.W() >= 2000 && pbig.Items[1].Rect.L - pbig.Items[0].Rect.R == 8 && pbig.Items[1].Rect.W() >= 800);
        NoOverlap("master too wide: ", pbig);
        // pair
        auto ppl = pair.Compute(InM(w, sp, { { 1500, 0 }, z }));
        Check("pair: left minimum 1500 moves the cut, right keeps the rest", ppl.Items[0].Rect.W() >= 1500 && ppl.Items[1].Rect.L - ppl.Items[0].Rect.R == 8 && ppl.Items[1].Rect.R == ppl.Area.R);
        NoOverlap("pair left min: ", ppl);
        auto ppr = pair.Compute(InM(w, sp, { z, { 1700, 0 } }));
        Check("pair: right minimum 1700 moves the cut the other way", ppr.Items[1].Rect.W() >= 1700 && ppr.Items[0].Rect.R + 8 == ppr.Items[1].Rect.L);
        NoOverlap("pair right min: ", ppr);
        auto ppb = pair.Compute(InM(w, sp, { { 1600, 0 }, { 1600, 0 } }));
        Check("pair: both need 1600 (3200 > 2560): both keep it, the right one sticks out at the edge, no overlap", ppb.Items[0].Rect.W() >= 1600 && ppb.Items[1].Rect.W() >= 1600 && ppb.Items[1].Rect.L - ppb.Items[0].Rect.R == 8);
        NoOverlap("pair both: ", ppb);
        { // the movable cut (shared-edge resize): pair left | right, master | stack
            PairLayout pc;
            MasterLayout mc;
            StageLayout sc;
            Check("cut: stage has none, pair and master do", !sc.HasCut() && pc.HasCut() && mc.HasCut());
            Check("cut: defaults are 50/50 for pair and the master share", std::abs(pc.Cut() - 0.5) < 1e-9 && std::abs(mc.Cut() - Metrics::DefaultMasterShare) < 1e-9);
            pc.SetCut(0.3);
            auto p30 = pc.Compute(In(w, S0, 2));
            Check("cut: pair at 30% gives the left window about 30% of the width, the right one the rest, gap exact", std::abs(p30.Items[0].Rect.W() / static_cast<double>(w.W()) - 0.3) < 0.01 && p30.Items[1].Rect.L - p30.Items[0].Rect.R == 0 && p30.Items[1].Rect.R == w.R);
            NoOverlap("cut pair 30: ", p30);
            auto p30g = pc.Compute(In(w, S8, 2));
            Check("cut: with a gap the two are exactly the gap apart", p30g.Items[1].Rect.L - p30g.Items[0].Rect.R == 8);
            NoOverlap("cut pair 30 gap: ", p30g);
            auto p30s = pc.Compute(In(w, S8, 4));
            Check("cut: with a strip the cut is a ratio of the docked width (left of the strip zone)", std::abs(p30s.Items[0].Rect.W() / static_cast<double>(pc.CutSpan(p30s.Area, p30s.Strip, 4).W()) - 0.3) < 0.01 && p30s.Items[1].Rect.R <= p30s.Strip.L);
            NoOverlap("cut pair 30 strip: ", p30s);
            Check("cut: CutSpan is the whole area for two windows, the part left of the strip from three", pc.CutSpan(p30.Area, p30.Strip, 2).W() == p30.Area.W() && pc.CutSpan(p30s.Area, p30s.Strip, 4).R == p30s.Strip.L);
            pc.SetCut(0.01);
            Check("cut: kept at 15% at least", std::abs(pc.Cut() - 0.15) < 1e-9);
            pc.SetCut(0.99);
            Check("cut: kept at 85% at most", std::abs(pc.Cut() - 0.85) < 1e-9);
            mc.SetCut(0.7);
            Check("cut: master Cut is the master share", std::abs(mc.Share - 0.7) < 1e-9);
            auto pm70 = mc.Compute(In(w, S8, 3));
            Check("cut: master at 70%: the master is about 70% of the width", std::abs((pm70.Items[0].Rect.W() + 4.0) / pm70.Area.W() - 0.7) < 0.01);
            NoOverlap("cut master 70: ", pm70);
            { // stack heights (master): the edge between two stack windows moves
                MasterLayout mr;
                auto q0 = mr.Compute(In(w, S8, 4));
                std::vector<int> hs{ q0.Items[1].Rect.H(), q0.Items[2].Rect.H(), q0.Items[3].Rect.H() };
                Check("rows: equal heights to start with (within 1 px)", std::abs(hs[0] - hs[1]) <= 1 && std::abs(hs[1] - hs[2]) <= 1 && mr.Rows().empty() && !pc.MoveRow(hs, 0, 10) && !sc.MoveRow(hs, 0, 10));
                Check("rows: the last stack window has no edge below it, zero moves nothing", !mr.MoveRow(hs, 2, 50) && !mr.MoveRow(hs, 0, 0) && !mr.MoveRow(hs, -1, 5));
                Check("rows: moving the first edge down 100 px", mr.MoveRow(hs, 0, 100));
                auto q1 = mr.Compute(In(w, S8, 4));
                NoOverlap("rows moved: ", q1);
                int t0 = q0.Items[1].Rect.H() + q0.Items[2].Rect.H() + q0.Items[3].Rect.H(), t1 = q1.Items[1].Rect.H() + q1.Items[2].Rect.H() + q1.Items[3].Rect.H();
                Check("rows: first window 100 px taller, second 100 px shorter, third unchanged, total unchanged", std::abs(q1.Items[1].Rect.H() - (hs[0] + 100)) <= 2 && std::abs(q1.Items[2].Rect.H() - (hs[1] - 100)) <= 2 && std::abs(q1.Items[3].Rect.H() - hs[2]) <= 2 && std::abs(t1 - t0) <= 1);
                Check("rows: the gaps stay exact and the stack still fills the area", q1.Items[2].Rect.T - q1.Items[1].Rect.B == 8 && q1.Items[3].Rect.T - q1.Items[2].Rect.B == 8 && q1.Items[3].Rect.B == q1.Area.B);
                MasterLayout bigm;
                bigm.MoveRow(hs, 1, 100000);
                auto q2 = bigm.Compute(In(w, S8, 4));
                Check("rows: a huge move leaves the other window at least 8% of the stack", q2.Items[3].Rect.H() >= static_cast<int>(t0 * 0.07) && q2.Items[2].Rect.H() > q2.Items[3].Rect.H());
                NoOverlap("rows huge: ", q2);
                auto q3 = mr.Compute(In(w, S8, 3));
                Check("rows: another stack size ignores the shares (equal heights again)", std::abs(q3.Items[1].Rect.H() - q3.Items[2].Rect.H()) <= 1);
                auto q4 = mr.Compute(In(w, S8, 4));
                Check("rows: back at the same stack size they apply again", std::abs(q4.Items[1].Rect.H() - q1.Items[1].Rect.H()) <= 1);
                auto sv = mr.Rows();
                mr.SetRows({});
                auto q5 = mr.Compute(In(w, S8, 4));
                mr.SetRows(sv);
                Check("rows: Rows can be saved, cleared and restored", !sv.empty() && sv.size() == 3 && std::abs(q5.Items[1].Rect.H() - q5.Items[2].Rect.H()) <= 1 && std::abs(mr.Compute(In(w, S8, 4)).Items[1].Rect.H() - q1.Items[1].Rect.H()) <= 1);
                MasterLayout mm;
                auto qm = InM(w, S8, { z, { 0, 700 }, z, z });
                mm.MoveRow({ 400, 400, 400 }, 1, -150);
                auto qq = mm.Compute(qm);
                Check("rows: a minimum height still wins over the shares (700 px for the first)", qq.Items[1].Rect.H() >= 700);
                NoOverlap("rows min: ", qq);
            }
            pc.SetCut(0.2);
            auto pmin = pc.Compute(InM(w, sp, { z, { 1700, 0 } }));
            Check("cut: a minimum width still wins over the cut (right needs 1700)", pmin.Items[1].Rect.W() >= 1700);
            NoOverlap("cut pair min: ", pmin);
        }
        auto pph = pair.Compute(InM(w, sp, { { 0, 1500 }, z }));
        Check("pair: a window that needs 1500 px of height is as tall as it needs (sticks out at the bottom)", pph.Items[0].Rect.H() >= 1500 && pph.Items[1].Rect.H() == pph.Area.H());
        auto pz = master.Compute(InM(LRect(0, 0, 2000, 1000), S0, { z, { 0, 1000 }, z, z }));
        Check("master: a stack window with a 1000 px minimum in a 1000 px area: the others still get a positive height", pz.Items[2].Rect.H() >= 1 && pz.Items[3].Rect.H() >= 1);
        {
            auto o = Ids({ 1, 2, 3, 4, 5 });
            Check("master: promote a window that was cut by minimum sizes: it becomes master, the old master heads the stack", master.PromoteHidden(o, 4) && Eq(o, { 4, 1, 2, 3, 5 }));
        }
        Check("pair and master use minimum sizes, stage does not", pair.UsesMinSizes() && master.UsesMinSizes() && !StageLayout().UsesMinSizes());
        // property: pseudo-random minimums, 1..8 windows, both layouts, margins and gaps: docked rects never overlap and every docked window gets at least its minimum
        lcgSeed = 12345;
        LRect areas[] = { w, LRect(0, 0, 1919, 1077), LRect(2560, 0, 6400, 2088) };
        std::vector<std::unique_ptr<ILayout>> lays;
        lays.push_back(MakePair());
        lays.push_back(MakeMaster());
        for (const auto& lay : lays)
        {
            for (int round = 0; round < 400; round++)
            {
                const LRect& work = areas[round % 3];
                int n = 1 + round % 8;
                LayoutInput inp;
                inp.Work = work;
                inp.Spacing = round % 2 == 0 ? S8 : S10;
                for (int k = 0; k < n; k++)
                {
                    int minW = Rnd(4) == 0 ? Rnd(1500) : 0; // argument order of the C# call: width first
                    int minH = Rnd(3) == 0 ? Rnd(900) : 0;
                    inp.Wins.push_back(LWin(k + 1, WinFlags::None, minW, minH));
                }
                auto pl = lay->Compute(inp);
                std::string tag = lay->Name() + " n=" + std::to_string(n) + " round " + std::to_string(round) + ": ";
                NoOverlap(tag, pl);
                for (int k = 0; k < n; k++)
                {
                    const auto& it = pl.Items[k];
                    if (it.Role == Role::Main || it.Role == Role::Docked)
                        Check(tag + "window " + std::to_string(k + 1) + " gets at least its minimum", it.Rect.W() >= inp.Wins[k].MinW && it.Rect.H() >= inp.Wins[k].MinH);
                }
                Check(tag + "docked + hidden = all windows", pl.Docked + (n - pl.Docked) == n && pl.Docked >= (std::min)(n, 1));
            }
        }
    }

    // ---- LayoutTests.Orders / Rotations ----
    void Rotations(const std::vector<std::unique_ptr<ILayout>>& all)
    {
        for (const auto& lay : all)
        {
            for (int n = 0; n <= 7; n++)
            {
                std::vector<int> start;
                for (int i = 1; i <= n; i++)
                    start.push_back(i * 10);
                std::string tag = lay->Name() + " n=" + std::to_string(n) + ": ";
                auto o = start;
                bool changed = lay->Rotate(o, -1);
                Check(tag + "rotate changes the list only with 2+ windows", changed == (n >= 2));
                std::set<int> seen(o.begin(), o.end());
                Check(tag + "every id exactly once after a rotation", static_cast<int>(o.size()) == n && static_cast<int>(seen.size()) == n);
                o = start;
                for (int i = 0; i < n; i++)
                    lay->Rotate(o, -1);
                Check(tag + "n left rotations return to the start", S(o) == S(start));
                o = start;
                for (int i = 0; i < n; i++)
                    lay->Rotate(o, 1);
                Check(tag + "n right rotations return to the start", S(o) == S(start));
                o = start;
                lay->Rotate(o, -1);
                lay->Rotate(o, 1);
                Check(tag + "left then right is the identity", S(o) == S(start));
                o = start;
                lay->Rotate(o, 1);
                lay->Rotate(o, -1);
                Check(tag + "right then left is the identity", S(o) == S(start));
                if (n >= 2)
                {
                    o = start;
                    lay->Rotate(o, -1);
                    auto want = start;
                    want.erase(want.begin());
                    want.push_back(start[0]);
                    Check(tag + "left = old first goes to the end", S(o) == S(want));
                }
                if (n >= 2)
                {
                    o = start;
                    lay->Rotate(o, 1);
                    auto want = start;
                    want.erase(Nth(want, (n - 1)));
                    want.insert(want.begin(), start[n - 1]);
                    Check(tag + "right = old last comes to the front", S(o) == S(want));
                }
            }
        }
    }

    void Orders(const std::vector<std::unique_ptr<ILayout>>& all)
    {
        PairLayout pair;
        StageLayout stage;
        MasterLayout master;
        std::vector<int> o, t;
        // pair: add (new window always left)
        o = Ids();
        pair.Add(o, 1, false);
        Check("pair add 1st", Eq(o, { 1 }));
        pair.Add(o, 2, false);
        Check("pair 1->2: new takes left, old takes the right slot", Eq(o, { 2, 1 }));
        pair.Add(o, 3, false);
        Check("pair 2->3: old left goes to the strip, right unchanged", Eq(o, { 3, 1, 2 }));
        pair.Add(o, 4, true);
        Check("pair 3->4: same again, strip keeps MRU order", Eq(o, { 4, 1, 3, 2 }));
        // pair: promote
        Check("pair promote strip tile 2 replaces the left slot, old left to the strip front", pair.Promote(o, 2) && Eq(o, { 2, 1, 4, 3 }));
        Check("pair promote the right-slot window swaps the slots", pair.Promote(o, 1) && Eq(o, { 1, 2, 4, 3 }));
        Check("pair promote the left window is a no-op", !pair.Promote(o, 1) && Eq(o, { 1, 2, 4, 3 }));
        Check("pair promote unknown id is a no-op", !pair.Promote(o, 99));
        // pair: demote
        Check("pair demote the left window: first strip window takes the slot, it heads the strip", pair.Demote(o, 1) && Eq(o, { 4, 2, 1, 3 }));
        Check("pair demote the right window", pair.Demote(o, 2) && Eq(o, { 4, 1, 2, 3 }));
        Check("pair demote a strip window is a no-op", !pair.Demote(o, 3) && Eq(o, { 4, 1, 2, 3 }));
        { auto a = Ids({ 1, 2 }); auto b = Ids({ 1 }); Check("pair demote with no strip (2 windows) is a no-op", !pair.Demote(a, 1) && !pair.Demote(b, 1)); }
        // pair: rotate (carousel)
        o = Ids({ 4, 1, 3, 2 });
        Check("pair rotate left: [L,R,s1,s2] -> [R,s1,s2,L]", pair.Rotate(o, -1) && Eq(o, { 1, 3, 2, 4 }));
        Check("pair rotate right is the reverse: -> [s2,L,R,s1]", pair.Rotate(o, 1) && Eq(o, { 4, 1, 3, 2 }) && pair.Rotate(o, 1) && Eq(o, { 2, 4, 1, 3 }));
        o = Ids({ 2, 1 });
        Check("pair rotate with 2 windows swaps the sides", pair.Rotate(o, 1) && Eq(o, { 1, 2 }) && pair.Rotate(o, -1) && Eq(o, { 2, 1 }));
        { auto a = Ids({ 1 }); auto b = Ids(); Check("pair rotate with 1 window or none is a no-op", !pair.Rotate(a, 1) && !pair.Rotate(b, -1)); }
        // pair: remove
        o = Ids({ 4, 1, 3, 2 });
        pair.Remove(o, 4);
        Check("pair remove left: first strip window takes the left slot", Eq(o, { 3, 1, 2 }));
        o = Ids({ 4, 1, 3, 2 });
        pair.Remove(o, 1);
        Check("pair remove right: first strip window takes the right slot", Eq(o, { 4, 3, 2 }));
        o = Ids({ 4, 1, 3, 2 });
        pair.Remove(o, 2);
        Check("pair remove a strip window", Eq(o, { 4, 1, 3 }));
        o = Ids({ 4, 1 });
        pair.Remove(o, 4);
        Check("pair remove left of 2: the other fills the work area", Eq(o, { 1 }));
        // pair: join (window arrives from another monitor)
        o = Ids({ 1, 2, 3 });
        pair.Join(o, 9, true);
        Check("pair join as docked = a new window (left slot)", Eq(o, { 9, 2, 1, 3 }));
        o = Ids({ 1, 2, 3 });
        pair.Join(o, 9, false);
        Check("pair join from a strip: front of the strip", Eq(o, { 1, 2, 9, 3 }));
        o = Ids({ 1 });
        pair.Join(o, 9, false);
        Check("pair join into a lone window: takes the right slot", Eq(o, { 1, 9 }));
        // stage
        o = Ids();
        stage.Add(o, 1, true);
        stage.Add(o, 2, false);
        Check("stage add unfocused goes to strip slot 1", Eq(o, { 1, 2 }));
        stage.Add(o, 3, true);
        Check("stage add focused takes main", Eq(o, { 3, 1, 2 }));
        Check("stage promote a tile", stage.Promote(o, 2) && Eq(o, { 2, 3, 1 }));
        Check("stage promote main is a no-op", !stage.Promote(o, 2));
        Check("stage demote main swaps it with the first tile", stage.Demote(o, 2) && Eq(o, { 3, 2, 1 }));
        { auto a = Ids({ 1 }); Check("stage demote a tile / lone window is a no-op", !stage.Demote(o, 1) && !stage.Demote(a, 1)); }
        o = Ids({ 1, 2, 3, 4 });
        Check("stage rotate left: [main,s1,s2,s3] -> [s1,s2,s3,main]", stage.Rotate(o, -1) && Eq(o, { 2, 3, 4, 1 }));
        Check("stage rotate right undoes it", stage.Rotate(o, 1) && Eq(o, { 1, 2, 3, 4 }));
        o = Ids({ 1, 2, 3 });
        stage.Remove(o, 1);
        Check("stage remove main: next in MRU order becomes main", Eq(o, { 2, 3 }));
        o = Ids({ 1, 2, 3 });
        stage.Remove(o, 3);
        Check("stage remove a tile", Eq(o, { 1, 2 }));
        o = Ids({ 1, 2 });
        stage.Join(o, 9, false);
        Check("stage join from a strip: strip slot 1", Eq(o, { 1, 9, 2 }));
        stage.Join(o, 8, true);
        Check("stage join as main", Eq(o, { 8, 1, 9, 2 }));
        // master
        o = Ids();
        master.Add(o, 1, false);
        master.Add(o, 2, false);
        master.Add(o, 3, true);
        Check("master: a new window becomes master, the old master goes to the top of the stack", Eq(o, { 3, 2, 1 }));
        Check("master promote a stack window swaps it with the master", master.Promote(o, 1) && Eq(o, { 1, 2, 3 }));
        Check("master promote the master is a no-op", !master.Promote(o, 1));
        o = Ids({ 1, 2, 3, 4, 5, 6, 7 });
        Check("master promote a hidden window: it becomes master, the old master heads the stack", master.Promote(o, 6) && Eq(o, { 6, 1, 2, 3, 4, 5, 7 }) && master.Slots(7) == 5);
        o = Ids({ 1, 2, 3 });
        Check("master send to the bottom: master goes to the end, next window becomes master", master.Demote(o, 1) && Eq(o, { 2, 3, 1 }));
        Check("master send a stack window to the bottom", master.Demote(o, 2) && Eq(o, { 3, 1, 2 }));
        { auto a = Ids({ 1 }); Check("master send the last window / a lone window is a no-op", !master.Demote(o, 2) && !master.Demote(a, 1)); }
        o = Ids({ 1, 2, 3, 4, 5, 6, 7 });
        Check("master send a stack window past the hidden ones (end of the list)", master.Demote(o, 3) && Eq(o, { 1, 2, 4, 5, 6, 7, 3 }));
        o = Ids({ 1, 2, 3, 4, 5, 6, 7 });
        master.Remove(o, 1);
        Check("master remove the master: everything moves up, the first hidden window enters the stack", Eq(o, { 2, 3, 4, 5, 6, 7 }) && master.Slots(6) == 5);
        o = Ids({ 1, 2, 3 });
        master.Join(o, 9, false);
        Check("master join not in front: top of the stack", Eq(o, { 1, 9, 2, 3 }));
        master.Join(o, 8, true);
        Check("master join as docked: master", Eq(o, { 8, 1, 9, 2, 3 }));
        o = Ids({ 1, 2, 3, 4, 5, 6, 7 });
        master.Rotate(o, -1);
        LayoutInput inp;
        inp.Work = LRect(0, 0, 2560, 1400);
        inp.Spacing = S8;
        for (int id : o)
            inp.Wins.push_back(LWin(id));
        auto pl = master.Compute(inp);
        Check("master rotate: window 6 (hidden before) is docked now, window 1 (the old master) went out and is hidden", Eq(o, { 2, 3, 4, 5, 6, 7, 1 }) && pl.Items[4].Id == 6 && pl.Items[4].Role == Role::Docked && pl.Items[6].Id == 1 && pl.Items[6].Role == Role::Hidden && pl.Items[5].Id == 7 && pl.Items[5].Role == Role::Hidden);
        Rotations(all);
    }

    // ---- golden vectors: C++ mirror of the generator embedded in tools\stage-vectors\gen.ps1 (same lines, same order) ----
    std::string R(const LRect& r) { return std::to_string(r.L) + "," + std::to_string(r.T) + "," + std::to_string(r.R) + "," + std::to_string(r.B); }
    std::string Sp(const Spacing& s) { return std::to_string(s.Gap) + "," + std::to_string(s.MarginL) + "," + std::to_string(s.MarginT) + "," + std::to_string(s.MarginR) + "," + std::to_string(s.MarginB); }
    std::string WinsStr(const std::vector<LWin>& w)
    {
        std::string s;
        for (size_t i = 0; i < w.size(); i++)
            s += (i ? " " : "") + std::to_string(w[i].Id) + ":" + std::to_string(static_cast<int>(w[i].Flags)) + ":" + std::to_string(w[i].MinW) + ":" + std::to_string(w[i].MinH);
        return s;
    }
    std::string PStr(const Plan& p)
    {
        std::string b = "strip=" + std::to_string(p.HasStrip ? 1 : 0) + " area=" + R(p.Area) + " zone=" + R(p.Strip) + " park=" + R(p.Park) + " tile=" + std::to_string(p.TileW) + "x" + std::to_string(p.TileH) + " gap=" + std::to_string(p.Gap) + " docked=" + std::to_string(p.Docked) + " |";
        for (const auto& it : p.Items)
            b += " " + std::to_string(it.Id) + ":" + std::to_string(static_cast<int>(it.Role)) + ":" + std::to_string(it.Slot) + ":" + R(it.Rect);
        return b;
    }
    std::unique_ptr<ILayout> Make(const std::string& name) { return MakeLayout(name); }
    std::vector<LWin> Plain(int n, int hiddenIdx)
    {
        std::vector<LWin> l;
        for (int k = 0; k < n; k++)
            l.push_back(LWin(k + 1, k == hiddenIdx ? WinFlags::Hidden : WinFlags::None));
        return l;
    }
    long long Ppm(double x) { return std::llround(x * 1e6); }

    void VCase(std::vector<std::string>& out, const std::string& tag, ILayout& lay, const LRect& w, const Spacing& sp, const std::vector<LWin>& wins)
    {
        LayoutInput inp;
        inp.Work = w;
        inp.Spacing = sp;
        inp.Wins = wins;
        auto p = lay.Compute(inp);
        out.push_back(tag + " " + lay.Name() + " w=" + R(w) + " sp=" + Sp(sp) + " wins=" + WinsStr(wins) + " => " + PStr(p));
    }

    std::vector<std::string> GenerateVectors()
    {
        std::vector<std::string> out;
        auto works = Works();
        auto spacings = Spacings();
        const std::vector<std::string> names{ "stage", "pair", "master" };
        // G
        for (const auto& nm : names)
            for (const auto& w : works)
                for (const auto& sp : spacings)
                    for (int n = 0; n <= 9; n++)
                    {
                        auto lay = Make(nm);
                        VCase(out, "G", *lay, w, sp, Plain(n, -1));
                    }
        // H
        for (const auto& nm : names)
            for (const auto& w : works)
                for (int si = 0; si < 3; si++)
                    for (int n = 2; n <= 9; n++)
                    {
                        auto lay = Make(nm);
                        VCase(out, "H", *lay, w, spacings[si], Plain(n, 1));
                    }
        // M
        lcgSeed = 12345;
        LRect areas[] = { works[0], LRect(0, 0, 1919, 1077), LRect(2560, 0, 6400, 2088) };
        for (const std::string nm : { "pair", "master" })
            for (int round = 0; round < 400; round++)
            {
                const LRect& work = areas[round % 3];
                int n = 1 + round % 8;
                std::vector<LWin> wins;
                for (int k = 0; k < n; k++)
                {
                    int mw = Rnd(4) == 0 ? Rnd(1500) : 0;
                    int mh = Rnd(3) == 0 ? Rnd(900) : 0;
                    int hid = Rnd(10) == 0 ? 1 : 0;
                    wins.push_back(LWin(k + 1, hid == 1 ? WinFlags::Hidden : WinFlags::None, mw, mh));
                }
                auto lay = Make(nm);
                VCase(out, "M", *lay, work, round % 2 == 0 ? Spacing::MuntinDefault() : Spacing(10, 10, 10, 10, 10), wins);
            }
        // C
        const std::pair<double, const char*> cuts[] = { { 0.01, "0.01" }, { 0.15, "0.15" }, { 0.3, "0.30" }, { 0.5, "0.50" }, { 0.7, "0.70" }, { 0.85, "0.85" }, { 0.99, "0.99" } };
        for (const std::string nm : { "pair", "master" })
            for (const auto& c : cuts)
                for (int wi : { 0, 3 })
                    for (int n = 2; n <= 6; n++)
                    {
                        auto lay = Make(nm);
                        lay->SetCut(c.first);
                        Spacing sp = wi == 0 ? Spacing::MuntinDefault() : Spacing(10, 10, 10, 10, 10);
                        LayoutInput inp;
                        inp.Work = works[wi];
                        inp.Spacing = sp;
                        inp.Wins = Plain(n, -1);
                        auto p = lay->Compute(inp);
                        out.push_back("C " + nm + " cut=" + c.second + " w=" + R(works[wi]) + " sp=" + Sp(sp) + " n=" + std::to_string(n) + " cutppm=" + std::to_string(Ppm(lay->Cut())) + " span=" + R(lay->CutSpan(p.Area, p.Strip, n)) + " => " + PStr(p));
                    }
        // R
        for (int n = 3; n <= 6; n++)
        {
            MasterLayout m;
            std::vector<std::pair<int, int>> ops{ { 0, 100 }, { 1, -60 }, { n - 3, 37 }, { 0, -100000 }, { 1, 100000 } };
            for (const auto& op : ops)
            {
                LayoutInput inp;
                inp.Work = works[0];
                inp.Spacing = Spacing::MuntinDefault();
                inp.Wins = Plain(n, -1);
                auto p0 = m.Compute(inp);
                std::vector<int> hs;
                for (int i = 1; i < p0.Docked; i++)
                    hs.push_back(p0.Items[i].Rect.H());
                bool ok = m.MoveRow(hs, op.first, op.second);
                auto rows = m.Rows();
                std::string rs = "null";
                if (!rows.empty())
                {
                    rs.clear();
                    for (size_t i = 0; i < rows.size(); i++)
                        rs += (i ? "," : "") + std::to_string(Ppm(rows[i]));
                }
                auto p1 = m.Compute(inp);
                out.push_back("R n=" + std::to_string(n) + " heights=" + S(hs) + " op=" + std::to_string(op.first) + "," + std::to_string(op.second) + " ok=" + std::to_string(ok ? 1 : 0) + " rows=" + rs + " => " + PStr(p1));
            }
        }
        // O
        for (int li = 0; li < 3; li++)
        {
            auto lay = Make(names[li]);
            lcgSeed = static_cast<uint32_t>(1000 + li);
            std::vector<int> o;
            int nextId = 1;
            for (int step = 0; step < 300; step++)
            {
                int op = Rnd(8);
                if (o.size() >= 10 && op <= 2)
                    op = 6;
                if (o.empty() && op >= 3)
                    op = 0;
                std::string args = "-", ret = "-";
                switch (op)
                {
                case 0:
                case 1:
                {
                    int id = nextId++;
                    int f = Rnd(2);
                    lay->Add(o, id, f == 1);
                    args = std::to_string(id) + "," + std::to_string(f);
                    break;
                }
                case 2:
                {
                    int id = nextId++;
                    int d = Rnd(2);
                    lay->Join(o, id, d == 1);
                    args = std::to_string(id) + "," + std::to_string(d);
                    break;
                }
                case 3:
                {
                    int id = o[Rnd(static_cast<int>(o.size()))];
                    ret = lay->Promote(o, id) ? "1" : "0";
                    args = std::to_string(id);
                    break;
                }
                case 4:
                {
                    int id = o[Rnd(static_cast<int>(o.size()))];
                    ret = lay->PromoteHidden(o, id) ? "1" : "0";
                    args = std::to_string(id);
                    break;
                }
                case 5:
                {
                    int id = o[Rnd(static_cast<int>(o.size()))];
                    ret = lay->Demote(o, id) ? "1" : "0";
                    args = std::to_string(id);
                    break;
                }
                case 6:
                {
                    int id = o[Rnd(static_cast<int>(o.size()))];
                    lay->Remove(o, id);
                    args = std::to_string(id);
                    break;
                }
                default:
                {
                    int dir = Rnd(2) == 0 ? -1 : 1;
                    ret = lay->Rotate(o, dir) ? "1" : "0";
                    args = std::to_string(dir);
                    break;
                }
                }
                int cnt = static_cast<int>(o.size());
                out.push_back("O " + names[li] + " step=" + std::to_string(step) + " op=" + std::to_string(op) + " args=" + args + " ret=" + ret + " list=" + S(o) + " slots=" + std::to_string(lay->Slots(cnt)) + " strip=" + std::to_string(lay->HasStrip(cnt) ? 1 : 0));
            }
        }
        return out;
    }

    std::filesystem::path VectorsPath()
    {
        // .../src/modules/fancyzones/FancyZonesTests/UnitTests/StageLayouts.Spec.cpp -> repo root is six parent_path steps up (file -> UnitTests ... -> src -> root)
        std::filesystem::path p = std::filesystem::path(__FILE__).lexically_normal();
        for (int i = 0; i < 6; i++)
            p = p.parent_path();
        return p / "tools" / "stage-vectors" / "StageLayouts.vectors.txt";
    }
}

namespace FancyZonesUnitTests
{
    TEST_CLASS (StageLayoutsUnitTests)
    {
    public:
        TEST_METHOD (Geometry)
        {
            Begin();
            auto all = FreshAll();
            auto works = Works();
            auto spacings = Spacings();
            for (const auto& lay : all)
                for (const auto& w : works)
                    for (const auto& sp : spacings)
                        for (int n = 0; n <= 9; n++)
                            ::Geometry(*lay, w, sp, n);
            Finish("geometry", CsGeometryCases);
        }

        TEST_METHOD (Scenarios)
        {
            Begin();
            ::Scenarios();
            Finish("scenarios", CsScenarioCases);
        }

        TEST_METHOD (MinimumWindowSizes)
        {
            Begin();
            ::MinSizes();
            Finish("minimum window sizes", CsMinSizeCases);
        }

        TEST_METHOD (OrderPolicy)
        {
            Begin();
            auto all = FreshAll();
            ::Orders(all);
            Finish("order policy", CsOrderCases);
        }

        TEST_METHOD (GoldenVectors)
        {
            auto path = VectorsPath();
            std::ifstream f(path, std::ios::binary);
            Assert::IsTrue(f.good(), (L"vectors file not found: " + path.wstring()).c_str());
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::vector<std::string> want;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find('\n', pos);
                std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? text.size() : nl + 1;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty() && line[0] != '#')
                    want.push_back(line);
            }
            auto got = GenerateVectors();
            Assert::AreEqual(want.size(), got.size(), L"vector line count differs from the C# golden file");
            for (size_t i = 0; i < want.size(); i++)
            {
                if (want[i] != got[i])
                {
                    std::string msg = "vector line " + std::to_string(i + 1) + " differs\n  C#  : " + want[i] + "\n  C++ : " + got[i];
                    Assert::Fail(Widen(msg).c_str());
                }
            }
        }
    };
}
