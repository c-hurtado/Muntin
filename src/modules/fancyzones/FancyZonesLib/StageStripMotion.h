#pragma once

// Stage mode strip, pure: scrolling (which tiles show), tile motion (interpolation of a rect towards its target) and the small
// geometry helpers of the hover and the rounded corners. No windows, no clocks (the caller passes the time), no DWM.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
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

    // One wheel notch (120) is one tile. Accumulates what is left of a notch (high resolution wheels send small deltas). Wheel up (positive)
    // scrolls towards the start of the strip: the result is the change of the offset.
    inline int WheelSteps(int& accumulated, int delta)
    {
        accumulated += delta;
        const int notches = accumulated / 120; // truncates towards zero
        accumulated -= notches * 120;
        return -notches;
    }

    // What eases: only the wheel scroll slides the tiles. A structural change (promote, demote, swap, a window added or removed, a layout
    // switch, a rehome) lands at once: the tiles jump to their final rects, new ones appear in place, removed ones disappear. The two
    // flags make it a one-line change to bring the old easing of structural changes back.
    constexpr bool AnimateStructuralChanges = false;
    constexpr bool ScrollAnimation = true;

    // fromWheel: the picture is the one of a wheel step. An overlay that moved or was created never animates.
    constexpr bool ShouldAnimate(bool fromWheel, bool isUpdate, bool areaChanged, bool animateStructural = AnimateStructuralChanges, bool scrollAnimation = ScrollAnimation)
    {
        return isUpdate && !areaChanged && (fromWheel ? scrollAnimation : animateStructural);
    }

    // The part of a rect inside a clip (an empty rect when they do not meet): a tile on its way in or out of the strip is cut at the
    // edge of the strip, not drawn over what lies beyond
    inline LRect ClipRect(const LRect& r, const LRect& clip)
    {
        const int l = (std::max)(r.L, clip.L), t = (std::max)(r.T, clip.T), rr = (std::min)(r.R, clip.R), b = (std::min)(r.B, clip.B);
        return rr > l && b > t ? LRect(l, t, rr, b) : LRect();
    }

    // The first tile that shows when the list is scrolled all the way down: the smallest offset from which every tile that is left
    // fits in the strip. With tiles of different heights that is not "total minus fit". No clip known: the count rule.
    inline int MaxOffsetFor(const Strip& full)
    {
        const int total = static_cast<int>(full.tiles.size());
        if (total == 0)
        {
            return 0;
        }

        if (full.clip.H() <= 0)
        {
            return MaxOffset(total, full.fit);
        }

        std::vector<LRect> rects;
        for (const auto& t : full.tiles)
        {
            rects.push_back(t.rect);
        }

        return StageStripPlan::MaxScrollOffset(rects, full.clip.H());
    }

    inline int ClampOffsetFor(const Strip& full, int offset)
    {
        return std::clamp(offset, 0, MaxOffsetFor(full));
    }

    // What the strip draws for a scroll offset (the index of the first tile at the top). A scroll moves the whole list up by the height
    // of the tiles before the offset (each tile's own height plus the gap), so tiles of any height keep their places relative to each
    // other and slide as one. The tiles that meet the clip show; `keep` names more (tiles that are on their way out and must slide off
    // the strip before they go): they are at the rect they move to. No clip known: the `fit` tiles from the offset.
    inline Strip ProjectSlide(const Strip& full, int offset, const std::set<std::uintptr_t>* keep = nullptr)
    {
        Strip shown;
        shown.key = full.key;
        shown.area = full.area;
        shown.clip = full.clip;
        shown.fit = full.fit;
        shown.layout = full.layout;
        const int total = static_cast<int>(full.tiles.size());
        if (total == 0)
        {
            return shown;
        }

        const int first = ClampOffsetFor(full, offset);
        const int dy = full.tiles[static_cast<size_t>(first)].rect.T - full.tiles.front().rect.T;
        for (int i = 0; i < total; i++)
        {
            auto tile = full.tiles[static_cast<size_t>(i)];
            tile.rect = LRect(tile.rect.L, tile.rect.T - dy, tile.rect.R, tile.rect.B - dy);
            const bool meets = full.clip.H() > 0 ? ClipRect(tile.rect, full.clip).H() > 0 : (i >= first && i < first + (std::max)(full.fit, 1));
            if (meets || (keep && keep->contains(tile.window)))
            {
                shown.tiles.push_back(tile);
            }
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

    // What Apply does with the motions of a strip when a new picture comes. A picture that does not animate puts every tile at its rect.
    // One that does: a tile that was shown eases from where it is drawn; one that comes in starts as far from its rect as the tiles that
    // stay are from theirs (the same displacement, so the list slides as one and nothing snaps or overlaps); one that goes is dropped.
    inline void ApplyMotions(std::map<std::uintptr_t, Motion>& motion, const std::vector<StageStripPlan::Tile>& tiles, std::uint64_t now, bool animate)
    {
        std::erase_if(motion, [&](const auto& m) { return !StageStripPlan::FindTile(tiles, m.first); });
        bool haveReference = false;
        int displacement = 0;
        if (animate)
        {
            for (const auto& tile : tiles)
            {
                const auto it = motion.find(tile.window);
                if (it != motion.end())
                {
                    Retarget(it->second, tile.rect, now);
                    if (!haveReference)
                    {
                        displacement = it->second.cur.T - tile.rect.T;
                        haveReference = true;
                    }
                }
            }
        }

        for (const auto& tile : tiles)
        {
            const auto it = motion.find(tile.window);
            if (it != motion.end() && animate)
            {
                continue;
            }

            auto entering = Motion::At(tile.rect, now);
            if (animate && haveReference && displacement != 0)
            {
                entering.cur = LRect(tile.rect.L, tile.rect.T + displacement, tile.rect.R, tile.rect.B + displacement);
                entering.from = entering.cur;
            }

            motion[tile.window] = entering;
        }
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

        // What was remembered, not clamped (the caller clamps it to the strip it has now)
        int Raw(const std::wstring& monitor, const std::string& layout) const
        {
            const auto it = offsets.find(KeyOf(monitor, layout));
            return it == offsets.end() ? 0 : it->second;
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

    // Where a source of this size is shown in a box: as large as fits, the aspect kept, centered
    inline LRect FitRect(const LRect& box, int width, int height)
    {
        if (width <= 0 || height <= 0 || box.W() <= 0 || box.H() <= 0)
        {
            return box;
        }

        const double scale = (std::min)(static_cast<double>(box.W()) / width, static_cast<double>(box.H()) / height);
        const int w = (std::max)(1, static_cast<int>(width * scale + 0.5));
        const int h = (std::max)(1, static_cast<int>(height * scale + 0.5));
        const int l = box.L + (box.W() - w) / 2;
        const int t = box.T + (box.H() - h) / 2;
        return LRect(l, t, l + w, t + h);
    }

    // The corner radius of the setting (stageStripRadius): 0 .. 40 pixels, anything else is cut to that
    constexpr int MaxRadiusSetting = 40;
    inline int RadiusOf(int setting)
    {
        return (std::max)(0, (std::min)(setting, MaxRadiusSetting));
    }

    // The corner radius a tile can take
    inline int ClampRadius(int radius, int width, int height)
    {
        return (std::max)(0, (std::min)(radius, (std::min)(width, height) / 2));
    }

    // The shape of a tile on the overlay: the thumbnail (rounded at thumbRadius) and, when hovered, the highlight ring around it (the
    // thumbnail grown by ringPad, rounded at its radius + ringPad so the ring is as wide at the corners as along the edges). The overlay's
    // region and the painted highlight are both made from this, so nothing of the window's own (grey) background shows. Not hovered:
    // outer == thumb, one radius.
    struct TileShape
    {
        LRect thumb;
        LRect outer;
        int thumbRadius = 0;
        int outerRadius = 0;
        bool ring = false;
    };

    inline TileShape ShapeOf(const LRect& thumb, int radius, bool hovered, int ringPad)
    {
        TileShape shape;
        shape.thumb = thumb;
        shape.thumbRadius = ClampRadius(radius, thumb.W(), thumb.H());
        shape.ring = hovered && ringPad > 0;
        shape.outer = shape.ring ? LRect(thumb.L - ringPad, thumb.T - ringPad, thumb.R + ringPad, thumb.B + ringPad) : thumb;
        shape.outerRadius = shape.ring ? ClampRadius(shape.thumbRadius > 0 ? shape.thumbRadius + ringPad : 0, shape.outer.W(), shape.outer.H()) : shape.thumbRadius;
        return shape;
    }
}
