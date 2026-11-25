#pragma once

#include <windows.h>

#include <memory>
#include <vector>

#include <FancyZonesLib/StageStripPlan.h>

// The Stage mode strip: live DWM thumbnails of the windows that are parked (not docked), one overlay per monitor stage that has a strip.
// One thread owns every overlay and thumbnail. The main thread only publishes a picture (Publish: a short lock and a posted message,
// never a wait); the thread applies what differs from what it shows. A click on a tile (press and release on the same one) is posted
// to the target window as clickMessage, wparam = the HWND of the tile's window. The overlays take no focus and cover nothing but the
// tiles (a window region), so the windows under the gaps stay reachable.
class StageStrip
{
public:
    StageStrip(HWND target, UINT clickMessage);
    ~StageStrip(); // unregisters everything, destroys the overlays, joins the thread

    StageStrip(const StageStrip&) = delete;
    StageStrip& operator=(const StageStrip&) = delete;

    // The strips to show from now on (all of them: a strip that is missing is removed). Empty: nothing shown.
    // wait: returns once the picture is applied, at most half a second (Leave). Otherwise it never waits.
    void Publish(std::vector<StageStripPlan::Strip> strips, bool wait = false) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
