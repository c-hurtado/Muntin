#pragma once

// Stage mode strip, pure: what the strip renderer shows (one strip per monitor stage: its area and its tiles) and what changes between
// two such pictures. No windows, no DWM, no threads in this file: the renderer (StageStrip) applies the changes this computes.
//
// A tile is the rect of one strip window in screen pixels, and whether its thumbnail can be shown: a window that is not live
// (minimized, hidden, cloaked, gone) has a blank thumbnail, so its tile is just the solid background until it is live again.

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
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

        bool operator==(const Strip& o) const { return key == o.key && area.Same(o.area) && tiles == o.tiles && fit == o.fit && layout == o.layout; }
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

    // A strip over a window that is maximized or fills its monitor would cover its caption buttons and scroll bars, and a click on them
    // would hit a tile: the strip is not shown while a window of the stage is.
    inline bool CoversMonitor(const LRect& window, const LRect& monitor)
    {
        return window.L <= monitor.L && window.T <= monitor.T && window.R >= monitor.R && window.B >= monitor.B;
    }

    inline bool Suppressed(bool anyZoomed, bool anyCoversMonitor)
    {
        return anyZoomed || anyCoversMonitor;
    }

    // The strip of a plan: one tile per StripTile item (in plan order), `fit` of them completely inside the usable area; the others are
    // reached by scrolling (StageStripMotion::Project picks what shows). No strip in the plan (or no tile that fits): nothing to show.
    inline std::optional<Strip> Build(const std::wstring& key, const StageLayouts::Plan& plan, const std::function<Source(int)>& sourceOf)
    {
        if (!plan.HasStrip)
        {
            return std::nullopt;
        }

        Strip strip;
        strip.key = key;
        strip.area = plan.Strip;
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
            if (c.areaChanged || !c.tiles.Empty())
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
