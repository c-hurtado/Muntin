#pragma once

#include <array>
#include <memory>
#include <vector>

#include <FancyZonesLib/StageMotion.h>

// Docked windows spring to their new places. The real windows move at once and take the mouse there, while
// live DWM thumbnails over them travel from the old places to the new ones. A move during a move keeps the
// speed of the windows, so quick moves glide as one. With the ease curve a new target continues from where
// the window is on screen, though not from its speed: an ease starts at rest.
class DockMotion
{
public:
    explicit DockMotion(HINSTANCE instance);
    ~DockMotion();

    // Holds these windows on the screen as they are. Back after a few frames, when it is safe to move them.
    void Hold(const std::vector<HWND>& windows) noexcept;

    // A window to hold and what its sprite does: spec.from/to override the window's own rects (a sprite that starts or ends somewhere the
    // real window is not, as a strip tile), spec.kind fades it in or out in place instead of moving it.
    struct Held
    {
        HWND window{};
        StageMotion::Spec spec;
    };
    // Returns the windows whose landing it took (spec.landsAtEnd, and a sprite stands for the window): the caller parks those out of
    // sight instead of moving them, and DockMotion moves each to its spec.to when its sprite has come to rest. Any other window: the
    // caller moves it as always.
    std::vector<HWND> Hold(const std::vector<Held>& windows) noexcept;

    // The windows have moved: their thumbnails set off. moved false: nothing changed.
    void Release(bool moved) noexcept;

    // The ease curve, as the share of the way gone at this share of EaseSeconds (0 to 1): one 1-cos curve without overshoot.
    // anticipation: starts with a small pull back, so the share dips below 0 first.
    static double Ease(double progress, bool anticipation) noexcept;
    static constexpr double EaseSeconds = 0.3;

    // Whether a new goal of a running ease (progress: share of EaseSeconds gone) starts a new ease from where the sprite is.
    // A shift of a few pixels, as a DPI rounding or a terminal's cell grid makes, only moves the end of the ease.
    static bool RestartsEase(const std::array<double, 4>& goal, const std::array<double, 4>& target, double progress) noexcept;
    static constexpr double EaseRestartPixels = 32;

    // Moves a spring on by this many seconds, as Hyprland's springs do. displacement: how far from where it comes to rest.
    static void Advance(double& displacement, double& velocity, double seconds) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
