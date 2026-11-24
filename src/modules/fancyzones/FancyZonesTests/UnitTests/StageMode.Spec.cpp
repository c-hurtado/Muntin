#include "pch.h"

#include <climits>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

#include <FancyZonesLib/Arrange.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/StageAdopt.h>
#include <FancyZonesLib/StageConfig.h>
#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StagePark.h>
#include <FancyZonesLib/StageState.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pure parts of Stage mode (Phase 3): state file, restore planning, clamping, adoption filter, dialog guard, parking, persisted
// choices, the hands-off registry. Files only in a temp folder of their own: never the real Muntin data folder, never SaveData.

namespace
{
    using StageLayouts::LRect;

    // Folder of its own, gone at the end of the test
    struct TempFolder
    {
        std::filesystem::path path;
        TempFolder()
        {
            path = std::filesystem::temp_directory_path() / (L"muntin-stage-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(path);
        }

        ~TempFolder()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    // Any temp file left in the folder of the state file?
    bool HasTempFiles(const std::filesystem::path& folder)
    {
        for (const auto& item : std::filesystem::recursive_directory_iterator(folder))
        {
            if (item.path().extension() == L".tmp")
            {
                return true;
            }
        }

        return false;
    }

    StageState::Entry MakeEntry(std::int64_t hwnd, std::uint32_t pid, const std::wstring& cls)
    {
        StageState::Entry e;
        e.hwnd = hwnd;
        e.pid = pid;
        e.rect = LRect(-1920, 40, -640, 900);
        e.showCmd = 3;
        e.flags = 2;
        e.minX = -1;
        e.minY = -1;
        e.maxX = -32000;
        e.maxY = -32000;
        e.normal = LRect(100, 120, 900, 700);
        e.cls = cls;
        return e;
    }

    StageState::Probe FakeProbe()
    {
        // 10: fine, 11: gone, 12: handle reused by another process, 13: class changed, 14: not responding
        return {
            [](std::int64_t h) { return h != 11; },
            [](std::int64_t h) { return static_cast<std::uint32_t>(h == 12 ? 999 : 5); },
            [](std::int64_t h) { return std::wstring(h == 13 ? L"Other" : L"Notepad"); },
            [](std::int64_t h) { return h == 14; },
        };
    }

    StageAdopt::AdoptInfo Plain()
    {
        StageAdopt::AdoptInfo i;
        i.cls = L"Notepad";
        return i;
    }
}

namespace FancyZonesUnitTests
{
    TEST_CLASS (StageModeUnitTests)
    {
        TEST_METHOD_CLEANUP(Cleanup)
        {
            Stage::SetHeld({});
            FancyZonesSettings::instance().SetSettings(Settings{});
        }

        TEST_METHOD (StateSerializeParseRoundTrip)
        {
            const std::vector<StageState::Entry> entries = { MakeEntry(1234, 77, L"Notepad"), MakeEntry(-5, 8, L"Chrome_WidgetWin_1"), MakeEntry(99, 3, L"Class With Spaces é") };
            const auto parsed = StageState::Parse(StageState::Serialize(entries));
            Assert::AreEqual(entries.size(), parsed.size());
            for (size_t i = 0; i < entries.size(); i++)
            {
                Assert::IsTrue(entries[i] == parsed[i], std::to_wstring(i).c_str());
            }
        }

        TEST_METHOD (StateParseSkipsBrokenLinesAndForeignFiles)
        {
            Assert::IsTrue(StageState::Parse(L"").empty());
            Assert::IsTrue(StageState::Parse(L"not our file\n1 2 3\n").empty(), L"no header");
            const auto good = StageState::Serialize({ MakeEntry(1, 2, L"A") });
            const auto text = good + L"garbage line\n1 2 3\n" + StageState::Serialize({ MakeEntry(7, 8, L"B") }).substr(wcslen(StageState::Header) + 1);
            const auto parsed = StageState::Parse(text);
            Assert::AreEqual(size_t{ 2 }, parsed.size(), L"half-written lines are skipped, the rest restores");
            Assert::AreEqual(std::int64_t{ 7 }, parsed[1].hwnd);

            // CRLF line ends (a file touched by an editor)
            std::wstring crlf;
            for (const wchar_t c : good)
            {
                crlf += c == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, c);
            }

            const auto fromCrlf = StageState::Parse(crlf);
            Assert::AreEqual(size_t{ 1 }, fromCrlf.size());
            Assert::AreEqual(std::wstring(L"A"), fromCrlf[0].cls);
        }

