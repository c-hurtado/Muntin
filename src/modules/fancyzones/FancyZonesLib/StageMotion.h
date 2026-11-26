#pragma once

// Stage mode window motion, pure: which windows glide when the stage moves them (DockMotion draws the glide). No windows, no clocks.
// Only the windows that end up Main or Docked glide; a parked window (a strip tile) moves at once, as does one that stays put.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageStripMotion.h>
#include <FancyZonesLib/StageStripPlan.h>

namespace StageMotion
{
    // Kill switches: Enabled off = every stage move is instant, as before. The two others take one kind of move out.
    constexpr bool Enabled = true;
    constexpr bool PromoteGlideAvailable = true; // off = a window promoted out of the strip always moves at once, whatever the tray says (the optional "Animate promoting" setting)
    constexpr bool AnimateLeave = true; // leaving stage mode (not a shutdown) glides the windows back

    // Runs a function when it goes out of scope (a hold is always released, an exception or not)
    struct ScopeGuard
    {
        std::function<void()> onExit;
        ScopeGuard() = default;
        explicit ScopeGuard(std::function<void()> f) : onExit(std::move(f)) {}
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
        ~ScopeGuard()
        {
            if (onExit)
            {
                onExit();
            }
        }
    };

    // What is true of the whole move
    struct Context
    {
        bool setting{}; // the tray "Animations" toggle (settings().dockAnimation)
        bool dragging{}; // a drag of a held window is in progress
        bool areaChanged{}; // a monitor changed (size, scale, plugged, unplugged): the windows are re-homed, not moved
        bool shuttingDown{}; // Leave on shutdown or the watchdog: no time for a glide
    };

    // What is true of one window
    struct Candidate
    {
        bool landsDocked{}; // its role in the plan is Main or Docked (false: parked, which never glides)
        bool wasParked{}; // it sits where Park put it
        bool parkedBehind{}; // ... and that is behind the master (no sliver): its thumbnail would show over the master unless the others are held too
        bool moves{}; // its rect is not the one it is going to
        bool minimized{};
        bool cloaked{};
        bool maximized{};
        bool fullscreen{}; // fullscreen or an overlay: left alone
        bool changesMonitor{}; // the move takes it to another monitor (another scale)
        bool visible{ true };
    };

    constexpr bool ShouldGlide(const Context& c, const Candidate& w, bool fromParked, bool enabled = Enabled)
    {
        if (!enabled || !c.setting || c.dragging || c.areaChanged || c.shuttingDown)
        {
            return false;
        }

        if (!w.landsDocked || !w.moves || !w.visible || w.minimized || w.cloaked || w.maximized || w.fullscreen || w.changesMonitor)
        {
            return false;
        }

        return fromParked || !w.wasParked;
    }

    // The indexes of the candidates that glide, in order
    inline std::vector<size_t> Gliders(const Context& c, const std::vector<Candidate>& windows, bool fromParked, bool enabled = Enabled)
    {
        std::vector<size_t> picked;
        for (size_t i = 0; i < windows.size(); i++)
        {
            if (ShouldGlide(c, windows[i], fromParked, enabled))
            {
                picked.push_back(i);
            }
        }

        return picked;
    }

    // The windows to hold besides the gliders: when a glider comes out from behind the master, every other usable docked window is held
    // too, so its sprite is drawn above the glider's (DockMotion draws in z-order; unheld windows are covered by the topmost overlay).
    // Held windows that do not move sit still at their goal.
    inline std::vector<size_t> ExtraHolds(const std::vector<Candidate>& windows, const std::vector<size_t>& gliders)
    {
        std::vector<size_t> extra;
        bool fromBehind = false;
        std::vector<bool> glides(windows.size(), false);
        for (const size_t i : gliders)
        {
            if (i < windows.size())
            {
                glides[i] = true;
                fromBehind = fromBehind || (windows[i].wasParked && windows[i].parkedBehind);
            }
        }

        if (!fromBehind)
        {
            return extra;
        }

        for (size_t i = 0; i < windows.size(); i++)
        {
            const Candidate& w = windows[i];
            if (!glides[i] && w.landsDocked && w.visible && !w.minimized && !w.cloaked && !w.maximized && !w.fullscreen && !w.changesMonitor)
            {
                extra.push_back(i);
            }
        }

        return extra;
    }

