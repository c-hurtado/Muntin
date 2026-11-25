#pragma once

// Stage mode: which windows the stage may take over (adoption filter) and which windows must not be parked because a modal
// dialog hangs off them (dialog guard). Ported from StageProto (stage-proto.ps1 Eligible, DialogOwners). Pure: the facts about a
// window are gathered by the caller into AdoptInfo, so the rules can be tested without windows.

#include <windows.h>

#include <set>
#include <string>
#include <vector>

namespace StageAdopt
{
    // Shell and system windows that look like normal top-level windows. UWP frames are cloaked/uncloaked by the shell and are not handled cleanly.
    inline bool IsSkippedClass(const std::wstring& cls)
    {
        static const wchar_t* const skipped[] = { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"XamlExplorerHostIslandWindow",
                                                  L"TopLevelWindowForOverflowXamlIsland", L"Windows.UI.Core.CoreWindow", L"NotifyIconOverflowWindow", L"ApplicationFrameWindow" };
        for (const auto* name : skipped)
        {
            if (cls == name)
            {
                return true;
            }
        }

        return false;
    }

    struct AdoptInfo
    {
        bool isWindow{ true };
        bool topLevel{ true }; // its root is itself
        bool visible{ true };
        bool iconic{};
        bool hasOwner{}; // owned windows are dialogs of something else
        bool hung{};
        LONG_PTR style{ WS_OVERLAPPEDWINDOW };
        LONG_PTR exStyle{};
        bool cloaked{};
        bool ownProcess{}; // Muntin's own windows
        bool emptyTitle{};
        std::wstring cls;
        int width{ 800 };
        int height{ 600 };
        int onScreenPx{ 800 }; // StageState::OnScreenPx: under 4 looks parked already
        bool monitorKnown{ true }; // the window is on a monitor with a stage
        bool elevatedBlocked{}; // the window's process is elevated and Muntin is not: it cannot be moved
        bool stuckMaximized{}; // we asked it to restore and it would not: not taken again until it is not maximized
        bool cutLoose{}; // the stack had no room for it at its minimum size (StageState::MinSizeCut): left floating until room appears
        bool heldElsewhere{}; // parked by the desktop scroll, or arranged by StageProto (Muntin.HandsOff)
    };

    constexpr int MinAdoptWidth = 100;
    constexpr int MinAdoptHeight = 60;

    // Why the stage does not take the window; nullptr: it is eligible. The text goes to the log, so a window that is ignored says why.
    inline const wchar_t* SkipReason(const AdoptInfo& w)
    {
        if (!w.isWindow)
            return L"not a window";
        if (!w.topLevel)
            return L"not top level";
        if (!w.visible)
            return L"not visible";
        if (w.iconic)
            return L"minimized";
        if (w.hasOwner)
            return L"owned window (a dialog)";
        if (w.hung)
            return L"not responding";
        if ((w.exStyle & WS_EX_TOOLWINDOW) != 0)
            return L"tool window";
        if ((w.exStyle & WS_EX_NOACTIVATE) != 0)
            return L"no-activate window";
        if ((w.exStyle & WS_EX_TOPMOST) != 0)
            return L"topmost";
        if ((w.style & WS_THICKFRAME) == 0)
            return L"not resizable (no thick frame)";
        if (w.cloaked)
            return L"cloaked";
        if (w.ownProcess)
            return L"Muntin's own window";
        if (w.emptyTitle)
            return L"no title";
        if (IsSkippedClass(w.cls))
            return L"shell or system class";
        if (!w.monitorKnown)
            return L"no monitor with a stage";
        if (w.width < MinAdoptWidth || w.height < MinAdoptHeight)
            return L"too small";
        if (w.stuckMaximized)
            return L"stuck maximized (it would not restore)";
        if (w.cutLoose)
            return L"cut by minimum sizes (left floating until there is room)";
        if (w.heldElsewhere)
            return L"held by the desktop scroll or StageProto";
        if (w.onScreenPx < 4)
            return L"looks parked already (under 4 px on screen)";
        if (w.elevatedBlocked)
            return L"elevated process";
        return nullptr;
    }

    // Eligible: a plain, visible, resizable top-level window of another process that Muntin may move
    inline bool IsEligible(const AdoptInfo& w)
    {
        return SkipReason(w) == nullptr;
    }

    // A window that may own a modal dialog
    struct DialogCandidate
    {
        HWND window{};
        HWND owner{};
        bool visible{};
        bool toolOrNoActivate{}; // tooltips and floating palettes do not count
        bool ownProcess{};
        bool cloaked{};
        bool ownerEnabled{ true };
    };

    // Owners with an open modal dialog: the owner is disabled while the dialog is open. A modeless Find or palette dialog does not
    // count: it can stay open for hours. Such a window stays where it is until the dialog closes, or the dialog would float on its own.
    inline std::set<HWND> DialogOwners(const std::vector<DialogCandidate>& candidates)
    {
        std::set<HWND> owners;
        for (const auto& c : candidates)
        {
            if (!c.visible || !c.owner || c.toolOrNoActivate || c.ownProcess || c.cloaked)
            {
                continue;
            }

            if (!c.ownerEnabled)
            {
                owners.insert(c.owner);
            }
        }

        return owners;
    }

    // A held window that is parked (strip tile or hidden) and now has a modal dialog open: the dialog would float over nothing, so the
    // window comes forward (promoted; the focus is left alone). owners: DialogOwners of the open dialogs. Windows that are guarded already (never
    // parked while the dialog is open) and windows that are not parked are fine as they are.
    struct ParkedFacts
    {
        HWND window{};
        bool parked{};
        bool guarded{};
        bool iconic{};
    };

    // The order does not change under a window the user holds by the mouse
    inline bool MayPromoteOwners(bool dragging)
    {
        return !dragging;
    }

    inline std::vector<HWND> NeedsFront(const std::vector<ParkedFacts>& windows, const std::set<HWND>& owners)
    {
        std::vector<HWND> result;
        for (const auto& w : windows)
        {
            if (w.parked && !w.guarded && !w.iconic && owners.contains(w.window))
            {
                result.push_back(w.window);
            }
        }

        return result;
    }
}