        TEST_METHOD (StateFileIsWrittenAtomicallyAndRemovedWhenEmpty)
        {
            TempFolder folder;
            const auto file = folder.path / L"sub" / L"stage-state.txt";
            const std::vector<StageState::Entry> one = { MakeEntry(10, 5, L"Notepad") };
            Assert::IsTrue(StageState::Write(file, one), L"creates the folder");
            Assert::IsTrue(std::filesystem::exists(file));
            Assert::IsFalse(HasTempFiles(folder.path), L"the temp file was renamed over the target");
            Assert::IsTrue(StageState::Read(file).front() == one.front());

            const std::vector<StageState::Entry> two = { MakeEntry(10, 5, L"Notepad"), MakeEntry(20, 5, L"über") };
            Assert::IsTrue(StageState::Write(file, two));
            Assert::AreEqual(size_t{ 2 }, StageState::Read(file).size(), L"replaced");
            Assert::IsTrue(StageState::Read(file)[1] == two[1], L"UTF-8 round trip");
            Assert::IsFalse(HasTempFiles(folder.path));
            Assert::IsTrue(StageState::TempName(file).find(std::to_wstring(GetCurrentProcessId())) != std::wstring::npos, L"the temp name is this process's own");

            Assert::IsTrue(StageState::Write(file, {}), L"nothing to restore: the file goes");
            Assert::IsFalse(std::filesystem::exists(file));
            Assert::IsTrue(StageState::Read(file).empty());
        }

        TEST_METHOD (StateWriteFailureIsReported)
        {
            TempFolder folder;
            const auto blocker = folder.path / L"blocker";
            std::ofstream(blocker) << "a file where the folder should be";
            Assert::IsFalse(StageState::Write(blocker / L"stage-state.txt", { MakeEntry(1, 2, L"A") }), L"the caller must not move windows then");
        }

        TEST_METHOD (RestorePlan)
        {
            const std::vector<StageState::Entry> entries = {
                MakeEntry(10, 5, L"Notepad"), MakeEntry(11, 5, L"Notepad"), MakeEntry(12, 5, L"Notepad"), MakeEntry(13, 5, L"Notepad"), MakeEntry(14, 5, L"Notepad")
            };
            const auto plan = StageState::PlanRestore(entries, FakeProbe());
            Assert::IsTrue(plan[0] == StageState::RestoreAction::Restore);
            Assert::IsTrue(plan[1] == StageState::RestoreAction::Skip, L"gone");
            Assert::IsTrue(plan[2] == StageState::RestoreAction::Skip, L"handle reused by another process");
            Assert::IsTrue(plan[3] == StageState::RestoreAction::Skip, L"class changed");
            Assert::IsTrue(plan[4] == StageState::RestoreAction::Keep, L"hung: stays in the file");
        }

        TEST_METHOD (SavedRectUsableOnlyWhileOnAMonitor)
        {
            const std::vector<LRect> monitors = { LRect(0, 0, 1920, 1080), LRect(1920, 0, 3840, 1080) };
            Assert::IsTrue(StageState::RectUsable(LRect(100, 100, 900, 700), monitors));
            Assert::IsTrue(StageState::RectUsable(LRect(1800, 100, 2200, 700), monitors), L"across two monitors");
            Assert::IsFalse(StageState::RectUsable(LRect(5000, 100, 5800, 700), monitors), L"its monitor is gone");
            Assert::IsFalse(StageState::RectUsable(LRect(100, 1079, 900, 1800), monitors), L"a 1 px sliver is not visible");
            Assert::IsFalse(StageState::RectUsable(LRect(10, 10, 10, 50), monitors), L"empty");
            Assert::IsTrue(StageState::RectUsable(LRect(5000, 0, 5800, 700), {}), L"no monitor known: trust the rect");
        }

