#pragma once

// Stage mode and the monitors, pure (Phase 6). After a display change (resolution or scale, unplug / replug, sleep / wake) each stage
// has to find its monitor again. Matching by rect alone loses the stage (and its cut, rows and window order) whenever the resolution
// changes; the stable monitor key (StageConfig::MonitorKey) survives that. A monitor that is gone hands its windows to the primary
// stage, remembers who they were (Exiles), and gets them back when a monitor with that key returns.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <FancyZonesLib/StageLayouts.h>

namespace StageMonitors
{
    using StageLayouts::LRect;

    struct Id
    {
        std::wstring key;
        LRect rect;
    };

    // "@left,top": the monitor could not be identified, its key is only its place (StageConfig::MonitorKey)
    inline bool IsPlaceKey(const std::wstring& key)
    {
        return key.empty() || key.front() == L'@';
    }

    // For each monitor in `next`: the index of the stage in `old` that belongs to it, or -1 (a new stage).
    // 1 the same real key (a resolution or scale change keeps the stage, wherever the monitor sits now)
    // 2 the same rect (another monitor took the place of the one that went: the windows are on it already)
    // 3 the same origin, when one of the two keys is only a place (a monitor that was not identified yet)
    inline std::vector<int> MatchStages(const std::vector<Id>& old, const std::vector<Id>& next)
    {
        std::vector<int> match(next.size(), -1);
        std::vector<bool> used(old.size(), false);
        const auto take = [&](auto&& same) {
            for (size_t n = 0; n < next.size(); n++)
            {
                if (match[n] >= 0)
                {
                    continue;
                }

                for (size_t o = 0; o < old.size(); o++)
                {
                    if (!used[o] && same(old[o], next[n]))
                    {
                        match[n] = static_cast<int>(o);
                        used[o] = true;
                        break;
                    }
                }
            }
        };

        take([](const Id& a, const Id& b) { return !IsPlaceKey(a.key) && a.key == b.key; });
        take([](const Id& a, const Id& b) { return a.rect.Same(b.rect); });
        take([](const Id& a, const Id& b) { return (IsPlaceKey(a.key) || IsPlaceKey(b.key)) && a.rect.L == b.rect.L && a.rect.T == b.rect.T; });
        return match;
    }

    // The stages of `old` that no monitor claimed
    inline std::vector<int> GoneStages(size_t oldCount, const std::vector<int>& match)
    {
        std::vector<bool> used(oldCount, false);
        for (const int o : match)
        {
            if (o >= 0 && static_cast<size_t>(o) < oldCount)
            {
                used[static_cast<size_t>(o)] = true;
            }
        }

        std::vector<int> gone;
        for (size_t o = 0; o < oldCount; o++)
        {
            if (!used[o])
            {
                gone.push_back(static_cast<int>(o));
            }
        }

        return gone;
    }

    // Window (HWND value, stable across a release and a re-adoption: a window cut by minimum sizes keeps its record) -> key of the
    // monitor it was sent away from
    using Exiles = std::map<std::uintptr_t, std::wstring>;

    // The windows of a monitor that went away: remembered (the first monitor wins: a window that was exiled twice goes back to the
    // first one), unless the monitor had no real key (a place only: it cannot be recognized later).
    inline void Exile(Exiles& exiles, const std::vector<std::uintptr_t>& ids, const std::wstring& key)
    {
        if (IsPlaceKey(key))
        {
            return;
        }

        for (const auto id : ids)
        {
            exiles.emplace(id, key);
        }
    }

    // The windows to give back to a monitor with this key. They leave the book (the caller moves them, or they are no longer there).
    inline std::vector<std::uintptr_t> TakeReturning(Exiles& exiles, const std::wstring& key)
    {
        std::vector<std::uintptr_t> back;
        if (IsPlaceKey(key))
        {
            return back;
        }

        for (auto it = exiles.begin(); it != exiles.end();)
        {
            if (it->second == key)
            {
                back.push_back(it->first);
                it = exiles.erase(it);
            }
            else
            {
                ++it;
            }
        }

        return back;
    }

    // The scale or size of a monitor changed: what windows reported as their minimum size (physical pixels of the old scale) is stale
    inline bool SizeChanged(const LRect& before, const LRect& after, unsigned dpiBefore = 0, unsigned dpiAfter = 0)
    {
        return before.W() != after.W() || before.H() != after.H() || (dpiBefore != 0 && dpiAfter != 0 && dpiBefore != dpiAfter);
    }

    // A window that is adopted (or adopted again after a cut) while its monitor is back: the index of the stage with the key it was
    // exiled from, and the record is spent. -1: no record, or that monitor is still away (the record stays).
    inline int ReturnTarget(Exiles& exiles, std::uintptr_t window, const std::vector<std::wstring>& stageKeys)
    {
        const auto record = exiles.find(window);
        if (record == exiles.end() || IsPlaceKey(record->second))
        {
            return -1;
        }

        for (size_t i = 0; i < stageKeys.size(); i++)
        {
            if (stageKeys[i] == record->second)
            {
                exiles.erase(record);
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    // The key a monitor may inherit from the monitors of the last look when the display lookup could not identify it: the key of the one
    // with the same rect; the one with the same origin only when its key is itself only a place. A real device key is never handed to
    // another monitor just because it sits at the same corner (an undock / wake can put a different monitor at 0,0).
    inline std::wstring InheritKey(const std::vector<Id>& previous, const LRect& rect)
    {
        for (const auto& p : previous)
        {
            if (p.rect.Same(rect))
            {
                return p.key;
            }
        }

        for (const auto& p : previous)
        {
            if (p.rect.L == rect.L && p.rect.T == rect.T && IsPlaceKey(p.key))
            {
                return p.key;
            }
        }

        return {};
    }
}

