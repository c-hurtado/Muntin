#include "pch.h"

#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageConfig.h>
#include <FancyZonesLib/StageStripPlan.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pure part of the Stage strip: the tile list of a plan and what changes between two of them. No windows, no DWM.

namespace
{
    using namespace StageStripPlan;
    using StageLayouts::LRect;

    Tile T(std::uintptr_t w, int top, bool live = true) { return { w, LRect(100, top, 200, top + 50), live }; }

    Strip S(const wchar_t* key, std::vector<Tile> tiles) { return { key, LRect(90, 0, 300, 1000), std::move(tiles) }; }

    StageLayouts::Plan PlanOf(const StageLayouts::ILayout& layout, int windows)
    {
        StageLayouts::LayoutInput in;
        in.Work = LRect(0, 0, 1920, 1080);
        for (int i = 1; i <= windows; i++)
        {
            in.Wins.emplace_back(i);
        }

        return layout.Compute(in);
    }

    int StageConfig_CapacityForTest(const StageLayouts::ILayout& layout)
    {
        return StageConfig::CapacityOf(layout, LRect(0, 0, 1920, 1080), StageLayouts::Spacing::MuntinDefault());
    }

    const std::function<Source(int)> Src = [](int id) { return Source{ static_cast<std::uintptr_t>(id) * 10, true, { 7, 0, 7, 7 } }; };
}

