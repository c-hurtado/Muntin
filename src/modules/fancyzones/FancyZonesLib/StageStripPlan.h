#pragma once

// Stage mode strip, pure: what the strip renderer shows (one strip per monitor stage: its area and its tiles) and what changes between
// two such pictures. No windows, no DWM, no threads in this file: the renderer (StageStrip) applies the changes this computes.
//
// A tile is the rect of one strip window in screen pixels, and whether its thumbnail can be shown: a window that is not live
// (minimized, hidden, cloaked, gone) has a blank thumbnail, so its tile is just the solid background until it is live again.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <FancyZonesLib/StageLayouts.h>

namespace StageStripPlan
{
    using StageLayouts::LRect;

    struct Tile
    {
        std::uintptr_t window{}; // the HWND of the parked window
        LRect rect; // screen pixels
        bool live{}; // a thumbnail of it is worth registering
        std::array<int, 4> insets{}; // the invisible frame of the window (left, top, right, bottom), cropped from the thumbnail

        bool operator==(const Tile& o) const { return window == o.window && rect.Same(o.rect) && live == o.live && insets == o.insets; }
    };

    struct Strip
    {
        std::wstring key; // the monitor: strips with the same key are the same overlay
        LRect area; // the overlay window: the strip zone of the plan
        std::vector<Tile> tiles; // every parked window in plan order; the first `fit` lie inside the usable area, the others are scrolled to
        int fit{}; // how many tiles show at once (the strip scrolls when there are more)
        std::string layout; // the layout name: another layout resets the scroll
        LRect clip; // where tiles show (the usable area of the plan): a tile that slides in or out is cut at its edges

        bool operator==(const Strip& o) const { return key == o.key && area.Same(o.area) && clip.Same(o.clip) && tiles == o.tiles && fit == o.fit && layout == o.layout; }
    };

    inline std::wstring KeyOf(const LRect& monitor)
    {
        return std::to_wstring(monitor.L) + L"," + std::to_wstring(monitor.T) + L"," + std::to_wstring(monitor.R) + L"," + std::to_wstring(monitor.B);
    }

    // What the runtime knows about the window of a plan id (window 0 = unknown: no tile)
    struct Source
    {
        std::uintptr_t window{};
        bool live{}; // whether its thumbnail can be shown (not minimized, hidden or cloaked)
        std::array<int, 4> insets{};
    };

    // The part of a source window (its size in pixels) the thumbnail shows: without the invisible frame. An inset that would leave
    // nothing is ignored (the whole window).
    inline LRect SourceCrop(int width, int height, const std::array<int, 4>& insets)
    {
        const int l = (std::max)(0, insets[0]), t = (std::max)(0, insets[1]), r = (std::max)(0, insets[2]), b = (std::max)(0, insets[3]);
        if (l + r >= width || t + b >= height)
        {
            return LRect(0, 0, width, height);
        }

        return LRect(l, t, width - r, height - b);
    }

    // The crop to apply for a source of this size, or nothing when the size is not known (zero or failed query): then no rcSource is
    // sent at all (never an invalid one), so DWM keeps the one it has (the whole window for a thumbnail that never had one). A known
    // size always gives a valid crop (the whole source where the insets would leave nothing), which replaces any older rcSource.
    inline std::optional<LRect> CropFor(int width, int height, const std::array<int, 4>& insets)
    {
        if (width <= 0 || height <= 0)
        {
            return std::nullopt;
        }

        return SourceCrop(width, height, insets);
    }

    // The height of the strip tiles. A presentation step on the plan after Compute (the layout engine is not touched): every tile hugs
    // its window. Its height is the tile width times the aspect (height / width) of the window's visible frame, the aspect clamped to
    // AspectMin..AspectMax; a window of unknown size gets the layout's aspect height (plan.TileH). Tiles stack from the top of the
    // usable area with the layout gap.
    //  - Too tall together: every tile shrinks by one common factor, solved so that the total fits, but no tile goes below its own floor
    //    min(natural height, MinTileHeight): a tile that reached its floor stays there and the others take up the rest. When even the
    //    floors do not fit, all tiles are at their floor and the strip scrolls.
    //  - Shorter together: the leftover is shared out as extra gap between the tiles, so the first top and the last bottom line up with
    //    the docked windows, at most MaxExtraGapFactor times the layout gap and MaxExtraGapPx per gap (exactly: never one pixel more);
    //    what is left stays unused at the bottom (top aligned, not centered). One tile: top aligned at its natural height.
    // A tile that is shorter than its natural height is narrower as well, to keep the shape of its window, and centered in the strip.
    // Rounding: when the tiles fill the height, the last bottom is exactly the bottom of the area.
    // plan.TileH stays the layout's aspect height. Everything that uses the strip rects (overlay, hit tests, hover, drop to swap, Build and
    // its fit count, the scroll) takes the plan from here, so they agree.
    constexpr int MinTileHeight = 120;
    constexpr double AspectMin = 0.4;
    constexpr double AspectMax = 2.5;
    constexpr int MaxExtraGapFactor = 3;
    constexpr int MaxExtraGapPx = 40;
    constexpr double AspectTolerance = 0.05; // a window that resizes re-plans its tile only when its aspect moved by more than this