        TEST_METHOD (MinSizesAreClampedToTheWorkArea)
        {
            Assert::AreEqual(0, StageState::ClampMin(-5, 1000));
            Assert::AreEqual(1000, StageState::ClampMin(INT_MAX, 1000));
            Assert::AreEqual(1000, StageState::ClampMin(1LL << 40, 1000));
            Assert::AreEqual(0, StageState::ClampMin(500, -3), L"a negative limit");
            Assert::AreEqual(0, StageState::VisibleMin(10, 8, 8, 1000), L"smaller than its frame");
            Assert::AreEqual(1000, StageState::VisibleMin(LONG_MAX, 0, 0, 1000), L"never overflows");
            Assert::AreEqual(1000, StageState::VisibleMin(LONG_MAX, INT_MIN, 0, 1000));

            const LRect work(0, 0, 1920, 1040);
            std::vector<StageLayouts::LWin> wins = { { 1, StageLayouts::WinFlags::None, INT_MAX, INT_MAX }, { 2, StageLayouts::WinFlags::None, INT_MAX, 300 },
                                                     { 3, StageLayouts::WinFlags::None, -7, INT_MAX }, { 4, StageLayouts::WinFlags::None, 0, 0 } };
            StageState::ClampMinSizes(wins, work);
            for (const auto& w : wins)
            {
                Assert::IsTrue(w.MinW >= 0 && w.MinW <= work.W());
                Assert::IsTrue(w.MinH >= 0 && w.MinH <= work.H());
            }

            // The layout takes the clamped values and stays within sane bounds (no wrapped sums, no negative sizes)
            StageLayouts::LayoutInput in;
            in.Work = work;
            in.Wins = wins;
            const auto plan = StageLayouts::MakeMaster()->Compute(in);
            Assert::AreEqual(wins.size(), plan.Items.size());
            for (const auto& item : plan.Items)
            {
                if (item.Role == StageLayouts::Role::Main || item.Role == StageLayouts::Role::Docked)
                {
                    Assert::IsTrue(item.Rect.W() > 0 && item.Rect.H() > 0);
                    Assert::IsTrue(item.Rect.W() <= 2 * work.W() && item.Rect.H() <= 8 * work.H());
                }
            }
        }

        TEST_METHOD (AdoptionFilter)
        {
            Assert::IsTrue(StageAdopt::IsEligible(Plain()));

            const auto reject = [](const wchar_t* why, auto change) {
                auto i = Plain();
                change(i);
                Assert::IsFalse(StageAdopt::IsEligible(i), why);
            };
            reject(L"not a window", [](auto& i) { i.isWindow = false; });
            reject(L"child window", [](auto& i) { i.topLevel = false; });
            reject(L"invisible", [](auto& i) { i.visible = false; });
            reject(L"minimized", [](auto& i) { i.iconic = true; });
            reject(L"owned (a dialog)", [](auto& i) { i.hasOwner = true; });
            reject(L"hung", [](auto& i) { i.hung = true; });
            reject(L"tool window", [](auto& i) { i.exStyle = WS_EX_TOOLWINDOW; });
            reject(L"no-activate", [](auto& i) { i.exStyle = WS_EX_NOACTIVATE; });
            reject(L"topmost", [](auto& i) { i.exStyle = WS_EX_TOPMOST; });
            reject(L"not resizable", [](auto& i) { i.style = WS_POPUP | WS_CAPTION; });
            reject(L"cloaked", [](auto& i) { i.cloaked = true; });
            reject(L"Muntin's own", [](auto& i) { i.ownProcess = true; });
            reject(L"no title", [](auto& i) { i.emptyTitle = true; });
            reject(L"shell class", [](auto& i) { i.cls = L"Shell_TrayWnd"; });
            reject(L"UWP frame", [](auto& i) { i.cls = L"ApplicationFrameWindow"; });
            reject(L"no monitor with a stage", [](auto& i) { i.monitorKnown = false; });
            reject(L"too narrow", [](auto& i) { i.width = 99; });
            reject(L"too short", [](auto& i) { i.height = 59; });
            reject(L"looks parked already", [](auto& i) { i.onScreenPx = 3; });
            reject(L"parked by the scroll or arranged by StageProto", [](auto& i) { i.heldElsewhere = true; });
            reject(L"elevated", [](auto& i) { i.elevatedBlocked = true; });

            auto edge = Plain();
            edge.width = 100;
            edge.height = 60;
            edge.onScreenPx = 4;
            Assert::IsTrue(StageAdopt::IsEligible(edge), L"the limits themselves are fine");
        }