namespace StageStripPlanUnitTests
{
    TEST_CLASS(StageStripPlanUnitTests)
    {
    public:
        TEST_METHOD(BuildTakesTheStripTilesOnly)
        {
            const auto stage = StageLayouts::MakeStage();
            const auto plan = PlanOf(*stage, 4);
            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            Assert::AreEqual(size_t{ 3 }, strip->tiles.size(), L"1 main, 3 tiles");
            Assert::AreEqual(std::uintptr_t{ 20 }, strip->tiles[0].window);
            Assert::AreEqual(std::uintptr_t{ 40 }, strip->tiles[2].window);
            Assert::IsTrue(strip->area.Same(plan.Strip));

            const auto pair = StageLayouts::MakePair();
            Assert::AreEqual(size_t{ 2 }, Build(L"m", PlanOf(*pair, 4), Src)->tiles.size(), L"2 docked, 2 tiles");
        }

        TEST_METHOD(NoStripNoTiles)
        {
            const auto stage = StageLayouts::MakeStage();
            Assert::IsFalse(Build(L"m", PlanOf(*stage, 1), Src).has_value());
            Assert::IsFalse(Build(L"m", PlanOf(*StageLayouts::MakePair(), 2), Src).has_value());
            Assert::IsFalse(Build(L"m", PlanOf(*StageLayouts::MakeMaster(), 5), Src).has_value());
            Assert::IsFalse(Build(L"m", PlanOf(*stage, 3), [](int) { return Source{}; }).has_value(), L"unknown windows: no tiles");
        }

        TEST_METHOD(BuildMarksNonLiveTiles)
        {
            const auto strip = Build(L"m", PlanOf(*StageLayouts::MakeStage(), 3), [](int id) { return Source{ static_cast<std::uintptr_t>(id) * 10, id != 2, {} }; });
            Assert::IsFalse(strip->tiles[0].live);
            Assert::IsTrue(strip->tiles[1].live);
        }

        TEST_METHOD(TilesThatWouldBeClippedAreLeftOut)
        {
            // 1080 px high: only so many tiles fit inside the usable area, the others get no tile (no scrolling yet)
            const auto stage = StageLayouts::MakeStage();
            const auto plan = PlanOf(*stage, 12);
            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            Assert::IsTrue(strip->tiles.size() < 11u && strip->tiles.size() >= 1u);
            for (const auto& t : strip->tiles)
            {
                Assert::IsTrue(t.rect.Inside(plan.Area));
            }

            // the same count the capacity rule gives
            Assert::AreEqual(strip->tiles.size() + 1, static_cast<size_t>(StageConfig_CapacityForTest(*stage)));
        }

        TEST_METHOD(BuildKeepsTheInsets)
        {
            const auto strip = Build(L"m", PlanOf(*StageLayouts::MakeStage(), 2), Src);
            Assert::IsTrue((strip->tiles[0].insets == std::array<int, 4>{ 7, 0, 7, 7 }));
            auto moved = *strip;
            moved.tiles[0].insets = { 0, 0, 0, 0 };
            Assert::AreEqual(size_t{ 1 }, DiffTiles(strip->tiles, moved.tiles).move.size(), L"new insets: the crop is applied again");
        }

        TEST_METHOD(SourceCropRemovesTheInvisibleFrame)
        {
            const auto crop = SourceCrop(1000, 600, { 7, 0, 7, 7 });
            Assert::IsTrue(crop.Same(LRect(7, 0, 993, 593)));
            Assert::IsTrue(SourceCrop(10, 10, { 5, 0, 5, 0 }).Same(LRect(0, 0, 10, 10)), L"insets that leave nothing: the whole window");
            Assert::IsTrue(SourceCrop(100, 100, { -3, 0, 0, 0 }).Same(LRect(0, 0, 100, 100)));
        }

        TEST_METHOD(StripIsSuppressedOverAMaximizedOrFullscreenWindow)
        {
            const LRect monitor(0, 0, 1920, 1080);
            Assert::IsFalse(Suppressed(false, false));
            Assert::IsTrue(Suppressed(true, false));
            Assert::IsTrue(Suppressed(false, true));
            Assert::IsTrue(CoversMonitor(LRect(-8, -8, 1928, 1088), monitor), L"a maximized frame");
            Assert::IsTrue(CoversMonitor(monitor, monitor));
            Assert::IsFalse(CoversMonitor(LRect(0, 0, 1920, 1040), monitor), L"the work area only (taskbar left)");
            Assert::IsFalse(CoversMonitor(LRect(0, 1079, 1920, 2159), monitor), L"a parked sliver");
        }

        TEST_METHOD(KeyOfIsTheMonitorRect)
        {
            Assert::AreEqual(std::wstring(L"1920,0,3840,1080"), KeyOf(LRect(1920, 0, 3840, 1080)));
        }

        TEST_METHOD(SamePictureNothingToDo)
        {
            const std::vector<Tile> a{ T(1, 0), T(2, 60) };
            Assert::IsTrue(DiffTiles(a, a).Empty());
            Assert::IsTrue(DiffStrips({ S(L"m", a) }, { S(L"m", a) }).empty());
        }

        TEST_METHOD(NewTileIsAddedGoneTileIsRemoved)
        {
            const auto d = DiffTiles({ T(1, 0), T(2, 60) }, { T(1, 0), T(3, 60) });
            Assert::AreEqual(size_t{ 1 }, d.add.size());
            Assert::AreEqual(std::uintptr_t{ 3 }, d.add[0].window);
            Assert::AreEqual(size_t{ 1 }, d.remove.size());
            Assert::AreEqual(std::uintptr_t{ 2 }, d.remove[0]);
            Assert::IsTrue(d.move.empty());
            Assert::IsFalse(d.regionChanged, L"same rects, another window in one");
        }

        TEST_METHOD(SwapMovesBothTilesWithoutNewThumbnails)
        {
            const auto d = DiffTiles({ T(1, 0), T(2, 60) }, { T(2, 0), T(1, 60) });
            Assert::IsTrue(d.add.empty());
            Assert::IsTrue(d.remove.empty());
            Assert::AreEqual(size_t{ 2 }, d.move.size());
            Assert::IsFalse(d.regionChanged);
        }

        TEST_METHOD(TilesThatBecomeNotLiveLoseTheirThumbnailAndKeepTheirTile)
        {
            const auto d = DiffTiles({ T(1, 0), T(2, 60) }, { T(1, 0), T(2, 60, false) });
            Assert::AreEqual(size_t{ 1 }, d.remove.size());
            Assert::IsTrue(d.add.empty());
            Assert::IsFalse(d.regionChanged, L"the solid tile stays");

            const auto back = DiffTiles({ T(1, 0), T(2, 60, false) }, { T(1, 0), T(2, 60) });
            Assert::AreEqual(size_t{ 1 }, back.add.size());
            Assert::IsTrue(back.remove.empty());
        }

        TEST_METHOD(NotLiveTilesNeverGetAThumbnail)
        {
            const auto d = DiffTiles({}, { T(1, 0, false) });
            Assert::IsTrue(d.add.empty() && d.remove.empty() && d.move.empty());
            Assert::IsTrue(d.regionChanged);
            Assert::IsTrue(DiffTiles({ T(1, 0, false) }, {}).remove.empty());
        }

        TEST_METHOD(RectChangeMovesAndRebuildsTheRegion)
        {
            const auto d = DiffTiles({ T(1, 0) }, { T(1, 10) });
            Assert::AreEqual(size_t{ 1 }, d.move.size());
            Assert::IsTrue(d.regionChanged);
            Assert::IsTrue(d.add.empty() && d.remove.empty());
        }

        TEST_METHOD(StripsCreateUpdateDestroy)
        {
            const auto changes = DiffStrips({ S(L"a", { T(1, 0) }), S(L"b", { T(2, 0) }) }, { S(L"b", { T(2, 0), T(3, 60) }), S(L"c", { T(4, 0) }) });
            Assert::AreEqual(size_t{ 3 }, changes.size());
            Assert::IsTrue(Kind::Destroy == changes[0].kind, L"destroys first");
            Assert::AreEqual(std::wstring(L"a"), changes[0].key);
            Assert::AreEqual(size_t{ 1 }, changes[0].tiles.remove.size());
            Assert::IsTrue(Kind::Update == changes[1].kind);
            Assert::AreEqual(size_t{ 1 }, changes[1].tiles.add.size());
            Assert::IsTrue(Kind::Create == changes[2].kind);
            Assert::IsTrue(changes[2].tiles.regionChanged);
            Assert::AreEqual(size_t{ 1 }, changes[2].tiles.add.size());
        }

        TEST_METHOD(WindowMovedToAnotherMonitorIsRemovedBeforeItIsAdded)
        {
            const auto changes = DiffStrips({ S(L"a", { T(1, 0), T(2, 60) }), S(L"b", { T(3, 0) }) }, { S(L"a", { T(1, 0) }), S(L"b", { T(3, 0), T(2, 60) }) });
            Assert::AreEqual(size_t{ 2 }, changes.size());
            Assert::AreEqual(std::wstring(L"a"), changes[0].key);
            Assert::AreEqual(std::uintptr_t{ 2 }, changes[0].tiles.remove[0]);
            Assert::AreEqual(std::uintptr_t{ 2 }, changes[1].tiles.add[0].window);
        }

        TEST_METHOD(AreaChangeIsAnUpdate)
        {
            auto moved = S(L"m", { T(1, 0) });
            moved.area = LRect(0, 0, 10, 10);
            const auto changes = DiffStrips({ S(L"m", { T(1, 0) }) }, { moved });
            Assert::AreEqual(size_t{ 1 }, changes.size());
            Assert::IsTrue(changes[0].areaChanged);
            Assert::IsTrue(Kind::Update == changes[0].kind);
        }

        TEST_METHOD(HitTileFindsTheTileUnderThePoint)
        {
            const std::vector<Tile> tiles{ T(1, 0), T(2, 60) };
            Assert::AreEqual(std::uintptr_t{ 1 }, HitTile(tiles, 100, 0));
            Assert::AreEqual(std::uintptr_t{ 2 }, HitTile(tiles, 150, 109));
            Assert::AreEqual(std::uintptr_t{ 0 }, HitTile(tiles, 150, 55), L"the gap: clicks go through");
            Assert::AreEqual(std::uintptr_t{ 0 }, HitTile(tiles, 200, 10), L"right edge is outside");
        }

        TEST_METHOD(ClickPromotesAStripTile)
        {
            // What StripClick relies on: the stage layout promotes a tile to the main slot
            const auto stage = StageLayouts::MakeStage();
            std::vector<int> order{ 1, 2, 3, 4 };
            Assert::IsTrue(stage->PromoteHidden(order, 4));
            Assert::IsTrue((order == std::vector<int>{ 4, 1, 2, 3 }));
            Assert::IsTrue(StageLayouts::Role::Main == PlanOf(*stage, 4).Items[0].Role);
        }
    };
}
