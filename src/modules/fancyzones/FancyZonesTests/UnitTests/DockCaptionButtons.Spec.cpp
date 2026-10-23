#include "pch.h"
#include "FancyZonesLib\DockCaptionButtons.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace FancyZonesUnitTests
{
    TEST_CLASS (DockCaptionButtonsUnitTests)
    {
    public:
        TEST_METHOD (PairSitsLeftOfCaptionButtons)
        {
            // Three buttons of 46 pixels: the cells of the pair are 27 wide
            const RECT pair = DockCaptionButtons::PairRect({ 862, 0, 1000, 32 });
            Assert::AreEqual(808L, pair.left);
            Assert::AreEqual(862L, pair.right);
            Assert::AreEqual(0L, pair.top);
            Assert::AreEqual(32L, pair.bottom);
        }

        TEST_METHOD (PairIsSizedByTheNumberOfCaptionButtons)
        {
            // A button is 46 wide whatever their number
            Assert::AreEqual(954L - 54, DockCaptionButtons::PairRect({ 954, 0, 1000, 32 }, 1).left);
            Assert::AreEqual(908L - 54, DockCaptionButtons::PairRect({ 908, 0, 1000, 32 }, 2).left);
        }

        TEST_METHOD (ButtonCountFollowsTheStyle)
        {
            Assert::AreEqual(3, DockCaptionButtons::ButtonCount(WS_MINIMIZEBOX | WS_SYSMENU, 0));
            Assert::AreEqual(3, DockCaptionButtons::ButtonCount(WS_MAXIMIZEBOX, 0));
            Assert::AreEqual(1, DockCaptionButtons::ButtonCount(WS_SYSMENU, 0));
            Assert::AreEqual(2, DockCaptionButtons::ButtonCount(WS_SYSMENU, WS_EX_CONTEXTHELP));
        }

        TEST_METHOD (ButtonAtSplitsThePairInHalves)
        {
            Assert::AreEqual(-1, DockCaptionButtons::ButtonAt(-1, 92));
            Assert::AreEqual(0, DockCaptionButtons::ButtonAt(0, 92));
            Assert::AreEqual(0, DockCaptionButtons::ButtonAt(45, 92));
            Assert::AreEqual(1, DockCaptionButtons::ButtonAt(46, 92));
            Assert::AreEqual(1, DockCaptionButtons::ButtonAt(91, 92));
            Assert::AreEqual(-1, DockCaptionButtons::ButtonAt(92, 92));
            // An odd width: the extra pixel belongs to next
            Assert::AreEqual(0, DockCaptionButtons::ButtonAt(44, 91));
            Assert::AreEqual(1, DockCaptionButtons::ButtonAt(45, 91));
        }

        TEST_METHOD (PairMustBeInsideTheWindow)
        {
            const RECT window{ 0, 0, 100, 500 };
            Assert::IsTrue(DockCaptionButtons::IsInside({ 0, 0, 100, 32 }, window));
            Assert::IsFalse(DockCaptionButtons::IsInside({ -4, 0, 88, 32 }, window));
            Assert::IsFalse(DockCaptionButtons::IsInside({ 20, 0, 112, 32 }, window));
        }

        TEST_METHOD (TriangleCoversItsMiddleAndNothingOutside)
        {
            // A triangle 6 wide and 10 tall, pointing left, centered on (10, 10): the base is at x = 13
            Assert::AreEqual(255, static_cast<int>(DockCaptionButtons::TriangleCoverage(12, 10, 10, 10, 6, 10, true)));
            Assert::AreEqual(0, static_cast<int>(DockCaptionButtons::TriangleCoverage(4, 10, 10, 10, 6, 10, true)));
            Assert::AreEqual(0, static_cast<int>(DockCaptionButtons::TriangleCoverage(12, 2, 10, 10, 6, 10, true)));
            // Pointing right it is the other way round
            Assert::AreEqual(0, static_cast<int>(DockCaptionButtons::TriangleCoverage(12, 5, 10, 10, 6, 10, false)));
            Assert::AreEqual(255, static_cast<int>(DockCaptionButtons::TriangleCoverage(7, 10, 10, 10, 6, 10, false)));
        }

        TEST_METHOD (GlyphIsOpaqueWhereItCovers)
        {
            Assert::AreEqual(0xFFFFFFFFul, DockCaptionButtons::Pixel(255, true, 255, 1));
            Assert::AreEqual(0xFF000000ul, DockCaptionButtons::Pixel(255, false, 255, 1));
        }

        TEST_METHOD (InactiveGlyphIsDimmed)
        {
            Assert::AreEqual(0x96969696ul, DockCaptionButtons::Pixel(255, true, 150, 0));
        }

        TEST_METHOD (IdleBackgroundStaysHittable)
        {
            // An alpha of 0 would let the mouse through
            Assert::AreEqual(0x01010101ul, DockCaptionButtons::Pixel(0, true, 255, 1));
            Assert::AreEqual(0x01000000ul, DockCaptionButtons::Pixel(0, false, 255, 1));
        }

        TEST_METHOD (HoverShadesTheBackground)
        {
            Assert::AreEqual(0x18181818ul, DockCaptionButtons::Pixel(0, true, 255, 24));
        }
    };
}
