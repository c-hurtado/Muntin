#pragma once

// Stage mode strip, pure: scrolling (which tiles show), tile motion (interpolation of a rect towards its target) and the small
// geometry helpers of the hover and the rounded corners. No windows, no clocks (the caller passes the time), no DWM.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <FancyZonesLib/StageStripPlan.h>

namespace StageStripMotion
{
    using StageLayouts::LRect;
    using StageStripPlan::Strip;

    // Scrolling is in whole tiles: the offset is the index of the first tile that shows
    inline int MaxOffset(int total, int fit)
    {
        return (std::max)(0, total - (std::max)(fit, 0));
    }

    inline int ClampOffset(int offset, int total, int fit)
    {
        return std::clamp(offset, 0, MaxOffset(total, fit));
    }

    // One wheel notch (120) is one tile. Accumulates what is left of a notch (high resolution wheels send small deltas). Wheel up (positive)
    // scrolls towards the start of the strip: the result is the change of the offset.
    inline int WheelSteps(int& accumulated, int delta)
    {
        accumulated += delta;
        const int notches = accumulated / 120; // truncates towards zero
        accumulated -= notches * 120;
        return -notches;
    }

    // What shows with this scroll offset: `fit` tiles from the offset on, each in the slot rect of its position. Tiles scrolled out
    // of view are not in the result (no thumbnail, no region, no hit); an offset out of range is clamped.
    inline Strip Project(const Strip& full, int offset)
    {
        Strip shown;
        shown.key = full.key;
        shown.area = full.area;
        shown.fit = full.fit;
        shown.layout = full.layout;
        const int total = static_cast<int>(full.tiles.size());
        const int first = ClampOffset(offset, total, full.fit);
        const int count = (std::min)((std::max)(full.fit, 0), total - first);
        for (int i = 0; i < count; i++)
        {
            auto tile = full.tiles[static_cast<size_t>(first) + static_cast<size_t>(i)];
            tile.rect = full.tiles[static_cast<size_t>(i)].rect; // the slot
            shown.tiles.push_back(tile);
        }

        return shown;
    }

    inline LRect Lerp(const LRect& from, const LRect& to, double share)
    {
        const auto mix = [&](int a, int b) { return static_cast<int>(std::lround(a + (static_cast<double>(b) - a) * share)); };
        return LRect(mix(from.L, to.L), mix(from.T, to.T), mix(from.R, to.R), mix(from.B, to.B));
    }

    // One tile's rect on its way to the place the plan gave it
    struct Motion
    {
        LRect from;
        LRect to;
        LRect cur; // where it is drawn now
        std::uint64_t start{}; // milliseconds

        bool Active() const { return !cur.Same(to); }
        static Motion At(const LRect& r, std::uint64_t now) { return { r, r, r, now }; }
    };

    // A new target: the ease starts from where the tile is drawn (a target that did not change leaves a running ease alone)
    inline void Retarget(Motion& m, const LRect& target, std::uint64_t now)
    {
        if (m.to.Same(target))
        {
            return;
        }

        m.from = m.cur;
        m.to = target;
        m.start = now;
    }

    // Moves the tile on to this time; true when the drawn rect changed. ease maps 0..1 to the share of the way gone.
    inline bool Step(Motion& m, std::uint64_t now, std::uint64_t durationMs, double (*ease)(double))
    {
        if (!m.Active())
        {
            return false;
        }

        double progress = 1.0;
        if (durationMs > 0)
        {
            progress = now <= m.start ? 0.0 : static_cast<double>(now - m.start) / static_cast<double>(durationMs);
        }

        const LRect before = m.cur;
        m.cur = progress >= 1.0 ? m.to : Lerp(m.from, m.to, ease(progress));
        return !before.Same(m.cur);
    }

