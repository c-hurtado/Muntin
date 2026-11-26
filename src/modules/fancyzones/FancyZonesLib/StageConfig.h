#pragma once

// Stage mode choices that survive a restart (layout, cuts, master row heights): pure helpers between the settings values and the
// layout objects. Nothing here reads or writes a file; StageMode saves what Changes() returns.

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <FancyZonesLib/Arrange.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/SettingsConstants.h>
#include <FancyZonesLib/StageLayouts.h>

namespace StageConfig
{
    struct Values
    {
        std::wstring layout = L"master"; // stage, pair or master. Not Settings' fresh-install "stage" on purpose: master is the
                                         // fallback of an install from before (ArrangeFallback in Settings.h), as in EffectiveLayoutName
        int pairCut = 500; // thousandths, 150 .. 850
        int masterCut = 600;
        std::wstring rows; // master stack height shares, thousandths, comma separated; empty = equal
        int stripCut = 150; // strip tile width, thousandths of the usable width, 100 .. 400 (per monitor, no global setting)

        bool operator==(const Values&) const = default;
    };

    inline int ToStripPermille(double share)
    {
        return (std::max)(100, (std::min)(400, static_cast<int>(std::lround(share * 1000.0))));
    }

    inline int ToPermille(double ratio)
    {
        return (std::max)(150, (std::min)(850, static_cast<int>(std::lround(ratio * 1000.0))));
    }

    inline Values FromSettings(const Settings& s)
    {
        return { s.stageLayout, s.stagePairCut, s.stageMasterCut, s.stageRows };
    }

    // Cut and rows are per monitor; the layout choice stays global. The three global values are the default of a monitor without an entry.
    //
    // Monitor identity: the display device id and instance of the monitor (the 'DELA0B1' and '5&2d1c6b3a&0&UID4352' parts of its device
    // path, the same pair the dock layouts and applied layouts key on). It survives a reboot, the display number Windows renumbers, the
    // order the monitors are enumerated in, and unplug / replug into the same output. It changes when the monitor moves to another GPU
    // output (a new instance), which then starts from the global values. Serial numbers (WMI) are left out on purpose: they would make
    // every display change a slow, retrying lookup on the main thread. When no device id is known (the lookup failed, the id is only
    // a \.\DISPLAYn name) the key is "@left,top" of the monitor rect, the best that is left.
    inline std::wstring MonitorKey(const std::wstring& deviceId, const std::wstring& instanceId, int left, int top)
    {
        if (deviceId.empty() || instanceId.empty())
        {
            return L"@" + std::to_wstring(left) + L"," + std::to_wstring(top);
        }

        return deviceId + L"_" + instanceId;
    }

    // The key of a monitor from what the display lookup said. A monitor the lookup could not identify (not active yet after a wake or
    // hot-plug) keeps the key it had for the same rect (previous, if it was a real one); failing that it gets the rect origin and
    // resolved = false: the caller looks again shortly instead of saving under a key that will change.
    struct KeyChoice
    {
        std::wstring key;
        bool resolved{};
    };

    inline KeyChoice ChooseKey(const std::wstring& deviceId, const std::wstring& instanceId, int left, int top, const std::wstring& previous)
    {
        if (!deviceId.empty() && !instanceId.empty())
        {
            return { MonitorKey(deviceId, instanceId, left, top), true };
        }

        if (!previous.empty() && previous.front() != L'@')
        {
            return { previous, true };
        }

        return { MonitorKey(deviceId, instanceId, left, top), false };
    }

    // The values of the monitor: the global ones, the fields of its entry on top. An unknown or stale key is the global values.
    inline Values Resolve(const Values& global, const std::map<std::wstring, StageMonitorValues>& monitors, const std::wstring& key)
    {
        Values values = global;
        const auto entry = monitors.find(key);
        if (entry != monitors.end())
        {
            if (entry->second.pairCut)
            {
                values.pairCut = (std::max)(150, (std::min)(850, *entry->second.pairCut));
            }

            if (entry->second.masterCut)
            {
                values.masterCut = (std::max)(150, (std::min)(850, *entry->second.masterCut));
            }

            if (entry->second.rows)
            {
                values.rows = *entry->second.rows;
            }

            if (entry->second.stripCut)
            {
                values.stripCut = (std::max)(100, (std::min)(400, *entry->second.stripCut));
            }
        }

        return values;
    }

    // What differs between the values a monitor had and the ones it has now: the fields to save to its entry (empty: nothing to save)
    inline StageMonitorValues MonitorChange(const Values& before, const Values& after)
    {
        StageMonitorValues change;
        if (before.pairCut != after.pairCut)
        {
            change.pairCut = after.pairCut;
        }

        if (before.masterCut != after.masterCut)
        {
            change.masterCut = after.masterCut;
        }

        if (before.rows != after.rows)
        {
            change.rows = after.rows;
        }

        if (before.stripCut != after.stripCut)
        {
            change.stripCut = after.stripCut;
        }

        return change;
    }

    // The set fields of change go into the entry of key; the other fields and monitors stay (what SaveStageMonitor does in the file)
    inline void Merge(std::map<std::wstring, StageMonitorValues>& monitors, const std::wstring& key, const StageMonitorValues& change)
    {
        if (change.Empty())
        {
            return;
        }

        monitors[key].Overlay(change);
    }

    inline std::string Narrow(const std::wstring& name)
    {
        std::string narrow;
        for (const wchar_t c : name)
        {
            narrow += c < 128 ? static_cast<char>(c) : '?'; // layout names are ASCII
        }

        return narrow;
    }

