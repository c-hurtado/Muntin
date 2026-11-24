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
        bool heldElsewhere{}; // parked by the desktop scroll, or arranged by StageProto (Muntin.HandsOff)
    };

    constexpr int MinAdoptWidth = 100;
    constexpr int MinAdoptHeight = 60;

    // Eligible: a plain, visible, resizable top-level window of another process that Muntin may move
    inline bool IsEligible(const AdoptInfo& w)
    {
        constexpr LONG_PTR skippedEx = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;

        if (!w.isWindow || !w.topLevel || !w.visible || w.iconic || w.hasOwner || w.hung)
        {
            return false;
        }

        if ((w.exStyle & skippedEx) != 0 || (w.style & WS_THICKFRAME) == 0) // tool window, no-activate, topmost, not resizable
        {
            return false;
        }

        if (w.cloaked || w.ownProcess || w.emptyTitle || IsSkippedClass(w.cls) || !w.monitorKnown)
        {
            return false;
        }

        if (w.width < MinAdoptWidth || w.height < MinAdoptHeight)
        {
            return false;
        }

        // A window that is parked already (by an earlier run that died, or by the scroll) must not be recorded as "the original"
        if (w.onScreenPx < 4 || w.heldElsewhere)
        {
            return false;
        }

        return !w.elevatedBlocked;
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
}
