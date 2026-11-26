#include "pch.h"

#include <optional>

#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageMouse.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pure parts of the Stage mode mouse handling (Phase 5): edge detection from rect deltas, cut / row computation, swap target
// hit-testing, drop decision. No windows, no settings, nothing on screen.

namespace
{
    using StageLayouts::LayoutInput;
    using StageLayouts::LRect;
    using StageLayouts::MasterLayout;
    using StageLayouts::Plan;
    using StageLayouts::Role;

    constexpr int Gap = 8;

    LayoutInput Input(int count)
    {
        LayoutInput in;
        in.Work = LRect(0, 0, 1000, 800);
        in.Spacing = StageLayouts::Spacing(Gap, 0, 0, 0, 0);
        for (int i = 1; i <= count; i++)
        {
            in.Wins.emplace_back(i);
        }

        return in;
    }

    std::vector<int> Order(int count)
    {
        std::vector<int> order;
        for (int i = 1; i <= count; i++)
        {
            order.push_back(i);
        }

        return order;
    }

    // master 60%, `count` windows (master + count - 1 stack)
    struct Fixture
    {
        MasterLayout layout;
        int count;
        Plan plan;
        explicit Fixture(int n) :
            count(n)
        {
            layout.MaxStack = 8;
            plan = layout.Compute(Input(n));
        }

        std::optional<StageMouse::DragCtx> Capture(int slot) { return StageMouse::Capture(layout, plan, slot, count); }
        const LRect& Rect(int slot) const { return plan.Items[slot].Rect; }
    };

    LRect Edge(LRect r, int dl, int dt, int dr, int db)
    {
        return LRect(r.L + dl, r.T + dt, r.R + dr, r.B + db);
    }
}

