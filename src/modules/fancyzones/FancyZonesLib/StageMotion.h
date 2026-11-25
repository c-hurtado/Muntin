#pragma once

// Stage mode window motion, pure: which windows glide when the stage moves them (DockMotion draws the glide). No windows, no clocks.
// Only the windows that end up Main or Docked glide; a parked window (a strip tile) moves at once, as does one that stays put.

#include <cstddef>
#include <functional>
#include <vector>

namespace StageMotion
{
    // Kill switches: Enabled off = every stage move is instant, as before. The two others take one kind of move out.
    constexpr bool Enabled = true;
    constexpr bool AnimateFromParked = false; // a window promoted out of the strip would flash over the master it was parked behind: it moves at once
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
        bool moves{}; // its rect is not the one it is going to
        bool minimized{};
        bool cloaked{};
        bool maximized{};
        bool fullscreen{}; // fullscreen or an overlay: left alone
        bool changesMonitor{}; // the move takes it to another monitor (another scale)
        bool visible{ true };
    };

    constexpr bool ShouldGlide(const Context& c, const Candidate& w, bool enabled = Enabled, bool fromParked = AnimateFromParked)
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
    inline std::vector<size_t> Gliders(const Context& c, const std::vector<Candidate>& windows, bool enabled = Enabled, bool fromParked = AnimateFromParked)
    {
        std::vector<size_t> picked;
        for (size_t i = 0; i < windows.size(); i++)
        {
            if (ShouldGlide(c, windows[i], enabled, fromParked))
            {
                picked.push_back(i);
            }
        }

        return picked;
    }

    // Leaving stage mode: whether the windows glide back (not on a shutdown, and only when the leave itself is a user's switch)
    constexpr bool ShouldGlideLeave(const Context& c, bool enabled = Enabled, bool leave = AnimateLeave)
    {
        return enabled && leave && c.setting && !c.shuttingDown;
    }
}
