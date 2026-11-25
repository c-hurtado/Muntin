#pragma once

// Stage mode safety net, pure parts: what Muntin records about every window it is about to move (stage-state.txt), how that
// file is written and read, which of its windows a restore may touch, and the clamping of the sizes the apps report.
// No window is touched in this file; every input that would come from Windows is passed in, so tests need no windows.
//
// The file is written (atomically: temp file + rename) BEFORE the first move of a window and again whenever the set of managed
// windows changes. It is deleted by a clean restore. If Muntin dies, the watchdog process (or the next start) reads it and puts
// every window back. Restore is by window handle, process id and window class: a reused handle is never touched.

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <FancyZonesLib/StageLayouts.h>

namespace StageState
{
    // One managed window as it was before the stage touched it
    struct Entry
    {
        std::int64_t hwnd{};
        std::uint32_t pid{};
        StageLayouts::LRect rect; // GetWindowRect
        int showCmd{ 1 }; // WINDOWPLACEMENT
        int flags{};
        int minX{ -1 }, minY{ -1 }, maxX{ -1 }, maxY{ -1 };
        StageLayouts::LRect normal; // rcNormalPosition
        std::wstring cls;

        bool operator==(const Entry& o) const
        {
            return hwnd == o.hwnd && pid == o.pid && rect.Same(o.rect) && showCmd == o.showCmd && flags == o.flags && minX == o.minX && minY == o.minY &&
                   maxX == o.maxX && maxY == o.maxY && normal.Same(o.normal) && cls == o.cls;
        }
    };

    inline constexpr wchar_t Header[] = L"muntin-stage-state 1";

    // Line format (one window per line, fields separated by one space, the class name is the rest of the line):
    //   hwnd pid wl wt wr wb showCmd flags minX minY maxX maxY nl nt nr nb class
    inline std::wstring Serialize(const std::vector<Entry>& entries)
    {
        std::wostringstream out;
        out << Header << L'\n';
        for (const auto& e : entries)
        {
            std::wstring cls = e.cls;
            std::replace_if(cls.begin(), cls.end(), [](wchar_t c) { return c == L'\n' || c == L'\r'; }, L' ');
            out << e.hwnd << L' ' << e.pid << L' ' << e.rect.L << L' ' << e.rect.T << L' ' << e.rect.R << L' ' << e.rect.B << L' '
                << e.showCmd << L' ' << e.flags << L' ' << e.minX << L' ' << e.minY << L' ' << e.maxX << L' ' << e.maxY << L' '
                << e.normal.L << L' ' << e.normal.T << L' ' << e.normal.R << L' ' << e.normal.B << L' ' << cls << L'\n';
        }

        return out.str();
    }

    // Lines that don't parse are skipped (a half-written or foreign file restores what it can). A missing header: nothing.
    inline std::vector<Entry> Parse(const std::wstring& text)
    {
        std::vector<Entry> entries;
        std::wistringstream in(text);
        std::wstring line;
        if (!std::getline(in, line))
        {
            return entries;
        }

        while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
        {
            line.pop_back();
        }

        if (line != Header)
        {
            return entries;
        }

        while (std::getline(in, line))
        {
            std::wistringstream fields(line);
            Entry e;
            if (fields >> e.hwnd >> e.pid >> e.rect.L >> e.rect.T >> e.rect.R >> e.rect.B >> e.showCmd >> e.flags >> e.minX >> e.minY >> e.maxX >> e.maxY >>
                e.normal.L >> e.normal.T >> e.normal.R >> e.normal.B)
            {
                std::getline(fields >> std::ws, e.cls);
                while (!e.cls.empty() && e.cls.back() == L'\r')
                {
                    e.cls.pop_back();
                }

                entries.push_back(std::move(e));
            }
        }

        return entries;
    }

