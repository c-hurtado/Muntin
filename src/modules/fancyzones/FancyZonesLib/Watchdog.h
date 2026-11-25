#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

// Muntin.exe --watchdog <process id> <creation time of that process, as the 64 bits of a FILETIME>
// The creation time tells the watchdog that the process is still the Muntin that started it, and not another that got the id.
namespace Watchdog
{
    struct Parent
    {
        unsigned long process{};
        unsigned long long created{};
    };

    inline std::optional<unsigned long long> ParseNumber(const wchar_t* text) noexcept
    {
        if (!text || !*text || *text == L'-' || *text == L'+' || *text == L' ')
        {
            return std::nullopt;
        }

        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long value = wcstoull(text, &end, 10);
        return (errno != 0 || *end != L'\0') ? std::nullopt : std::optional{ value };
    }

    // argv as of CommandLineToArgvW, with the program first
    inline std::optional<Parent> ParseCommandLine(int count, const wchar_t* const* args) noexcept
    {
        if (count != 4 || std::wstring{ args[1] } != L"--watchdog")
        {
            return std::nullopt;
        }

        const auto process = ParseNumber(args[2]);
        const auto created = ParseNumber(args[3]);
        if (!process || !created || *process == 0 || *process > 0xFFFFFFFFull)
        {
            return std::nullopt;
        }

        return Parent{ static_cast<unsigned long>(*process), *created };
    }

    // How Muntin starts its watchdog, best first. taskkill /T (and anything else that ends "the tree of Muntin") follows the parent
    // process id: a watchdog whose parent is the shell is not in that tree, so it survives and puts the windows back. A child takes the
    // token of the parent it is given, so an elevated Muntin does not use the shell as parent (the watchdog would not be allowed to
    // move elevated windows). Breakaway keeps it out of a job that ends with Muntin; a job that forbids breakaway makes the second try.
    struct SpawnAttempt
    {
        bool shellParent{};
        bool breakaway{};
    };

    inline std::vector<SpawnAttempt> SpawnAttempts(bool shellAvailable, bool elevated)
    {
        std::vector<SpawnAttempt> attempts;
        if (shellAvailable && !elevated)
        {
            attempts.push_back({ true, true });
            attempts.push_back({ true, false });
        }

        attempts.push_back({ false, true });
        attempts.push_back({ false, false });
        return attempts;
    }
}

