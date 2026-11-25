#include "pch.h"

#include <FancyZonesLib/StageKeys.h>
#include <FancyZonesLib/StageLayouts.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pure parts of the Stage mode keys (Phase 5, keys): key table, last managed foreground, swallow gate, next monitor, and the
// order operations the keys call on the layouts. No windows, no hook, nothing on screen.

namespace
{
    using namespace StageKeys;

    const std::vector<int> Ids{ 1, 2, 3, 4 };

    std::vector<int> Master(std::vector<int> order, bool (StageLayouts::ILayout::*op)(std::vector<int>&, int) const, int arg)
    {
        StageLayouts::MasterLayout layout;
        (layout.*op)(order, arg);
        return order;
    }
}

namespace StageKeysUnitTests
{
    TEST_CLASS(StageKeysUnitTests)
    {
    public:
        TEST_METHOD(DefaultTableMapsTheChords)
        {
            const auto& t = DefaultBindings();
            Assert::IsTrue(Action::RotateBack == Lookup(t, ModWin, VkLeft));
            Assert::IsTrue(Action::RotateForward == Lookup(t, ModWin, VkRight));
            Assert::IsTrue(Action::Promote == Lookup(t, ModWin, VkUp));
            Assert::IsTrue(Action::Demote == Lookup(t, ModWin, VkDown));
            Assert::IsTrue(Action::MoveToPrevMonitor == Lookup(t, ModWin | ModShift, VkLeft));
            Assert::IsTrue(Action::MoveToNextMonitor == Lookup(t, ModWin | ModShift, VkRight));
            Assert::IsTrue(Action::CycleLayout == Lookup(t, ModWin | ModShift, VkL));
        }

        TEST_METHOD(WinLIsNotBound)
        {
            Assert::IsTrue(Action::None == Lookup(DefaultBindings(), ModWin, VkL)); // Win+L locks Windows
        }

        TEST_METHOD(LookupIsExactOnModifiers)
        {
            const auto& t = DefaultBindings();
            Assert::IsTrue(Action::None == Lookup(t, ModWin | ModCtrl, VkLeft));
            Assert::IsTrue(Action::None == Lookup(t, ModWin | ModAlt, VkUp));
            Assert::IsTrue(Action::None == Lookup(t, 0, VkLeft));
            Assert::IsTrue(Action::None == Lookup(t, ModWin | ModShift, VkUp)); // not taken: keeps its Windows meaning
            Assert::IsTrue(Action::None == Lookup(t, ModWin, 'A'));
            Assert::AreEqual(ModWin | ModShift, Mods(true, true, false, false));
        }

        TEST_METHOD(LookupUsesTheGivenTable)
        {
            const std::vector<Binding> custom{ { ModCtrl | ModAlt, VkUp, Action::Promote } };
            Assert::IsTrue(Action::Promote == Lookup(custom, ModCtrl | ModAlt, VkUp));
            Assert::IsTrue(Action::None == Lookup(custom, ModWin, VkUp));
        }

        TEST_METHOD(OnlyRotateRepeats)
        {
            Assert::IsTrue(Repeats(Action::RotateBack) && Repeats(Action::RotateForward));
            Assert::IsFalse(Repeats(Action::Promote) || Repeats(Action::Demote) || Repeats(Action::CycleLayout));
            Assert::IsFalse(Repeats(Action::MoveToPrevMonitor) || Repeats(Action::MoveToNextMonitor));
        }

        TEST_METHOD(HeldForegroundIsTheTarget)
        {
            Tracker t;
            const ForegroundFacts fg{ 10, true, false };
            Assert::AreEqual<uintptr_t>(10, t.Resolve(fg, [](uintptr_t) { return false; }));
        }

        TEST_METHOD(ShellForegroundUsesTheLastHeld)
        {
            Tracker t;
            t.Note({ 10, true, false });
            const ForegroundFacts taskbar{ 99, false, true };
            t.Note(taskbar);
            Assert::AreEqual<uintptr_t>(10, t.Resolve(taskbar, [](uintptr_t w) { return w == 10; }));
            Assert::AreEqual<uintptr_t>(0, t.Resolve(taskbar, [](uintptr_t) { return false; })); // it is gone
        }

        TEST_METHOD(UnmanagedForegroundPassesThrough)
        {
            Tracker t;
            t.Note({ 10, true, false });
            const ForegroundFacts other{ 50, false, false };
            t.Note(other);
            Assert::AreEqual<uintptr_t>(0, t.Resolve(other, [](uintptr_t) { return true; }));
            // and the shell afterwards does not bring the old one back
            const ForegroundFacts taskbar{ 99, false, true };
            t.Note(taskbar);
            Assert::AreEqual<uintptr_t>(0, t.Resolve(taskbar, [](uintptr_t) { return true; }));
        }

        TEST_METHOD(NothingHeldYetPassesThrough)
        {
            Tracker t;
            const ForegroundFacts taskbar{ 99, false, true };
            Assert::AreEqual<uintptr_t>(0, t.Resolve(taskbar, [](uintptr_t) { return true; }));
            t.Note({ 10, true, false });
            t.Reset();
            Assert::AreEqual<uintptr_t>(0, t.Resolve(taskbar, [](uintptr_t) { return true; }));
        }

        TEST_METHOD(TransientClassification)
        {
            Assert::IsTrue(IsTransient(L"Shell_TrayWnd", 0, false));
            Assert::IsTrue(IsTransient(L"Shell_SecondaryTrayWnd", 0, false));
            Assert::IsTrue(IsTransient(L"Notepad", 0, true)); // Muntin's own window
            Assert::IsTrue(IsTransient(L"SomeOverlay", 0x08000000ull, false)); // WS_EX_NOACTIVATE
            Assert::IsTrue(IsTransient(L"SomeOverlay", 0x80ull, false)); // WS_EX_TOOLWINDOW
            Assert::IsFalse(IsTransient(L"Notepad", 0x100ull, false));
            Assert::IsFalse(IsTransient(L"Chrome_WidgetWin_1", 0, false));
        }

        TEST_METHOD(GateSwallowsDownRepeatAndUp)
        {
            Gate g;
            auto d = g.OnDown(Action::Promote, VkUp);
            Assert::IsTrue(d.swallow && d.fire);
            g.Completed();
            d = g.OnDown(Action::Promote, VkUp); // auto-repeat: swallowed, not repeated
            Assert::IsTrue(d.swallow && !d.fire);
            Assert::IsTrue(g.OnUp(VkUp));
            Assert::IsFalse(g.OnUp(VkUp)); // once
        }

        TEST_METHOD(GatePassesWhatIsNotOurs)
        {
            Gate g;
            const auto d = g.OnDown(Action::None, VkUp);
            Assert::IsFalse(d.swallow || d.fire);
            Assert::IsFalse(g.OnUp(VkUp)); // its key-up passes too
            Assert::IsFalse(g.IsDown(VkUp));
        }

        TEST_METHOD(GateRotateRepeatsSmoothlyButCoalesces)
        {
            Gate g;
            Assert::IsTrue(g.OnDown(Action::RotateBack, VkLeft).fire);
            Assert::IsTrue(g.OnDown(Action::RotateBack, VkLeft).fire); // repeat: pending 2
            const auto d = g.OnDown(Action::RotateBack, VkLeft); // main thread is behind: coalesced, still swallowed
            Assert::IsTrue(d.swallow && !d.fire);
            g.Completed();
            Assert::IsTrue(g.OnDown(Action::RotateBack, VkLeft).fire);
            Assert::AreEqual<size_t>(2, g.Pending());
        }

        TEST_METHOD(GateKeepsSwallowingWhenTheChordChangesMidHold)
        {
            Gate g;
            g.OnDown(Action::RotateBack, VkLeft);
            const auto d = g.OnDown(Action::None, VkLeft); // Shift released, or focus left the managed window
            Assert::IsTrue(d.swallow && !d.fire);
            Assert::IsTrue(g.OnUp(VkLeft)); // no stuck key
        }

        TEST_METHOD(GateKeysAreIndependentAndResettable)
        {
            Gate g;
            g.OnDown(Action::Promote, VkUp);
            g.OnDown(Action::Demote, VkDown);
            Assert::IsTrue(g.OnUp(VkDown));
            Assert::IsTrue(g.IsDown(VkUp));
            g.Reset();
            Assert::IsFalse(g.IsDown(VkUp));
            Assert::AreEqual<size_t>(0, g.Pending());
        }

        TEST_METHOD(MinimizedLastHeldIsNotResolved)
        {
            Tracker t;
            t.Note({ 10, true, false });
            const ForegroundFacts desktop{ 99, false, true };
            t.Note(desktop);
            Assert::AreEqual<uintptr_t>(0, t.Resolve(desktop, [](uintptr_t) { return false; })); // 10 was minimized: not held for the keys
            const ForegroundFacts minimizedFg{ 10, false, false }; // held but iconic is reported as not held
            Assert::AreEqual<uintptr_t>(0, t.Resolve(minimizedFg, [](uintptr_t) { return false; }));
        }

        TEST_METHOD(RotateSkipsMinimizedForTheMaster)
        {
            StageLayouts::MasterLayout layout;
            std::vector<int> order{ 1, 2, 3, 4 };
            const auto iconic = [](int id) { return id == 2 || id == 3; };
            Assert::IsTrue(RotateToLive(order, [&](std::vector<int>& o) { return layout.Rotate(o, -1); }, [&](int id) { return !iconic(id); }));
            Assert::IsTrue((std::vector<int>{ 4, 1, 2, 3 }) == order); // 2 and 3 skipped: 4 leads (back rotation = 1 to end, repeated)
        }

        TEST_METHOD(RotateWithOnlyMinimizedLeftChangesNothing)
        {
            StageLayouts::MasterLayout layout;
            std::vector<int> order{ 1, 2, 3 };
            const auto before = order;
            Assert::IsFalse(RotateToLive(order, [&](std::vector<int>& o) { return layout.Rotate(o, 1); }, [](int) { return false; }));
            Assert::IsTrue(before == order);
            std::vector<int> one{ 5 };
            Assert::IsFalse(RotateToLive(one, [&](std::vector<int>& o) { return layout.Rotate(o, 1); }, [](int) { return true; }));
        }

        TEST_METHOD(FocusChoiceIsTheFirstLiveWindow)
        {
            const std::vector<int> order{ 1, 2, 3 };
            Assert::AreEqual(3, FocusChoice(order, [](int id) { return id != 3; }));
            Assert::AreEqual(1, FocusChoice(order, [](int) { return false; }));
            Assert::AreEqual(0, FocusChoice(order, [](int) { return true; }));
        }

        TEST_METHOD(MaskOnlyWhileWinIsDownAndOncePerPress)
        {
            Assert::IsTrue(ShouldMask(false, true));
            Assert::IsFalse(ShouldMask(true, true));
            Assert::IsFalse(ShouldMask(false, false)); // Win already up: not sent, and the flag stays clear for the next press
        }

        TEST_METHOD(GatePrunesKeysThatAreNoLongerDown)
        {
            Gate g;
            g.OnDown(Action::Promote, VkUp);
            g.OnDown(Action::RotateBack, VkLeft);
            g.Prune([](uint32_t) { return false; }, VkLeft); // nothing physically down; the current key is kept
            Assert::IsFalse(g.IsDown(VkUp));
            Assert::IsTrue(g.IsDown(VkLeft));
            Assert::IsFalse(g.OnDown(Action::None, VkUp).swallow); // a plain Up press is no longer swallowed
            g.Prune([](uint32_t) { return true; }, 0);
            Assert::IsTrue(g.IsDown(VkLeft));
        }

        TEST_METHOD(NextMonitorTwoMonitorsWraps)
        {
            const std::vector<MonitorPos> m{ { 0, 0 }, { 1920, 0 } };
            Assert::AreEqual(1, NextMonitor(m, 0, 1));
            Assert::AreEqual(0, NextMonitor(m, 1, 1)); // wraps
            Assert::AreEqual(1, NextMonitor(m, 0, -1)); // Left on the leftmost goes to the rightmost
            Assert::AreEqual(0, NextMonitor(m, 1, -1));
        }

        TEST_METHOD(NextMonitorSortsByPositionNotByIndex)
        {
            const std::vector<MonitorPos> m{ { 1920, 0 }, { -1280, 100 }, { 0, 0 } }; // indexes in enumeration order
            Assert::AreEqual(2, NextMonitor(m, 1, 1)); // leftmost -> middle
            Assert::AreEqual(0, NextMonitor(m, 2, 1)); // middle -> right
            Assert::AreEqual(1, NextMonitor(m, 0, 1)); // right wraps to leftmost
            Assert::AreEqual(0, NextMonitor(m, 1, -1));
        }

        TEST_METHOD(NextMonitorTiesByTopEdge)
        {
            const std::vector<MonitorPos> m{ { 0, 1080 }, { 0, 0 } };
            Assert::AreEqual(0, NextMonitor(m, 1, 1));
        }

        TEST_METHOD(NextMonitorOneOrBadIndexIsNone)
        {
            Assert::AreEqual(-1, NextMonitor({ { 0, 0 } }, 0, 1));
            Assert::AreEqual(-1, NextMonitor({}, 0, 1));
            Assert::AreEqual(-1, NextMonitor({ { 0, 0 }, { 10, 0 } }, 5, 1));
            Assert::AreEqual(-1, NextMonitor({ { 0, 0 }, { 10, 0 } }, -1, 1));
        }

        // The key actions run the layout's own methods on the order list (StageMode::KeyAction)
        TEST_METHOD(RotateBackSendsMasterToTheEnd)
        {
            StageLayouts::MasterLayout layout;
            auto order = Ids;
            Assert::IsTrue(layout.Rotate(order, -1));
            Assert::IsTrue((std::vector<int>{ 2, 3, 4, 1 }) == order);
        }

        TEST_METHOD(RotateForwardBringsTheLastToMaster)
        {
            StageLayouts::MasterLayout layout;
            auto order = Ids;
            Assert::IsTrue(layout.Rotate(order, 1));
            Assert::IsTrue((std::vector<int>{ 4, 1, 2, 3 }) == order);
        }

        TEST_METHOD(RotateWithOneWindowIsNoOp)
        {
            StageLayouts::MasterLayout layout;
            std::vector<int> order{ 7 };
            Assert::IsFalse(layout.Rotate(order, 1));
        }

        TEST_METHOD(PromoteSwapsStackWindowWithMaster)
        {
            Assert::IsTrue((std::vector<int>{ 3, 2, 1, 4 }) == Master(Ids, &StageLayouts::ILayout::Promote, 3));
            Assert::IsTrue(Ids == Master(Ids, &StageLayouts::ILayout::Promote, 1)); // master: nothing
        }

        TEST_METHOD(DemoteSendsToTheEndAndNextBecomesMaster)
        {
            Assert::IsTrue((std::vector<int>{ 2, 3, 4, 1 }) == Master(Ids, &StageLayouts::ILayout::Demote, 1));
            Assert::IsTrue((std::vector<int>{ 1, 3, 4, 2 }) == Master(Ids, &StageLayouts::ILayout::Demote, 2));
            Assert::IsTrue(Ids == Master(Ids, &StageLayouts::ILayout::Demote, 4)); // already last
        }

        TEST_METHOD(LayoutCycleOrder)
        {
            Assert::AreEqual<std::string>("pair", StageLayouts::NextLayoutName("stage"));
            Assert::AreEqual<std::string>("master", StageLayouts::NextLayoutName("pair"));
            Assert::AreEqual<std::string>("stage", StageLayouts::NextLayoutName("master"));
        }
    };
}
