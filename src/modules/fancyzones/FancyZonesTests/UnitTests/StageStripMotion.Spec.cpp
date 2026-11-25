#include "pch.h"

#include <FancyZonesLib/StageConfig.h>
#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageStripMotion.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pure part of the strip polish: scroll offset and projection, tile motion, hover pad, corner radius. No windows.

namespace
{
    using namespace StageStripMotion;
    using StageLayouts::LRect;
    using StageStripPlan::Strip;
    using StageStripPlan::Tile;

    constexpr double Linear(double p) { return p; }

    // fit slots of 50 px from y=0, total tiles (the ones beyond fit lie below the slots)
    Strip Full(int total, int fit)
    {
        Strip s;
        s.key = L"m";
        s.area = LRect(0, 0, 100, 1000);
        s.fit = fit;
        for (int i = 0; i < total; i++)
        {
            s.tiles.push_back({ std::uintptr_t{ 100 } + static_cast<std::uintptr_t>(i), LRect(10, i * 60, 90, i * 60 + 50), true, {} });
        }

        return s;
    }
}

namespace StageStripMotionUnitTests
{
    TEST_CLASS(StageStripMotionUnitTests)
    {
    public:
        TEST_METHOD(OffsetIsClampedToTheScrollableRange)
        {
            Assert::AreEqual(0, MaxOffset(3, 5), L"everything fits: no scrolling");
            Assert::AreEqual(7, MaxOffset(12, 5));
            Assert::AreEqual(0, MaxOffset(0, 0));
            Assert::AreEqual(0, ClampOffset(-4, 12, 5));
            Assert::AreEqual(7, ClampOffset(99, 12, 5));
            Assert::AreEqual(3, ClampOffset(3, 12, 5));
            Assert::AreEqual(0, ClampOffset(2, 3, 5), L"windows removed: back in range");
        }

        TEST_METHOD(WheelNotchesScrollOneTileAndTheRestIsKept)
        {
            int acc = 0;
            Assert::AreEqual(1, WheelSteps(acc, -120), L"wheel down: later tiles");
            Assert::AreEqual(-1, WheelSteps(acc, 120));
            Assert::AreEqual(0, WheelSteps(acc, -40));
            Assert::AreEqual(0, WheelSteps(acc, -40));
            Assert::AreEqual(1, WheelSteps(acc, -40), L"small deltas add up to a notch");
            Assert::AreEqual(0, acc);
            Assert::AreEqual(2, WheelSteps(acc, -240));
        }

        TEST_METHOD(ProjectShowsFitTilesFromTheOffsetInTheSlots)
        {
            const auto full = Full(8, 3);
            auto shown = Project(full, 0);
            Assert::AreEqual(size_t{ 3 }, shown.tiles.size());
            Assert::AreEqual(std::uintptr_t{ 100 }, shown.tiles[0].window);

            shown = Project(full, 2);
            Assert::AreEqual(size_t{ 3 }, shown.tiles.size());
            Assert::AreEqual(std::uintptr_t{ 102 }, shown.tiles[0].window);
            Assert::AreEqual(std::uintptr_t{ 104 }, shown.tiles[2].window);
            for (size_t i = 0; i < 3; i++)
            {
                Assert::IsTrue(shown.tiles[i].rect.Same(full.tiles[i].rect), L"each tile sits in the slot of its position");
            }

            shown = Project(full, 50);
            Assert::AreEqual(std::uintptr_t{ 105 }, shown.tiles[0].window, L"offset clamped: last window at the end");
            Assert::AreEqual(size_t{ 3 }, shown.tiles.size());
        }

        TEST_METHOD(ProjectWithFewerTilesThanSlots)
        {
            const auto shown = Project(Full(2, 5), 3);
            Assert::AreEqual(size_t{ 2 }, shown.tiles.size());
            Assert::AreEqual(std::uintptr_t{ 100 }, shown.tiles[0].window);
            Assert::AreEqual(size_t{ 0 }, Project(Full(0, 3), 0).tiles.size());
        }

        TEST_METHOD(ScrollingMovesTilesAndAddsAndRemovesTheEnds)
        {
            const auto full = Full(6, 3);
            const auto diff = StageStripPlan::DiffTiles(Project(full, 0).tiles, Project(full, 1).tiles);
            Assert::AreEqual(size_t{ 1 }, diff.remove.size(), L"the first one scrolled out");
            Assert::AreEqual(size_t{ 1 }, diff.add.size(), L"a new one scrolled in");
            Assert::AreEqual(size_t{ 2 }, diff.move.size(), L"the two that stay move up a slot");
        }

        TEST_METHOD(LerpIsExactAtTheEndsAndRounds)
        {
            const LRect a(0, 0, 100, 50), b(100, 40, 300, 90);
            Assert::IsTrue(Lerp(a, b, 0).Same(a));
            Assert::IsTrue(Lerp(a, b, 1).Same(b));
            Assert::IsTrue(Lerp(a, b, 0.5).Same(LRect(50, 20, 200, 70)));
        }

        TEST_METHOD(MotionEasesToItsTargetAndStops)
        {
            const LRect a(0, 0, 100, 50), b(200, 0, 300, 50);
            auto m = Motion::At(a, 1000);
            Assert::IsFalse(m.Active());
            Assert::IsFalse(Step(m, 1100, 300, Linear), L"idle: nothing to do");

            Retarget(m, b, 1000);
            Assert::IsTrue(m.Active());
            Assert::IsTrue(Step(m, 1150, 300, Linear));
            Assert::AreEqual(100, m.cur.L);
            Assert::IsTrue(m.Active());
            Assert::IsTrue(Step(m, 1300, 300, Linear));
            Assert::IsTrue(m.cur.Same(b));
            Assert::IsFalse(m.Active(), L"done: the timer can stop");
            Assert::IsFalse(Step(m, 1400, 300, Linear));
        }

        TEST_METHOD(RetargetContinuesFromWhereTheTileIs)
        {
            auto m = Motion::At(LRect(0, 0, 100, 50), 0);
            Retarget(m, LRect(200, 0, 300, 50), 0);
            Step(m, 150, 300, Linear);
            Assert::AreEqual(100, m.cur.L);
            Retarget(m, LRect(200, 0, 300, 50), 160);
            Assert::AreEqual(std::uint64_t{ 0 }, m.start, L"the same target leaves the ease alone");
            Retarget(m, LRect(0, 100, 100, 150), 200);
            Assert::AreEqual(100, m.from.L, L"new ease starts from the drawn rect");
            Assert::AreEqual(std::uint64_t{ 200 }, m.start);
            Step(m, 200, 300, Linear);
            Assert::AreEqual(100, m.cur.L, L"no jump at the start");
            Step(m, 500, 300, Linear);
            Assert::IsTrue(m.cur.Same(LRect(0, 100, 100, 150)));
        }

        TEST_METHOD(ZeroDurationSnaps)
        {
            auto m = Motion::At(LRect(0, 0, 10, 10), 5);
            Retarget(m, LRect(50, 0, 60, 10), 5);
            Assert::IsTrue(Step(m, 5, 0, Linear));
            Assert::IsTrue(m.cur.Same(m.to));
        }

        TEST_METHOD(HoverPadAndRadius)
        {
            Assert::IsTrue(PadOf(true) < PadOf(false), L"hovered: the thumbnail comes closer to the edge");
            Assert::IsTrue(Deflate(LRect(0, 0, 100, 50), 3).Same(LRect(3, 3, 97, 47)));
            Assert::IsTrue(Deflate(LRect(0, 0, 4, 4), 10).W() >= 1, L"never inverted");
            Assert::IsTrue(Deflate(LRect(0, 0, 100, 50), -2).Same(LRect(0, 0, 100, 50)));
            Assert::AreEqual(6, ClampRadius(6, 100, 50));
            Assert::AreEqual(5, ClampRadius(6, 10, 100));
            Assert::AreEqual(0, ClampRadius(6, 1, 1));
            Assert::AreEqual(0, ClampRadius(-3, 100, 100));
        }

        TEST_METHOD(HitTestUsesTheDrawnRectDuringAnimation)
        {
            const auto shown = Project(Full(6, 3), 1); // tile 101 targets slot 0 (y 0..50)
            std::map<std::uintptr_t, Motion> motion;
            for (const auto& t : shown.tiles)
            {
                motion[t.window] = Motion::At(t.rect, 0);
            }

            // 101 is still drawn in slot 1 (y 60..110) on its way to slot 0; 102 is drawn in slot 2
            motion[101] = Motion::At(LRect(10, 60, 90, 110), 0);
            motion[101].to = shown.tiles[0].rect;
            Assert::AreEqual(std::uintptr_t{ 101 }, HitDrawn(shown.tiles, motion, 50, 80), L"the tile drawn under the cursor, not the one arriving");
            Assert::AreEqual(std::uintptr_t{ 0 }, HitDrawn(shown.tiles, motion, 50, 20), L"its final slot is empty meanwhile");
            Assert::AreEqual(std::uintptr_t{ 0 }, HitDrawn(shown.tiles, {}, 50, 400));
            Assert::AreEqual(std::uintptr_t{ 101 }, HitDrawn(shown.tiles, {}, 50, 20), L"no motion: the plan rect");
        }

        TEST_METHOD(OffsetIsRememberedPerMonitorAndLayout)
        {
            OffsetMemory memory;
            Assert::AreEqual(0, memory.Recall(L"m", "stage", 12, 5));
            memory.Remember(L"m", "stage", 4);
            Assert::AreEqual(4, memory.Recall(L"m", "stage", 12, 5), L"the strip was hidden and shows again: same place");
            Assert::AreEqual(0, memory.Recall(L"m", "pair", 12, 5), L"another layout starts at the top");
            Assert::AreEqual(0, memory.Recall(L"other", "stage", 12, 5), L"another monitor");
            Assert::AreEqual(2, memory.Recall(L"m", "stage", 7, 5), L"fewer windows meanwhile: clamped");
            memory.Remember(L"m", "pair", 1);
            Assert::AreEqual(4, memory.Recall(L"m", "stage", 12, 5), L"back to the first layout: still there");
        }

        TEST_METHOD(WindowsOverTheNewCapacityAreTheLastInOrder)
        {
            const std::vector<int> order{ 5, 3, 9, 1, 7 };
            Assert::IsTrue(StageConfig::OverCapacity(order, 9).empty());
            Assert::IsTrue(StageConfig::OverCapacity(order, 5).empty());
            Assert::IsTrue((StageConfig::OverCapacity(order, 3) == std::vector<int>{ 1, 7 }));
            Assert::IsTrue((StageConfig::OverCapacity(order, 0) == order));
            Assert::IsTrue(StageConfig::OverCapacity({}, 3).empty());

            // strip layout -> master: 40 windows, capacity 9
            std::vector<int> many(40);
            for (int i = 0; i < 40; i++) many[static_cast<size_t>(i)] = i;
            StageConfig::Values values;
            values.layout = L"master";
            const auto master = StageConfig::MakeLayout(values);
            const int cap = StageConfig::CapacityOf(*master, LRect(0, 0, 1920, 1080), StageLayouts::Spacing::MuntinDefault());
            Assert::AreEqual(size_t{ 31 }, StageConfig::OverCapacity(many, cap).size());
        }

        TEST_METHOD(CropIsNotSentWhenTheSourceSizeIsUnknown)
        {
            const std::array<int, 4> insets{ 7, 0, 7, 7 };
            Assert::IsFalse(StageStripPlan::CropFor(0, 0, insets).has_value(), L"query failed or zero: no rcSource is sent (DWM keeps the last one)");
            Assert::IsFalse(StageStripPlan::CropFor(500, 0, insets).has_value());
            Assert::IsFalse(StageStripPlan::CropFor(-1, 300, insets).has_value());
            const auto crop = StageStripPlan::CropFor(1000, 600, insets);
            Assert::IsTrue(crop.has_value() && crop->Same(LRect(7, 0, 993, 593)));
            Assert::IsTrue(StageStripPlan::CropFor(10, 10, { 5, 0, 5, 0 })->Same(LRect(0, 0, 10, 10)), L"known size, insets that leave nothing: the whole source as a valid rect");
            Assert::IsTrue(StageStripPlan::CropFor(300, 200, { 0, 0, 0, 0 })->Same(LRect(0, 0, 300, 200)), L"a valid crop always replaces an older rcSource");
        }

        TEST_METHOD(SettleReappliesWhenTheSourceChangedOrIsUnknownOrForced)
        {
            Assert::IsFalse(ShouldReplace(800, 600, 800, 600, 1), L"same size, not the forced step");
            Assert::IsTrue(ShouldReplace(800, 600, 1920, 1040, 1), L"the window was resized after the crop was computed");
            Assert::IsTrue(ShouldReplace(0, 0, 800, 600, 1), L"source size unknown");
            Assert::IsTrue(ShouldReplace(800, 600, 0, 0, 3), L"never applied with a known size");
            Assert::IsTrue(ShouldReplace(800, 600, 800, 600, ForcedStep), L"forced re-apply once the windows have settled");
        }

        TEST_METHOD(TileSettledNeedsRegistrationLivenessAndAStableSize)
        {
            Assert::IsTrue(TileSettled(true, true, 800, 600, 800, 600));
            Assert::IsFalse(TileSettled(false, true, 800, 600, 800, 600), L"no thumbnail");
            Assert::IsFalse(TileSettled(true, false, 800, 600, 800, 600), L"not live");
            Assert::IsFalse(TileSettled(true, true, 0, 0, 0, 0), L"empty source");
            Assert::IsFalse(TileSettled(true, true, 800, 600, 700, 600), L"size changed since the last look");
        }

        TEST_METHOD(SettleLogVolumeIsBounded)
        {
            int logged = 0;
            for (int step = 1; step <= MaxSteps; step++)
            {
                logged += ShouldLogStep(step) ? 1 : 0;
            }

            Assert::AreEqual(3, logged, L"first look, forced step, last step");
            Assert::IsTrue(ShouldLogStep(1) && ShouldLogStep(ForcedStep) && ShouldLogStep(MaxSteps));
            Assert::IsFalse(ShouldLogStep(3));
        }

        TEST_METHOD(SettleCycleIsBounded)
        {
            Assert::IsFalse(SettleDone(1, true), L"runs the minimum even when all is in order");
            Assert::IsFalse(SettleDone(MinSteps - 1, true));
            Assert::IsTrue(SettleDone(MinSteps, true), L"settled: the timer stops, no idle polling");
            Assert::IsFalse(SettleDone(MinSteps, false), L"something is still off: keep looking");
            Assert::IsTrue(SettleDone(MaxSteps, false), L"gives up after about two seconds");
            Assert::IsTrue(ForcedStep < MinSteps, L"the forced re-apply happens inside the minimum run");
            Assert::IsTrue(MaxSteps * static_cast<int>(SettleMillis) <= 2500);
        }

        TEST_METHOD(ThumbnailIsFittedInTheTileKeepingTheWindowShape)
        {
            const LRect tile(100, 200, 500, 425); // 400 x 225
            Assert::IsTrue(FitRect(tile, 1920, 1080).Same(tile), L"the shape of the tile: fills it");
            const auto tall = FitRect(tile, 500, 500);
            Assert::AreEqual(225, tall.W());
            Assert::AreEqual(225, tall.H());
            Assert::AreEqual(100 + (400 - 225) / 2, tall.L, L"centered");
            Assert::IsTrue(tall.Inside(tile));
            const auto wide = FitRect(tile, 800, 100);
            Assert::AreEqual(400, wide.W());
            Assert::AreEqual(50, wide.H());
            Assert::IsTrue(wide.Inside(tile));
            Assert::IsTrue(FitRect(tile, 0, 0).Same(tile), L"unknown size: the tile");
            // a source at another DPI (bigger in pixels, same shape) fills the tile just the same
            Assert::IsTrue(FitRect(tile, 2880, 1620).Same(tile));
        }

        TEST_METHOD(ClipCutsATileAtTheEdgeOfTheStrip)
        {
            const LRect clip(0, 100, 100, 500);
            Assert::IsTrue(ClipRect(LRect(10, 120, 90, 200), clip).Same(LRect(10, 120, 90, 200)), L"inside: as it is");
            Assert::IsTrue(ClipRect(LRect(10, 60, 90, 140), clip).Same(LRect(10, 100, 90, 140)), L"leaving at the top: cut");
            Assert::IsTrue(ClipRect(LRect(10, 460, 90, 540), clip).Same(LRect(10, 460, 90, 500)), L"coming in at the bottom: cut");
            Assert::AreEqual(0, ClipRect(LRect(10, 500, 90, 580), clip).W(), L"just outside: nothing");
            Assert::AreEqual(0, ClipRect(LRect(10, 0, 90, 100), clip).H());
        }

        TEST_METHOD(SlideProjectionKeepsTilesJustOutsideTheStripOnBothSides)
        {
            const auto full = Full(12, 3); // pitch 60
            auto shown = ProjectSlide(full, 5, 2);
            Assert::AreEqual(size_t{ 7 }, shown.tiles.size(), L"3 showing + two waiting on each side");
            Assert::AreEqual(std::uintptr_t{ 103 }, shown.tiles.front().window);
            Assert::AreEqual(std::uintptr_t{ 109 }, shown.tiles.back().window);
            Assert::AreEqual(-120, shown.tiles[0].rect.T, L"two slots above the first");
            Assert::AreEqual(0, shown.tiles[2].rect.T);
            Assert::AreEqual(120, shown.tiles[4].rect.T);
            Assert::AreEqual(240, shown.tiles[6].rect.T, L"two slots below the last");
            Assert::AreEqual(size_t{ 5 }, ProjectSlide(full, 0, 2).tiles.size(), L"at the start: nothing above");
            Assert::AreEqual(size_t{ 5 }, ProjectSlide(full, 9, 2).tiles.size(), L"at the end: nothing below");
            Assert::AreEqual(size_t{ 3 }, ProjectSlide(full, 0, 0).tiles.size(), L"no margin: as Project");
        }

        TEST_METHOD(AScrollIsOneSlideOfTheWholeList)
        {
            const auto full = Full(12, 3);
            const auto before = ProjectSlide(full, 4, 2);
            const auto after = ProjectSlide(full, 5, 2);
            const int viewBottom = full.tiles[2].rect.B;
            std::map<std::uintptr_t, Motion> motion;
            for (const auto& t : before.tiles)
            {
                motion[t.window] = Motion::At(t.rect, 0);
            }

            std::vector<std::uintptr_t> staying;
            for (const auto& t : after.tiles)
            {
                const auto it = motion.find(t.window);
                if (it == motion.end())
                {
                    Assert::IsTrue(t.rect.T >= viewBottom, L"a tile that enters appears beyond the edge of the strip, not inside it");
                    motion[t.window] = Motion::At(t.rect, 0);
                }
                else
                {
                    Retarget(it->second, t.rect, 0);
                    staying.push_back(t.window);
                }
            }

            const auto duration = 300;
            for (const std::uint64_t now : { 0, 75, 150, 225, 300 })
            {
                std::vector<int> tops;
                for (const auto window : staying)
                {
                    auto& m = motion[window];
                    Step(m, now, duration, Linear);
                    tops.push_back(m.cur.T);
                }

                for (size_t i = 1; i < tops.size(); i++)
                {
                    Assert::IsTrue(std::abs((tops[i] - tops[i - 1]) - 60) <= 1, L"constant spacing at every moment: the list slides as one");
                }
            }
        }

        TEST_METHOD(HitTestFollowsTheProjectedTiles)
        {
            const auto shown = Project(Full(6, 3), 2);
            Assert::AreEqual(std::uintptr_t{ 102 }, StageStripPlan::HitTile(shown.tiles, 50, 20), L"the first slot is the third window");
            Assert::AreEqual(std::uintptr_t{ 0 }, StageStripPlan::HitTile(shown.tiles, 50, 55), L"the gap");
            Assert::AreEqual(std::uintptr_t{ 0 }, StageStripPlan::HitTile(shown.tiles, 50, 400), L"scrolled-out tiles cannot be hit");
        }
    };
}
