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
}
