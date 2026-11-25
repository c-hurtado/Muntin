#include "pch.h"

#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageStripPlan.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The height of the strip tiles: they share the height of the docked windows when they fit, shrink to show the most windows when
// they do not, and scroll only below the minimum height. Pure: the plan is computed by the layout engine, then stretched.

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
}

namespace StageStripStretchUnitTests
{
    TEST_CLASS(StageStripStretchUnitTests)
    {
    public:
        TEST_METHOD(SevenWindowsInPairAllShowOnATallStrip)
        {
            const auto pair = StageLayouts::MakePair();
            auto plan = TallPlan(*pair, 7); // 2 docked, 5 tiles
            const auto aspectH = plan.TileH;
            Assert::IsTrue((plan.Area.H() + plan.Gap) / (aspectH + plan.Gap) < 5, L"at the aspect height not all of them fit (the live bug)");
            StretchTiles(plan);
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(size_t{ 5 }, tiles.size());
            Assert::AreEqual(plan.Area.T, tiles.front()->Rect.T);
            Assert::AreEqual(plan.Area.B, tiles.back()->Rect.B, L"the last bottom exactly at the bottom of the docked windows");
            for (const auto* t : tiles)
            {
                Assert::IsTrue(t->Rect.Inside(plan.Area));
                Assert::IsTrue(t->Rect.H() >= MinTileHeight && t->Rect.H() < aspectH);
            }

            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            Assert::AreEqual(5, strip->fit, L"every window has a visible tile");
            Assert::AreEqual(static_cast<int>(strip->tiles.size()), strip->fit);
        }

        TEST_METHOD(SixTilesInStageAllShowToo)
        {
            const auto stage = StageLayouts::MakeStage();
            auto plan = TallPlan(*stage, 7); // 1 main, 6 tiles
            StretchTiles(plan);
            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            Assert::AreEqual(6, strip->fit);
            Assert::AreEqual(plan.Area.B, TilesOf(plan).back()->Rect.B);
        }

        TEST_METHOD(TwentyWindowsKeepTheMinimumHeightAndScroll)
        {
            const auto stage = StageLayouts::MakeStage();
            auto plan = TallPlan(*stage, 20); // 19 tiles
            StretchTiles(plan);
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(size_t{ 19 }, tiles.size());
            for (const auto* t : tiles)
            {
                Assert::AreEqual(MinTileHeight, t->Rect.H(), L"the minimum height");
            }

            for (size_t i = 1; i < tiles.size(); i++)
            {
                Assert::AreEqual(plan.Gap, tiles[i]->Rect.T - tiles[i - 1]->Rect.B);
            }

            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            const int expected = (plan.Area.H() + plan.Gap) / (MinTileHeight + plan.Gap);
            Assert::AreEqual(expected, strip->fit, L"as many as fit at the minimum");
            Assert::IsTrue(strip->fit < 19, L"the rest scrolls");
            Assert::IsTrue(strip->fit >= 8, L"about ten tiles on a 1350 px strip");
        }

        TEST_METHOD(TilesFillTheHeightExactlyWhenTheyFitAtAnyHeightInRange)
        {
            const auto stage = StageLayouts::MakeStage();
            for (int windows = 2; windows <= 14; windows++)
            {
                auto plan = TallPlan(*stage, windows);
                const auto original = plan;
                StretchTiles(plan);
                const auto tiles = TilesOf(plan);
                const int n = static_cast<int>(tiles.size());
                const int available = plan.Area.H() - (n - 1) * plan.Gap;
                const int cap = static_cast<int>(std::floor(plan.TileH * MaxStretch));
                if (available / n >= MinTileHeight && available / n <= cap)
                {
                    Assert::AreEqual(plan.Area.T, tiles.front()->Rect.T, L"top aligned with the docked windows");
                    Assert::AreEqual(plan.Area.B, tiles.back()->Rect.B, L"bottom exactly aligned (rounding remainder spread)");
                    for (size_t i = 1; i < tiles.size(); i++)
                    {
                        Assert::AreEqual(plan.Gap, tiles[i]->Rect.T - tiles[i - 1]->Rect.B);
                        Assert::IsTrue(std::abs(tiles[i]->Rect.H() - tiles[i - 1]->Rect.H()) <= 1);
                    }
                }

                const auto before = TilesOf(original);
                for (size_t i = 0; i < tiles.size(); i++)
                {
                    Assert::AreEqual(before[i]->Rect.L, tiles[i]->Rect.L, L"x unchanged");
                    Assert::AreEqual(before[i]->Rect.W(), tiles[i]->Rect.W(), L"width unchanged");
                }
            }
        }

        TEST_METHOD(AFewTilesAreCappedAndCentered)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 2); // 1 main, 1 tile
            const int aspectH = plan.TileH;
            StretchTiles(plan);
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(size_t{ 1 }, tiles.size());
            Assert::AreEqual(static_cast<int>(std::floor(aspectH * MaxStretch)), tiles[0]->Rect.H(), L"capped: no absurdly tall tile");
            const int above = tiles[0]->Rect.T - plan.Area.T;
            const int below = plan.Area.B - tiles[0]->Rect.B;
            Assert::IsTrue(std::abs(above - below) <= 1, L"centered vertically against the docked windows");
        }

        TEST_METHOD(TwoTilesOnATallStripAreTallerThanTheAspectHeight)
        {
            auto plan = TallPlan(*StageLayouts::MakePair(), 4); // 2 docked, 2 tiles (the live screenshot)
            const int aspectH = plan.TileH;
            StretchTiles(plan);
            const auto tiles = TilesOf(plan);
            Assert::AreEqual(size_t{ 2 }, tiles.size());
            Assert::IsTrue(tiles[0]->Rect.H() > aspectH, L"taller than the aspect height");
            Assert::IsTrue(tiles[0]->Rect.H() <= static_cast<int>(aspectH * MaxStretch), L"capped");
            if ((plan.Area.H() - plan.Gap) / 2 <= static_cast<int>(aspectH * MaxStretch))
            {
                Assert::AreEqual(plan.Area.B, tiles.back()->Rect.B, L"they fill the height");
            }
        }

        TEST_METHOD(NoStripNothingToStretch)
        {
            StageLayouts::LayoutInput in;
            in.Work = LRect(0, 0, 1920, 1080);
            for (int i = 1; i <= 5; i++)
            {
                in.Wins.emplace_back(i);
            }

            auto plan = StageLayouts::MakeMaster()->Compute(in);
            const auto original = plan;
            StretchTiles(plan);
            for (size_t i = 0; i < plan.Items.size(); i++)
            {
                Assert::IsTrue(plan.Items[i].Rect.Same(original.Items[i].Rect));
            }
        }

        TEST_METHOD(StretchedTilesAreHitWhereTheyAreDrawn)
        {
            auto plan = TallPlan(*StageLayouts::MakeStage(), 5);
            StretchTiles(plan);
            const auto strip = Build(L"m", plan, Src);
            Assert::IsTrue(strip.has_value());
            const auto& last = strip->tiles.back();
            Assert::AreEqual(last.window, HitTile(strip->tiles, last.rect.L + 5, last.rect.B - 1), L"the bottom row of the last tile is hit");
            Assert::AreEqual(std::uintptr_t{ 0 }, HitTile(strip->tiles, last.rect.L + 5, last.rect.B + 1));
        }
    };
}