        TEST_METHOD (DialogGuard)
        {
            const auto h = [](int v) { return reinterpret_cast<HWND>(static_cast<intptr_t>(v)); };
            StageAdopt::DialogCandidate modal{ h(2), h(1), true, false, false, false, false }; // owner disabled
            StageAdopt::DialogCandidate modeless = modal;
            modeless.window = h(4);
            modeless.owner = h(3);
            modeless.ownerEnabled = true; // a Find box: the owner stays enabled
            StageAdopt::DialogCandidate tooltip = modal;
            tooltip.window = h(6);
            tooltip.owner = h(5);
            tooltip.toolOrNoActivate = true;
            StageAdopt::DialogCandidate hidden = modal;
            hidden.window = h(8);
            hidden.owner = h(7);
            hidden.visible = false;
            StageAdopt::DialogCandidate own = modal;
            own.window = h(10);
            own.owner = h(9);
            own.ownProcess = true;
            StageAdopt::DialogCandidate cloaked = modal;
            cloaked.window = h(12);
            cloaked.owner = h(11);
            cloaked.cloaked = true;

            const auto owners = StageAdopt::DialogOwners({ modal, modeless, tooltip, hidden, own, cloaked });
            Assert::AreEqual(size_t{ 1 }, owners.size());
            Assert::IsTrue(owners.contains(h(1)));
        }

        TEST_METHOD (ParkingAsASliverAtTheBottomOfTheMonitor)
        {
            const LRect monitor(0, 0, 1920, 1080);
            const std::vector<LRect> monitors = { monitor };
            const LRect master(0, 0, 1152, 1040);
            const auto park = StagePark::Park(master, { 7, 0, 7, 7 }, monitor, monitors);
            Assert::IsTrue(park.sliver);
            Assert::AreEqual(-7, park.outer.L);
            Assert::AreEqual(1079, park.outer.T, L"one pixel left on the monitor");
            Assert::AreEqual(1152 + 14, park.outer.W(), L"same size as the master: the app relayouts once");
            Assert::AreEqual(1040 + 7, park.outer.H());
            Assert::IsTrue(StagePark::LooksParked(park.outer, monitor));
        }

        TEST_METHOD (ParkingNeverReachesAnotherMonitor)
        {
            const LRect upper(0, 0, 1920, 1080), lower(0, 1080, 1920, 2160);
            const std::vector<LRect> monitors = { upper, lower };
            const LRect master(100, 0, 1000, 1040);
            const auto below = StagePark::Park(master, {}, upper, monitors);
            Assert::IsFalse(below.sliver, L"a sliver would show on the monitor below");
            Assert::IsTrue(below.outer.Same(master), L"it waits behind the master instead");

            const auto bottom = StagePark::Park(master, {}, lower, monitors);
            Assert::IsTrue(bottom.sliver, L"nothing below the lower monitor");
            Assert::AreEqual(2159, bottom.outer.T);

            const LRect right(1920, 0, 3840, 1080);
            Assert::IsTrue(StagePark::Park(master, {}, upper, { upper, right }).sliver, L"a monitor on the side does not block");
        }

        TEST_METHOD (OuterRectStopsAtSharedMonitorEdges)
        {
            const LRect left(0, 0, 1920, 1080), right(1920, 0, 3840, 1080);
            const StagePark::Insets ins{ 7, 0, 7, 7 };
            const auto alone = StagePark::OuterRect(LRect(100, 100, 900, 700), ins, left, { left, right });
            Assert::IsTrue(alone.Same(LRect(93, 100, 907, 707)));
            const auto edge = StagePark::OuterRect(LRect(1000, 0, 1920, 1040), ins, left, { left, right });
            Assert::AreEqual(1920, edge.R, L"the invisible border does not spill onto the other monitor");
            Assert::AreEqual(993, edge.L);
            const auto single = StagePark::OuterRect(LRect(1000, 0, 1920, 1040), ins, left, { left });
            Assert::AreEqual(1927, single.R);
        }

