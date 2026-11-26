#include "pch.h"

#include <FancyZonesLib/StageMotion.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Which windows glide when the stage moves them. Pure: no windows.

namespace
{
    using namespace StageMotion;

    constexpr Context On()
    {
        Context c;
        c.setting = true;
        return c;
    }

    constexpr Candidate Mover()
    {
        Candidate w;
        w.landsDocked = true;
        w.moves = true;
        return w;
    }
}

namespace StageMotionUnitTests
{
    TEST_CLASS(StageMotionUnitTests)
    {
    public:
        TEST_METHOD(ADockedWindowThatMovesGlides)
        {
            Assert::IsTrue(ShouldGlide(On(), Mover(), false));
        }

        TEST_METHOD(NothingGlidesWithTheSettingOffOrTheKillSwitch)
        {
            Context c = On();
            c.setting = false;
            Assert::IsFalse(ShouldGlide(c, Mover(), false));
            Assert::IsFalse(ShouldGlide(On(), Mover(), false, false));
        }

        TEST_METHOD(ParkedWindowsAndWindowsThatStayPutDoNot)
        {
            Candidate parked = Mover();
            parked.landsDocked = false;
            Assert::IsFalse(ShouldGlide(On(), parked, false));
            Candidate still = Mover();
            still.moves = false;
            Assert::IsFalse(ShouldGlide(On(), still, false));
        }

        TEST_METHOD(AWindowOutOfTheStripMovesAtOnceUnlessTheSettingSaysGlide)
        {
            Candidate promoted = Mover();
            promoted.wasParked = true;
            Assert::IsFalse(ShouldGlide(On(), promoted, false), L"setting off: moves at once");
            Assert::IsTrue(ShouldGlide(On(), promoted, true));
            Assert::IsFalse(ShouldGlide(On(), promoted, true, false), L"kill switch");
            Assert::IsTrue(ShouldGlide(On(), Mover(), false), L"a window that was docked is not affected");
            Assert::IsTrue(PromoteGlideAvailable);
        }

        TEST_METHOD(ExtraHoldsOnlyWhenAGliderComesFromBehindTheMaster)
        {
            Candidate behind = Mover();
            behind.wasParked = true;
            behind.parkedBehind = true;
            Candidate sliver = Mover();
            sliver.wasParked = true;
            Candidate master = Mover();
            master.moves = false;
            Candidate hidden = master;
            hidden.minimized = true;
            Candidate away = master;
            away.changesMonitor = true;

            std::vector<Candidate> windows{ behind, master, hidden, away, Mover() };
            const auto picked = Gliders(On(), windows, true);
            Assert::AreEqual(size_t{ 2 }, picked.size());
            const auto extra = ExtraHolds(windows, picked);
            Assert::AreEqual(size_t{ 1 }, extra.size());
            Assert::AreEqual(size_t{ 1 }, extra[0]);

            std::vector<Candidate> slivers{ sliver, master };
            Assert::AreEqual(size_t{ 0 }, ExtraHolds(slivers, Gliders(On(), slivers, true)).size(), L"from a sliver: nothing extra");
            Assert::AreEqual(size_t{ 0 }, ExtraHolds(windows, {}).size());
            Assert::AreEqual(size_t{ 0 }, ExtraHolds(windows, Gliders(On(), windows, false)).size(), L"setting off: no glider from behind");
        }

        TEST_METHOD(ScopeGuardRunsOnExitEvenOnAnException)
        {
            int runs = 0;
            try
            {
                ScopeGuard guard{ [&] { runs++; } };
                throw 1;
            }
            catch (int)
            {
            }

            Assert::AreEqual(1, runs);
        }

        TEST_METHOD(UnusableWindowsDoNotGlide)
        {
            const auto check = [](auto set) {
                Candidate w = Mover();
                set(w);
                Assert::IsFalse(ShouldGlide(On(), w, false));
            };
            check([](Candidate& w) { w.minimized = true; });
            check([](Candidate& w) { w.cloaked = true; });
            check([](Candidate& w) { w.maximized = true; });
            check([](Candidate& w) { w.fullscreen = true; });
            check([](Candidate& w) { w.changesMonitor = true; });
            check([](Candidate& w) { w.visible = false; });
        }

        TEST_METHOD(ADragAMonitorChangeOrAShutdownStopTheGlide)
        {
            Context drag = On();
            drag.dragging = true;
            Context area = On();
            area.areaChanged = true;
            Context shutdown = On();
            shutdown.shuttingDown = true;
            Assert::IsFalse(ShouldGlide(drag, Mover(), false));
            Assert::IsFalse(ShouldGlide(area, Mover(), false));
            Assert::IsFalse(ShouldGlide(shutdown, Mover(), false));
        }

        TEST_METHOD(GlidersPicksTheMoversInOrder)
        {
            Candidate parked = Mover();
            parked.landsDocked = false;
            Candidate still = Mover();
            still.moves = false;
            const std::vector<Candidate> windows{ Mover(), parked, still, Mover() };
            const auto picked = Gliders(On(), windows, false);
            Assert::AreEqual(size_t{ 2 }, picked.size());
            Assert::AreEqual(size_t{ 0 }, picked[0]);
            Assert::AreEqual(size_t{ 3 }, picked[1]);
            Assert::AreEqual(size_t{ 0 }, Gliders(On(), windows, false, false).size());
        }

        TEST_METHOD(LeavingGlidesOnlyOnAUsersSwitch)
        {
            Assert::IsTrue(ShouldGlideLeave(On()));
            Context shutdown = On();
            shutdown.shuttingDown = true;
            Assert::IsFalse(ShouldGlideLeave(shutdown));
            Context off = On();
            off.setting = false;
            Assert::IsFalse(ShouldGlideLeave(off));
            Assert::IsFalse(ShouldGlideLeave(On(), true, false));
            Assert::IsFalse(ShouldGlideLeave(On(), false));
        }

        TEST_METHOD(TheStyleSettingMapsToGrowOrCrossfade)
        {
            Assert::IsTrue(StyleOf(0) == PromoteStyle::Grow);
            Assert::IsTrue(StyleOf(1) == PromoteStyle::Crossfade);
            Assert::IsTrue(StyleOf(7) == PromoteStyle::Grow, L"anything else: the default");
            Assert::IsTrue(StyleOf(-1) == PromoteStyle::Grow);
        }

        TEST_METHOD(OuterForTileMatchesTheThumbnailTheStripDraws)
        {
            // A 1000x600 window with 10px side/bottom frames and none on top: the visible 980x590 is fitted into the deflated tile, not stretched
            const StageLayouts::LRect tile(100, 50, 590, 345);
            const auto box = StageStripMotion::Deflate(tile, StageStripMotion::TilePad);
            const auto visible = StageStripMotion::FitRect(box, 980, 590);
            const auto outer = OuterForTile(tile, { 10, 0, 10, 10 }, 1000, 600);
            const double sx = static_cast<double>(visible.W()) / 980;
            Assert::AreEqual(visible.L - static_cast<int>(std::lround(10 * sx)), outer.L);
            Assert::AreEqual(visible.T, outer.T);
            Assert::AreEqual(visible.R + static_cast<int>(std::lround(10 * sx)), outer.R);
            Assert::IsTrue(outer.W() * 600 - outer.H() * 1000 <= 1600 && outer.H() * 1000 - outer.W() * 600 <= 1600, L"the window's shape, not the tile's");
            Assert::IsTrue(outer.Inside(StageLayouts::LRect(tile.L - 10, tile.T, tile.R + 10, tile.B + 10)));

            // No insets: the whole window fitted in the box; unknown size or insets that leave nothing: the box
            Assert::IsTrue(OuterForTile(tile, { 0, 0, 0, 0 }, 1000, 600).Same(StageStripMotion::FitRect(box, 1000, 600)));
            Assert::IsTrue(OuterForTile(tile, { 600, 0, 400, 0 }, 1000, 600).Same(StageStripMotion::FitRect(box, 1000, 600)));
            Assert::IsTrue(OuterForTile(tile, { 10, 0, 10, 10 }, 0, 0).Same(box));
        }

        TEST_METHOD(AScrolledTileIsNotCutButNeedsToBeInView)
        {
            const StageLayouts::LRect area(1500, 100, 1800, 700);
            const std::vector<StageLayouts::LRect> tiles{
                { 1500, 100, 1800, 300 }, { 1500, 310, 1800, 510 }, { 1500, 520, 1800, 720 }, { 1500, 730, 1800, 930 } };
            const auto none = ScrolledTile(tiles, 0, area, 0);
            Assert::IsTrue(none && none->Same(tiles[0]));
            const auto cut = ScrolledTile(tiles, 2, area, 0);
            Assert::IsTrue(cut && cut->Same(tiles[2]), L"cut by the area in the strip, whole here");
            Assert::IsFalse(ScrolledTile(tiles, 3, area, 0).has_value(), L"wholly below the area");
            const auto moved = ScrolledTile(tiles, 3, area, 1);
            Assert::IsTrue(moved && moved->T == 730 - 210, L"scrolled up by one tile");
            Assert::IsFalse(ScrolledTile(tiles, 0, area, 1).has_value(), L"scrolled out above");
            Assert::IsFalse(ScrolledTile(tiles, 9, area, 0).has_value());
            Assert::IsFalse(ScrolledTile({}, 0, area, 0).has_value());
        }

        TEST_METHOD(GrowStartsTheSpriteAtTheTileAndCrossfadeFadesInAtTheSlot)
        {
            const StageLayouts::LRect tile(1500, 100, 1800, 300), slot(0, 0, 1000, 800);
            const auto grow = PromotedSpec(PromoteStyle::Grow, tile, slot);
            Assert::IsTrue(grow.kind == Kind::Travel);
            Assert::IsTrue(grow.from && grow.from->Same(tile));
            Assert::IsFalse(grow.to.has_value(), L"it ends where the real window is");
            const auto unknown = PromotedSpec(PromoteStyle::Grow, std::nullopt, slot);
            Assert::IsTrue(unknown.kind == Kind::Travel);
            Assert::IsFalse(unknown.from.has_value(), L"no tile: from the parked rect, as the window is");
            const auto fade = PromotedSpec(PromoteStyle::Crossfade, tile, slot);
            Assert::IsTrue(fade.kind == Kind::FadeIn);
            Assert::IsTrue(fade.from && fade.from->Same(slot));
            Assert::IsTrue(fade.to && fade.to->Same(slot));
        }

        TEST_METHOD(TheDemotedWindowShrinksIntoItsTileOrFadesOutWhereItIs)
        {
            const StageLayouts::LRect tile(1500, 100, 1800, 300), old(0, 0, 1000, 800);
            const auto shrink = DemotedSpec(PromoteStyle::Grow, tile, old);
            Assert::IsTrue(shrink.has_value());
            Assert::IsTrue(shrink->kind == Kind::Travel);
            Assert::IsTrue(shrink->from->Same(old));
            Assert::IsTrue(shrink->to->Same(tile));
            Assert::IsFalse(DemotedSpec(PromoteStyle::Grow, std::nullopt, old).has_value(), L"no tile: it parks at once");
            const auto fade = DemotedSpec(PromoteStyle::Crossfade, std::nullopt, old);
            Assert::IsTrue(fade.has_value());
            Assert::IsTrue(fade->kind == Kind::FadeOut);
            Assert::IsTrue(fade->from->Same(old) && fade->to->Same(old));
        }

        TEST_METHOD(TheMoveStyleSettingMapsToSlideOrCrossfade)
        {
            Assert::IsTrue(MoveStyleOf(0) == MoveStyle::Slide);
            Assert::IsTrue(MoveStyleOf(1) == MoveStyle::Crossfade);
            Assert::IsTrue(MoveStyleOf(5) == MoveStyle::Slide, L"anything else: the default");
            Assert::IsTrue(MoveStyleOf(-1) == MoveStyle::Slide);
        }

        TEST_METHOD(ASlideMoveIsThePlainTravelAndACrossfadeFadesOldIntoNew)
        {
            const StageLayouts::LRect old(0, 0, 500, 800), now(500, 0, 1000, 800);
            const auto slide = MovedSpec(MoveStyle::Slide, old, now);
            Assert::IsTrue(slide.kind == Kind::Travel);
            Assert::IsFalse(slide.from.has_value() || slide.to.has_value(), L"the sprite travels from the window to the window");
            Assert::IsTrue(FadeThroughParts(slide).empty());

            const auto fade = MovedSpec(MoveStyle::Crossfade, old, now);
            Assert::IsTrue(fade.kind == Kind::FadeThrough);
            const auto parts = FadeThroughParts(fade);
            Assert::AreEqual(static_cast<size_t>(2), parts.size());
            Assert::IsTrue(parts[0].kind == Kind::FadeOut && parts[0].from->Same(old) && parts[0].to->Same(old));
            Assert::IsTrue(parts[1].kind == Kind::FadeIn && parts[1].from->Same(now) && parts[1].to->Same(now));
            Assert::AreEqual(255, OpacityByte(parts[0].kind, 0) );
            Assert::AreEqual(0, OpacityByte(parts[1].kind, 0));
            Assert::AreEqual(OpacityByte(parts[0].kind, 0.25), 255 - OpacityByte(parts[1].kind, 0.25), L"complementary");
        }

        TEST_METHOD(ACrossfadeWithoutAKnownOldRectSlidesInstead)
        {
            const StageLayouts::LRect now(500, 0, 1000, 800), none(0, 0, 0, 0);
            Assert::IsTrue(MovedSpec(MoveStyle::Crossfade, none, now).kind == Kind::Travel);
            Assert::IsTrue(MovedSpec(MoveStyle::Crossfade, now, none).kind == Kind::Travel);
        }

        TEST_METHOD(AGhostStartsAtTheOldRectOpaqueWithoutALiveSprite)
        {
            const StageLayouts::LRect old(0, 0, 500, 800), now(500, 0, 1000, 800);
            const auto start = GhostStart(MovedSpec(MoveStyle::Crossfade, old, now), std::nullopt);
            Assert::IsTrue(start.has_value());
            Assert::IsTrue(start->place == std::array<double, 4>{ 0, 0, 500, 800 });
            Assert::AreEqual(255, start->opacity);
        }

        TEST_METHOD(AQuickSecondMoveStartsTheGhostWhereTheLiveSpriteIsSeen)
        {
            const StageLayouts::LRect old(0, 0, 500, 800), now(500, 0, 1000, 800);
            const Look live{ { 250, 10, 750, 810 }, 128 };
            const auto start = GhostStart(MovedSpec(MoveStyle::Crossfade, old, now), live);
            Assert::IsTrue(start.has_value());
            Assert::IsTrue(start->place == live.place, L"from the sprite mid-animation, not the old rect");
            Assert::AreEqual(128, start->opacity, L"at its current opacity, not opaque");
            Assert::AreEqual(255, GhostStart(MovedSpec(MoveStyle::Crossfade, old, now), Look{ live.place, 400 })->opacity, L"clamped");
        }

        TEST_METHOD(OnlyACrossfadeHasAGhost)
        {
            const StageLayouts::LRect old(0, 0, 500, 800), now(500, 0, 1000, 800);
            Assert::IsFalse(GhostStart(MovedSpec(MoveStyle::Slide, old, now), std::nullopt).has_value());
            Assert::IsFalse(GhostStart(MovedSpec(MoveStyle::Slide, old, now), Look{}).has_value());
            Spec fadeIn{ Kind::FadeIn, now, now };
            Assert::IsFalse(GhostStart(fadeIn, std::nullopt).has_value());
        }

        TEST_METHOD(AGhostFadesOutFromItsStartOpacity)
        {
            Assert::AreEqual(OpacityByte(Kind::FadeOut, 0), GhostOpacityByte(255, 0));
            Assert::AreEqual(OpacityByte(Kind::FadeOut, 0.4), GhostOpacityByte(255, 0.4), L"from opaque: the plain fade out");
            Assert::AreEqual(128, GhostOpacityByte(128, 0));
            Assert::AreEqual(64, GhostOpacityByte(128, 0.5));
            Assert::AreEqual(0, GhostOpacityByte(128, 1));
            Assert::AreEqual(0, GhostOpacityByte(-5, 0.2));
        }

        TEST_METHOD(GhostsAreRegisteredBeforeEveryLiveSprite)
        {
            const StageLayouts::LRect a(0, 0, 500, 800), b(500, 0, 1000, 800);
            const std::vector<Spec> specs{ Spec{}, MovedSpec(MoveStyle::Crossfade, a, b), Spec{}, MovedSpec(MoveStyle::Crossfade, b, a) };
            const auto order = RegistrationOrder(specs);
            Assert::AreEqual(static_cast<size_t>(6), order.size());
            Assert::IsTrue(order[0].ghost && order[0].index == 1);
            Assert::IsTrue(order[1].ghost && order[1].index == 3);
            for (size_t i = 2; i < order.size(); ++i)
            {
                Assert::IsFalse(order[i].ghost);
                Assert::AreEqual(i - 2, order[i].index, L"the live sprites keep the z-order");
            }
        }

        TEST_METHOD(ASlideHoldRegistersOnlyLiveSpritesInZOrder)
        {
            const StageLayouts::LRect a(0, 0, 500, 800), b(500, 0, 1000, 800);
            const std::vector<Spec> specs{ MovedSpec(MoveStyle::Slide, a, b), Spec{ Kind::FadeIn, b, b }, Spec{} };
            const auto order = RegistrationOrder(specs);
            Assert::AreEqual(static_cast<size_t>(3), order.size());
            for (size_t i = 0; i < order.size(); ++i)
            {
                Assert::IsFalse(order[i].ghost);
                Assert::AreEqual(i, order[i].index);
            }
        }

        TEST_METHOD(ADemotedWindowOnlyAnimatesWithAPromotionThatGlidesAndNoSkip)
        {
            Candidate d;
            d.visible = true;
            Assert::IsTrue(DemoteAnimates(On(), d, true));
            Assert::IsFalse(DemoteAnimates(On(), d, false), L"no glide for the promoted window: the stage just moves");
            Assert::IsFalse(DemoteAnimates(On(), d, true, false), L"kill switch");
            Context off = On();
            off.setting = false;
            Assert::IsFalse(DemoteAnimates(off, d, true), L"Animations off");
            Context drag = On();
            drag.dragging = true;
            Assert::IsFalse(DemoteAnimates(drag, d, true));
            Context area = On();
            area.areaChanged = true;
            Assert::IsFalse(DemoteAnimates(area, d, true));
            Context shutdown = On();
            shutdown.shuttingDown = true;
            Assert::IsFalse(DemoteAnimates(shutdown, d, true));
            const auto check = [&](auto set) {
                Candidate w = d;
                set(w);
                Assert::IsFalse(DemoteAnimates(On(), w, true));
            };
            check([](Candidate& w) { w.wasParked = true; });
            check([](Candidate& w) { w.visible = false; });
            check([](Candidate& w) { w.minimized = true; });
            check([](Candidate& w) { w.cloaked = true; });
            check([](Candidate& w) { w.maximized = true; });
            check([](Candidate& w) { w.fullscreen = true; });
            check([](Candidate& w) { w.changesMonitor = true; });
        }

        TEST_METHOD(ExtraHoldsOnlyWhenTheSpriteStartsBehindTheMaster)
        {
            Assert::IsTrue(NeedsExtraHolds(PromoteStyle::Grow, true, false), L"no tile: it starts behind the master");
            Assert::IsFalse(NeedsExtraHolds(PromoteStyle::Grow, true, true), L"it starts on the strip");
            Assert::IsFalse(NeedsExtraHolds(PromoteStyle::Grow, false, false));
            Assert::IsFalse(NeedsExtraHolds(PromoteStyle::Crossfade, true, false), L"it never leaves its slot");
        }

        TEST_METHOD(OpacityFollowsTheFadeAndStaysInRange)
        {
            Assert::AreEqual(0.0, Opacity(Kind::FadeIn, 0.0));
            Assert::AreEqual(0.5, Opacity(Kind::FadeIn, 0.5));
            Assert::AreEqual(1.0, Opacity(Kind::FadeIn, 1.0));
            Assert::AreEqual(1.0, Opacity(Kind::FadeOut, 0.0));
            Assert::AreEqual(0.25, Opacity(Kind::FadeOut, 0.75));
            Assert::AreEqual(0.0, Opacity(Kind::FadeOut, 1.0));
            Assert::AreEqual(1.0, Opacity(Kind::Travel, 0.5), L"a moving sprite is opaque");
            Assert::AreEqual(0.0, Opacity(Kind::FadeIn, -3.0));
            Assert::AreEqual(1.0, Opacity(Kind::FadeIn, 9.0));
            Assert::AreEqual(0, OpacityByte(Kind::FadeIn, 0.0), L"a fade in starts invisible");
            Assert::AreEqual(255, OpacityByte(Kind::FadeOut, 0.0));
            Assert::AreEqual(255, OpacityByte(Kind::FadeIn, 1.0));
            Assert::AreEqual(128, OpacityByte(Kind::FadeIn, 0.5));
        }
    };
}