    // Whether an aspect moved by more than the tolerance (a new tile height is due)
    inline bool AspectChanged(double previous, double current)
    {
        if (current <= 0)
        {
            return false;
        }

        return previous <= 0 || std::abs(current - previous) > previous * AspectTolerance;
    }

    // The aspect a tile keeps while the window's aspect stays within the tolerance of it: no re-plan for a few pixels
    inline double StableAspect(double previous, double current)
    {
        return AspectChanged(previous, current) ? current : previous;
    }

    inline void LayoutTiles(StageLayouts::Plan& plan, const std::function<double(int)>& aspectOf)
    {
        if (!plan.HasStrip || plan.TileH <= 0)
        {
            return;
        }

        std::vector<StageLayouts::PlanItem*> tiles;
        for (auto& item : plan.Items)
        {
            if (item.Role == StageLayouts::Role::StripTile && item.Rect.W() > 0 && item.Rect.H() > 0)
            {
                tiles.push_back(&item);
            }
        }

        const size_t n = tiles.size();
        const int total = plan.Area.H();
        const int gap = plan.Gap;
        if (n == 0 || total <= 0)
        {
            return;
        }

        std::vector<int> natural, floors;
        int sum = 0;
        int floorSum = 0;
        for (const auto* tile : tiles)
        {
            const double aspect = aspectOf ? aspectOf(tile->Id) : 0.0;
            const int h = aspect > 0 ? (std::max)(1, static_cast<int>(std::lround(tile->Rect.W() * std::clamp(aspect, AspectMin, AspectMax)))) : plan.TileH;
            natural.push_back(h);
            floors.push_back((std::min)(h, MinTileHeight));
            sum += h;
            floorSum += floors.back();
        }

        const int available = total - (static_cast<int>(n) - 1) * gap;
        std::vector<int> heights = natural;
        std::vector<int> gaps(n, gap);
        if (sum > available)
        {
            if (floorSum >= available)
            {
                heights = floors; // not even at the floors: the strip scrolls
            }
            else
            {
                // one factor for the tiles above their floor; a tile that would fall below its floor stays on it, the rest take up what is left
                std::vector<bool> onFloor(n, false);
                double factor = 1.0;
                for (size_t round = 0; round <= n; round++)
                {
                    int fixedSum = 0;
                    double freeNatural = 0;
                    for (size_t i = 0; i < n; i++)
                    {
                        if (onFloor[i])
                        {
                            fixedSum += floors[i];
                        }
                        else
                        {
                            freeNatural += natural[i];
                        }
                    }

                    factor = freeNatural > 0 ? (available - fixedSum) / freeNatural : 1.0;
                    bool changed = false;
                    for (size_t i = 0; i < n; i++)
                    {
                        if (!onFloor[i] && natural[i] * factor < floors[i])
                        {
                            onFloor[i] = true;
                            changed = true;
                        }
                    }

                    if (!changed)
                    {
                        break;
                    }
                }

                int used = 0;
                for (size_t i = 0; i < n; i++)
                {
                    heights[i] = onFloor[i] ? floors[i] : (std::max)(floors[i], static_cast<int>(std::floor(natural[i] * factor + 1e-9)));
                    used += heights[i];
                }

                for (size_t i = 0; used < available && std::any_of(onFloor.begin(), onFloor.end(), [](bool f) { return !f; }); i = (i + 1) % n)
                {
                    if (!onFloor[i])
                    {
                        heights[i]++; // the rounding remainder: the last bottom is exactly the bottom
                        used++;
                    }
                }
            }
        }
        else if (n > 1 && sum < available)
        {
            const int leftover = available - sum;
            const int cap = (std::min)(MaxExtraGapFactor * gap, MaxExtraGapPx);
            const int share = leftover / static_cast<int>(n - 1);
            const int extra = (std::min)(share, cap);
            for (size_t i = 0; i + 1 < n; i++)
            {
                gaps[i] += extra;
            }

            if (share < cap)
            {
                // under the cap: the remainder of the division goes one pixel at a time to the first gaps (still within the cap)
                for (int i = 0; i < leftover - extra * static_cast<int>(n - 1); i++)
                {
                    gaps[static_cast<size_t>(i)]++;
                }
            }
        }

        int y = plan.Area.T;
        for (size_t i = 0; i < n; i++)
        {
            auto& rect = tiles[i]->Rect;
            const int width = heights[i] >= natural[i] ? rect.W() : (std::clamp)(static_cast<int>(std::lround(static_cast<double>(rect.W()) * heights[i] / natural[i])), 1, rect.W());
            const int left = rect.L + (rect.W() - width) / 2; // shorter than its window's shape allows: narrower too, centered in the strip
            rect = LRect(left, y, left + width, y + heights[i]);
            y += heights[i] + gaps[i];
        }
    }