        TEST_METHOD (HeldRegistryAndHandsOff)
        {
            const HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr); // Never shown
            Assert::IsNotNull(window);
            Assert::IsFalse(Stage::IsHeld(window));
            Stage::SetHeld({ window });
            Assert::IsTrue(Stage::IsHeld(window));
            Assert::IsFalse(Stage::IsHeld(nullptr));

            Assert::IsFalse(Arrange::HandsOff(window), L"dock mode: a held window is not hands-off");
            Settings stage;
            stage.arrangeMode = 1;
            FancyZonesSettings::instance().SetSettings(stage);
            Assert::IsTrue(Arrange::HandsOff(window), L"stage mode: the stage's windows are left alone");

            Stage::SetHeld({});
            Assert::IsFalse(Stage::IsHeld(window));
            Assert::IsFalse(Arrange::HandsOff(window));
            DestroyWindow(window);
        }

        TEST_METHOD (PersistedChoicesRoundTrip)
        {
            Settings s;
            s.stageLayout = L"master";
            s.stagePairCut = 420;
            s.stageMasterCut = 700;
            s.stageRows = L"500,300,200";
            const auto values = StageConfig::FromSettings(s);
            Assert::AreEqual(std::wstring(L"master"), values.layout);
            Assert::AreEqual(700, values.masterCut);

            auto layout = StageConfig::MakeLayout(values);
            Assert::AreEqual(std::string("master"), layout->Name());
            Assert::AreEqual(0.7, layout->Cut(), 1e-9);
            Assert::AreEqual(size_t{ 3 }, layout->Rows().size());
            Assert::IsTrue(StageConfig::Read(*layout, values) == values, L"nothing changed, nothing to save");

            layout->SetCut(0.55);
            layout->SetRows({ 0.4, 0.6 });
            const auto after = StageConfig::Read(*layout, values);
            Assert::AreEqual(550, after.masterCut);
            Assert::AreEqual(420, after.pairCut, L"the other layout's cut is left alone");
            Assert::AreEqual(std::wstring(L"400,600"), after.rows);

            const auto changes = StageConfig::Changes(values, after);
            Assert::AreEqual(size_t{ 2 }, changes.size());
            Assert::AreEqual(std::wstring(NonLocalizable::StageMasterCutID), std::wstring(changes[0].id));
            Assert::IsFalse(changes[0].text);
            Assert::AreEqual(550, changes[0].number);
            Assert::AreEqual(std::wstring(NonLocalizable::StageRowsID), std::wstring(changes[1].id));
            Assert::IsTrue(changes[1].text);
            Assert::AreEqual(std::wstring(L"400,600"), changes[1].textValue);
            Assert::IsTrue(StageConfig::Changes(values, values).empty());

            auto renamed = values;
            renamed.layout = L"pair";
            const auto layoutChange = StageConfig::Changes(values, renamed);
            Assert::AreEqual(size_t{ 1 }, layoutChange.size());
            Assert::AreEqual(std::wstring(NonLocalizable::StageLayoutID), std::wstring(layoutChange[0].id));
        }

        TEST_METHOD (PersistedValuesAreSane)
        {
            Assert::AreEqual(150, StageConfig::ToPermille(0.0));
            Assert::AreEqual(850, StageConfig::ToPermille(7.0));
            Assert::AreEqual(333, StageConfig::ToPermille(1.0 / 3));

            StageConfig::Values values;
            values.rows = L"garbage";
            Assert::IsTrue(StageConfig::MakeLayout(values)->Rows().empty(), L"bad rows: equal heights");

            // Phase 3 draws no strip: whatever is configured, the layout that runs is the one without one
            values.layout = L"stage";
            Assert::AreEqual(std::string("master"), StageConfig::MakeLayout(values)->Name());
        }

