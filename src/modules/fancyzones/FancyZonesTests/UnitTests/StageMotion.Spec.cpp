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
            Assert::IsTrue(ShouldGlide(On(), Mover()));
        }

        TEST_METHOD(NothingGlidesWithTheSettingOffOrTheKillSwitch)
        {
            Context c = On();
            c.setting = false;
            Assert::IsFalse(ShouldGlide(c, Mover()));
            Assert::IsFalse(ShouldGlide(On(), Mover(), false));
        }

        TEST_METHOD(ParkedWindowsAndWindowsThatStayPutDoNot)
        {
            Candidate parked = Mover();
            parked.landsDocked = false;
            Assert::IsFalse(ShouldGlide(On(), parked));
            Candidate still = Mover();
            still.moves = false;
            Assert::IsFalse(ShouldGlide(On(), still));
        }

        TEST_METHOD(AWindowOutOfTheStripMovesAtOnceUnlessTheFlagSaysGlide)
        {
            Candidate promoted = Mover();
            promoted.wasParked = true;
            Assert::IsFalse(ShouldGlide(On(), promoted), L"the default: no flash over the master");
            Assert::IsFalse(AnimateFromParked);
            Assert::IsTrue(ShouldGlide(On(), promoted, Enabled, true));
            Assert::IsTrue(ShouldGlide(On(), Mover()), L"a window that was docked is not affected");
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
                Assert::IsFalse(ShouldGlide(On(), w));
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
            Assert::IsFalse(ShouldGlide(drag, Mover()));
            Assert::IsFalse(ShouldGlide(area, Mover()));
            Assert::IsFalse(ShouldGlide(shutdown, Mover()));
        }

        TEST_METHOD(GlidersPicksTheMoversInOrder)
        {
            Candidate parked = Mover();
            parked.landsDocked = false;
            Candidate still = Mover();
            still.moves = false;
            const std::vector<Candidate> windows{ Mover(), parked, still, Mover() };
            const auto picked = Gliders(On(), windows);
            Assert::AreEqual(size_t{ 2 }, picked.size());
            Assert::AreEqual(size_t{ 0 }, picked[0]);
            Assert::AreEqual(size_t{ 3 }, picked[1]);
            Assert::AreEqual(size_t{ 0 }, Gliders(On(), windows, false).size());
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
    };
}
