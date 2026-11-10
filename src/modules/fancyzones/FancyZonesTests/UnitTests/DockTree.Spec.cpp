#include "pch.h"
#include "FancyZonesLib\DesktopScroll.h"
#include "FancyZonesLib\DockMotion.h"
#include "FancyZonesLib\DockTree.h"
#include "FancyZonesLib\FancyZonesData\DockLayouts.h"
#include "FancyZonesLib\Settings.h"

#include <filesystem>
#include <fstream>

#include "Util.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FancyZonesUnitTests
{
    TEST_CLASS (DockTreeUnitTests)
    {
    public:
        TEST_METHOD (EachScrollPageHasItsOwnLayout)
        {
            auto& layouts = DockLayouts::instance();
            FancyZonesDataTypes::WorkAreaId id{};
            id.monitorId.deviceId.id = L"scroll-page-test";

            auto& tree = layouts.Tree(id);
            tree.root = std::make_unique<Dock::Node>();
            tree.root->app = L"kept";

            layouts.SwitchPage(id, 1);
            Assert::IsNull(layouts.Tree(id).root.get());

            layouts.SwitchPage(id, 0);
            Assert::AreEqual(std::wstring(L"kept"), layouts.Tree(id).root->app);

            layouts.SwitchPage(id, 2);
            layouts.SwitchPagesHome();
            Assert::AreEqual(std::wstring(L"kept"), layouts.Tree(id).root->app);
            layouts.Tree(id).root.reset();
        }

        TEST_METHOD (SavingASettingKeepsTheOthers)
        {
            const auto file = (std::filesystem::temp_directory_path() / L"muntin-test-settings.json").wstring(); // Never the real settings
            json::to_file(file, json::JsonObject::Parse(LR"({"properties":{"fancyzones_dockGap":{"value":12}},"name":"FancyZones","version":"1.0"})"));

            FancyZonesSettings::SaveValue(NonLocalizable::DesktopScrollPagesID, json::value(3), file);

            const auto properties = json::from_file(file)->GetNamedObject(L"properties");
            DeleteFileW(file.c_str());
            Assert::AreEqual(12.0, properties.GetNamedObject(L"fancyzones_dockGap").GetNamedNumber(L"value"));
            Assert::AreEqual(3.0, properties.GetNamedObject(L"fancyzones_desktopScrollPages").GetNamedNumber(L"value"));
        }

        TEST_METHOD (SavingASettingWithoutAFileMakesOne)
        {
            const auto file = (std::filesystem::temp_directory_path() / L"muntin-test-settings.json").wstring();
            DeleteFileW(file.c_str());

            FancyZonesSettings::SaveValue(NonLocalizable::ShiftDragID, json::value(false), file);

            const auto saved = json::from_file(file);
            DeleteFileW(file.c_str());
            Assert::IsTrue(saved.has_value());
            Assert::IsFalse(saved->GetNamedObject(L"properties").GetNamedObject(L"fancyzones_shiftDrag").GetNamedBoolean(L"value"));
        }

        TEST_METHOD (SavingASettingLeavesAnUnreadableFileAlone)
        {
            const auto file = (std::filesystem::temp_directory_path() / L"muntin-test-settings.json").wstring();
            const std::string broken = R"({"properties":{"fancyzones_dockGap":{"value":12}},)";
            std::ofstream{ file, std::ios::binary } << broken;

            const bool saved = FancyZonesSettings::SaveValue(NonLocalizable::ShiftDragID, json::value(false), file);

            std::ifstream in{ file, std::ios::binary };
            const std::string after{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
            in.close();
            DeleteFileW(file.c_str());
            Assert::IsFalse(saved);
            Assert::AreEqual(broken, after);
        }

        TEST_METHOD (DesktopScrollStopsAtThePageLimit)
        {
            Assert::AreEqual(0.0, DesktopScroll::ScrollLimit(0, 2160, 1)); // One page: no scrolling
            Assert::AreEqual(2160.0, DesktopScroll::ScrollLimit(4000, 2160, 2));
            Assert::AreEqual(8640.0, DesktopScroll::ScrollLimit(9000, 2160, 2)); // A window left on page 5 stays reachable
            Assert::AreEqual(6480.0, DesktopScroll::ScrollLimit(0, 2160, 4)); // Every page is reachable, even with nothing docked
        }

        TEST_METHOD (DesktopScrollOverviewIsTheRealPlaceAtFullSize)
        {
            Assert::AreEqual(300.0, DesktopScroll::OverviewY(300, 0, 1000, 0, 1.0), 0.001);
            Assert::AreEqual(300.0, DesktopScroll::OverviewY(1300, 1000, 1000, 1, 1.0), 0.001);
        }

        TEST_METHOD (DesktopScrollOverviewStacksThePagesWithAGap)
        {
            Assert::AreEqual(250.0, DesktopScroll::OverviewY(0, 0, 1000, 0, DesktopScroll::OverviewZoom), 0.001);
            Assert::AreEqual(850.0, DesktopScroll::OverviewY(1000, 0, 1000, 1, DesktopScroll::OverviewZoom), 0.001);
        }

        TEST_METHOD (DesktopScrollWindowDockedBetweenPagesGoesToThePage)
        {
            Assert::AreEqual(1600L, DesktopScroll::IntoPage(2050, 400, 1, 1000));
            Assert::AreEqual(1000L, DesktopScroll::IntoPage(700, 400, 1, 1000));
            Assert::AreEqual(1200L, DesktopScroll::IntoPage(1200, 400, 1, 1000));
            Assert::AreEqual(1000L, DesktopScroll::IntoPage(1900, 1500, 1, 1000));
        }

        TEST_METHOD (SideOfKeepsTheSideOfTheWindow)
        {
            const RECT area{ 0, 0, 1000, 800 };
            Assert::IsTrue(Dock::SideOf({ 500, 0, 1000, 800 }, area) == Dock::Edge::Right);
            Assert::IsTrue(Dock::SideOf({ 500, 400, 1000, 800 }, area) == Dock::Edge::Right);
            Assert::IsTrue(Dock::SideOf({ 0, 0, 500, 800 }, area) == Dock::Edge::Left);
            Assert::IsTrue(Dock::SideOf({ 0, 0, 1000, 400 }, area) == Dock::Edge::Top);
            Assert::IsTrue(Dock::SideOf({ 0, 400, 1000, 800 }, area) == Dock::Edge::Bottom);
        }

        TEST_METHOD (DockMotionStartsFromRest)
        {
            double displacement = -1000;
            double velocity = 0;
            DockMotion::Advance(displacement, velocity, 1.0 / 60);
            Assert::IsTrue(displacement < -850 && displacement > -970);
            Assert::IsTrue(velocity > 0);
        }

        TEST_METHOD (DockMotionComesToRest)
        {
            double displacement = -1000;
            double velocity = 0;
            for (int frame = 0; frame < 60; ++frame)
            {
                DockMotion::Advance(displacement, velocity, 1.0 / 60);
            }

            Assert::AreEqual(0.0, displacement, 0.5);
            Assert::AreEqual(0.0, velocity, 5.0);
        }

        TEST_METHOD (DesktopScrollSixNotchesAreAScreen)
        {
            Assert::AreEqual(2160.0, 6 * DesktopScroll::WheelPixels(-WHEEL_DELTA, 2160), 0.001);
            Assert::AreEqual(-360.0, DesktopScroll::WheelPixels(WHEEL_DELTA, 2160), 0.001);
        }

        TEST_METHOD (DesktopScrollStopsWhereTheWheelStopped)
        {
            Assert::AreEqual(1080.0, DesktopScroll::RestingOffset(1080.0, 2160.0));
        }

        TEST_METHOD (DesktopScrollRestsOnAPageWhenVeryClose)
        {
            Assert::AreEqual(2160.0, DesktopScroll::RestingOffset(2120.0, 2160.0));
            Assert::AreEqual(0.0, DesktopScroll::RestingOffset(40.0, 2160.0));
        }

        TEST_METHOD (DesktopScrollWindowBelongsToThePageOfItsMiddle)
        {
            Assert::AreEqual(0L, DesktopScroll::PageOf(-8, 2089, 2160));
            Assert::AreEqual(1L, DesktopScroll::PageOf(2180, 500, 2160));
            Assert::AreEqual(1L, DesktopScroll::PageOf(1900, 1000, 2160));
        }

        TEST_METHOD (EdgesOnlyDocksTheLeftAndRightEdgeNotTheOthers)
        {
            Dock::Tree tree;
            const auto left = tree.TargetFromPoint(m_area, { 3, 400 }, 0, 12, 0.3f, true);
            Assert::IsTrue(left.has_value());
            Assert::IsTrue(left->edge == Dock::Edge::Left);
            Assert::AreEqual(500L, left->preview.right);

            const auto right = tree.TargetFromPoint(m_area, { 997, 400 }, 0, 12, 0.3f, true);
            Assert::IsTrue(right.has_value());
            Assert::IsTrue(right->edge == Dock::Edge::Right);

            // The top maximizes, the bottom is the taskbar
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 500, 3 }, 0, 12, 0.3f, true).has_value());
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 500, 797 }, 0, 12, 0.3f, true).has_value());
        }

        TEST_METHOD (EdgesOnlyUsesItsOwnBandAndSkipsDockedWindows)
        {
            Dock::Tree tree;
            tree.Insert(m_a, L"", tree.TargetFromPoint(m_area, { 5, 400 }, 0).value());

            // Outside the band, and inside the docked window: nothing
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 20, 400 }, 0, 12, 0.3f, true).has_value());
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 700, 400 }, 0, 12, 0.3f, true).has_value());
            // On the edge it docks against the whole area, as with Shift
            Assert::IsTrue(tree.TargetFromPoint(m_area, { 995, 400 }, 0, 12, 0.3f, true).has_value());
        }

    private:
        const RECT m_area{ 0, 0, 1000, 800 };
        const HWND m_a = reinterpret_cast<HWND>(1);
        const HWND m_b = reinterpret_cast<HWND>(2);
        const HWND m_c = reinterpret_cast<HWND>(3);

        void DockAt(Dock::Tree& tree, HWND window, POINT pt)
        {
            const auto target = tree.TargetFromPoint(m_area, pt, 0);
            Assert::IsTrue(target.has_value());
            tree.Insert(window, L"", target.value());
        }

        // An invalid rect when the window has no space in the layout
        RECT RectOf(const Dock::Tree& tree, HWND window, int gap = 0)
        {
            RECT result{ -1, -1, -1, -1 };
            for (const auto& [rectWindow, rect] : tree.WindowRects(m_area, gap))
            {
                if (rectWindow == window)
                {
                    result = rect;
                }
            }

            return result;
        }

        // a on the left half, b on the right half
        Dock::Tree SideBySide()
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            DockAt(tree, m_b, { 750, 400 });
            return tree;
        }

    public:
        TEST_METHOD (FirstWindowTakesHalfOfWorkArea)
        {
            Dock::Tree tree;
            const auto target = tree.TargetFromPoint(m_area, { 10, 400 }, 0);
            Assert::IsTrue(target.has_value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, target->preview);

            tree.Insert(m_a, L"", target.value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (NoTargetAwayFromEdgesOfEmptyWorkArea)
        {
            Dock::Tree tree;
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 500, 400 }, 0).has_value());
        }

        TEST_METHOD (SecondWindowFillsEmptySlot)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });

            const auto target = tree.TargetFromPoint(m_area, { 750, 400 }, 0);
            Assert::IsTrue(target.has_value());
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, target->preview);

            tree.Insert(m_b, L"", target.value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (SecondWindowOnTheOtherEdgeLeavesNoHole)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            DockAt(tree, m_b, { 990, 400 });

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
            Assert::AreEqual(size_t{ 2 }, tree.Leaves().size());

            // The first window keeps the whole left side when the right side is split
            DockAt(tree, m_c, { 750, 700 });
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (SecondWindowOnTheSameEdgePushesTheFirstOver)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            DockAt(tree, m_b, { 10, 400 });

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_b));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (SecondWindowOnTheBottomEdgeStretchesTheFirst)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            DockAt(tree, m_b, { 500, 790 });

            CustomAssert::AreEqual(RECT{ 0, 0, 1000, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (EdgeDropKeepsSlotsOfApps)
        {
            auto tree = SideBySide();
            KeepSlot(tree, m_b, L"b.exe");
            DockAt(tree, m_c, { 500, 790 });

            Assert::AreEqual(size_t{ 3 }, tree.Leaves().size());
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (EdgeDropLeavesAHiddenWindowItsPlace)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            tree.Find(m_a)->hidden = true;

            DockAt(tree, m_b, { 990, 400 });

            Assert::IsNotNull(tree.Find(m_a));
        }

        TEST_METHOD (EdgeDropClosesNestedEmptySlots)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            DockAt(tree, m_b, { 100, 700 }); // Below a, on the left. The right half stays empty.
            DockAt(tree, m_c, { 990, 400 });

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 400, 500, 800 }, RectOf(tree, m_b));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_c));
            Assert::AreEqual(size_t{ 3 }, tree.Leaves().size());
        }

        TEST_METHOD (DropOnWindowEdgeSplitsWindow)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_b));
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (DropOnWorkAreaEdgeSplitsWorkArea)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 500, 790 });

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_b));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (NoTargetInMiddleOfWindow)
        {
            auto tree = SideBySide();
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 750, 400 }, 0).has_value());
        }

        TEST_METHOD (RemovedWindowGivesSpaceToNeighbor)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });

            Assert::IsTrue(tree.Remove(m_c));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));

            Assert::IsTrue(tree.Remove(m_b));
            CustomAssert::AreEqual(m_area, RectOf(tree, m_a));

            Assert::IsFalse(tree.Remove(m_b));
        }

        TEST_METHOD (RemovingOnlyWindowEmptiesTree)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });

            Assert::IsTrue(tree.Remove(m_a));
            Assert::IsTrue(tree.root == nullptr);
        }

        TEST_METHOD (HiddenWindowGivesSpaceAndTakesItBack)
        {
            auto tree = SideBySide();

            tree.Find(m_b)->hidden = true;
            Assert::AreEqual(size_t{ 1 }, tree.WindowRects(m_area, 0).size());
            CustomAssert::AreEqual(m_area, RectOf(tree, m_a));

            tree.Find(m_b)->hidden = false;
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (ResizedWindowResizesNeighbor)
        {
            auto tree = SideBySide();

            Assert::IsTrue(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 500, 800 }, RECT{ 0, 0, 300, 800 }));
            CustomAssert::AreEqual(RECT{ 0, 0, 300, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 300, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (ResizeOnSideWithoutNeighborChangesNothing)
        {
            auto tree = SideBySide();

            Assert::IsFalse(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 500, 800 }, RECT{ 0, 0, 500, 600 }));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (ResizeIgnoresEdgesTheUserDidNotMove)
        {
            auto tree = SideBySide();

            // The window was already wider than its place, only its bottom edge was dragged
            Assert::IsFalse(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 600, 800 }, RECT{ 0, 0, 600, 600 }));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (ResizeMovesSplitFromWhereTheWindowWas)
        {
            auto tree = SideBySide();

            Assert::IsTrue(tree.Resize(m_a, m_area, 0, RECT{ 0, 0, 600, 800 }, RECT{ 0, 0, 400, 800 }));
            CustomAssert::AreEqual(RECT{ 400, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (MoveSwapsWithNeighbor)
        {
            auto tree = SideBySide();

            Assert::IsTrue(tree.Move(m_a, m_area, 0, Dock::Edge::Right));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (MoveWithoutNeighborOnThatSideChangesNothing)
        {
            auto tree = SideBySide();

            Assert::IsFalse(tree.Move(m_a, m_area, 0, Dock::Edge::Left));
            Assert::IsFalse(tree.Move(m_a, m_area, 0, Dock::Edge::Top));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (MoveTakesEmptySlot)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });

            Assert::IsTrue(tree.Move(m_a, m_area, 0, Dock::Edge::Right));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (MovePicksNeighborOnThatSide)
        {
            // a on the left, b above c on the right
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });

            Assert::IsTrue(tree.Move(m_c, m_area, 0, Dock::Edge::Top));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, RectOf(tree, m_b));

            Assert::IsTrue(tree.Move(m_c, m_area, 0, Dock::Edge::Left));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, m_a));
        }

        TEST_METHOD (EdgeBandsSetWhereWindowDocks)
        {
            auto tree = SideBySide();

            // 100 px from the bottom edge of the work area: inside a 150 px band, outside the default one
            const auto wide = tree.TargetFromPoint(m_area, { 750, 700 }, 0, 150);
            Assert::IsTrue(wide.has_value());
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, wide->preview);

            // 12.5% into the window from its bottom edge: outside a 10% band
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 750, 700 }, 0, 48, 0.1f).has_value());
        }

        TEST_METHOD (GapIsLeftBetweenWindows)
        {
            const auto tree = SideBySide();

            CustomAssert::AreEqual(RECT{ 0, 0, 495, 800 }, RectOf(tree, m_a, 10));
            CustomAssert::AreEqual(RECT{ 505, 0, 1000, 800 }, RectOf(tree, m_b, 10));
        }

        // The window is gone, its place is kept for its app
        void KeepSlot(Dock::Tree& tree, HWND window, const wchar_t* app)
        {
            auto leaf = tree.Find(window);
            leaf->window = nullptr;
            leaf->app = app;
        }

        TEST_METHOD (KeptSlotSurvivesRemovingTheLastWindow)
        {
            auto tree = SideBySide();
            KeepSlot(tree, m_b, L"b.exe");

            Assert::IsTrue(tree.Remove(m_a));
            Assert::IsTrue(tree.root != nullptr);
            Assert::AreEqual(std::wstring{ L"b.exe" }, tree.root->app);
        }

        TEST_METHOD (KeptSlotsAreNotMerged)
        {
            // a on the left, b above c on the right
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });
            KeepSlot(tree, m_a, L"a.exe");
            KeepSlot(tree, m_b, L"b.exe");

            Assert::IsTrue(tree.Remove(m_c));
            Assert::AreEqual(size_t{ 2 }, tree.Leaves().size());
        }

        TEST_METHOD (KeptSlotsCanBeFilledWhenNoWindowTakesSpace)
        {
            auto tree = SideBySide();
            KeepSlot(tree, m_a, L"a.exe");
            KeepSlot(tree, m_b, L"b.exe");

            const auto target = tree.TargetFromPoint(m_area, { 750, 400 }, 0);
            Assert::IsTrue(target.has_value());
            Assert::IsTrue(target->fill);
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, target->preview);

            tree.Insert(m_c, L"c.exe", target.value());
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (KeptSlotsNextToWindowShareTheirSpace)
        {
            // a on the left, slots for b above c on the right
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });
            KeepSlot(tree, m_b, L"b.exe");
            KeepSlot(tree, m_c, L"c.exe");

            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));

            const auto target = tree.TargetFromPoint(m_area, { 750, 600 }, 0);
            Assert::IsTrue(target.has_value());
            Assert::IsTrue(target->fill);
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, target->preview);
        }

        TEST_METHOD (EdgeDropNextToOnlyHiddenWindowsTakesHalf)
        {
            // a alone, as the root, and hidden (closed to the tray)
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            tree.Remove(nullptr);
            tree.root = std::move(tree.root->first);
            tree.Find(m_a)->hidden = true;

            const auto target = tree.TargetFromPoint(m_area, { 990, 400 }, 0);
            Assert::IsTrue(target.has_value());
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, target->preview);

            tree.Insert(m_b, L"", target.value());
            CustomAssert::AreEqual(target->preview, RectOf(tree, m_b));
            Assert::IsNull(tree.Find(m_a));
        }

        TEST_METHOD (HiddenWindowAloneIsNoTarget)
        {
            auto tree = SideBySide();
            tree.Remove(m_a);
            tree.Find(m_b)->hidden = true;

            // 20% into the work area: inside the share of a window, outside the band of the work area
            Assert::IsFalse(tree.TargetFromPoint(m_area, { 200, 400 }, 0).has_value());
        }

        TEST_METHOD (NoTargetWhereWindowWouldBeTooSmall)
        {
            Dock::Tree tree;
            Assert::IsFalse(tree.TargetFromPoint(RECT{ 0, 0, 150, 800 }, { 10, 400 }, 0).has_value());
        }

        TEST_METHOD (AutoDockOnEmptyWorkAreaTakesHalf)
        {
            Dock::Tree tree;
            const auto target = tree.AutoTarget(m_area, 0, nullptr);
            Assert::IsTrue(target.has_value());
            tree.Insert(m_a, L"", target.value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (AutoDockFillsAnEmptySlotFirst)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });

            tree.Insert(m_b, L"", tree.AutoTarget(m_area, 0, m_a).value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (AutoDockSplitsTheFocusedWindowAlongItsLongerSide)
        {
            auto tree = SideBySide();

            // a is taller than wide: the new window goes below it
            tree.Insert(m_c, L"", tree.AutoTarget(m_area, 0, m_a).value());
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 400, 500, 800 }, RectOf(tree, m_c));

            // c is wider than tall: the next one goes to its right
            const auto d = reinterpret_cast<HWND>(4);
            tree.Insert(d, L"", tree.AutoTarget(m_area, 0, m_c).value());
            CustomAssert::AreEqual(RECT{ 0, 400, 250, 800 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 250, 400, 500, 800 }, RectOf(tree, d));
        }

        TEST_METHOD (AutoDockWithoutFocusSplitsTheLargestWindow)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 }); // b and c share the right half, a is the largest

            const auto target = tree.AutoTarget(m_area, 0, nullptr);
            Assert::IsTrue(target.has_value());
            Assert::IsTrue(target->node == tree.Find(m_a));
        }

        TEST_METHOD (AutoDockLeavesASlotKeptForAnApp)
        {
            auto tree = SideBySide();
            auto leaf = tree.Find(m_b);
            leaf->window = nullptr;
            leaf->app = L"kept.exe";

            const auto target = tree.AutoTarget(m_area, 0, m_a);
            Assert::IsTrue(target.has_value());
            Assert::IsFalse(target->fill);
            Assert::IsTrue(target->node == tree.Find(m_a));
        }

        TEST_METHOD (DockingAtAnEdgeFillsTheEmptySlotThere)
        {
            // a and b share the left half, the right half is kept for an app that is closed
            auto tree = SideBySide();
            DockAt(tree, m_c, { 250, 700 });
            auto leaf = tree.Find(m_b);
            leaf->window = nullptr;
            leaf->app = L"kept.exe";
            leaf->launch = L"kept.exe --profile";

            const auto d = reinterpret_cast<HWND>(4);
            tree.InsertAtEdge(d, L"d.exe", m_area, 0, Dock::Edge::Right);
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 400, 500, 800 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, d));
            Assert::AreEqual(std::wstring{ L"d.exe" }, tree.Find(d)->app);
            Assert::IsTrue(tree.Find(d)->launch.empty());
        }

        TEST_METHOD (ExpandTakesTheEmptySpaceAndTheOthersKeepTheirSize)
        {
            // a | c on the left half, the right half kept for an app
            auto tree = SideBySide();
            DockAt(tree, m_c, { 450, 400 });
            tree.Find(m_b)->app = L"kept.exe";
            tree.Find(m_b)->window = nullptr;

            Assert::IsTrue(tree.Expand(m_c, L"", m_area, 0));
            CustomAssert::AreEqual(RECT{ 0, 0, 250, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 250, 0, 1000, 800 }, RectOf(tree, m_c));
            Assert::AreEqual(size_t{ 2 }, tree.Leaves().size());

            // Nothing empty left
            Assert::IsFalse(tree.Expand(m_c, L"", m_area, 0));
        }

        TEST_METHOD (ExpandKeepsTheGapsAndTheSizeOfTheOthers)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 450, 400 });
            tree.Find(m_b)->window = nullptr;
            CustomAssert::AreEqual(RECT{ 252, 0, 496, 800 }, RectOf(tree, m_c, 8));

            Assert::IsTrue(tree.Expand(m_a, L"", m_area, 8));
            CustomAssert::AreEqual(RECT{ 0, 0, 748, 800 }, RectOf(tree, m_a, 8));
            CustomAssert::AreEqual(RECT{ 756, 0, 1000, 800 }, RectOf(tree, m_c, 8));
        }

        TEST_METHOD (ExpandOfAFloatingWindowFillsTheLargestEmptySlot)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });
            tree.Find(m_b)->window = nullptr;

            const auto d = reinterpret_cast<HWND>(4);
            Assert::IsTrue(tree.Expand(d, L"d.exe", m_area, 0));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 400 }, RectOf(tree, d));
            Assert::IsFalse(tree.Expand(reinterpret_cast<HWND>(5), L"", m_area, 0));
        }

        TEST_METHOD (ExpandGoesUpAsFarAsTheSpaceIsEmpty)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 450, 400 });
            tree.Find(m_a)->window = nullptr;
            tree.Find(m_b)->window = nullptr;

            Assert::IsTrue(tree.Expand(m_c, L"", m_area, 0));
            CustomAssert::AreEqual(RECT{ 0, 0, 1000, 800 }, RectOf(tree, m_c));
            Assert::AreEqual(size_t{ 1 }, tree.Leaves().size());
        }

        TEST_METHOD (ExpandWidensTheWholeColumn)
        {
            // a above c on the left, the right half empty: a binary split can't widen a alone
            auto tree = SideBySide();
            DockAt(tree, m_c, { 250, 700 });
            tree.Find(m_b)->window = nullptr;

            Assert::IsTrue(tree.Expand(m_a, L"", m_area, 0));
            CustomAssert::AreEqual(RECT{ 0, 0, 1000, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (ExpandLeavesTheSpaceOfAHiddenWindow)
        {
            auto tree = SideBySide();
            tree.Find(m_b)->hidden = true;

            Assert::IsFalse(tree.Expand(m_a, L"", m_area, 0));
            Assert::IsNotNull(tree.Find(m_b));
        }

        TEST_METHOD (ExpandOfAFloatingWindowPrefersAnOpenSlot)
        {
            // Top right kept for an app, bottom left open, both as large
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 });
            const auto x = reinterpret_cast<HWND>(5);
            DockAt(tree, x, { 250, 700 });
            tree.Find(m_b)->app = L"kept.exe";
            tree.Find(m_b)->window = nullptr;
            tree.Find(x)->window = nullptr;

            const auto d = reinterpret_cast<HWND>(4);
            Assert::IsTrue(tree.Expand(d, L"", m_area, 0));
            CustomAssert::AreEqual(RECT{ 0, 400, 500, 800 }, RectOf(tree, d));
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (DockingAtAnEdgeWithOnlyHiddenWindowsTakesTheArea)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            tree.Find(m_a)->hidden = true;

            tree.InsertAtEdge(m_c, L"", m_area, 0, Dock::Edge::Right);
            CustomAssert::AreEqual(RECT{ 0, 0, 1000, 800 }, RectOf(tree, m_c));
            Assert::IsNotNull(tree.Find(m_a));
        }

        TEST_METHOD (HiddenWindowsAndKeptSlotsShowNothing)
        {
            auto tree = SideBySide();
            Assert::IsTrue(tree.ShowsWindow());
            tree.Find(m_a)->hidden = true;
            Assert::IsTrue(tree.ShowsWindow());
            tree.Find(m_b)->app = L"kept.exe";
            tree.Find(m_b)->window = nullptr;
            Assert::IsFalse(tree.ShowsWindow());
            Assert::IsFalse(Dock::Tree{}.ShowsWindow());
        }

        TEST_METHOD (PairingReplacesHiddenWindowsAndKeptSlotsWithHalves)
        {
            // What a middle click does with two floating windows over leftovers: closed Settings windows (hidden) and kept slots
            auto tree = SideBySide();
            tree.Find(m_a)->hidden = true;
            tree.Find(m_b)->app = L"kept.exe";
            tree.Find(m_b)->window = nullptr;
            const auto d = reinterpret_cast<HWND>(4);

            tree.Pair(m_c, L"c.exe", d, L"d.exe", true);
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(tree, d));
            Assert::AreEqual(size_t{ 2 }, tree.Leaves().size());

            tree.Pair(d, L"d.exe", m_c, L"c.exe", false);
            CustomAssert::AreEqual(RECT{ 0, 0, 1000, 400 }, RectOf(tree, d));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (DockingAtTheBottomFillsTheEmptySlotThere)
        {
            Dock::Tree tree;
            DockAt(tree, m_a, { 500, 10 });

            tree.InsertAtEdge(m_c, L"", m_area, 0, Dock::Edge::Bottom);
            CustomAssert::AreEqual(RECT{ 0, 0, 1000, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (DockingAtAnEdgeWithoutASlotThereSplitsTheArea)
        {
            auto tree = SideBySide();
            auto leaf = tree.Find(m_b);
            leaf->window = nullptr;
            leaf->app = L"kept.exe";

            tree.InsertAtEdge(m_c, L"", m_area, 0, Dock::Edge::Left);
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_c));
            CustomAssert::AreEqual(RECT{ 500, 0, 750, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (DockingAtAnEdgePrefersAnOpenSlotAndDropsTheOthers)
        {
            // Top right is kept for an app, bottom right and bottom left are open
            Dock::Tree tree;
            DockAt(tree, m_a, { 10, 400 });
            DockAt(tree, m_b, { 750, 400 });
            DockAt(tree, m_c, { 750, 700 });
            DockAt(tree, reinterpret_cast<HWND>(5), { 250, 700 });
            tree.Find(m_b)->app = L"kept.exe";
            tree.Find(m_b)->window = nullptr;
            tree.Find(m_c)->window = nullptr;
            tree.Find(reinterpret_cast<HWND>(5))->window = nullptr;

            const auto d = reinterpret_cast<HWND>(4);
            tree.InsertAtEdge(d, L"", m_area, 0, Dock::Edge::Right);
            CustomAssert::AreEqual(RECT{ 500, 400, 1000, 800 }, RectOf(tree, d));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
            Assert::AreEqual(std::wstring{ L"kept.exe" }, tree.Leaves()[1]->app);
        }

        TEST_METHOD (AutoDockRefusesASplitThatLeavesTooLittle)
        {
            const RECT narrow{ 0, 0, 300, 150 };
            Dock::Tree tree;
            tree.Insert(m_a, L"", Dock::Target{ .node = nullptr, .edge = Dock::Edge::Left });
            tree.Insert(m_b, L"", tree.AutoTarget(narrow, 0, m_a).value());
            Assert::IsFalse(tree.AutoTarget(narrow, 0, m_a).has_value());
        }

        TEST_METHOD (ToggleSplitTurnsTheSplitAroundTheWindow)
        {
            auto tree = SideBySide();
            Assert::IsTrue(tree.ToggleSplit(m_a, m_area, 0));
            CustomAssert::AreEqual(RECT{ 0, 0, 1000, 400 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_b));

            Assert::IsTrue(tree.ToggleSplit(m_b, m_area, 0));
            CustomAssert::AreEqual(RECT{ 0, 0, 500, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (ToggleSplitSkipsASplitWhoseOtherSideIsHidden)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 }); // b above c on the right
            tree.Find(m_c)->hidden = true;

            Assert::IsTrue(tree.ToggleSplit(m_b, m_area, 0));
            CustomAssert::AreEqual(RECT{ 0, 400, 1000, 800 }, RectOf(tree, m_b));
        }

        TEST_METHOD (ToggleSplitOfAnUnknownWindowChangesNothing)
        {
            auto tree = SideBySide();
            Assert::IsFalse(tree.ToggleSplit(m_c, m_area, 0));
        }

        TEST_METHOD (GrowMovesTheSplitTowardsTheNeighbor)
        {
            auto tree = SideBySide();
            Assert::IsTrue(tree.Grow(m_a, m_area, 0, true, 100));
            CustomAssert::AreEqual(RECT{ 0, 0, 600, 800 }, RectOf(tree, m_a));
            CustomAssert::AreEqual(RECT{ 600, 0, 1000, 800 }, RectOf(tree, m_b));

            // The window on the other side of the split gets its pixels the same way
            Assert::IsTrue(tree.Grow(m_b, m_area, 0, true, 200));
            CustomAssert::AreEqual(RECT{ 0, 0, 400, 800 }, RectOf(tree, m_a));

            Assert::IsTrue(tree.Grow(m_a, m_area, 0, true, -100));
            CustomAssert::AreEqual(RECT{ 0, 0, 300, 800 }, RectOf(tree, m_a));
        }

        TEST_METHOD (GrowFindsTheSplitOfThatDirection)
        {
            auto tree = SideBySide();
            DockAt(tree, m_c, { 750, 700 }); // b above c on the right

            Assert::IsTrue(tree.Grow(m_c, m_area, 0, true, 100));
            CustomAssert::AreEqual(RECT{ 400, 400, 1000, 800 }, RectOf(tree, m_c));
            Assert::IsTrue(tree.Grow(m_c, m_area, 0, false, 100));
            CustomAssert::AreEqual(RECT{ 400, 300, 1000, 800 }, RectOf(tree, m_c));
        }

        TEST_METHOD (GrowWithoutASplitOfThatDirectionChangesNothing)
        {
            auto tree = SideBySide();
            Assert::IsFalse(tree.Grow(m_a, m_area, 0, false, 100));
        }

        TEST_METHOD (GrowStopsAtTheSmallestRatio)
        {
            auto tree = SideBySide();
            Assert::IsTrue(tree.Grow(m_a, m_area, 0, true, 5000));
            CustomAssert::AreEqual(RECT{ 0, 0, 900, 800 }, RectOf(tree, m_a));
            Assert::IsFalse(tree.Grow(m_a, m_area, 0, true, 100));
        }

        TEST_METHOD (ARenumberedDisplayIsTheSameMonitor)
        {
            const auto monitor = [](const wchar_t* instance, int number, const wchar_t* serial) {
                return FancyZonesDataTypes::MonitorId{ .deviceId = { .id = L"ACR0808", .instanceId = instance, .number = number }, .serialNumber = serial };
            };
            std::map<std::wstring, int> saved{ { L"ACR0808_5&728d9c2&1&UID4352_2_134744072", 1 }, { L"ACR0808_5&728d9c2&1&UID4353_1_134744072", 2 }, { L"ACR0808_UID9_3_AB_12", 3 } };

            Assert::AreEqual(1, DockCommands::FindMonitor(saved, monitor(L"5&728d9c2&1&UID4352", 7, L"134744072"))->second);
            Assert::AreEqual(2, DockCommands::FindMonitor(saved, monitor(L"5&728d9c2&1&UID4353", 6, L"134744072"))->second);
            Assert::AreEqual(3, DockCommands::FindMonitor(saved, monitor(L"UID9", 4, L"AB_12"))->second); // A serial with _
            Assert::IsTrue(DockCommands::FindMonitor(saved, monitor(L"5&728d9c2&1&UID4354", 2, L"134744072")) == saved.end()); // Another port
            Assert::IsTrue(DockCommands::FindMonitor(saved, monitor(L"5&728d9c2&1&UID4352", 2, L"999")) == saved.end()); // Another serial
            Assert::IsTrue(DockCommands::FindMonitor(saved, monitor(L"UID9", 4, L"12")) == saved.end()); // Only the end of a serial
        }

        TEST_METHOD (LayoutNamesThatCantBeShortcutsAreRefused)
        {
            Assert::IsTrue(DockCommands::IsValidName(L"DevLayout"));
            Assert::IsTrue(DockCommands::IsValidName(L"Dev Layout 2"));
            for (const auto name : { L"", L"a:b", L"a\\b", L"..\\x", L"%USERNAME%", L"name.", L"name ", L"con", L"a\nb", L"a\"b" })
            {
                Assert::IsFalse(DockCommands::IsValidName(name), name);
            }

            Assert::IsFalse(DockCommands::IsValidName(std::wstring(101, L'a')));
        }

        TEST_METHOD (CloneIsIndependent)
        {
            auto tree = SideBySide();
            tree.Find(m_b)->launch = L"b.exe";
            auto copy = tree.Clone();
            Assert::AreEqual(std::wstring{ L"b.exe" }, copy.Find(m_b)->launch);

            tree.Remove(m_b);
            CustomAssert::AreEqual(RECT{ 500, 0, 1000, 800 }, RectOf(copy, m_b));
        }
    };
}