    // The tile shrunk by pad on every side (never below 1x1)
    inline LRect Deflate(const LRect& r, int pad)
    {
        const int px = (std::min)((std::max)(pad, 0), (std::max)(0, (r.W() - 1) / 2));
        const int py = (std::min)((std::max)(pad, 0), (std::max)(0, (r.H() - 1) / 2));
        return LRect(r.L + px, r.T + py, r.R - px, r.B - py);
    }

    // The hover: the thumbnail of the tile under the cursor sits closer to its edge (a subtle raise)
    constexpr int TilePad = 3;
    constexpr int HoverPad = 1;

    constexpr int PadOf(bool hovered)
    {
        return hovered ? HoverPad : TilePad;
    }

    // The tile under a point (screen pixels) where the tiles are DRAWN (their motion), not where they are going: a click or hover during
    // an animation hits what the eye sees. A tile without motion is where the plan has it. 0 = none.
    inline std::uintptr_t HitDrawn(const std::vector<StageStripPlan::Tile>& tiles, const std::map<std::uintptr_t, Motion>& motion, int x, int y)
    {
        for (const auto& t : tiles)
        {
            const auto it = motion.find(t.window);
            const LRect& r = it != motion.end() ? it->second.cur : t.rect;
            if (x >= r.L && x < r.R && y >= r.T && y < r.B)
            {
                return t.window;
            }
        }

        return 0;
    }

    // The scroll offset of every stage (monitor + layout), kept while its strip is hidden (over a maximized window) and only dropped
    // when the stage mode ends
    struct OffsetMemory
    {
        std::map<std::wstring, int> offsets;

        static std::wstring KeyOf(const std::wstring& monitor, const std::string& layout)
        {
            return monitor + L"|" + std::wstring(layout.begin(), layout.end());
        }

        int Recall(const std::wstring& monitor, const std::string& layout, int total, int fit) const
        {
            const auto it = offsets.find(KeyOf(monitor, layout));
            return ClampOffset(it == offsets.end() ? 0 : it->second, total, fit);
        }

        void Remember(const std::wstring& monitor, const std::string& layout, int offset)
        {
            offsets[KeyOf(monitor, layout)] = offset;
        }
    };

    // Settling: a thumbnail is registered while its window is still being parked (resized, moved) or not painted yet, and the crop
    // (rcSource) was computed from the size it had then. After a plan change the renderer looks again a few times.
    constexpr unsigned SettleMillis = 150;
    constexpr int ForcedStep = 2; // ~300 ms: every thumbnail is applied again whatever it looks like
    constexpr int MinSteps = 4; // ~600 ms: the shortest a settle cycle runs (a slow app resizes late)
    constexpr int MaxSteps = 14; // ~2 s: the longest

    // Whether a tile is applied again at this step: forced, or the source size is unknown, or it is not the size the last crop was
    // computed from.
    constexpr bool ShouldReplace(int width, int height, int lastWidth, int lastHeight, int step)
    {
        return step == ForcedStep || width <= 0 || height <= 0 || width != lastWidth || height != lastHeight;
    }

    // Whether a tile is in order: registered, live, with a known source size that did not change since the last look
    constexpr bool TileSettled(bool registered, bool live, int width, int height, int lastWidth, int lastHeight)
    {
        return registered && live && width > 0 && height > 0 && width == lastWidth && height == lastHeight;
    }

    // A tile that is not in order is logged at the first look, at the forced step and at the last one only
    constexpr bool ShouldLogStep(int step)
    {
        return step == 1 || step == ForcedStep || step == MaxSteps;
    }

    // The cycle stops when everything is settled (after the minimum) or at the maximum: no idle polling
    constexpr bool SettleDone(int step, bool allSettled)
    {
        return step >= MaxSteps || (allSettled && step >= MinSteps);
    }

    // The corner radius a tile can take
    inline int ClampRadius(int radius, int width, int height)
    {
        return (std::max)(0, (std::min)(radius, (std::min)(width, height) / 2));
    }
}
