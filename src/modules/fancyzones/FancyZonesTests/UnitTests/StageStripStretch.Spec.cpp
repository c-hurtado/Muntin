#include "pch.h"

#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageMouse.h>
#include <FancyZonesLib/StageStripMotion.h>
#include <FancyZonesLib/StageStripPlan.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The height of the strip tiles: each hugs its window (width times the aspect of the window), scaled by one factor when they are too
// tall together (not below the minimum), spread with extra gap when they are too short (capped). Pure: the plan is computed by the layout
// engine, then laid out.

namespace
{
    using namespace StageStripPlan;
    using StageLayouts::LRect;

    const std::function<Source(int)> Src = [](int id) { return Source{ static_cast<std::uintptr_t>(id) * 10, true, { 7, 0, 7, 7 } }; };

    std::vector<const StageLayouts::PlanItem*> TilesOf(const StageLayouts::Plan& plan)
    {
        std::vector<const StageLayouts::PlanItem*> tiles;
        for (const auto& item : plan.Items)
        {
            if (item.Role == StageLayouts::Role::StripTile)
            {
                tiles.push_back(&item);
            }
        }

        return tiles;
    }

    // A tall screen as in the live report: 1920 x 1395 work area
    StageLayouts::Plan TallPlan(const StageLayouts::ILayout& layout, int windows)
    {
        StageLayouts::LayoutInput in;
        in.Work = LRect(0, 0, 1920, 1395);
        for (int i = 1; i <= windows; i++)
        {
            in.Wins.emplace_back(i);
        }

        return layout.Compute(in);
    }

    const auto Same = [](double aspect) { return [aspect](int) { return aspect; }; };

    int Natural(const StageLayouts::Plan& plan, double aspect)
    {
        return static_cast<int>(std::lround(TilesOf(plan).front()->Rect.W() * aspect));
    }
}