namespace StageMouseTests
{
    TEST_CLASS(StageMouseUnitTests)
    {
    public:
        TEST_METHOD(CaptureNeedsACutAndTwoDockedWindows)
        {
            Fixture one(1);
            Assert::IsFalse(one.Capture(0).has_value());

            Fixture two(2);
            const auto c = two.Capture(1);
            Assert::IsTrue(c.has_value());
            Assert::AreEqual(1, c->slot);
            Assert::AreEqual(2, c->n);
            Assert::AreEqual(1, static_cast<int>(c->rowH.size()));
            Assert::IsFalse(two.Capture(2).has_value()); // not a docked slot
            Assert::IsFalse(two.Capture(-1).has_value());

            // the stage layout has no cut: with a strip showing its main window can still drag the strip edge, alone it cannot
            auto stage = StageLayouts::MakeStage();
            const auto plan = stage->Compute(Input(3));
            const auto sc = StageMouse::Capture(*stage, plan, 0, 3);
            Assert::IsTrue(sc.has_value());
            Assert::IsFalse(sc->cutOk);
            Assert::IsTrue(sc->stripOk);
            Assert::IsFalse(StageMouse::Capture(*stage, stage->Compute(Input(1)), 0, 1).has_value());
        }

        TEST_METHOD(StripWidthFollowsTheRightEdgeOfTheLastDockedWindowInStage)
        {
            auto stage = StageLayouts::MakeStage();
            const auto plan = stage->Compute(Input(3));
            const auto c = StageMouse::Capture(*stage, plan, 0, 3);
            const LRect from = plan.Items[0].Rect;
            Assert::AreEqual(150, plan.TileW); // 15% of 1000
            Assert::IsTrue(StageMouse::ResizeEdge(*stage, *c, from, Edge(from, 0, 0, -50, 0))); // edge left: strip grows
            Assert::AreEqual(0.20, stage->StripShare(), 1e-9);
            Assert::AreEqual(200, stage->Compute(Input(3)).TileW);
            Assert::IsTrue(StageMouse::ResizeEdge(*stage, *c, from, Edge(from, 0, 0, 30, 0))); // always from the start
            Assert::AreEqual(0.12, stage->StripShare(), 1e-9);
            Assert::IsFalse(StageMouse::ResizeEdge(*stage, *c, from, from));
            Assert::AreEqual(0.15, stage->StripShare(), 1e-12);
            // the window's left edge, top and bottom edges: nothing
            Assert::IsFalse(StageMouse::ResizeEdge(*stage, *c, from, Edge(from, -50, 0, 0, 0)));
            Assert::IsFalse(StageMouse::ResizeEdge(*stage, *c, from, Edge(from, 0, -50, 0, 0)));
            Assert::AreEqual(0.15, stage->StripShare(), 1e-12);
        }

        TEST_METHOD(StripWidthIsClampedAndPairOnlyUsesItsRightWindow)
        {
            auto pair = StageLayouts::MakePair();
            const auto plan = pair->Compute(Input(4));
            const auto c1 = StageMouse::Capture(*pair, plan, 1, 4);
            const LRect from = plan.Items[1].Rect;
            Assert::IsTrue(c1->stripOk && c1->cutOk);
            Assert::IsTrue(StageMouse::ResizeEdge(*pair, *c1, from, Edge(from, 0, 0, -900, 0)));
            Assert::AreEqual(0.40, pair->StripShare(), 1e-9);
            Assert::IsTrue(StageMouse::ResizeEdge(*pair, *c1, from, Edge(from, 0, 0, 900, 0)));
            Assert::AreEqual(0.10, pair->StripShare(), 1e-9);
            Assert::IsTrue(std::abs(pair->Compute(Input(4)).Items[0].Rect.R - plan.Items[0].Rect.R) <= 1, L"the cut ratio follows the strip, the cut pixel stays");
            // the shared edge still moves the cut, not the strip
            Assert::IsTrue(StageMouse::ResizeEdge(*pair, *c1, from, Edge(from, -40, 0, 0, 0)));
            Assert::AreEqual(0.15, pair->StripShare(), 1e-12);
            // the left window is not next to the strip
            const auto c0 = StageMouse::Capture(*pair, plan, 0, 4);
            Assert::IsFalse(c0->stripOk);
            StageMouse::ResizeEdge(*pair, *c0, plan.Items[0].Rect, Edge(plan.Items[0].Rect, 0, 0, -50, 0)); // its right edge is the cut
            Assert::AreEqual(0.15, pair->StripShare(), 1e-12);
        }

        TEST_METHOD(NoStripNoStripEdge)
        {
            // pair with two windows and master have no strip: the right edge is an outer edge
            auto pair = StageLayouts::MakePair();
            const auto plan = pair->Compute(Input(2));
            const auto c = StageMouse::Capture(*pair, plan, 1, 2);
            Assert::IsFalse(c->stripOk);
            Assert::IsFalse(StageMouse::ResizeEdge(*pair, *c, plan.Items[1].Rect, Edge(plan.Items[1].Rect, 0, 0, -50, 0)));
            Fixture m(3);
            for (int slot = 0; slot < 3; slot++)
            {
                Assert::IsFalse(m.Capture(slot)->stripOk);
            }

            const auto c1 = m.Capture(1);
            Assert::IsFalse(StageMouse::ResizeEdge(m.layout, *c1, m.Rect(1), Edge(m.Rect(1), 0, 0, -50, 0)));
            Assert::AreEqual(0.15, m.layout.StripShare(), 1e-12);
        }

        TEST_METHOD(PairStripDragKeepsTheLeftWindowWhereItIs)
        {
            auto pair = StageLayouts::MakePair();
            const auto plan = pair->Compute(Input(3)); // 2 docked + 1 tile
            const auto c = StageMouse::Capture(*pair, plan, 1, 3);
            Assert::IsTrue(c->spanToStrip);
            const LRect from = plan.Items[1].Rect;
            for (const int dx : { -40, -150, 30, -900, 900 })
            {
                Assert::IsTrue(StageMouse::ResizeEdge(*pair, *c, from, Edge(from, 0, 0, dx, 0)) || std::abs(pair->StripShare() - 0.15) < 1e-9);
                const auto now = pair->Compute(Input(3));
                Assert::IsTrue(std::abs(now.Items[0].Rect.R - plan.Items[0].Rect.R) <= 1, L"the left window keeps its right edge");
                Assert::AreEqual(plan.Items[0].Rect.L, now.Items[0].Rect.L);
            }

            // back at the start (Esc): exactly as before
            StageMouse::Restore(*pair, *c);
            Assert::AreEqual(0.15, pair->StripShare(), 1e-12);
            Assert::AreEqual(0.5, pair->Cut(), 1e-12);

            // two windows (no strip): the strip is not a thing, no compensation
            const auto twoPlan = pair->Compute(Input(2));
            const auto two = StageMouse::Capture(*pair, twoPlan, 1, 2);
            Assert::IsFalse(two->spanToStrip);
            Assert::AreEqual(two->cut0, StageMouse::CutKeepingPixel(*two, 0.3), 1e-12);
        }

        TEST_METHOD(CutCompensationRespectsTheCutClamp)
        {
            auto pair = StageLayouts::MakePair();
            pair->SetCut(0.80);
            const auto plan = pair->Compute(Input(3));
            const auto c = StageMouse::Capture(*pair, plan, 1, 3);
            // the strip grows: the span shrinks, so the pixel's ratio would pass 0.85; the layout clamps it
            const double wanted = StageMouse::CutKeepingPixel(*c, 0.40);
            Assert::IsTrue(wanted > 0.80);
            pair->SetStripShare(0.40);
            pair->SetCut(wanted);
            Assert::IsTrue(pair->Cut() <= 0.85 + 1e-12);
            Assert::AreEqual(c->cut0, StageMouse::CutKeepingPixel(*c, c->strip0), 1e-12, L"unchanged share: the start ratio");
        }

        TEST_METHOD(StripShareMathAndClamp)
        {
            Assert::AreEqual(0.20, StageMouse::StripShareAfterDrag(150, 1000, -50), 1e-12);
            Assert::AreEqual(0.40, StageMouse::StripShareAfterDrag(150, 1000, -5000), 1e-12);
            Assert::AreEqual(0.10, StageMouse::StripShareAfterDrag(150, 1000, 5000), 1e-12);
            Assert::AreEqual(0.40, StageMouse::StripShareAfterDrag(150, 0, 10), 1e-12, L"no usable width: no division by zero, clamped");
            auto stage = StageLayouts::MakeStage();
            stage->SetStripShare(0.9);
            Assert::AreEqual(0.40, stage->StripShare(), 1e-12);
            stage->SetStripShare(0.0);
            Assert::AreEqual(0.10, stage->StripShare(), 1e-12);
        }

        TEST_METHOD(AStripEdgeDragIsAResizeDrop)
        {
            StageMouse::DropFacts f;
            f.resized = true;
            f.intent = StageMouse::Intent::Size;
            f.edgeChanged = true; // ResizeEdge reports a strip change the same way
            Assert::IsTrue(StageMouse::PlanDrop(f).action == StageMouse::Drop::ResizeEdge);
            f.edgeChanged = false; // an outer edge, Master included
            Assert::IsTrue(StageMouse::PlanDrop(f).action == StageMouse::Drop::SnapBack);
        }

        TEST_METHOD(CutPixelsAreTheEdgeBetweenMasterAndStack)
        {
            Fixture f(3);
            const auto c = f.Capture(0);
            Assert::IsTrue(c.has_value());
            Assert::AreEqual(600, c->cutPx); // 60% of 1000, the gap shared around it
            Assert::AreEqual(0, c->spanL);
            Assert::AreEqual(1000, c->spanW);
            Assert::AreEqual(f.Rect(0).R + Gap / 2, c->cutPx);
        }

        TEST_METHOD(MasterRightEdgeMovesTheCut)
        {
            Fixture f(3);
            const auto c = f.Capture(0);
            const LRect from = f.Rect(0);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, 100, 0)));
            Assert::AreEqual(0.7, f.layout.Cut(), 1e-9);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, -100, 0))); // always from the start: not 0.6
            Assert::AreEqual(0.5, f.layout.Cut(), 1e-9);
        }

        TEST_METHOD(StackLeftEdgeMovesTheCut)
        {
            Fixture f(3);
            const auto c = f.Capture(1);
            const LRect from = f.Rect(1);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, -100, 0, 0, 0)));
            Assert::AreEqual(0.5, f.layout.Cut(), 1e-9);
        }

        TEST_METHOD(CutIsClampedToTheLayoutLimits)
        {
            Fixture f(3);
            const auto c = f.Capture(0);
            const LRect from = f.Rect(0);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, 900, 0)));
            Assert::AreEqual(0.85, f.layout.Cut(), 1e-9);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, -900, 0)));
            Assert::AreEqual(0.15, f.layout.Cut(), 1e-9);
        }

        TEST_METHOD(MovingBackRestoresTheStartExactly)
        {
            Fixture f(3);
            const auto c = f.Capture(0);
            const LRect from = f.Rect(0);
            StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, 120, 0));
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, from)); // Esc: the window is back at its start rect
            Assert::AreEqual(0.6, f.layout.Cut(), 1e-12);
            Assert::IsTrue(f.layout.Rows().empty());
        }

        TEST_METHOD(OuterEdgesChangeNothing)
        {
            Fixture f(4);
            // master: left, top, bottom edges
            auto c = f.Capture(0);
            LRect from = f.Rect(0);
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, -50, 0, 0, 0)));
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, -50, 0, 0)));
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, 0, 50)));
            // stack window: right edge, first window's top, last window's bottom
            c = f.Capture(1);
            from = f.Rect(1);
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, 50, 0)));
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, -50, 0, 0)));
            c = f.Capture(3);
            from = f.Rect(3);
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, 0, 50)));
            Assert::AreEqual(0.6, f.layout.Cut(), 1e-12);
            Assert::IsTrue(f.layout.Rows().empty());
        }

        TEST_METHOD(AMoveOrACornerThatMovesTheOppositeEdgeIsNotAnEdgeDrag)
        {
            Fixture f(3);
            const auto c = f.Capture(0);
            const LRect from = f.Rect(0);
            // moved right by 100 (same size): left edge not fixed
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 100, 0, 100, 0)));
            // within the tolerance counts as fixed
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 2, 0, 100, 0)));
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 3, 0, 100, 0)));
            Assert::IsTrue(StageMouse::IsResize(from, Edge(from, 0, 0, 5, 0)));
            Assert::IsFalse(StageMouse::IsResize(from, Edge(from, 40, 40, 40, 40)));
        }

        TEST_METHOD(StackRowEdgesMoveTheRows)
        {
            Fixture f(4); // master + 3 stack windows
            const auto c0 = f.Capture(2);
            const LRect from = f.Rect(2);
            // the top edge of the middle stack window moves up 50 px: the upper neighbour shrinks
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c0, from, Edge(from, 0, -50, 0, 0)));
            auto rows = f.layout.Rows();
            Assert::AreEqual(3, static_cast<int>(rows.size()));
            Assert::IsTrue(rows[0] < rows[1]);
            Assert::AreEqual(1.0, rows[0] + rows[1] + rows[2], 1e-9);

            // the plan follows: the upper window is shorter, the middle one taller
            const auto plan = f.layout.Compute(Input(4));
            Assert::IsTrue(plan.Items[1].Rect.H() < f.plan.Items[1].Rect.H());
            Assert::IsTrue(plan.Items[2].Rect.H() > f.plan.Items[2].Rect.H());
            Assert::IsTrue(std::abs(f.plan.Items[3].Rect.H() - plan.Items[3].Rect.H()) <= 2);

            // the bottom edge of the first stack window moves down 30 px
            const auto c1 = f.Capture(1);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c1, f.Rect(1), Edge(f.Rect(1), 0, 0, 0, 30)));
            rows = f.layout.Rows();
            Assert::IsTrue(rows[0] > rows[1]);
        }

        TEST_METHOD(RowsAreClampedToEightPercentOfTheStack)
        {
            Fixture f(3);
            const auto c = f.Capture(1);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, f.Rect(1), Edge(f.Rect(1), 0, 0, 0, 5000)));
            const auto rows = f.layout.Rows();
            Assert::AreEqual(2, static_cast<int>(rows.size()));
            Assert::AreEqual(0.92, rows[0], 1e-9);
            Assert::AreEqual(0.08, rows[1], 1e-9);
        }

        TEST_METHOD(CutAndRowsTogetherAtACorner)
        {
            Fixture f(4);
            const auto c = f.Capture(2);
            const LRect from = f.Rect(2);
            Assert::IsTrue(StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, -80, -40, 0, 0))); // top-left corner
            Assert::IsTrue(f.layout.Cut() < 0.6);
            Assert::AreEqual(3, static_cast<int>(f.layout.Rows().size()));
        }

        TEST_METHOD(TheNeighboursFollowTheCutInThePlan)
        {
            Fixture f(3);
            const auto c = f.Capture(0);
            const LRect from = f.Rect(0);
            StageMouse::ResizeEdge(f.layout, *c, from, Edge(from, 0, 0, 100, 0));
            const auto plan = f.layout.Compute(Input(3));
            Assert::AreEqual(from.R + 100, plan.Items[0].Rect.R);
            Assert::AreEqual(plan.Items[0].Rect.R + Gap, plan.Items[1].Rect.L);
        }

        TEST_METHOD(StillValidTracksStageChanges)
        {
            Fixture f(3);
            const auto c = f.Capture(1);
            Assert::IsTrue(StageMouse::StillValid(*c, 3, 1));
            Assert::IsFalse(StageMouse::StillValid(*c, 4, 1)); // a window came
            Assert::IsFalse(StageMouse::StillValid(*c, 3, 2)); // its slot changed
        }

        TEST_METHOD(RestoreBringsBackTheStartNumbers)
        {
            Fixture f(4);
            f.layout.SetRows({ 0.5, 0.3, 0.2 });
            const auto c = f.Capture(0);
            f.layout.SetCut(0.3);
            f.layout.SetRows({});
            StageMouse::Restore(f.layout, *c);
            Assert::AreEqual(0.6, f.layout.Cut(), 1e-12);
            Assert::AreEqual(3, static_cast<int>(f.layout.Rows().size()));
        }

        TEST_METHOD(SnapshotComparesCutAndRows)
        {
            Fixture f(4);
            StageMouse::Snapshot a, b;
            StageMouse::Take(f.layout, a);
            StageMouse::Take(f.layout, b);
            Assert::IsTrue(a == b);
            f.layout.SetCut(0.61);
            StageMouse::Take(f.layout, b);
            Assert::IsFalse(a == b);
            StageMouse::Take(f.layout, a);
            f.layout.SetRows({ 0.4, 0.4, 0.2 });
            StageMouse::Take(f.layout, b);
            Assert::IsFalse(a == b);
        }

        TEST_METHOD(HitTestFindsTheSlotUnderThePointer)
        {
            Fixture f(3);
            const auto order = Order(3);
            const LRect m = f.Rect(0), s1 = f.Rect(1), s2 = f.Rect(2);
            Assert::AreEqual(1, StageMouse::HitTest(f.plan, order, 2, m.L + 10, m.T + 10)); // master, dragging a stack window
            Assert::AreEqual(2, StageMouse::HitTest(f.plan, order, 1, s1.L + 10, s1.T + 10));
            Assert::AreEqual(3, StageMouse::HitTest(f.plan, order, 1, s2.R - 1, s2.B - 1));
            Assert::AreEqual(0, StageMouse::HitTest(f.plan, order, 1, s2.R, s2.B)); // the edge is outside
            Assert::AreEqual(0, StageMouse::HitTest(f.plan, order, 1, m.L + 10, m.T + 10)); // its own slot
            Assert::AreEqual(0, StageMouse::HitTest(f.plan, order, 1, (m.R + s1.L) / 2, 100)); // the gap between master and stack
            Assert::AreEqual(0, StageMouse::HitTest(f.plan, order, 1, -5, -5)); // outside every slot
        }

        TEST_METHOD(HitTestSkipsHiddenWindows)
        {
            MasterLayout layout;
            layout.MaxStack = 1; // master + 1 stack, the third is hidden (parked)
            const auto plan = layout.Compute(Input(3));
            Assert::IsTrue(plan.Items[2].Role == Role::Hidden);
            const auto order = Order(3);
            Assert::AreEqual(0, StageMouse::HitTest(plan, order, 1, 0, 0)); // dragging the master: the hidden window's empty rect is no slot
            Assert::AreEqual(1, StageMouse::HitTest(plan, order, 2, 0, 0));
        }

        TEST_METHOD(SwapExchangesTwoPositions)
        {
            auto order = Order(4);
            Assert::IsTrue(StageMouse::Swap(order, 1, 3)); // master <-> stack
            Assert::AreEqual(3, order[0]);
            Assert::AreEqual(1, order[2]);
            Assert::AreEqual(2, order[1]);
            Assert::IsFalse(StageMouse::Swap(order, 2, 2));
            Assert::IsFalse(StageMouse::Swap(order, 2, 99));
            Assert::AreEqual(4, static_cast<int>(order.size()));

            auto stack = Order(4); // stack <-> stack
            Assert::IsTrue(StageMouse::Swap(stack, 2, 4));
            Assert::AreEqual(4, stack[1]);
            Assert::AreEqual(2, stack[3]);
        }

        TEST_METHOD(SwapThenPlanPutsTheWindowsInEachOthersSlots)
        {
            Fixture f(3);
            auto order = Order(3);
            const LRect masterRect = f.Rect(0), stackRect = f.Rect(2);
            StageMouse::Swap(order, 1, 3);
            LayoutInput in = Input(3);
            in.Wins.clear();
            for (const int id : order)
            {
                in.Wins.emplace_back(id);
            }

            const auto plan = f.layout.Compute(in);
            Assert::AreEqual(3, plan.Items[0].Id);
            Assert::IsTrue(plan.Items[0].Rect.Same(masterRect));
            Assert::AreEqual(1, plan.Items[2].Id);
            Assert::IsTrue(plan.Items[2].Rect.Same(stackRect));
        }

        TEST_METHOD(IntentComesFromTheHitTest)
        {
            using StageMouse::Intent;
            Assert::IsTrue(StageMouse::IntentFromHitTest(2) == Intent::Move); // HTCAPTION
            Assert::IsTrue(StageMouse::IntentFromHitTest(1) == Intent::Move); // HTCLIENT (Alt-drag)
            Assert::IsTrue(StageMouse::IntentFromHitTest(0) == Intent::Move); // HTNOWHERE
            Assert::IsTrue(StageMouse::IntentFromHitTest(-1) == Intent::Move); // HTTRANSPARENT
            Assert::IsTrue(StageMouse::IntentFromHitTest(9) == Intent::Move); // HTMAXBUTTON range
            for (int code = 10; code <= 17; code++) // HTLEFT .. HTBOTTOMRIGHT
            {
                Assert::IsTrue(StageMouse::IntentFromHitTest(code) == Intent::Size);
            }

            Assert::IsTrue(StageMouse::IntentFromHitTest(18) == Intent::Move); // HTBORDER
        }

        TEST_METHOD(RowsIntoAndMoveRowKeepTheirCapacity)
        {
            Fixture f(4);
            std::vector<double> buffer;
            f.layout.RowsInto(buffer);
            Assert::IsTrue(buffer.empty());
            const auto c = f.Capture(2);
            StageMouse::ResizeEdge(f.layout, *c, f.Rect(2), Edge(f.Rect(2), 0, -40, 0, 0));
            f.layout.RowsInto(buffer);
            Assert::IsTrue(buffer == f.layout.Rows());
            Assert::AreEqual(3, static_cast<int>(buffer.size()));
            const auto* data = buffer.data();
            StageMouse::ResizeEdge(f.layout, *c, f.Rect(2), Edge(f.Rect(2), 0, -60, 0, 0));
            f.layout.RowsInto(buffer);
            Assert::IsTrue(data == buffer.data()); // same storage: nothing allocated per tick
            Assert::IsFalse(StageMouse::ResizeEdge(f.layout, *c, f.Rect(2), f.Rect(2))); // back to the start: Restore, rows empty again
            Assert::IsTrue(f.layout.Rows().empty());
        }

        TEST_METHOD(DropPlanEscAndClickSnapBack)
        {
            using StageMouse::Drop;
            StageMouse::DropFacts f;
            f.rectSameAsStart = true;
            f.targetId = 3; // whatever else is true, an unmoved window stays
            f.onOtherMonitor = true;
            const auto plan = StageMouse::PlanDrop(f);
            Assert::IsTrue(plan.action == Drop::SnapBack);
            Assert::IsFalse(plan.skipDragged);
        }

        TEST_METHOD(DropPlanSwapAndSnapBack)
        {
            using StageMouse::Drop;
            StageMouse::DropFacts f;
            Assert::IsTrue(StageMouse::PlanDrop(f).action == Drop::SnapBack); // nothing under it
            f.targetId = 3;
            auto plan = StageMouse::PlanDrop(f);
            Assert::IsTrue(plan.action == Drop::Swap);
            Assert::IsFalse(plan.skipDragged);
        }

        TEST_METHOD(DroppedWindowsThatGlideIntoUncoveredSlotsLandOutOfSight)
        {
            using StageMouse::Drop;
            using StageMouse::OutOfSightLanders;
            Assert::IsTrue(OutOfSightLanders(Drop::SnapBack, 4, 0) == std::vector<int>{ 4 }, L"back into its emptied slot");
            Assert::IsTrue(OutOfSightLanders(Drop::Swap, 4, 7) == std::vector<int>{ 4, 7 }, L"both: each lands where the other's sprite leaves or nothing is");
            Assert::IsTrue(OutOfSightLanders(Drop::Rehome, 4, 0).empty(), L"the rehome lands it later");
            Assert::IsTrue(OutOfSightLanders(Drop::ResizeEdge, 4, 0).empty(), L"a resize does not travel");
            Assert::IsTrue(OutOfSightLanders(Drop::Unmaximize, 4, 0).empty(), L"restored first, placed later");
        }

        TEST_METHOD(ARehomeRightAfterADropLandsTheDroppedWindowOutOfSight)
        {
            using StageMouse::RehomeLandsOutOfSight;
            Assert::IsTrue(RehomeLandsOutOfSight(4, 4, 1300, 1000), L"the debounced rehome of the drop");
            Assert::IsFalse(RehomeLandsOutOfSight(5, 4, 1300, 1000), L"another window");
            Assert::IsFalse(RehomeLandsOutOfSight(4, 4, 1000 + StageMouse::RehomeDropMillis, 1000), L"long after: a move of its own");
            Assert::IsFalse(RehomeLandsOutOfSight(4, 0, 1300, 1000), L"no drop pending");
            Assert::IsFalse(RehomeLandsOutOfSight(0, 0, 1300, 1000));
        }

        TEST_METHOD(DropPlanCrossMonitorRehomesAndLandsWithoutTheDraggedWindow)
        {
            using StageMouse::Drop;
            StageMouse::DropFacts f;
            f.onOtherMonitor = true;
            f.targetId = 3;
            f.resized = true;
            f.edgeChanged = true;
            f.intent = StageMouse::Intent::Size;
            const auto plan = StageMouse::PlanDrop(f);
            Assert::IsTrue(plan.action == Drop::Rehome);
            Assert::IsTrue(plan.skipDragged); // landing it here would pull it back from the other monitor
        }

        TEST_METHOD(DropPlanSnapDropOfATitleBarDragNeverChangesTheCut)
        {
            using StageMouse::Drop;
            StageMouse::DropFacts f; // Aero Snap half / Snap Layouts: the title bar was grabbed, the size changed, an outer edge stayed
            f.intent = StageMouse::Intent::Move;
            f.resized = true;
            f.edgeChanged = true; // even if the rects look like an edge drag
            f.targetId = 3;
            Assert::IsTrue(StageMouse::PlanDrop(f).action == Drop::SnapBack);
        }

        TEST_METHOD(DropPlanBorderDragSavesOnlyWhenTheEdgeMoved)
        {
            using StageMouse::Drop;
            StageMouse::DropFacts f;
            f.intent = StageMouse::Intent::Size;
            f.resized = true;
            f.targetId = 3; // a resize never swaps
            Assert::IsTrue(StageMouse::PlanDrop(f).action == Drop::SnapBack); // outer edge
            f.edgeChanged = true;
            const auto plan = StageMouse::PlanDrop(f);
            Assert::IsTrue(plan.action == Drop::ResizeEdge);
            Assert::IsFalse(plan.skipDragged);
        }

        TEST_METHOD(DropPlanMaximizedIsRestoredFirst)
        {
            using StageMouse::Drop;
            StageMouse::DropFacts f;
            f.zoomed = true;
            f.resized = true;
            f.edgeChanged = true;
            f.intent = StageMouse::Intent::Size;
            f.targetId = 3;
            Assert::IsTrue(StageMouse::PlanDrop(f).action == Drop::Unmaximize);
            f.onOtherMonitor = true; // another monitor still wins: the rehome restores it
            Assert::IsTrue(StageMouse::PlanDrop(f).action == Drop::Rehome);
        }
    };
}