    // The scroll limit of tiles laid one under the other: the first tile from which every tile that is left fits in the area height
    inline int MaxScrollOffset(const std::vector<LRect>& rects, int areaHeight)
    {
        if (rects.empty() || areaHeight <= 0)
        {
            return 0;
        }

        for (size_t k = 0; k < rects.size(); k++)
        {
            if (rects.back().B - rects[k].T <= areaHeight)
            {
                return static_cast<int>(k);
            }
        }

        return static_cast<int>(rects.size()) - 1;
    }

    // The plan as the strip is drawn at a scroll offset, for what has to find the tile under the mouse (drop to swap): the strip tiles are
    // moved up with the list and cut at the usable area; a tile that is not in view has an empty rect (nothing to hit).
    inline void ApplyScroll(StageLayouts::Plan& plan, int offset)
    {
        std::vector<StageLayouts::PlanItem*> tiles;
        std::vector<LRect> rects;
        for (auto& item : plan.Items)
        {
            if (item.Role == StageLayouts::Role::StripTile && item.Rect.W() > 0 && item.Rect.H() > 0)
            {
                tiles.push_back(&item);
                rects.push_back(item.Rect);
            }
        }

        if (tiles.empty())
        {
            return;
        }

        const int first = std::clamp(offset, 0, MaxScrollOffset(rects, plan.Area.H()));
        const int dy = rects[static_cast<size_t>(first)].T - rects.front().T;
        for (size_t i = 0; i < tiles.size(); i++)
        {
            const int l = (std::max)(rects[i].L, plan.Area.L), t = (std::max)(rects[i].T - dy, plan.Area.T);
            const int r = (std::min)(rects[i].R, plan.Area.R), b = (std::min)(rects[i].B - dy, plan.Area.B);
            tiles[i]->Rect = r > l && b > t ? LRect(l, t, r, b) : LRect();
        }
    }

    // A strip over a window that is maximized or fills its monitor would cover its caption buttons and scroll bars, and a click on them
    // would hit a tile: the strip is not shown while a window of the stage is.
    inline bool CoversMonitor(const LRect& window, const LRect& monitor)
    {
        return window.L <= monitor.L && window.T <= monitor.T && window.R >= monitor.R && window.B >= monitor.B;
    }

    // A window of ANOTHER process (not held by the stage) that is in front and fills the monitor: exclusive or borderless fullscreen.
    // The desktop and the taskbar fill monitors too and never count.
    inline bool FullscreenOver(const LRect& foreground, const LRect& monitor, bool isShell)
    {
        return !isShell && CoversMonitor(foreground, monitor);
    }

    inline bool Suppressed(bool anyZoomed, bool anyCoversMonitor, bool foreignFullscreen = false)
    {
        return anyZoomed || anyCoversMonitor || foreignFullscreen;
    }

    // The strip of a plan: one tile per StripTile item (in plan order), `fit` of them completely inside the usable area; the others are
    // reached by scrolling (StageStripMotion::ProjectSlide picks what shows). No strip in the plan (or no tile that fits): nothing to show.
    inline std::optional<Strip> Build(const std::wstring& key, const StageLayouts::Plan& plan, const std::function<Source(int)>& sourceOf)
    {
        if (!plan.HasStrip)
        {
            return std::nullopt;
        }

        Strip strip;
        strip.key = key;
        strip.area = plan.Strip;
        strip.clip = plan.Area;
        for (const auto& item : plan.Items)
        {
            if (item.Role != StageLayouts::Role::StripTile || item.Rect.W() <= 0 || item.Rect.H() <= 0)
            {
                continue;
            }

            const auto source = sourceOf(item.Id);
            if (source.window)
            {
                strip.tiles.push_back({ source.window, item.Rect, source.live, source.insets });
                if (item.Rect.Inside(plan.Area) && strip.fit == static_cast<int>(strip.tiles.size()) - 1)
                {
                    strip.fit++; // the tiles inside are a prefix
                }
            }
        }

        if (strip.tiles.empty() || strip.fit <= 0)
        {
            return std::nullopt;
        }

        return strip;
    }