namespace StageStripStretchUnitTests
{
    TEST_CLASS(StageStripStretchUnitTests)
    {
    public:
        TEST_METHOD(EachTileHugsItsWindow)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 3); // 1 main, 2 tiles
            const int width = TilesOf(plan).front()->Rect.W();
            const auto original = plan;
            LayoutTiles(plan, [](int id) { return id == 2 ? 0.5625 : 1.0; }); // a wide terminal, a square window
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(size_t{ 2 }, tiles.size());
            Assert::AreEqual(static_cast<int>(std::lround(width * 0.5625)), tiles[0]->Rect.H(), L"the width times the aspect of the window");
            Assert::AreEqual(width, tiles[1]->Rect.H());
            Assert::AreEqual(plan.Area.T, tiles[0]->Rect.T, L"stacked from the top");
            Assert::AreEqual(TilesOf(original)[0]->Rect.L, tiles[0]->Rect.L, L"x unchanged");
            Assert::AreEqual(width, tiles[0]->Rect.W(), L"width unchanged");
            Assert::IsTrue(tiles[1]->Rect.T > tiles[0]->Rect.B, L"a gap between");
        }

        TEST_METHOD(AspectIsClampedAndUnknownSizeFallsBackToTheLayout)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 3);
            const int width = TilesOf(plan).front()->Rect.W();
            const int aspectH = plan.TileH;
            LayoutTiles(plan, [](int id) { return id == 2 ? 0.05 : 0.0; }); // absurdly flat, unknown
            auto tiles = TilesOf(plan);
            Assert::AreEqual(static_cast<int>(std::lround(width * AspectMin)), tiles[0]->Rect.H(), L"clamped to the flattest allowed");
            Assert::AreEqual(aspectH, tiles[1]->Rect.H(), L"unknown size: the layout's aspect height");

            auto tall = TallPlan(*StageLayouts::MakeStage(), 2);
            LayoutTiles(tall, Same(9.0));
            Assert::AreEqual(static_cast<int>(std::lround(width * AspectMax)), TilesOf(tall)[0]->Rect.H(), L"clamped to the tallest allowed");
        }

        TEST_METHOD(OneTileIsTopAlignedAtItsNaturalHeight)
        {
            auto plan = TallPlan(*StageLayouts::MakePair(), 3); // 2 docked, 1 tile
            LayoutTiles(plan, Same(0.6));
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(size_t{ 1 }, tiles.size());
            Assert::AreEqual(plan.Area.T, tiles[0]->Rect.T);
            Assert::AreEqual(Natural(plan, 0.6) , tiles[0]->Rect.H());
            Assert::IsTrue(tiles[0]->Rect.B < plan.Area.B, L"not stretched to the height of the docked windows");

            auto tallOne = TallPlan(*StageLayouts::MakePair(), 3);
            LayoutTiles(tallOne, Same(2.5));
            Assert::IsTrue(TilesOf(tallOne)[0]->Rect.B <= tallOne.Area.B, L"never taller than the usable height");
        }

        TEST_METHOD(TooTallTogetherAreScaledByOneFactorAndFillTheHeightExactly)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 6); // 5 tiles
            const auto aspects = [](int id) { return id % 2 == 0 ? 0.75 : 1.5; }; // different shapes
            const int width = TilesOf(plan).front()->Rect.W();
            int sum = 0;
            for (const int id : { 2, 3, 4, 5, 6 })
            {
                sum += static_cast<int>(std::lround(width * aspects(id)));
            }

            Assert::IsTrue(sum + 4 * plan.Gap > plan.Area.H(), L"the natural heights do not fit");
            LayoutTiles(plan, aspects);
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(plan.Area.T, tiles.front()->Rect.T);
            Assert::AreEqual(plan.Area.B, tiles.back()->Rect.B, L"the last bottom exactly at the bottom of the docked windows");
            for (size_t i = 1; i < tiles.size(); i++)
            {
                Assert::AreEqual(plan.Gap, tiles[i]->Rect.T - tiles[i - 1]->Rect.B, L"the layout gap, no extra");
            }

            // the same factor for all: a tile twice as tall stays about twice as tall
            Assert::IsTrue(std::abs(tiles[1]->Rect.H() - 2 * tiles[0]->Rect.H()) <= 3, L"proportions kept"); // ids 2 (0.75) and 3 (1.5)
            for (const auto* t : tiles)
            {
                Assert::IsTrue(t->Rect.H() >= MinTileHeight);
                Assert::IsTrue(t->Rect.Inside(plan.Area));
            }

            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            Assert::AreEqual(5, strip->fit, L"every window shows");
        }

        TEST_METHOD(TwentyWindowsKeepTheMinimumAndScroll)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 20); // 19 tiles
            LayoutTiles(plan, Same(0.5625));
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(size_t{ 19 }, tiles.size());
            for (const auto* t : tiles)
            {
                Assert::AreEqual(MinTileHeight, t->Rect.H(), L"the minimum height");
            }

            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            Assert::IsTrue(strip->fit >= 8 && strip->fit < 19, L"as many as fit at the minimum, the rest scrolls");
        }

        TEST_METHOD(LeftoverBecomesExtraGapButOnlyUpToTheCap)
        {
            // 3 tiles that are a little short together: the leftover goes into the two gaps and the last bottom lines up
            auto plan = TallPlan(*StageLayouts::MakeStage(), 4);
            const int width = TilesOf(plan).front()->Rect.W();
            const int avail = plan.Area.H() - 2 * plan.Gap;
            const double aspect = (static_cast<double>(avail) - 20) / 3 / width;
            LayoutTiles(plan, Same(aspect));
            auto tiles = TilesOf(plan);
            const int natural = static_cast<int>(std::lround(width * aspect));
            const int leftover = avail - 3 * natural;
            Assert::IsTrue(leftover > 0 && leftover / 2 <= (std::min)(MaxExtraGapFactor * plan.Gap, MaxExtraGapPx));
            Assert::AreEqual(plan.Area.T, tiles.front()->Rect.T);
            Assert::AreEqual(plan.Area.B, tiles.back()->Rect.B, L"justified: the last bottom lines up (rounding too)");
            Assert::IsTrue(tiles[1]->Rect.T - tiles[0]->Rect.B > plan.Gap, L"extra gap");

            // much shorter: the gap stops at the cap, the rest stays unused at the bottom
            auto flat = TallPlan(*StageLayouts::MakeStage(), 4);
            LayoutTiles(flat, Same(0.4));
            tiles = TilesOf(flat);
            const int cap = (std::min)(MaxExtraGapFactor * flat.Gap, MaxExtraGapPx);
            Assert::AreEqual(flat.Gap + cap, tiles[1]->Rect.T - tiles[0]->Rect.B, L"extra gap at the cap");
            Assert::AreEqual(flat.Gap + cap, tiles[2]->Rect.T - tiles[1]->Rect.B);
            Assert::IsTrue(tiles.back()->Rect.B < flat.Area.B, L"the rest is unused at the bottom: top aligned, not centered");
            Assert::AreEqual(flat.Area.T, tiles.front()->Rect.T);
        }

        TEST_METHOD(AspectOnlyChangesTheTileWhenItMovesMoreThanFivePercent)
        {
            Assert::AreEqual(0.5625, StableAspect(0.0, 0.5625), 1e-9, L"first time");
            Assert::AreEqual(0.5625, StableAspect(0.5625, 0.57), 1e-9, L"a few pixels: no re-plan");
            Assert::AreEqual(0.5625, StableAspect(0.5625, 0.5625 * 1.049), 1e-9, L"just inside the tolerance");
            Assert::AreEqual(0.7, StableAspect(0.5625, 0.7), 1e-9, L"a real change");
            Assert::AreEqual(0.5625, StableAspect(0.5625, 0.0), 1e-9, L"unknown now: keep the last");
        }

        TEST_METHOD(OneSmallTileDoesNotStopTheOthersFromShrinking)
        {
            // 3 tiles: a tall one, and two little ones (already at their floor); the tall one takes the whole squeeze
            auto plan = TallPlan(*StageLayouts::MakeStage(), 8); // 7 tiles
            const int width = TilesOf(plan).front()->Rect.W();
            const auto aspects = [](int id) { return id == 2 ? 2.5 : 0.4; }; // ~712 high, ~114 high (below the floor of 120)
            LayoutTiles(plan, aspects);
            const auto tiles = TilesOf(plan);
            const int little = static_cast<int>(std::lround(width * 0.4));
            Assert::IsTrue(little < MinTileHeight, L"the little tiles are below the floor");
            Assert::AreEqual(little, tiles[1]->Rect.H(), L"a tile under the floor keeps its natural height");
            Assert::AreEqual(little, tiles[6]->Rect.H());
            Assert::IsTrue(tiles[0]->Rect.H() < static_cast<int>(std::lround(width * 2.5)), L"the tall one took the squeeze");
            Assert::AreEqual(plan.Area.B, tiles.back()->Rect.B, L"exact bottom");

            // a mix: the floors alone fit, one tile sits on its floor and the others share the rest
            auto mixed = TallPlan(*StageLayouts::MakeStage(), 8); // 7 tiles
            const auto mix = [](int id) { return id == 2 ? 2.5 : (id == 3 ? 0.45 : 1.6); };
            LayoutTiles(mixed, mix);
            const auto m = TilesOf(mixed);
            int total = 0;
            for (const auto* t : m)
            {
                Assert::IsTrue(t->Rect.H() >= (std::min)(MinTileHeight, static_cast<int>(std::lround(width * mix(t->Id)))));
                total += t->Rect.H();
            }

            Assert::AreEqual(mixed.Area.B, m.back()->Rect.B, L"exact bottom");
            Assert::AreEqual(mixed.Gap, m[1]->Rect.T - m[0]->Rect.B);
        }

        TEST_METHOD(ShrunkTilesAreNarrowerToKeepTheShapeAndCentered)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 7); // 6 tiles
            const int width = TilesOf(plan).front()->Rect.W();
            const int left = TilesOf(plan).front()->Rect.L;
            const double aspect = 1.0;
            LayoutTiles(plan, Same(aspect));
            const auto tiles = TilesOf(plan);
            Assert::IsTrue(tiles[0]->Rect.H() < width, L"shrunk below the natural height");
            Assert::IsTrue(std::abs(tiles[0]->Rect.H() - tiles[0]->Rect.W()) <= 1, L"the window shape kept: as narrow as it is short");
            Assert::AreEqual(left + (width - tiles[0]->Rect.W()) / 2, tiles[0]->Rect.L, L"centered in the strip");
            for (const auto* t : tiles)
            {
                Assert::IsTrue(t->Rect.L >= left && t->Rect.R <= left + width, L"never outside the strip");
            }

            // not shrunk: the full width
            auto few = TallPlan(*StageLayouts::MakeStage(), 3);
            LayoutTiles(few, Same(1.0));
            Assert::AreEqual(width, TilesOf(few)[0]->Rect.W());

            // the narrower tile is what is hit and what is clipped
            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            const auto& t = strip->tiles[0];
            Assert::AreEqual(std::uintptr_t{ 0 }, HitTile(strip->tiles, t.rect.L - 3, t.rect.T + 3), L"the band beside a narrow tile is not a tile");
        }

        TEST_METHOD(ExtraGapNeverExceedsTheCap)
        {
            // a leftover whose share is exactly the cap, with a remainder: still not one pixel more than the cap
            auto plan = TallPlan(*StageLayouts::MakeStage(), 4); // 3 tiles, 2 gaps
            const int width = TilesOf(plan).front()->Rect.W();
            const int cap = (std::min)(MaxExtraGapFactor * plan.Gap, MaxExtraGapPx);
            const int avail = plan.Area.H() - 2 * plan.Gap;
            // heights h each so that leftover = 2 * cap + 1 (share = cap, remainder 1)
            const int h = (avail - (2 * cap + 1)) / 3;
            const double aspect = static_cast<double>(h) / width;
            LayoutTiles(plan, Same(aspect));
            const auto tiles = TilesOf(plan);
            for (size_t i = 1; i < tiles.size(); i++)
            {
                Assert::IsTrue(tiles[i]->Rect.T - tiles[i - 1]->Rect.B <= plan.Gap + cap, L"never more than the cap");
            }
        }

        TEST_METHOD(DropToSwapHitsTheTileThatIsDrawnWhenTheStripIsScrolled)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 20); // 19 tiles at the minimum height, scrolled
            LayoutTiles(plan, Same(0.5625));
            const auto unscrolled = plan;
            const auto tilesBefore = TilesOf(unscrolled);
            StageStripPlan::ApplyScroll(plan, 4);
            const auto tiles = TilesOf(plan);
            // the first tile in view is now the fifth window
            const auto* firstInView = tiles[4];
            Assert::AreEqual(plan.Area.T, firstInView->Rect.T);
            const int x = firstInView->Rect.L + 5, y = firstInView->Rect.T + 5;
            std::vector<int> order;
            for (const auto& item : plan.Items)
            {
                order.push_back(item.Id);
            }

            Assert::AreEqual(firstInView->Id, StageMouse::HitTest(plan, order, 0, x, y), L"the drop lands on the window drawn under the cursor");
            Assert::AreNotEqual(tilesBefore[0]->Id, StageMouse::HitTest(plan, order, 0, x, tilesBefore[0]->Rect.T + 5), L"the tile that scrolled out is not hit any more");
            // a tile that is out of view has nothing to hit
            Assert::AreEqual(0, tiles[0]->Rect.W() * tiles[0]->Rect.H());
            Assert::AreEqual(StageStripPlan::MaxScrollOffset({ LRect(0, 0, 10, 300), LRect(0, 310, 10, 610) }, 400), 1);
        }

        TEST_METHOD(NoStripNothingToLayOut)
        {
            StageLayouts::LayoutInput in;
            in.Work = LRect(0, 0, 1920, 1080);
            for (int i = 1; i <= 5; i++)
            {
                in.Wins.emplace_back(i);
            }

            auto plan = StageLayouts::MakeMaster()->Compute(in);
            const auto original = plan;
            LayoutTiles(plan, Same(1.0));
            for (size_t i = 0; i < plan.Items.size(); i++)
            {
                Assert::IsTrue(plan.Items[i].Rect.Same(original.Items[i].Rect));
            }
        }

        TEST_METHOD(VariableHeightTilesScrollByWholeTilesAndStayConsistent)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 20);
            LayoutTiles(plan, [](int id) { return id % 3 == 0 ? 1.2 : 0.5625; }); // tall and wide tiles, too many to fit
            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            const int total = static_cast<int>(strip->tiles.size());
            Assert::AreEqual(19, total);
            Assert::IsTrue(strip->fit < total);

            const int maxOffset = StageStripMotion::MaxOffsetFor(*strip);
            Assert::IsTrue(maxOffset > 0 && maxOffset < total);
            Assert::AreEqual(maxOffset, StageStripMotion::ClampOffsetFor(*strip, 99));
            Assert::AreEqual(0, StageStripMotion::ClampOffsetFor(*strip, -3));

            // scrolled all the way down, every tile that is left fits in the strip
            const auto bottom = StageStripMotion::ProjectSlide(*strip, maxOffset);
            Assert::IsFalse(bottom.tiles.empty());
            for (const auto& t : bottom.tiles)
            {
                Assert::IsTrue(t.rect.Inside(strip->clip), L"the last tiles are completely in view");
            }

            Assert::AreEqual(strip->tiles.back().window, bottom.tiles.back().window);
            // one step down: the list moves up by the height of the first tile and its gap
            const auto top = StageStripMotion::ProjectSlide(*strip, 0);
            const auto next = StageStripMotion::ProjectSlide(*strip, 1);
            const int dy = strip->tiles[1].rect.T - strip->tiles[0].rect.T;
            Assert::AreEqual(top.tiles[1].window, next.tiles[0].window);
            Assert::AreEqual(top.tiles[1].rect.T - dy, next.tiles[0].rect.T, L"each tile moves by the height of the one before it plus the gap");
            Assert::AreEqual(strip->clip.T, next.tiles[0].rect.T);

            // the tiles that keep sliding out of view stay while they do
            const std::set<std::uintptr_t> keep{ strip->tiles.front().window };
            const auto slide = StageStripMotion::ProjectSlide(*strip, 3, &keep);
            Assert::IsTrue(slide.tiles.front().window == strip->tiles.front().window && slide.tiles.front().rect.B <= strip->clip.T, L"waiting above the strip until it has slid off");

            // hits and hover follow the projected rects
            const auto& first = top.tiles.front();
            Assert::AreEqual(first.window, StageStripPlan::HitTile(top.tiles, first.rect.L + 5, first.rect.T + 2));
        }

        TEST_METHOD(HuggingTilesAreHitWhereTheyAreDrawn)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 5);
            LayoutTiles(plan, [](int id) { return 0.5 + 0.1 * id; });
            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            for (const auto& t : strip->tiles)
            {
                Assert::AreEqual(t.window, HitTile(strip->tiles, t.rect.L + 5, t.rect.B - 1), L"the bottom row of every tile");
                Assert::AreEqual(t.window, HitTile(strip->tiles, t.rect.L + 5, t.rect.T));
            }

            const auto& last = strip->tiles.back();
            Assert::AreEqual(std::uintptr_t{ 0 }, HitTile(strip->tiles, last.rect.L + 5, last.rect.B + 1));
        }
    };
}
