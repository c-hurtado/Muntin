#pragma once

#include <windows.h>

#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <FancyZonesLib/DesktopScroll.h>
#include <FancyZonesLib/Settings.h>

// Which arrangement owns the windows: dock-by-drag (the default) or Stage mode. One question, asked at every seam
// where Muntin would move a window: may it? (Phase 2: Stage itself is not there yet, only the hands-off protocol.)
namespace Stage
{
    // The windows the stage manages, kept by StageMode (main thread) and read from any thread (DockMotion): a lock guards the set.
    // Header-only on purpose: the unit tests need no StageMode to test the hands-off rules.
    namespace Registry
    {
        inline std::mutex lock;
        inline std::set<HWND> held;
    }

    inline void SetHeld(std::set<HWND> windows)
    {
        std::lock_guard guard(Registry::lock);
        Registry::held = std::move(windows);
    }

    inline bool IsHeld(HWND window) noexcept
    {
        std::lock_guard guard(Registry::lock);
        return Registry::held.contains(window);
    }
}

namespace Arrange
{
    // Window property that another process (StageProto) sets to its process id on the windows it arranges
    inline constexpr wchar_t HandsOffProperty[] = L"Muntin.HandsOff";

    inline bool IsStage() noexcept
    {
        return FancyZonesSettings::settings().arrangeMode == 1;
    }

    // Image path whose file name is pwsh.exe or powershell.exe (case-insensitive, either slash)
    inline bool IsStageProtoImage(const std::wstring& path)
    {
        const size_t cut = path.find_last_of(L"\\/");
        const std::wstring name = cut == std::wstring::npos ? path : path.substr(cut + 1);
        return _wcsicmp(name.c_str(), L"pwsh.exe") == 0 || _wcsicmp(name.c_str(), L"powershell.exe") == 0;
    }

    inline bool ProcessIsAlive(DWORD pid) noexcept
    {
        if (pid == 0)
        {
            return false;
        }

        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
        {
            return false; // Gone, or not ours to open (not StageProto either)
        }

        DWORD code{};
        bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
        if (alive)
        {
            // A reused id must not hand off: only the shells that run StageProto count
            wchar_t path[MAX_PATH]{};
            DWORD size = ARRAYSIZE(path);
            alive = QueryFullProcessImageNameW(process, 0, path, &size) && IsStageProtoImage(path);
        }

        CloseHandle(process);
        return alive;
    }

    // The value of the property is a process id. It hands off only while that process is there: a stale id must not.
    template<typename Alive>
    bool PropertyHandsOff(HANDLE value, Alive&& alive)
    {
        const auto pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(value));
        return pid != 0 && alive(pid);
    }

    // Pure combination, for tests
    inline constexpr bool Combine(bool parked, bool stageMode, bool stageHeld, bool propertyHandsOff) noexcept
    {
        return parked || (stageMode && stageHeld) || propertyHandsOff;
    }

    // Muntin leaves this window where it is: it is parked on another desktop page, held by Stage, or arranged by another process
    inline bool HandsOff(HWND window) noexcept
    {
        if (DesktopScroll::IsParked(window))
        {
            return true;
        }

        const HANDLE property = window ? GetPropW(window, HandsOffProperty) : nullptr;
        const bool marked = property && PropertyHandsOff(property, ProcessIsAlive);
        return Combine(false, IsStage(), IsStage() && Stage::IsHeld(window), marked);
    }

    // As HandsOff, but a window Stage itself holds still counts as one to move: the glide of a stage move is Muntin's own
    inline bool HandsOffForMotion(HWND window) noexcept
    {
        if (DesktopScroll::IsParked(window))
        {
            return true;
        }

        const HANDLE property = window ? GetPropW(window, HandsOffProperty) : nullptr;
        return property && PropertyHandsOff(property, ProcessIsAlive);
    }

    // "0.5,0.3" style list of thousandths <-> shares. Anything that doesn't parse gives an empty list (equal heights).
    inline std::vector<double> ParseRows(const std::wstring& text)
    {
        std::vector<double> rows;
        size_t at = 0;
        while (at <= text.size() && !text.empty())
        {
            const size_t end = text.find(L',', at);
            const std::wstring part = text.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
            wchar_t* tail{};
            const long value = wcstol(part.c_str(), &tail, 10);
            if (part.empty() || *tail != L'\0' || value <= 0 || value > 1000)
            {
                return {};
            }

            rows.push_back(value / 1000.0);
            if (end == std::wstring::npos)
            {
                break;
            }

            at = end + 1;
        }

        return rows;
    }

    inline std::wstring FormatRows(const std::vector<double>& rows)
    {
        std::wstring text;
        for (const double row : rows)
        {
            text += (text.empty() ? L"" : L",") + std::to_wstring(static_cast<long>(row * 1000.0 + 0.5));
        }

        return text;
    }
}