    // How a promotion out of the strip looks (the tray "Promote animation", settings().stagePromoteStyle)
    enum class PromoteStyle
    {
        Grow = 0, // the promoted window's sprite grows from its strip tile to its slot; the demoted one shrinks into its new tile
        Crossfade = 1 // no travel: the promoted sprite fades in at its slot, the demoted one fades out at its old slot
    };

    constexpr PromoteStyle StyleOf(int setting)
    {
        return setting == 1 ? PromoteStyle::Crossfade : PromoteStyle::Grow;
    }

    // What a held window's sprite does (DockMotion draws it)
    enum class Kind
    {
        Travel, // from the start rect (default: where the window is) to the end rect (default: where the window is when it has moved)
        FadeIn, // sits at `from`, opacity 0 to 1 over the ease time
        FadeOut, // sits at `from`, opacity 1 to 0
        FadeThrough // a move between docked slots as a crossfade: DockMotion draws a FadeOut sprite at `from` and a FadeIn sprite at `to` (see FadeThroughParts)
    };

    struct Spec
    {
        Kind kind{ Kind::Travel };
        std::optional<StageLayouts::LRect> from; // explicit start rect (screen pixels, the window's outer rect), else the window's own
        std::optional<StageLayouts::LRect> to; // explicit end rect, else the window's real rect
    };

    // The outer rect of a window (with its invisible frame) as the strip draws it in a tile: the visible part (outer minus `insets`, left top right
    // bottom) is fitted into the tile deflated by the tile padding (aspect kept, centered: Deflate + FitRect of the strip), and the frame is added
    // around it at the same scale. The window is `width` x `height` outer. Insets that leave nothing, or an unknown size: the whole window fitted.
    inline StageLayouts::LRect OuterForTile(const StageLayouts::LRect& tile, const std::array<int, 4>& insets, int width, int height)
    {
        const auto box = StageStripMotion::Deflate(tile, StageStripMotion::TilePad);
        const int l = (std::max)(0, insets[0]), t = (std::max)(0, insets[1]), r = (std::max)(0, insets[2]), b = (std::max)(0, insets[3]);
        if (width <= 0 || height <= 0 || l + r >= width || t + b >= height)
        {
            return StageStripMotion::FitRect(box, width, height);
        }

        const auto visible = StageStripMotion::FitRect(box, width - l - r, height - t - b);
        const double sx = static_cast<double>(visible.W()) / (width - l - r);
        const double sy = static_cast<double>(visible.H()) / (height - t - b);
        return StageLayouts::LRect(
            visible.L - static_cast<int>(std::lround(l * sx)), visible.T - static_cast<int>(std::lround(t * sy)),
            visible.R + static_cast<int>(std::lround(r * sx)), visible.B + static_cast<int>(std::lround(b * sy)));
    }

    // Where tile `index` of the strip is drawn when the strip is scrolled by `offset`, NOT cut at the usable area (StageStripPlan::ApplyScroll
    // cuts it): the tiles are moved up with the list. Nothing when the index is out of range or the tile lies wholly outside the area.
    inline std::optional<StageLayouts::LRect> ScrolledTile(const std::vector<StageLayouts::LRect>& tiles, size_t index, const StageLayouts::LRect& area, int offset)
    {
        if (index >= tiles.size() || tiles[index].W() <= 0 || tiles[index].H() <= 0)
        {
            return std::nullopt;
        }

        const int first = std::clamp(offset, 0, StageStripPlan::MaxScrollOffset(tiles, area.H()));
        const int dy = tiles[static_cast<size_t>(first)].T - tiles.front().T;
        const StageLayouts::LRect moved(tiles[index].L, tiles[index].T - dy, tiles[index].R, tiles[index].B - dy);
        return moved.Overlaps(area) ? std::optional<StageLayouts::LRect>(moved) : std::nullopt;
    }