    // The layout that runs: the saved choice (stage, pair or master); a name nobody knows is the master layout (the behaviour before
    // the strip existed: nobody gets the strip until they choose it).
    inline std::string EffectiveLayoutName(const std::wstring& saved)
    {
        const auto name = Narrow(saved);
        const auto& names = StageLayouts::LayoutNames();
        return std::find(names.begin(), names.end(), name) != names.end() ? name : std::string("master");
    }

    // Windows in the master's stack (the layout allows 1 .. 8). The master layout has no strip: a window beyond what it shows is
    // never taken (left alone, not parked). The stage and pair layouts show every window beyond their slots as a strip tile.
    constexpr int MasterMaxStack = 8;

    // Windows one monitor's stage takes in a layout with a strip (which scrolls): effectively all.
    constexpr int StripMaxWindows = 1000;

    // How many windows one monitor's stage can show: master + stack; for a layout with a strip, StripMaxWindows (the strip scrolls),
    // unless not even one tile fits the screen (then only its docked slots: a window without a tile would have no mouse path back).
    inline int CapacityOf(const StageLayouts::ILayout& layout, const StageLayouts::LRect& work, const StageLayouts::Spacing& spacing)
    {
        constexpr int many = 1000;
        const int slots = layout.Slots(many);
        if (!layout.HasStrip(many))
        {
            return slots;
        }

        StageLayouts::LayoutInput input;
        input.Work = work;
        input.Spacing = spacing;
        for (int i = 1; i <= slots + 1; i++)
        {
            input.Wins.emplace_back(i);
        }

        const auto plan = layout.Compute(input);
        if (!plan.HasStrip || plan.TileH <= 0)
        {
            return slots;
        }

        // The strip scrolls: every window gets a tile, as long as at least one tile fits on the screen
        // A tile taller than the area (a wide strip beside the narrowed left window in pair) is cut at the area's edge, not a reason to
        // refuse windows: the layout's tile size itself stays as the golden vectors have it
        const int tileH = (std::min)(plan.TileH, plan.Area.H());
        const int fit = (std::max)(0, (plan.Area.H() + plan.Gap) / (tileH + plan.Gap));
        return fit > 0 ? StripMaxWindows : slots;
    }

    inline bool HasRoom(size_t managed, int capacity)
    {
        return managed < static_cast<size_t>((std::max)(capacity, 0));
    }

    // The windows a stage cannot keep when its capacity shrinks (another layout): the ones last in the layout order. They are released,
    // not parked hidden without a tile.
    inline std::vector<int> OverCapacity(const std::vector<int>& order, int capacity)
    {
        const size_t keep = static_cast<size_t>((std::max)(capacity, 0));
        if (order.size() <= keep)
        {
            return {};
        }

        return std::vector<int>(order.begin() + static_cast<std::ptrdiff_t>(keep), order.end());
    }

    // The layout object for the values, with its cut and rows set
    inline std::unique_ptr<StageLayouts::ILayout> MakeLayout(const Values& v)
    {
        std::unique_ptr<StageLayouts::ILayout> layout;
        if (EffectiveLayoutName(v.layout) == "master")
        {
            auto master = std::make_unique<StageLayouts::MasterLayout>();
            master->MaxStack = MasterMaxStack;
            layout = std::move(master);
        }
        else
        {
            layout = StageLayouts::MakeLayout(EffectiveLayoutName(v.layout));
        }

        if (layout->HasCut())
        {
            layout->SetCut((layout->Name() == "pair" ? v.pairCut : v.masterCut) / 1000.0);
        }

        layout->SetStripShare(v.stripCut / 1000.0);
        layout->SetRows(Arrange::ParseRows(v.rows));
        return layout;
    }

    // The values after the user changed the layout object (cut dragged, rows moved): only what that layout owns is taken over,
    // the other layout's cut stays as it was. The layout name is the effective one only when it is the one running.
    inline Values Read(const StageLayouts::ILayout& layout, Values base)
    {
        if (layout.HasCut())
        {
            (layout.Name() == "pair" ? base.pairCut : base.masterCut) = ToPermille(layout.Cut());
        }

        if (layout.HasStrip(StripMaxWindows)) // only a layout with a strip owns its width
        {
            base.stripCut = ToStripPermille(layout.StripShare());
        }

        if (layout.Name() == "master")
        {
            base.rows = Arrange::FormatRows(layout.Rows()); // Only the master layout owns rows: a pair / stage drag leaves them as they are
        }

        return base;
    }

    // One setting to write
    struct Change
    {
        const wchar_t* id{};
        bool text{};
        std::wstring textValue;
        int number{};
    };

    inline std::vector<Change> Changes(const Values& before, const Values& after)
    {
        std::vector<Change> changes;
        if (before.layout != after.layout)
        {
            changes.push_back({ NonLocalizable::StageLayoutID, true, after.layout, 0 });
        }

        if (before.pairCut != after.pairCut)
        {
            changes.push_back({ NonLocalizable::StagePairCutID, false, {}, after.pairCut });
        }

        if (before.masterCut != after.masterCut)
        {
            changes.push_back({ NonLocalizable::StageMasterCutID, false, {}, after.masterCut });
        }

        if (before.rows != after.rows)
        {
            changes.push_back({ NonLocalizable::StageRowsID, true, after.rows, 0 });
        }

        return changes;
    }
}
