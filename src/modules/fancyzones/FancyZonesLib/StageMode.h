#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <FancyZonesLib/StageAdopt.h>
#include <FancyZonesLib/StageConfig.h>
#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StagePark.h>
#include <FancyZonesLib/StageState.h>

// Stage mode runtime (Phase 3: master layout, no strip). Everything runs on the main (window) thread of FancyZones: the events are
// posted to it, the timer is its own. Muntin takes the eligible windows of each monitor, places the first as master and the next
// ones as a stack on its right; the ones that do not fit are parked (StagePark). Windows it manages are held (Stage::IsHeld):
// dock-by-drag and the desktop scroll leave them alone.
//
// SAFETY: stage-state.txt in the Muntin data folder lists every managed window with the place it had BEFORE. It is written (atomic
// rename) before the first move of a window, kept in step when windows come and go, and deleted by a clean restore (leaving stage
// mode, quitting). The watchdog process restores from it when Muntin dies (RecoverStateFile), and so does the next Enter.
class StageMode
{
public:
    // timerWindow / timerId: the window procedure that forwards WM_TIMER with that id to OnTimer
    StageMode(HWND timerWindow, UINT_PTR timerId);
    ~StageMode(); // Leave()

    StageMode(const StageMode&) = delete;
    StageMode& operator=(const StageMode&) = delete;

    // Takes the windows. First puts back what an earlier run that died left in the state file.
    void Enter() noexcept;
    // Puts every window back where it was and deletes the state file
    void Leave() noexcept;
    bool Active() const noexcept { return m_active; }

    // Window events (FancyZones' window procedure, main thread)
    void WindowAppeared(HWND window) noexcept; // created, shown, uncloaked, restored from minimized
    void WindowVisibility(HWND window, bool minimizing) noexcept; // minimized (true); restored, hidden or cloaked (false)
    void WindowDestroyed(HWND window) noexcept;
    void DisplayChanged() noexcept;
    void SettingsChanged() noexcept; // gap, margins, layout, cut or rows changed in settings.json

    void OnTimer() noexcept;

    // The user changed these (keys and mouse come with Phase 5): they take effect and are saved to settings.json
    void SetLayoutName(const std::wstring& name) noexcept;
    void SetMasterCut(double ratio) noexcept;
    void SetRows(const std::vector<double>& rows) noexcept;

    static std::filesystem::path StateFilePath();

    // Puts back the windows the state file lists (needs no instance: the watchdog process calls it too) and deletes the file.
    // Windows that do not respond stay in the file for a later try.
    // otherInstanceRuns (watchdog): a new Muntin that runs owns the file, the file is then left as it is.
    static void RecoverStateFile(const std::function<bool()>& otherInstanceRuns = {}) noexcept;

private:
    struct Managed
    {
        int id{};
        HWND window{};
        StageState::Entry original;
        StagePark::Insets insets;
        int minW{}, minH{}; // smallest visible size, 0 = none known
        int learned{}; // how often a minimum was learned from how the window landed
        bool parked{}; // sits where Park put it
        bool needMeasure{}; // was maximized: insets and minimum size are taken once it is restored
        int unmaximizeTries{};
        ULONGLONG grace{}; // no learning from its size before this tick
        StageState::Pending pending; // a size seen once, not yet believed
        bool guarded{}; // a modal dialog hangs off it: it is not parked until the dialog closes
    };

    struct MonitorInfo
    {
        HMONITOR handle{};
        StageLayouts::LRect rect; // whole monitor
        StageLayouts::LRect work;
    };

    struct MonitorStage
    {
        MonitorInfo monitor;
        std::vector<int> order; // layout order: master first
        std::unique_ptr<StageLayouts::ILayout> layout;
    };

    using Rect = StageLayouts::LRect;

    void RefreshMonitors();
    MonitorStage* StageFor(HWND window);
    MonitorStage* StageOfRect(const Rect& monitorRect);
    Managed* Find(HWND window);
    std::vector<HWND> TopLevelWindows() const;
    StageAdopt::AdoptInfo Describe(HWND window);
    std::optional<StageState::Entry> Capture(HWND window) const;
    std::vector<StageState::Entry> AllOriginals() const;
    bool WriteState();
    void PublishHeld();
    void Prepare(Managed& m, const Rect& work);
    void Measure(Managed& m, const Rect& work);
    StageLayouts::Plan ComputePlan(MonitorStage& stage);
    void Land(MonitorStage& stage);
    void LandAll();
    bool Adopt(HWND window);
    void Release(int id, bool minimized, bool restore);
    void Scan();
    void ProcessPending();
    void VerifyLanding();
    void RetryOrphans();
    void Persist(const StageConfig::Values& after);
    void ApplyValues(const StageConfig::Values& values);
    void Schedule(UINT millis);
    std::vector<Rect> MonitorRects() const;
    StageLayouts::Spacing CurrentSpacing() const;

    static bool RestoreEntry(const StageState::Entry& entry, const std::vector<Rect>& monitors, bool minimized);
    static std::vector<StageState::Entry> RestoreEntries(const std::vector<StageState::Entry>& entries);

    HWND m_timerWindow{};
    UINT_PTR m_timerId{};
    bool m_active{};
    int m_nextId{ 1 };
    StageConfig::Values m_values;
    std::vector<MonitorInfo> m_monitors;
    std::vector<MonitorStage> m_stages;
    std::map<int, Managed> m_windows;
    std::vector<StageState::Entry> m_orphans; // restore failed (hung app): they stay in the state file
    std::map<HWND, int> m_pending; // windows to look at once they are ready, with the attempts left
    ULONGLONG m_scanAt{};
    ULONGLONG m_relayoutAt{};
    ULONGLONG m_verifyAt{};
    ULONGLONG m_orphanAt{};
    int m_orphanTries{};
    ULONGLONG m_guardAt{};
};