    // The same for the tile of a window id in a plan (its strip tiles in plan order, as ApplyScroll takes them)
    inline std::optional<StageLayouts::LRect> ScrolledTileOf(const StageLayouts::Plan& plan, int id, int offset)
    {
        std::vector<StageLayouts::LRect> rects;
        size_t index = SIZE_MAX;
        for (const auto& item : plan.Items)
        {
            if (item.Role == StageLayouts::Role::StripTile && item.Rect.W() > 0 && item.Rect.H() > 0)
            {
                if (item.Id == id)
                {
                    index = rects.size();
                }

                rects.push_back(item.Rect);
            }
        }

        return index == SIZE_MAX ? std::nullopt : ScrolledTile(rects, index, plan.Area, offset);
    }

    // The promoted window's sprite. tile: its strip tile as an outer rect, when known; slot: the outer rect it lands on.
    // Grow without a known tile starts from where the window is parked (no explicit start).
    inline Spec PromotedSpec(PromoteStyle style, const std::optional<StageLayouts::LRect>& tile, const StageLayouts::LRect& slot)
    {
        Spec spec;
        if (style == PromoteStyle::Crossfade)
        {
            spec.kind = Kind::FadeIn;
            spec.from = slot;
            spec.to = slot;
        }
        else
        {
            spec.from = tile;
        }

        return spec;
    }

    // Whether the window that is demoted into the strip (it was docked, it parks now) gets a sprite: only together with a promotion that glides,
    // and not when its own move is out of the question (hidden, maximized, fullscreen, another monitor) or anything else forbids gliding.
    constexpr bool DemoteAnimates(const Context& c, const Candidate& w, bool promoteGlides, bool enabled = Enabled)
    {
        if (!enabled || !promoteGlides || !c.setting || c.dragging || c.areaChanged || c.shuttingDown)
        {
            return false;
        }

        return !w.wasParked && w.visible && !w.minimized && !w.cloaked && !w.maximized && !w.fullscreen && !w.changesMonitor;
    }

    // The demoted window's sprite, or nothing (it parks at once). oldRect: where it is now (outer); tile: its new tile as an outer rect, when known.
    // Grow needs the tile (it shrinks into it); Crossfade fades out where it is.
    inline std::optional<Spec> DemotedSpec(PromoteStyle style, const std::optional<StageLayouts::LRect>& tile, const StageLayouts::LRect& oldRect)
    {
        Spec spec;
        if (style == PromoteStyle::Crossfade)
        {
            spec.kind = Kind::FadeOut;
            spec.from = oldRect;
            spec.to = oldRect;
            return spec;
        }

        if (!tile)
        {
            return std::nullopt;
        }

        spec.from = oldRect;
        spec.to = tile;
        return spec;
    }

    // How a move between docked slots looks (the tray "Move animation", settings().stageMoveStyle)
    enum class MoveStyle
    {
        Slide = 0, // the sprite travels from the old rect to the new one
        Crossfade = 1 // the sprite fades out at the old rect while another fades in at the new one
    };

    constexpr MoveStyle MoveStyleOf(int setting)
    {
        return setting == 1 ? MoveStyle::Crossfade : MoveStyle::Slide;
    }

    // The sprite of a window that stays docked and moves from oldRect to newRect (same monitor). Slide: the default Travel spec.
    inline Spec MovedSpec(MoveStyle style, const StageLayouts::LRect& oldRect, const StageLayouts::LRect& newRect)
    {
        Spec spec;
        if (style == MoveStyle::Crossfade && oldRect.W() > 0 && oldRect.H() > 0 && newRect.W() > 0 && newRect.H() > 0)
        {
            spec.kind = Kind::FadeThrough;
            spec.from = oldRect;
            spec.to = newRect;
        }

        return spec;
    }