    // What to do to the thumbnails of one overlay to go from one tile list to the next. Only live tiles have a thumbnail.
    struct TileDiff
    {
        std::vector<Tile> add; // register a thumbnail (a new tile, or one that became live)
        std::vector<std::uintptr_t> remove; // unregister (the tile is gone, or no longer live)
        std::vector<Tile> move; // same window, new rect: update the destination
        bool regionChanged{}; // the set of tile rects (live or not) differs: the overlay's region has to be rebuilt

        bool Empty() const { return add.empty() && remove.empty() && move.empty() && !regionChanged; }
    };

    inline const Tile* FindTile(const std::vector<Tile>& tiles, std::uintptr_t window)
    {
        const auto it = std::find_if(tiles.begin(), tiles.end(), [&](const Tile& t) { return t.window == window; });
        return it == tiles.end() ? nullptr : &*it;
    }

    inline bool SameRects(const std::vector<Tile>& a, const std::vector<Tile>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }

        return std::all_of(a.begin(), a.end(), [&](const Tile& t) {
            return std::any_of(b.begin(), b.end(), [&](const Tile& o) { return o.rect.Same(t.rect); });
        });
    }

    inline TileDiff DiffTiles(const std::vector<Tile>& before, const std::vector<Tile>& after)
    {
        TileDiff diff;
        for (const auto& old : before)
        {
            const auto* now = FindTile(after, old.window);
            if (old.live && (!now || !now->live))
            {
                diff.remove.push_back(old.window);
            }
        }

        for (const auto& now : after)
        {
            if (!now.live)
            {
                continue;
            }

            const auto* old = FindTile(before, now.window);
            if (!old || !old->live)
            {
                diff.add.push_back(now);
            }
            else if (!old->rect.Same(now.rect) || old->insets != now.insets)
            {
                diff.move.push_back(now);
            }
        }

        diff.regionChanged = !SameRects(before, after);
        return diff;
    }

    enum class Kind
    {
        Create, // a new overlay
        Update, // same overlay, something changed
        Destroy // the overlay goes (and its thumbnails)
    };

    struct Change
    {
        Kind kind{};
        std::wstring key;
        Strip next; // Create / Update: the strip to show
        LRect area; // Update: the old area (Destroy and Create: unused)
        bool areaChanged{};
        TileDiff tiles;
    };

    // From the strips shown to the strips wanted. Strips that did not change give no entry. Destroys come first, then the updates and
    // creates in the order of the wanted list. A window that moves from one strip to another is only guaranteed to leave the first before
    // it joins the second when the first strip is destroyed; DWM allows a window to be thumbnailed on two overlays meanwhile.
    inline std::vector<Change> DiffStrips(const std::vector<Strip>& before, const std::vector<Strip>& after)
    {
        std::vector<Change> changes;
        const auto find = [](const std::vector<Strip>& strips, const std::wstring& key) -> const Strip* {
            const auto it = std::find_if(strips.begin(), strips.end(), [&](const Strip& s) { return s.key == key; });
            return it == strips.end() ? nullptr : &*it;
        };

        for (const auto& old : before)
        {
            if (!find(after, old.key))
            {
                Change c;
                c.kind = Kind::Destroy;
                c.key = old.key;
                c.tiles = DiffTiles(old.tiles, {});
                changes.push_back(std::move(c));
            }
        }

        for (const auto& now : after)
        {
            const auto* old = find(before, now.key);
            Change c;
            c.key = now.key;
            c.next = now;
            if (!old)
            {
                c.kind = Kind::Create;
                c.areaChanged = true;
                c.tiles = DiffTiles({}, now.tiles);
                c.tiles.regionChanged = true;
                changes.push_back(std::move(c));
                continue;
            }

            c.kind = Kind::Update;
            c.area = old->area;
            c.areaChanged = !old->area.Same(now.area);
            c.tiles = DiffTiles(old->tiles, now.tiles);
            if (c.areaChanged || !old->clip.Same(now.clip) || !c.tiles.Empty())
            {
                changes.push_back(std::move(c));
            }
        }

        return changes;
    }

    // The tile under a point (screen pixels), 0 = none. Live or not: a tile of a blank window can be clicked too.
    inline std::uintptr_t HitTile(const std::vector<Tile>& tiles, int x, int y)
    {
        for (const auto& t : tiles)
        {
            if (x >= t.rect.L && x < t.rect.R && y >= t.rect.T && y < t.rect.B)
            {
                return t.window;
            }
        }

        return 0;
    }
}