    inline std::string ToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>((std::max)(size, 0)), '\0');
        if (size > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
        }

        return utf8;
    }

    inline std::wstring FromUtf8(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>((std::max)(size, 0)), L'\0');
        if (size > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
        }

        return wide;
    }

    // The temp file of this process: the watchdog and a new Muntin may write at the same time, each to a name of its own
    inline std::wstring TempName(const std::filesystem::path& path)
    {
        return path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    }

    // Temp file next to the target, then a rename over it: a reader sees the old file or the new one, never half of one.
    // An empty list removes the file (nothing to restore). False: the file could not be written, and the caller must not move windows.
    inline bool Write(const std::filesystem::path& path, const std::vector<Entry>& entries) noexcept
    {
        try
        {
            std::error_code error;
            if (entries.empty())
            {
                std::filesystem::remove(path, error);
                std::filesystem::remove(TempName(path), error);
                return !std::filesystem::exists(path, error);
            }

            std::filesystem::create_directories(path.parent_path(), error);
            const std::wstring temp = TempName(path);
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                const auto bytes = ToUtf8(Serialize(entries));
                file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                file.flush();
                if (!file)
                {
                    file.close();
                    std::filesystem::remove(temp, error);
                    return false;
                }
            }

            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                std::filesystem::remove(temp, error);
                return false;
            }

            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    inline std::vector<Entry> Read(const std::filesystem::path& path) noexcept
    {
        try
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return {};
            }

            std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
            {
                bytes.erase(0, 3);
            }

            return Parse(FromUtf8(bytes));
        }
        catch (...)
        {
            return {};
        }
    }

    // What the restore may know about a window now
    struct Probe
    {
        std::function<bool(std::int64_t)> exists; // IsWindow
        std::function<std::uint32_t(std::int64_t)> pid; // owner process
        std::function<std::wstring(std::int64_t)> cls; // window class
        std::function<bool(std::int64_t)> hung;
    };

    enum class RestoreAction
    {
        Skip, // gone, or the handle now belongs to something else: nothing to do, nothing to keep
        Restore,
        Keep // not responding: stays in the file for a later try
    };

    inline RestoreAction PlanOne(const Entry& entry, const Probe& probe)
    {
        if (!probe.exists(entry.hwnd))
        {
            return RestoreAction::Skip;
        }

        if (probe.pid(entry.hwnd) != entry.pid || probe.cls(entry.hwnd) != entry.cls)
        {
            return RestoreAction::Skip;
        }

        return probe.hung(entry.hwnd) ? RestoreAction::Keep : RestoreAction::Restore;
    }

    inline std::vector<RestoreAction> PlanRestore(const std::vector<Entry>& entries, const Probe& probe)
    {
        std::vector<RestoreAction> plan;
        plan.reserve(entries.size());
        for (const auto& entry : entries)
        {
            plan.push_back(PlanOne(entry, probe));
        }

        return plan;
    }

    // Largest on-screen extent (smaller of width and height of the overlap with one monitor). Under 4 px: not really visible.
    inline int OnScreenPx(const StageLayouts::LRect& r, const std::vector<StageLayouts::LRect>& monitors)
    {
        int best = 0;
        for (const auto& m : monitors)
        {
            const int w = (std::min)(r.R, m.R) - (std::max)(r.L, m.L);
            const int h = (std::min)(r.B, m.B) - (std::max)(r.T, m.T);
            if (w > 0 && h > 0)
            {
                best = (std::max)(best, (std::min)(w, h));
            }
        }

        return best;
    }

    // The saved rect can be put back as it is while it is still on a monitor that exists (or when no monitor is known)
    inline bool RectUsable(const StageLayouts::LRect& r, const std::vector<StageLayouts::LRect>& monitors)
    {
        return r.R > r.L && (monitors.empty() || OnScreenPx(r, monitors) >= 4);
    }

    // A saved rect wider or taller than any monitor (plus the invisible frame, 32 px) cannot be a real window, unless it really spans
    // monitors: it then lies inside the bounding box of the monitors (plus the frame) and overlaps at least two of them. Otherwise it
    // came from a coordinate mix-up (a virtualized rect also reaches past the monitors) and is brought down to the size of the monitor it
    // overlaps most and moved onto it. Other rects stay as they are, so a window that spans two monitors comes back as it was.
    // KNOWN LIMIT: a virtualized rect of a window SMALLER than the biggest monitor cannot be told from a real one here; Enter
    // captures under per-monitor awareness (StageDpi), so only a state file of an older build can carry one.
    inline StageLayouts::LRect SaneRect(const StageLayouts::LRect& r, const std::vector<StageLayouts::LRect>& monitors)
    {
        constexpr int frame = 32;
        if (monitors.empty())
        {
            return r;
        }

        int maxW = 0, maxH = 0, overlapping = 0;
        int boxL = monitors.front().L, boxT = monitors.front().T, boxR = monitors.front().R, boxB = monitors.front().B;
        const StageLayouts::LRect* best = &monitors.front();
        long long bestArea = -1;
        for (const auto& m : monitors)
        {
            maxW = (std::max)(maxW, m.W());
            maxH = (std::max)(maxH, m.H());
            boxL = (std::min)(boxL, m.L);
            boxT = (std::min)(boxT, m.T);
            boxR = (std::max)(boxR, m.R);
            boxB = (std::max)(boxB, m.B);
            const long long w = (std::max)(0, (std::min)(r.R, m.R) - (std::max)(r.L, m.L));
            const long long h = (std::max)(0, (std::min)(r.B, m.B) - (std::max)(r.T, m.T));
            overlapping += (w > 0 && h > 0) ? 1 : 0;
            if (w * h > bestArea)
            {
                bestArea = w * h;
                best = &m;
            }
        }

        if (r.W() <= maxW + frame && r.H() <= maxH + frame)
        {
            return r;
        }

        const bool inBox = r.L >= boxL - frame && r.T >= boxT - frame && r.R <= boxR + frame && r.B <= boxB + frame;
        if (inBox && overlapping >= 2)
        {
            return r; // Really spans monitors
        }

        const int w = (std::min)(r.W(), best->W()), h = (std::min)(r.H(), best->H());
        const int l = (std::max)(best->L, (std::min)(r.L, best->R - w));
        const int t = (std::max)(best->T, (std::min)(r.T, best->B - h));
        return StageLayouts::LRect(l, t, l + w, t + h);
    }

    // The entry as it may be restored: both rects made sane
    inline Entry Sanitized(Entry e, const std::vector<StageLayouts::LRect>& monitors)
    {
        e.rect = SaneRect(e.rect, monitors);
        e.normal = SaneRect(e.normal, monitors);
        return e;
    }

    // The sizes that apps report (WM_GETMINMAXINFO) are not trusted: a bogus huge value would overflow the layout's int
    // arithmetic (undefined behaviour). Every minimum is brought into 0 .. the size of the work area.
    inline int ClampMin(long long value, int limit)
    {
        return static_cast<int>((std::max)(0LL, (std::min)(value, static_cast<long long>((std::max)(0, limit)))));
    }

    inline void ClampMinSizes(std::vector<StageLayouts::LWin>& wins, const StageLayouts::LRect& work)
    {
        for (auto& w : wins)
        {
            w.MinW = ClampMin(w.MinW, work.W());
            w.MinH = ClampMin(w.MinH, work.H());
        }
    }

    // Visible minimum from the window-rect minimum an app reports less its invisible frame. Everything in 64 bit, then clamped.
    inline int VisibleMin(long long reported, int insetA, int insetB, int limit)
    {
        return ClampMin(reported - insetA - insetB, limit);
    }

    // How a window's saved placement goes back (WINDOWPLACEMENT showCmd and flags)
    struct PlacementPlan
    {
        int showCmd{};
        int flags{};
        bool usePlacement{ true }; // false: only the rect goes back (SetWindowPos)
    };

    // Async always (WPF_ASYNCWINDOWPLACEMENT): a window that hangs must not hang Muntin. A window put back minimized that was
    // maximized restores to maximized (WPF_RESTORETOMAXIMIZED); so does one that is hidden (the app keeps it hidden).
    inline PlacementPlan PlacementFor(const Entry& e, bool minimized, bool hidden)
    {
        constexpr int async = 0x0004;       // WPF_ASYNCWINDOWPLACEMENT
        constexpr int toMaximized = 0x0002; // WPF_RESTORETOMAXIMIZED
        constexpr int maximized = 3;        // SW_SHOWMAXIMIZED
        constexpr int minimize = 2;         // SW_SHOWMINIMIZED
        constexpr int hide = 0;             // SW_HIDE

        const bool wasMaximized = e.showCmd == maximized;
        if (hidden)
        {
            return wasMaximized ? PlacementPlan{ hide, (e.flags | async | toMaximized), true } : PlacementPlan{ e.showCmd, e.flags, false };
        }

        if (minimized)
        {
            return { minimize, e.flags | async | (wasMaximized ? toMaximized : 0), true };
        }

        return { e.showCmd, e.flags | async, true };
    }

    // Rehome (a held window was moved onto another monitor): its saved original place follows it, so the restore does not send it back.
    // The rect moves by the difference of the monitor origins, then slides back inside the new monitor when the old one was larger.
    inline StageLayouts::LRect ShiftIntoMonitor(const StageLayouts::LRect& r, const StageLayouts::LRect& from, const StageLayouts::LRect& to)
    {
        const int dx = to.L - from.L, dy = to.T - from.T;
        const int nl = r.L + dx, nt = r.T + dy, nr = r.R + dx, nb = r.B + dy;
        int cx = nr > to.R ? to.R - nr : 0;
        if (nl + cx < to.L)
        {
            cx = to.L - nl;
        }

        int cy = nb > to.B ? to.B - nb : 0;
        if (nt + cy < to.T)
        {
            cy = to.T - nt;
        }

        return StageLayouts::LRect(nl + cx, nt + cy, nr + cx, nb + cy);
    }

    // The monitor a window goes to when its own is gone: the one at 0,0, else the first
    inline StageLayouts::LRect PrimaryOf(const std::vector<StageLayouts::LRect>& monitors)
    {
        for (const auto& m : monitors)
        {
            if (m.L <= 0 && 0 < m.R && m.T <= 0 && 0 < m.B)
            {
                return m;
            }
        }

        return monitors.empty() ? StageLayouts::LRect() : monitors.front();
    }

    // A saved rect that lies on no monitor any more (it was on one that is unplugged): moved onto the primary monitor, 32 px in from its
    // corner and slid inside. A rect that is still on a monitor stays as it is.
    inline StageLayouts::LRect OnAVisibleMonitor(const StageLayouts::LRect& r, const std::vector<StageLayouts::LRect>& monitors)
    {
        if (monitors.empty() || RectUsable(r, monitors) || r.R <= r.L)
        {
            return r;
        }

        const auto to = PrimaryOf(monitors);
        const StageLayouts::LRect fake(r.L - 32, r.T - 32, r.R - 32, r.B - 32); // makes the shift land 32 px from the monitor corner
        return ShiftIntoMonitor(r, fake, to);
    }

    inline Entry OnVisibleMonitors(Entry e, const std::vector<StageLayouts::LRect>& monitors)
    {
        e.rect = OnAVisibleMonitor(e.rect, monitors);
        e.normal = OnAVisibleMonitor(e.normal, monitors);
        return e;
    }

    // Was the window one of the docked (shown) windows of its stage? Then it joins the other stage docked too (the layout decides
    // where: master / left slot, or behind them), else not (strip, stack end, hidden): as StageProto's Rehome.
    inline bool WasDocked(const StageLayouts::ILayout& layout, const std::vector<int>& order, int id)
    {
        const auto it = std::find(order.begin(), order.end(), id);
        return it != order.end() && static_cast<int>(it - order.begin()) < layout.Slots(static_cast<int>(order.size()));
    }

    // A scan was held back by a grace period (or a drag): it is tried again right after, never dropped. pending: the earliest scan
    // already due (0: none). Returns when the next scan is due.
    inline unsigned long long RearmRehome(unsigned long long pending, unsigned long long blockedUntil)
    {
        const unsigned long long at = blockedUntil + 1;
        return pending == 0 ? at : (pending < at ? pending : at);
    }

    enum class RehomeAction
    {
        None,
        Move, // join the stage of the monitor it is on
        Unmanage // that stage has no room: the window is left where the user put it
    };

    struct RehomeFacts
    {
        bool usable{}; // exists, visible, not minimized, responding
        bool mouseDown{}; // a drag may be in progress
        unsigned long long now{}, grace{}; // our own moves are still landing before grace
        bool destinationKnown{};
        bool sameStage{};
        bool destinationHasRoom{};
    };

    inline RehomeAction DecideRehome(const RehomeFacts& f)
    {
        if (!f.usable || f.mouseDown || f.now < f.grace || !f.destinationKnown || f.sameStage)
        {
            return RehomeAction::None;
        }

        return f.destinationHasRoom ? RehomeAction::Move : RehomeAction::Unmanage;
    }

    // A window we asked to leave the maximized state (ShowWindowAsync SW_RESTORE): wait for it, but only until a deadline (not a
    // count of looks: the stage is laid out many times per second during a scan). A window that is still maximized at the deadline
    // is stuck: it is released as it was and never adopted again until it is no longer maximized (or gone).
    enum class Unmaximize
    {
        Done, // not maximized (any more)
        UserMaximized, // maximized, and we did not ask: left alone
        Wait,
        GiveUp
    };

    inline Unmaximize UnmaximizeDecision(bool weAsked, bool zoomed, unsigned long long now, unsigned long long until)
    {
        if (!zoomed)
        {
            return Unmaximize::Done;
        }

        if (!weAsked)
        {
            return Unmaximize::UserMaximized;
        }

        return now < until ? Unmaximize::Wait : Unmaximize::GiveUp;
    }

    // A window on the stuck list stays skipped only while it is still maximized
    inline bool StillStuck(bool onStuckList, bool zoomed)
    {
        return onStuckList && zoomed;
    }

    // The window's own minimum size is learned from how it lands (StageProto VerifyTick): a frame more than Tolerance px bigger
    // than its slot, seen on two checks in a row (a slow app may not have applied the move yet).
    constexpr int VerifyTolerance = 2;

    struct Pending
    {
        int w{}, h{};
    };

    struct Verdict
    {
        bool learnW{}, learnH{};
        bool lookAgain{}; // a size was seen once: check again before believing it
    };

    inline Verdict VerifyLanding(int frameW, int frameH, int wantW, int wantH, Pending& pending)
    {
        const bool bw = frameW > wantW + VerifyTolerance, bh = frameH > wantH + VerifyTolerance;
        if (!bw && !bh)
        {
            pending = {};
            return {};
        }

        if (!((!bw || frameW == pending.w) && (!bh || frameH == pending.h)))
        {
            pending = { bw ? frameW : 0, bh ? frameH : 0 };
            return { false, false, true };
        }

        pending = {};
        return { bw, bh, false };
    }

    // Minimum-size cut (Phase 6). A layout without a strip (master) cannot show a window its minimum sizes squeezed out of the stack:
    // the plan marks it Role::Hidden. Parked it would sit as an invisible sliver with no way back but the keys, so the stage lets such
    // a window go instead (it is released: back where it was, floating, visible, never held). Layouts with a strip have a tile for
    // every window that has no slot, so nothing is cut there. keep(id): windows to leave alone for now (a modal dialog hangs off it, the
    // user drags it). Last in the layout order first.
    inline std::vector<int> MinSizeCut(const StageLayouts::Plan& plan, const std::function<bool(int)>& keep = {})
    {
        std::vector<int> cut;
        if (plan.HasStrip)
        {
            return cut;
        }

        for (auto it = plan.Items.rbegin(); it != plan.Items.rend(); ++it)
        {
            if (it->Role == StageLayouts::Role::Hidden && !(keep && keep(it->Id)))
            {
                cut.push_back(it->Id);
            }
        }

        return cut;
    }
}