    // The two sprites a FadeThrough spec stands for: [0] fades out at the old rect, [1] fades in at the new one. Other kinds: nothing.
    inline std::vector<Spec> FadeThroughParts(const Spec& spec)
    {
        std::vector<Spec> parts;
        if (spec.kind == Kind::FadeThrough && spec.from && spec.to)
        {
            parts.push_back({ Kind::FadeOut, spec.from, spec.from });
            parts.push_back({ Kind::FadeIn, spec.to, spec.to });
        }

        return parts;
    }

    // Whether a glider out from behind the master needs the other docked windows held too (ExtraHolds): only when its sprite starts there.
    // A Grow from a known tile starts on the strip, and a Crossfade never leaves its slot.
    constexpr bool NeedsExtraHolds(PromoteStyle style, bool parkedBehind, bool tileKnown)
    {
        return parkedBehind && style == PromoteStyle::Grow && !tileKnown;
    }

    // The opacity of a sprite (0 to 1) at this share of the fade; a sprite that does not fade is opaque
    constexpr double Opacity(Kind kind, double progress)
    {
        const double p = progress < 0 ? 0 : (progress > 1 ? 1 : progress);
        return kind == Kind::FadeIn ? p : (kind == Kind::FadeOut ? 1 - p : 1.0);
    }

    inline int OpacityByte(Kind kind, double progress)
    {
        return static_cast<int>(std::lround(Opacity(kind, progress) * 255));
    }

    // How a sprite looks on screen: its rect (left, top, right, bottom, screen pixels) and opacity (0 to 255)
    struct Look
    {
        std::array<double, 4> place{};
        int opacity{ 255 };
    };

    // Where the fade-out half (the ghost) of a crossfaded move starts: where the window is seen now. A quick second move: the window's
    // live sprite (mid-travel or mid-fade, at its opacity); else the old rect, opaque. Not a FadeThrough spec: no ghost.
    inline std::optional<Look> GhostStart(const Spec& spec, const std::optional<Look>& live)
    {
        const auto parts = FadeThroughParts(spec);
        if (parts.empty())
        {
            return std::nullopt;
        }

        if (live)
        {
            return Look{ live->place, std::clamp(live->opacity, 0, 255) };
        }

        const auto& old = *parts[0].from;
        return Look{ { static_cast<double>(old.L), static_cast<double>(old.T), static_cast<double>(old.R), static_cast<double>(old.B) }, 255 };
    }

    // A ghost's opacity at this share of its fade: from its start opacity to 0
    inline int GhostOpacityByte(int start, double progress)
    {
        return static_cast<int>(std::lround(std::clamp(start, 0, 255) * Opacity(Kind::FadeOut, progress)));
    }

    // The order DockMotion registers the thumbnails of one hold in (DWM stacks a later thumbnail above an earlier one). specs: the held
    // windows' specs, bottom of the z-order first. Every ghost first, so none covers a travelling or fading sprite; then the live sprites.
    struct Registration
    {
        size_t index{}; // into specs
        bool ghost{};
    };

    inline std::vector<Registration> RegistrationOrder(const std::vector<Spec>& specs)
    {
        std::vector<Registration> order;
        for (size_t i = 0; i < specs.size(); ++i)
        {
            if (!FadeThroughParts(specs[i]).empty())
            {
                order.push_back({ i, true });
            }
        }

        for (size_t i = 0; i < specs.size(); ++i)
        {
            order.push_back({ i, false });
        }

        return order;
    }

    // Leaving stage mode: whether the windows glide back (not on a shutdown, and only when the leave itself is a user's switch)
    constexpr bool ShouldGlideLeave(const Context& c, bool enabled = Enabled, bool leave = AnimateLeave)
    {
        return enabled && leave && c.setting && !c.shuttingDown;
    }
}
