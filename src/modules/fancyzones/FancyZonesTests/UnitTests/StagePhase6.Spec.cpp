#include "pch.h"

#include <FancyZonesLib/StageAdopt.h>
#include <FancyZonesLib/StageConfig.h>
#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageMonitors.h>
#include <FancyZonesLib/StageState.h>
#include <FancyZonesLib/StageStripPlan.h>
#include <FancyZonesLib/Watchdog.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// Phase 6 (pure parts): minimum-size cut, dialogs of parked windows, monitor matching and exile, watchdog spawn order, strip over a
// fullscreen app, the SaneRect span rule. No windows, nothing on screen, no process is started.

namespace
{
    using StageLayouts::LRect;

    StageLayouts::Plan MasterPlan(int count, int minH)
    {
        StageLayouts::MasterLayout layout;
        StageLayouts::LayoutInput input;
        input.Work = LRect(0, 0, 2560, 1400);
        input.Spacing = StageLayouts::Spacing(8, 8, 8, 8, 8);
        for (int i = 1; i <= count; i++)
        {
            input.Wins.emplace_back(i, StageLayouts::WinFlags::None, 400, i == 1 ? 0 : minH);
        }

        return layout.Compute(input);
    }
}

namespace StagePhase6UnitTests
{
    TEST_CLASS(StagePhase6UnitTests)
    {
    public:
        TEST_METHOD(MinSizeCutTakesTheWindowsTheStackHasNoRoomFor)
        {
            // 8 stack windows that each need 400 px of height cannot fit in 1400 px: some are Hidden
            const auto plan = MasterPlan(9, 400);
            const auto cut = StageState::MinSizeCut(plan);
            Assert::IsFalse(cut.empty());
            for (const int id : cut)
            {
                const auto item = std::find_if(plan.Items.begin(), plan.Items.end(), [&](const auto& i) { return i.Id == id; });
                Assert::IsTrue(item->Role == StageLayouts::Role::Hidden);
            }

            Assert::AreEqual(9, cut.front(), L"the last in the order goes first");
            Assert::IsTrue(std::find(cut.begin(), cut.end(), 1) == cut.end(), L"the master is never cut");
        }

        TEST_METHOD(NothingIsCutWhenEverythingFits)
        {
            Assert::IsTrue(StageState::MinSizeCut(MasterPlan(5, 100)).empty());
            Assert::IsTrue(StageState::MinSizeCut(MasterPlan(1, 100)).empty());
        }

        TEST_METHOD(KeptWindowsAreNotCut)
        {
            const auto plan = MasterPlan(9, 400);
            const auto all = StageState::MinSizeCut(plan);
            const auto kept = StageState::MinSizeCut(plan, [&](int id) { return id == all.front(); });
            Assert::AreEqual(all.size() - 1, kept.size());
            Assert::IsTrue(std::find(kept.begin(), kept.end(), all.front()) == kept.end());
        }

        TEST_METHOD(LayoutsWithAStripNeverCut)
        {
            StageLayouts::StageLayout stage;
            StageLayouts::LayoutInput input;
            input.Work = LRect(0, 0, 2560, 1400);
            for (int i = 1; i <= 12; i++)
            {
                input.Wins.emplace_back(i, StageLayouts::WinFlags::None, 900, 900);
            }

            const auto plan = stage.Compute(input);
            Assert::IsTrue(plan.HasStrip);
            Assert::IsTrue(StageState::MinSizeCut(plan).empty());
        }

        TEST_METHOD(ACutWindowIsSkippedByAdoptionUntilRoomAppears)
        {
            StageAdopt::AdoptInfo info;
            info.cls = L"Notepad";
            Assert::IsTrue(StageAdopt::IsEligible(info));
            info.cutLoose = true;
            Assert::IsFalse(StageAdopt::IsEligible(info));
            Assert::AreEqual(std::wstring(L"cut by minimum sizes (left floating until there is room)"), std::wstring(StageAdopt::SkipReason(info)));
        }

        TEST_METHOD(AParkedWindowWithAModalDialogComesForward)
        {
            const HWND a = reinterpret_cast<HWND>(0x10), b = reinterpret_cast<HWND>(0x20), c = reinterpret_cast<HWND>(0x30), d = reinterpret_cast<HWND>(0x40);
            // a: parked, owns a modal dialog. b: parked, no dialog. c: not parked (docked). d: parked but already guarded.
            const std::vector<StageAdopt::ParkedFacts> facts{ { a, true, false, false }, { b, true, false, false }, { c, false, false, false }, { d, true, true, false } };
            const std::set<HWND> owners{ a, c, d };
            const auto front = StageAdopt::NeedsFront(facts, owners);
            Assert::AreEqual(size_t{ 1 }, front.size());
            Assert::IsTrue(front[0] == a);

            const std::vector<StageAdopt::ParkedFacts> minimized{ { a, true, false, true } };
            Assert::IsTrue(StageAdopt::NeedsFront(minimized, owners).empty(), L"a minimized window is not promoted");
        }

        TEST_METHOD(DialogOwnersNeedADisabledOwner)
        {
            // The existing rule the promote relies on: only a modal dialog (owner disabled) counts
            const HWND owner = reinterpret_cast<HWND>(0x10);
            StageAdopt::DialogCandidate modal;
            modal.window = reinterpret_cast<HWND>(0x11);
            modal.owner = owner;
            modal.visible = true;
            modal.ownerEnabled = false;
            auto modeless = modal;
            modeless.ownerEnabled = true;
            Assert::AreEqual(size_t{ 1 }, StageAdopt::DialogOwners({ modal }).size());
            Assert::AreEqual(size_t{ 0 }, StageAdopt::DialogOwners({ modeless }).size());
        }

        TEST_METHOD(StagesFindTheirMonitorByKeyAfterAResolutionChange)
        {
            using StageMonitors::Id;
            const std::vector<Id> old{ { L"DEL_A", LRect(0, 0, 2560, 1440) }, { L"GSM_B", LRect(2560, 0, 4480, 1080) } };
            // Both change resolution, the second one moves: the keys still match, and in the order the monitors are enumerated now
            const std::vector<Id> next{ { L"GSM_B", LRect(-1920, 0, 0, 1080) }, { L"DEL_A", LRect(0, 0, 1920, 1080) } };
            const auto match = StageMonitors::MatchStages(old, next);
            Assert::AreEqual(1, match[0]);
            Assert::AreEqual(0, match[1]);
            Assert::IsTrue(StageMonitors::GoneStages(old.size(), match).empty());
        }

        TEST_METHOD(AMonitorThatIsGoneIsReportedAndOneThatCameIsNew)
        {
            using StageMonitors::Id;
            const std::vector<Id> old{ { L"DEL_A", LRect(0, 0, 2560, 1440) }, { L"GSM_B", LRect(2560, 0, 4480, 1080) } };
            const std::vector<Id> next{ { L"DEL_A", LRect(0, 0, 2560, 1440) } };
            const auto match = StageMonitors::MatchStages(old, next);
            Assert::AreEqual(0, match[0]);
            const auto gone = StageMonitors::GoneStages(old.size(), match);
            Assert::AreEqual(size_t{ 1 }, gone.size());
            Assert::AreEqual(1, gone[0]);

            const std::vector<Id> back{ { L"DEL_A", LRect(0, 0, 2560, 1440) }, { L"NEW_C", LRect(2560, 0, 4480, 1080) } };
            const auto m2 = StageMonitors::MatchStages(old, back);
            Assert::AreEqual(1, m2[1], L"another monitor in the place of the one that went: same rect, it takes over the stage");
        }

        TEST_METHOD(AMonitorKnownOnlyByItsPlaceMatchesByOriginAfterAResize)
        {
            using StageMonitors::Id;
            const std::vector<Id> old{ { L"@0,0", LRect(0, 0, 2560, 1440) } };
            const std::vector<Id> next{ { L"DEL_A", LRect(0, 0, 1920, 1080) } };
            Assert::AreEqual(0, StageMonitors::MatchStages(old, next)[0]);

            // Two real keys at the same origin are two monitors: no match by origin
            const std::vector<Id> realOld{ { L"DEL_A", LRect(0, 0, 2560, 1440) } };
            const std::vector<Id> realNext{ { L"GSM_B", LRect(0, 0, 1920, 1080) } };
            Assert::AreEqual(-1, StageMonitors::MatchStages(realOld, realNext)[0]);
        }

        TEST_METHOD(ExiledWindowsReturnToTheirMonitorOnce)
        {
            StageMonitors::Exiles exiles;
            StageMonitors::Exile(exiles, { 1, 2 }, L"GSM_B");
            StageMonitors::Exile(exiles, { 2, 3 }, L"OTHER");
            Assert::AreEqual(size_t{ 3 }, exiles.size());
            Assert::AreEqual(std::wstring(L"GSM_B"), exiles.at(2), L"the first monitor wins");

            auto back = StageMonitors::TakeReturning(exiles, L"GSM_B");
            std::sort(back.begin(), back.end());
            Assert::AreEqual(size_t{ 2 }, back.size());
            Assert::IsTrue(back[0] == 1);
            Assert::AreEqual(size_t{ 1 }, exiles.size());
            Assert::IsTrue(StageMonitors::TakeReturning(exiles, L"GSM_B").empty(), L"only once");
        }

        TEST_METHOD(AMonitorKnownOnlyByItsPlaceIsNotRemembered)
        {
            StageMonitors::Exiles exiles;
            StageMonitors::Exile(exiles, { 1 }, L"@2560,0");
            Assert::IsTrue(exiles.empty());
            Assert::IsTrue(StageMonitors::TakeReturning(exiles, L"@2560,0").empty());
        }

        TEST_METHOD(ASizeOrScaleChangeMakesMinimumSizesStale)
        {
            const LRect a(0, 0, 2560, 1440), b(0, 0, 1920, 1080);
            Assert::IsTrue(StageMonitors::SizeChanged(a, b));
            Assert::IsFalse(StageMonitors::SizeChanged(a, a, 144, 144));
            Assert::IsTrue(StageMonitors::SizeChanged(a, a, 96, 144), L"same pixels, another scale");
            Assert::IsFalse(StageMonitors::SizeChanged(a, a, 0, 144), L"unknown scale: not a change");
        }

        TEST_METHOD(WatchdogPrefersTheShellAsParentAndFallsBack)
        {
            const auto normal = Watchdog::SpawnAttempts(true, false);
            Assert::AreEqual(size_t{ 4 }, normal.size());
            Assert::IsTrue(normal[0].shellParent && normal[0].breakaway);
            Assert::IsTrue(normal[1].shellParent && !normal[1].breakaway);
            Assert::IsTrue(!normal[2].shellParent && normal[2].breakaway);
            Assert::IsTrue(!normal[3].shellParent && !normal[3].breakaway);

            // Elevated: the child would take the shell's (lower) token, so the old way only
            const auto elevated = Watchdog::SpawnAttempts(true, true);
            Assert::AreEqual(size_t{ 2 }, elevated.size());
            Assert::IsTrue(!elevated[0].shellParent && !elevated[1].shellParent);
            Assert::AreEqual(size_t{ 2 }, Watchdog::SpawnAttempts(false, false).size(), L"no shell window: the old way");
        }

        TEST_METHOD(AForeignFullscreenWindowHidesTheStrip)
        {
            const LRect monitor(0, 0, 2560, 1440);
            Assert::IsTrue(StageStripPlan::FullscreenOver(monitor, monitor, false));
            Assert::IsTrue(StageStripPlan::FullscreenOver(LRect(-8, -8, 2568, 1448), monitor, false), L"borderless with an invisible frame");
            Assert::IsFalse(StageStripPlan::FullscreenOver(monitor, monitor, true), L"the desktop fills the monitor and does not count");
            Assert::IsFalse(StageStripPlan::FullscreenOver(LRect(0, 0, 2560, 1400), monitor, false), L"leaves the taskbar free: not fullscreen");
            Assert::IsTrue(StageStripPlan::Suppressed(false, false, true));
            Assert::IsFalse(StageStripPlan::Suppressed(false, false, false));
            Assert::IsTrue(StageStripPlan::Suppressed(true, false));
        }

        TEST_METHOD(AWindowSpanningBothMonitorsIsNotClamped)
        {
            const std::vector<LRect> monitors{ LRect(0, 0, 2560, 1440), LRect(2560, 0, 6400, 2160) };
            // Wider than the biggest monitor (3840) but inside the two together (6400): a real window across both
            const LRect spanning(100, 100, 4100, 1300);
            Assert::IsTrue(StageState::SaneRect(spanning, monitors).Same(spanning));
            const LRect all(0, 0, 6400, 2160);
            Assert::IsTrue(StageState::SaneRect(all, monitors).Same(all));
            // Bigger than both together: still a coordinate mix-up; it goes to the monitor it overlaps most, as wide as that one
            Assert::IsTrue(StageState::SaneRect(LRect(0, 0, 9000, 1300), monitors).Same(LRect(2560, 0, 6400, 1300)));
            // Wider than every monitor, reaching past all of them (the real 5757 px mix-up): clamped
            Assert::IsTrue(StageState::SaneRect(LRect(2561, 10, 8318, 2190), monitors).Same(LRect(2560, 0, 6400, 2160)));
        }

        TEST_METHOD(NoDialogPromoteWhileTheMouseHoldsAWindow)
        {
            Assert::IsFalse(StageAdopt::MayPromoteOwners(true));
            Assert::IsTrue(StageAdopt::MayPromoteOwners(false));
        }

        TEST_METHOD(AMonitorDoesNotInheritARealKeyFromTheSameCorner)
        {
            using StageMonitors::Id;
            const std::vector<Id> previous{ { L"DEL_A", LRect(0, 0, 2560, 1440) }, { L"@2560,0", LRect(2560, 0, 4480, 1080) } };
            Assert::AreEqual(std::wstring(L"DEL_A"), StageMonitors::InheritKey(previous, LRect(0, 0, 2560, 1440)), L"same rect: the same monitor");
            Assert::AreEqual(std::wstring(), StageMonitors::InheritKey(previous, LRect(0, 0, 1920, 1080)), L"same corner, other size: a real key is not handed on");
            Assert::AreEqual(std::wstring(L"@2560,0"), StageMonitors::InheritKey(previous, LRect(2560, 0, 3840, 1080)), L"a place key may follow its corner");
            Assert::AreEqual(std::wstring(), StageMonitors::InheritKey(previous, LRect(5000, 0, 6000, 1080)));
        }

        TEST_METHOD(AnUnidentifiedMonitorStaysUnresolvedSoTheRetryRuns)
        {
            // What RefreshMonitors feeds ChooseKey: no ids, and no inherited real key for another size at the corner
            const std::vector<StageMonitors::Id> previous{ { L"DEL_A", LRect(0, 0, 2560, 1440) } };
            const auto choice = StageConfig::ChooseKey(L"", L"", 0, 0, StageMonitors::InheritKey(previous, LRect(0, 0, 1920, 1080)));
            Assert::IsFalse(choice.resolved);
            Assert::AreEqual(std::wstring(L"@0,0"), choice.key);
        }

        TEST_METHOD(ExileRecordsSurviveACutAndSendTheWindowHomeWhenItIsTakenAgain)
        {
            StageMonitors::Exiles exiles;
            const std::uintptr_t window = 0xABC;
            StageMonitors::Exile(exiles, { window }, L"GSM_B");
            // The window was cut and released (the record is by window handle, nothing erases it); the monitor is away: no target yet
            Assert::AreEqual(-1, StageMonitors::ReturnTarget(exiles, window, { L"DEL_A" }));
            Assert::AreEqual(size_t{ 1 }, exiles.size(), L"the record stays while the monitor is away");
            // The monitor is back and the window is taken again: it goes to that stage, once
            Assert::AreEqual(1, StageMonitors::ReturnTarget(exiles, window, { L"DEL_A", L"GSM_B" }));
            Assert::IsTrue(exiles.empty());
            Assert::AreEqual(-1, StageMonitors::ReturnTarget(exiles, window, { L"DEL_A", L"GSM_B" }));
            // A closed window's record is erased by its handle
            StageMonitors::Exile(exiles, { window }, L"GSM_B");
            exiles.erase(window);
            Assert::IsTrue(exiles.empty());
        }

        TEST_METHOD(ASavedPlaceOnAnUnpluggedMonitorRestoresOntoThePrimaryOne)
        {
            const std::vector<LRect> now{ LRect(0, 0, 2560, 1440) }; // the monitor at 2560,0 is gone
            const LRect saved(3000, 200, 4200, 1000);
            const auto placed = StageState::OnAVisibleMonitor(saved, now);
            Assert::IsTrue(placed.Inside(now[0]));
            Assert::AreEqual(saved.W(), placed.W());
            Assert::AreEqual(saved.H(), placed.H());
            Assert::IsTrue(placed.Same(LRect(32, 32, 1232, 832)));
            // Still on a monitor: untouched
            const LRect here(100, 100, 900, 700);
            Assert::IsTrue(StageState::OnAVisibleMonitor(here, now).Same(here));
            // Bigger than the monitor: slid in as far as it goes, never outside on the left/top
            const auto big = StageState::OnAVisibleMonitor(LRect(3000, 200, 7000, 1900), now);
            Assert::IsTrue(big.L >= 0 && big.T >= 0);
            // The primary is the monitor at 0,0 whatever the order
            Assert::IsTrue(StageState::PrimaryOf({ LRect(2560, 0, 4480, 1080), LRect(0, 0, 2560, 1440) }).Same(LRect(0, 0, 2560, 1440)));
        }
    };
}
