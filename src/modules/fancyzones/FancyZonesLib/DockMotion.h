#pragma once

#include <memory>
#include <vector>

// Docked windows spring to their new places. The real windows move at once and take the mouse there, while
// live DWM thumbnails over them travel from the old places to the new ones. A move during a move keeps the
// speed of the windows, so quick moves glide as one.
class DockMotion
{
public:
    explicit DockMotion(HINSTANCE instance);
    ~DockMotion();

    // Holds these windows on the screen as they are. Back after a few frames, when it is safe to move them.
    void Hold(const std::vector<HWND>& windows) noexcept;

    // The windows have moved: their thumbnails set off. moved false: nothing changed.
    void Release(bool moved) noexcept;

    // Moves a spring on by this many seconds, as Hyprland's springs do. displacement: how far from where it comes to rest.
    static void Advance(double& displacement, double& velocity, double seconds) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
