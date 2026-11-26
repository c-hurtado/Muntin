#pragma once

// Stage mode: which windows the stage may take over (adoption filter) and which windows must not be parked because a modal
// dialog hangs off them (dialog guard). Ported from StageProto (stage-proto.ps1 Eligible, DialogOwners). Pure: the facts about a
// window are gathered by the caller into AdoptInfo, so the rules can be tested without windows.

#include <windows.h>

#include <algorithm>
#include <cwctype>
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
        bool excludedByUser{}; // its app is in fancyzones_excluded_apps
        bool excludedBuiltIn{}; // its app is on the built-in list the stage always leaves alone (IsBuiltInExcluded)
        bool coversMonitor{}; // its visible frame (DWM extended frame bounds) covers its monitor (within a couple of pixels)
        bool zoomed{}; // maximized (IsZoomed)
        bool youngFullSized{}; // not maximized, covers nearly all of a monitor and is only seconds old (YoungFullSized)
        bool inBackoff{}; // released as an overlay a moment ago and unchanged since (InBackoff)
    };

    // Releasing a window as an overlay and adopting it again must not loop: the same window (same style and extended style) is not
    // taken again for this long
    constexpr unsigned long long BackoffMillis = 10000;

    struct Backoff
    {
        LONG_PTR style{};
        LONG_PTR exStyle{};
        unsigned long long since{};
    };

    inline bool InBackoff(const Backoff* entry, LONG_PTR style, LONG_PTR exStyle, unsigned long long now)
    {
        return entry && entry->style == style && entry->exStyle == exStyle && now < entry->since + BackoffMillis;
    }

    // A capture overlay that keeps its title bar and is not maximized yet covers nearly the whole monitor, only seconds after it was
    // created. A conservative rule: a normal window opens maximized (then it is zoomed), or smaller; one that opens at 95% of a monitor
    // or more without being maximized is declined for its first seconds. False positive: an app that restores to almost full size
    // when it starts is left floating until it is looked at again after those seconds (the pending retries do that).
    constexpr double FullSizedShare = 0.95;
    constexpr unsigned long long YoungMillis = 5000;

    inline bool YoungFullSized(bool zoomed, double monitorShare, unsigned long long ageMs)
    {
        return !zoomed && monitorShare >= FullSizedShare && ageMs < YoungMillis;
    }

    // The share of the monitor that a rect covers (0 to 1)
    inline double MonitorShare(int l, int t, int r, int b, int monitorL, int monitorT, int monitorR, int monitorB)
    {
        const long long area = static_cast<long long>(monitorR - monitorL) * (monitorB - monitorT);
        const int w = (std::min)(r, monitorR) - (std::max)(l, monitorL);
        const int h = (std::min)(b, monitorB) - (std::max)(t, monitorT);
        return area <= 0 || w <= 0 || h <= 0 ? 0.0 : static_cast<double>(static_cast<long long>(w) * h) / static_cast<double>(area);
    }

    // A title bar: both of its style bits
    inline bool HasCaption(LONG_PTR style)
    {
        return (style & WS_CAPTION) == WS_CAPTION;
    }

    // Whether a rect covers a monitor, give or take a couple of pixels (borderless fullscreen apps, games, videos, capture overlays)
    inline bool CoversMonitor(int l, int t, int r, int b, int monitorL, int monitorT, int monitorR, int monitorB, int tolerance = 2)
    {
        return l <= monitorL + tolerance && t <= monitorT + tolerance && r >= monitorR - tolerance && b >= monitorB - tolerance;
    }

    // Windows that are overlays or fullscreen-like, or that the user excluded: never tiled (the Snipping Tool's capture overlay was
    // adopted and painted a grey box over the recorded area). Also what releases a held window that turns into one.
    // No title bar AND no resize frame: a popup the user cannot resize (an overlay, a splash, a borderless fullscreen window). A window
    // without a title bar that still has the resize frame (WezTerm with window_decorations = "RESIZE", app windows of Chrome or Edge,
    // Windows Terminal) is an ordinary resizable window and is never an overlay by this rule.
    inline bool NoFrame(LONG_PTR style)
    {
        return !HasCaption(style) && (style & WS_THICKFRAME) == 0;
    }

    // A window that went fullscreen (F11 in a browser, a video, Alt+Enter in a terminal): no title bar and it covers the monitor. It is
    // not released and moved: a held one is left alone (not tiled, not touched) until it is a normal window again. A frameless-looking
    // window that still has the resize frame and is maximized is an app window, not fullscreen (the stage unmaximizes it as any other).
    inline bool FullscreenLike(const AdoptInfo& w)
    {
        return !HasCaption(w.style) && w.coversMonitor && (NoFrame(w.style) || !w.zoomed);
    }

    // Apps the stage always leaves alone, as if the user had excluded them (the user's list is not changed). Snipping Tool: its main
    // window (XamlWindow "Snipping Tool") turns into a small capture bar while a capture runs; adopted, it was stretched into a slot
    // and left a grey box. processPath: the full path or the file name, any case.
    inline bool IsBuiltInExcluded(const std::wstring& processPath)
    {
        static const wchar_t* const names[] = { L"SNIPPINGTOOL.EXE" };
        const auto slash = processPath.find_last_of(L"\\/");
        std::wstring file = slash == std::wstring::npos ? processPath : processPath.substr(slash + 1);
        for (auto& c : file)
        {
            c = static_cast<wchar_t>(towupper(c));
        }

        for (const auto* name : names)
        {
            if (file == name)
            {
                return true;
            }
        }

        return false;
    }

    // What is released and restored when it is held: the user's exclusion, the built-in exclusions, click-through windows, and layered popups without any frame
    // (WS_EX_LAYERED alone proves nothing: terminals with a translucent background are layered and resizable)
    inline const wchar_t* HardOverlayReason(const AdoptInfo& w)
    {
        if (w.excludedByUser)
            return L"excluded by fancyzones_excluded_apps";
        if (w.excludedBuiltIn)
            return L"excluded by default (Snipping Tool: the stage leaves it alone)";
        if ((w.exStyle & WS_EX_TRANSPARENT) != 0)
            return L"overlay (click-through window)";
        if ((w.exStyle & WS_EX_LAYERED) != 0 && NoFrame(w.style))
            return L"overlay (layered popup without a title bar or a resize frame)";
        return nullptr;
    }

    // Whether a held window that looks fullscreen is merely the stage's own placement: its slot (the visible rect the layout gave it)
    // covers the monitor too (margins zero, taskbar hidden), so its size says nothing
    inline bool IsOwnPlacement(bool slotKnown, bool slotCoversMonitor)
    {
        return slotKnown && slotCoversMonitor;
    }

    // Everything that is not adopted as an overlay
    inline const wchar_t* OverlayReason(const AdoptInfo& w)
    {
        if (const auto* hard = HardOverlayReason(w))
            return hard;
        if (FullscreenLike(w))
            return L"overlay or fullscreen-like window (no title bar, covers the monitor)";
        if (w.youngFullSized)
            return L"covers nearly the whole monitor without being maximized, and is new (probably an overlay)";
        return nullptr;
    }

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
        if (const auto* overlay = OverlayReason(w))
            return overlay;
        if (w.inBackoff)
            return L"released as an overlay a moment ago (unchanged since)";
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

    // A held window that is not docked (strip tile, scrolled out of the strip, hidden) became the foreground window (taskbar, Alt-Tab, an
    // app or the shell activated it): it would have the focus in a sliver with nothing to see, so it is promoted like a click on its tile.
    struct ActivateFacts
    {
        bool held{}; // a window of the stage (not the shell, the taskbar, our own windows)
        bool minimized{}; // released and adopted again when it is restored: nothing to promote here
        bool docked{}; // in a visible slot already
        bool guarded{}; // a modal dialog keeps it where it is
        bool dragging{}; // the user holds a window by the mouse: the order does not change under it
        bool inGrace{}; // the stage itself changed the focus a moment before the foreground event (not before the debounce ended): no feedback loop
        bool stillForeground{true}; // at the end of the debounce it is still the foreground window (the last of an Alt-Tab cycle wins)
    };

    inline bool ShouldPromoteOnActivate(const ActivateFacts& f)
    {
        return f.held && !f.minimized && !f.docked && !f.guarded && !f.dragging && !f.inGrace && f.stillForeground;
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
