#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <FancyZonesLib/StageAdopt.h>
#include <FancyZonesLib/StageConfig.h>
#include <FancyZonesLib/StageKeys.h>
#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StageMonitors.h>
#include <FancyZonesLib/StageMouse.h>
#include <FancyZonesLib/StagePark.h>
#include <FancyZonesLib/StageState.h>
#include <FancyZonesLib/StageStripPlan.h>

class StageStrip;

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
    // dragTimerId: the same for the live feedback of a drag (OnDragTimer, ~25 Hz); 0 = no live feedback
    StageMode(HWND timerWindow, UINT_PTR timerId, UINT_PTR dragTimerId = 0);
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
    void ScheduleRehome() noexcept; // something that may have moved a window happened (foreground change): a debounced look
    void WindowMoved(HWND window) noexcept; // end of a drag, or a location change: a held window may be on another monitor now
    // Mouse (main thread). A held window is dragged: by its title bar (a move: dropped on another slot it swaps places) or by an edge it
    // shares with a neighbour (the layout's cut / stack heights follow live, saved at the drop). Every other drop snaps back to the slot.
    void DragStart(HWND window) noexcept; // MOVESIZESTART; returns quietly for a window the stage does not hold
    void DragEnd(HWND window) noexcept; // MOVESIZEEND (Esc ends the drag too: the window is back at its start rect, so it snaps back)
    void OnDragTimer() noexcept;
    bool Dragging() const noexcept { return m_drag.has_value(); }

    // Keys (main thread). The keyboard hook asks KeyTarget() which window a chord acts on (null: the key passes through) and posts the
    // action; KeyAction runs it. NoteForeground at every foreground change keeps the "last managed foreground".
    HWND KeyTarget() noexcept;
    void NoteForeground(HWND foreground) noexcept; // the window of the foreground event
    void StripClick(HWND window) noexcept; // a tile of the strip was clicked (posted by the strip renderer)
    void KeyAction(StageKeys::Action action, HWND target) noexcept;

    void DisplayChanged() noexcept;
    void SettingsChanged() noexcept; // gap, margins, layout, cut or rows changed in settings.json

    void OnTimer() noexcept;

    // The user changed the layout choice (global for all monitors): it takes effect and is saved to settings.json.
    // The cut and the stack heights are per monitor: a drag saves only the entry of its monitor (PersistMonitor).
    void SetLayoutName(const std::wstring& name) noexcept;

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
        ULONGLONG unmaximizeUntil{}; // deadline for it to leave the maximized state
        ULONGLONG grace{}; // no learning from its size before this tick
        StageState::Pending pending; // a size seen once, not yet believed
        bool guarded{}; // a modal dialog hangs off it: it is not parked until the dialog closes
    };

    struct MonitorInfo
    {
        HMONITOR handle{};
        StageLayouts::LRect rect; // whole monitor
        StageLayouts::LRect work;
        std::wstring key; // StageConfig::MonitorKey: what this monitor's cut and rows are saved under
        UINT dpi{}; // effective DPI, 0 = unknown: a change makes the windows' minimum sizes stale
    };

    struct MonitorStage
    {
        MonitorInfo monitor;
        StageConfig::Values values; // the layout choice plus this monitor's own cut and rows (the global ones where it has no entry)
        std::vector<int> order; // layout order: master first
        std::unique_ptr<StageLayouts::ILayout> layout;
        std::optional<StageStripPlan::Strip> strip; // what the last Land planned for the strip renderer
        bool stripHidden{}; // a maximized or monitor-filling window: the strip is not shown
    };

    using Rect = StageLayouts::LRect;

    // A drag of a held window in progress. Its window is not moved by the stage meanwhile (Land skips it).
    struct Drag
    {
        int id{};
        HWND window{};
        Rect start; // GetWindowRect at the start
        std::optional<StageMouse::DragCtx> ctx; // set when an edge drag is possible (started on a border)
        StageMouse::Intent intent{ StageMouse::Intent::Move };
        StageMouse::Snapshot applied; // the layout values the neighbours were last laid out with
        StageMouse::Snapshot probe; // scratch for the tick (no allocation per tick once grown)
        bool timer{};
    };

    void StopDrag(bool restoreLayout);
    MonitorStage* StageOfId(int id, int* index = nullptr);
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
    // skipId: a window the stage leaves where it is (a drop onto another monitor). dockedOnly: the live preview of an edge drag moves
    // the docked windows only (no parking, no dialog scan, no minimum-size check).
    void Land(MonitorStage& stage, int skipId = 0, bool dockedOnly = false);
    void LandAll();
    void LooseCut(const std::vector<int>& ids);
    void PromoteDialogOwners();
    void ResetMin(Managed& m);
    bool ForeignFullscreenOn(const MonitorStage& stage) const;
    void SyncStrip();
    bool StripCovered(const MonitorStage& stage) const;
    void RefreshStripCover();
    int Capacity(const MonitorStage& stage) const;
    bool Adopt(HWND window);
    const wchar_t* Consider(HWND window, MonitorStage*& stage, std::optional<StageState::Entry>& original);
    void LogDecision(HWND window, const wchar_t* verdict);
    void Release(int id, bool minimized, bool restore);
    void Scan();
    void ProcessPending();
    void VerifyLanding();
    void RetryOrphans();
    void RehomeScan();
    void ArmRehome();
    void Rehome(int id, MonitorStage& from, MonitorStage& to);
    int MonitorIndex(const Rect& monitorRect) const;
    void Persist(const StageConfig::Values& after);
    void PersistMonitor(MonitorStage& stage);
    void ApplyValues(const StageConfig::Values& global, const std::map<std::wstring, StageMonitorValues>& monitors);
    void Schedule(UINT millis);
    std::vector<Rect> MonitorRects() const;
    StageLayouts::Spacing CurrentSpacing() const;

    static bool RestoreEntry(const StageState::Entry& entry, const std::vector<Rect>& monitors, bool minimized);
    static std::vector<StageState::Entry> RestoreEntries(const std::vector<StageState::Entry>& entries);

    StageKeys::ForegroundFacts ForegroundOf(HWND foreground);

    std::unique_ptr<StageStrip> m_strip; // the thumbnails of the parked windows (own thread); created at the first Enter
    StageKeys::Tracker m_keyTracker;
    HWND m_timerWindow{};
    UINT_PTR m_timerId{};
    UINT_PTR m_dragTimerId{};
    std::optional<Drag> m_drag;
    bool m_active{};
    int m_nextId{ 1 };
    StageConfig::Values m_values; // the global values: layout choice, and the default cut and rows of a monitor without an entry
    std::map<std::wstring, StageMonitorValues> m_monitorMap; // per monitor entries, as the settings file has them (plus our own saves not yet reloaded)
    int m_keyRetries{}; // looks left for monitors the display lookup could not identify yet
    bool m_settingsPending{}; // settings.json changed during a drag: applied once it ended
    std::vector<MonitorInfo> m_monitors;
    std::vector<MonitorStage> m_stages;
    std::map<int, Managed> m_windows;
    std::vector<StageState::Entry> m_orphans; // restore failed (hung app): they stay in the state file
    std::map<HWND, int> m_pending; // windows to look at once they are ready, with the attempts left
    ULONGLONG m_scanAt{};
    ULONGLONG m_rehomeAt{};
    ULONGLONG m_pollAt{}; // low frequency rehome scan: moves that raise no event (Win+Shift+Arrow, an app moving its own window)
    int m_scansLeft{};
    ULONGLONG m_enterAt{}; // Enter is waiting (a Leave's restores, the scroll's return)
    ULONGLONG m_enterSince{};
    ULONGLONG m_enterNotBefore{};
    std::set<HWND> m_loose; // cut by minimum sizes and released: not taken again until room appears (a window left, layout or display changed)
    int m_loosening{}; // inside LooseCut: its releases do not clear m_loose
    StageMonitors::Exiles m_exiled; // windows sent to the primary stage by a monitor that went away, and the key of that monitor
    bool m_remeasure{}; // a monitor changed size or scale: minimum sizes are measured again at the next relayout
    ULONGLONG m_dialogAt{}; // a dialog (owned window) appeared: look whether a parked window owns a modal one
    std::set<HWND> m_stuck; // would not leave the maximized state: skipped until they do
    std::map<HWND, std::wstring> m_logged; // last verdict logged per window
    ULONGLONG m_relayoutAt{};
    ULONGLONG m_verifyAt{};
    ULONGLONG m_orphanAt{};
    int m_orphanTries{};
    ULONGLONG m_guardAt{};
};
