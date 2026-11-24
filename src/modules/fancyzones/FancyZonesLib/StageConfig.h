#pragma once

// Stage mode choices that survive a restart (layout, cuts, master row heights): pure helpers between the settings values and the
// layout objects. Nothing here reads or writes a file; StageMode saves what Changes() returns.

#include <limits>
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
        std::wstring layout = L"stage"; // stage, pair or master
        int pairCut = 500; // thousandths, 150 .. 850
        int masterCut = 600;
        std::wstring rows; // master stack height shares, thousandths, comma separated; empty = equal

        bool operator==(const Values&) const = default;
    };

    inline int ToPermille(double ratio)
    {
        return (std::max)(150, (std::min)(850, static_cast<int>(std::lround(ratio * 1000.0))));
    }

    inline Values FromSettings(const Settings& s)
    {
        return { s.stageLayout, s.stagePairCut, s.stageMasterCut, s.stageRows };
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

    // Phase 3 draws no strip: only the master layout can show every window it manages, so that is what runs, whatever the
    // setting says (the setting itself is kept as it is and takes effect once the strip is drawn, Phase 4).
    inline std::string EffectiveLayoutName(const std::wstring&)
    {
        return "master";
    }

    // Windows in the master's stack (the layout allows 1 .. 8). Phase 3 has no strip and no way to bring a hidden window back
    // (rotate / promote come with Phase 5), so a window beyond what is shown is never taken: it is left alone, not parked.
    constexpr int MasterMaxStack = 8;

    // How many windows one monitor's stage can show: master + stack. A layout with a strip shows every window as a tile.
    inline int CapacityOf(const StageLayouts::ILayout& layout)
    {
        constexpr int many = 1000;
        return layout.HasStrip(many) ? (std::numeric_limits<int>::max)() : layout.Slots(many);
    }

    inline bool HasRoom(size_t managed, int capacity)
    {
        return managed < static_cast<size_t>((std::max)(capacity, 0));
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

        base.rows = Arrange::FormatRows(layout.Rows());
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
