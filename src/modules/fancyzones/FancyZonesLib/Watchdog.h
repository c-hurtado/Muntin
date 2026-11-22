#pragma once

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

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
}