        TEST_METHOD (VerifyNeedsMoreThanTheToleranceSeenTwice)
        {
            StageState::Pending pending;
            auto v = StageState::VerifyLanding(1002, 800, 1000, 800, pending);
            Assert::IsFalse(v.learnW || v.learnH || v.lookAgain, L"2 px bigger is within the tolerance");

            v = StageState::VerifyLanding(1003, 800, 1000, 800, pending);
            Assert::IsFalse(v.learnW);
            Assert::IsTrue(v.lookAgain, L"seen once: look again");
            v = StageState::VerifyLanding(1003, 800, 1000, 800, pending);
            Assert::IsTrue(v.learnW && !v.learnH && !v.lookAgain, L"seen twice: it is the minimum");

            v = StageState::VerifyLanding(1100, 800, 1000, 800, pending);
            v = StageState::VerifyLanding(1200, 800, 1000, 800, pending);
            Assert::IsFalse(v.learnW, L"a size that keeps changing (a slow move) is not believed");
            Assert::IsTrue(v.lookAgain);

            v = StageState::VerifyLanding(1000, 800, 1000, 800, pending);
            Assert::IsFalse(v.lookAgain, L"it fits now: the pending size is forgotten");
            v = StageState::VerifyLanding(1100, 800, 1000, 800, pending);
            Assert::IsTrue(v.lookAgain && !v.learnW, L"so one more sighting does not count as the second");

            v = StageState::VerifyLanding(1100, 900, 1000, 800, pending);
            v = StageState::VerifyLanding(1100, 900, 1000, 800, pending);
            Assert::IsTrue(v.learnW && v.learnH, L"both directions");
        }

        TEST_METHOD (CapacityRule)
        {
            Assert::AreEqual(8, StageConfig::MasterMaxStack);
            StageConfig::Values values;
            const auto master = StageConfig::MakeLayout(values);
            Assert::AreEqual(9, StageConfig::CapacityOf(*master), L"master + a stack of 8");
            Assert::AreEqual((std::numeric_limits<int>::max)(), StageConfig::CapacityOf(*StageLayouts::MakeStage()), L"a strip shows every window");

            Assert::IsTrue(StageConfig::HasRoom(0, 9));
            Assert::IsTrue(StageConfig::HasRoom(8, 9));
            Assert::IsFalse(StageConfig::HasRoom(9, 9), L"the tenth window is not taken");
            Assert::IsFalse(StageConfig::HasRoom(0, 0));
            Assert::IsTrue(StageConfig::HasRoom(1000, (std::numeric_limits<int>::max)()));

            // Everything that is taken is shown: with 9 windows none is Hidden (parked)
            StageLayouts::LayoutInput in;
            in.Work = LRect(0, 0, 3000, 2000);
            for (int i = 1; i <= 9; i++)
            {
                in.Wins.emplace_back(i);
            }

            for (const auto& item : master->Compute(in).Items)
            {
                Assert::IsTrue(item.Role != StageLayouts::Role::Hidden && !item.Rect.Same(LRect()));
            }
        }

        TEST_METHOD (PlacementFlags)
        {
            constexpr int async = 4, toMaximized = 2;
            auto maximized = MakeEntry(1, 2, L"A");
            maximized.showCmd = 3;
            maximized.flags = 0;
            auto normal = MakeEntry(1, 2, L"A");
            normal.showCmd = 1;
            normal.flags = 0;

            auto p = StageState::PlacementFor(normal, false, false);
            Assert::AreEqual(1, p.showCmd);
            Assert::AreEqual(async, p.flags, L"always async");
            p = StageState::PlacementFor(maximized, false, false);
            Assert::AreEqual(3, p.showCmd);
            Assert::AreEqual(async, p.flags);
            p = StageState::PlacementFor(maximized, true, false);
            Assert::AreEqual(2, p.showCmd, L"put back minimized");
            Assert::AreEqual(async | toMaximized, p.flags, L"and restores to maximized");
            p = StageState::PlacementFor(normal, true, false);
            Assert::AreEqual(async, p.flags, L"a normal window restores to normal");
            p = StageState::PlacementFor(maximized, false, true);
            Assert::IsTrue(p.usePlacement);
            Assert::AreEqual(0, p.showCmd, L"stays hidden");
            Assert::AreEqual(async | toMaximized, p.flags);
            p = StageState::PlacementFor(normal, false, true);
            Assert::IsFalse(p.usePlacement, L"hidden and normal: only the rect goes back");
        }
    };
}
