#pragma once

// Stage mode works in PHYSICAL pixels only. The Muntin process is DPI aware only for the system (DPIAware::EnableDPIAwarenessForThisProcess),
// so on a monitor whose scale differs from the primary one, the rects its main thread reads and writes are virtualized and no longer
// match the work areas. Every StageMode entry point therefore runs under per-monitor v2 awareness for its duration.

#include <windows.h>

namespace StageDpi
{
    // Per-monitor aware v2 for this thread until it goes out of scope; the previous context comes back (nested use is fine)
    class Scope
    {
    public:
        Scope() noexcept :
            m_previous(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
        }

        ~Scope()
        {
            if (m_previous)
            {
                SetThreadDpiAwarenessContext(m_previous);
            }
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        DPI_AWARENESS_CONTEXT m_previous;
    };

    // A size an app reports in its own DPI (windowDpi) in physical pixels of a monitor at monitorDpi. 0 dpi: taken as equal.
    inline long long ScaleBetween(long long value, unsigned monitorDpi, unsigned windowDpi)
    {
        if (monitorDpi == 0 || windowDpi == 0 || monitorDpi == windowDpi)
        {
            return value;
        }

        return (value * monitorDpi + windowDpi / 2) / windowDpi;
    }
}
