#include "pch.h"

#include <climits>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>

#include <FancyZonesLib/Arrange.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/StageAdopt.h>
#include <FancyZonesLib/StageDpi.h>
#include <FancyZonesLib/StageConfig.h>
#include <FancyZonesLib/StageLayouts.h>
#include <FancyZonesLib/StagePark.h>
#include <FancyZonesLib/StageState.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// The pure parts of Stage mode (Phase 3): state file, restore planning, clamping, adoption filter, dialog guard, parking, persisted
// choices, the hands-off registry. Files only in a temp folder of their own: never the real Muntin data folder, never SaveData.

namespace
{
    using StageLayouts::LRect;

    // Folder of its own, gone at the end of the test
    struct TempFolder
    {
        std::filesystem::path path;
        TempFolder()
        {
            path = std::filesystem::temp_directory_path() / (L"muntin-stage-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            std::filesystem::create_directories(path);
        }

        ~TempFolder()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    // Any temp file left in the folder of the state file?
    bool HasTempFiles(const std::filesystem::path& folder)
    {
        for (const auto& item : std::filesystem::recursive_directory_iterator(folder))
        {
            if (item.path().extension() == L".tmp")
            {
                return true;
            }
        }

        return false;
    }

    StageState::Entry MakeEntry(std::int64_t hwnd, std::uint32_t pid, const std::wstring& cls)
    {
        StageState::Entry e;
        e.hwnd = hwnd;
        e.pid = pid;
        e.rect = LRect(-1920, 40, -640, 900);
        e.showCmd = 3;
        e.flags = 2;
        e.minX = -1;
        e.minY = -1;
        e.maxX = -32000;
        e.maxY = -32000;
        e.normal = LRect(100, 120, 900, 700);
        e.cls = cls;
        return e;
    }

    StageState::Probe FakeProbe()
    {
        // 10: fine, 11: gone, 12: handle reused by another process, 13: class changed, 14: not responding
        return {
            [](std::int64_t h) { return h != 11; },
            [](std::int64_t h) { return static_cast<std::uint32_t>(h == 12 ? 999 : 5); },
            [](std::int64_t h) { return std::wstring(h == 13 ? L"Other" : L"Notepad"); },
            [](std::int64_t h) { return h == 14; },
        };
    }

    StageAdopt::AdoptInfo Plain()
    {
        StageAdopt::AdoptInfo i;
        i.cls = L"Notepad";
        return i;
    }
}

namespace FancyZonesUnitTests
{
    TEST_CLASS (StageModeUnitTests)
    {
        TEST_METHOD_CLEANUP(Cleanup)
        {
            Stage::SetHeld({});
            FancyZonesSettings::instance().SetSettings(Settings{});
        }

        TEST_METHOD (StateSerializeParseRoundTrip)
        {
            const std::vector<StageState::Entry> entries = { MakeEntry(1234, 77, L"Notepad"), MakeEntry(-5, 8, L"Chrome_WidgetWin_1"), MakeEntry(99, 3, L"Class With Spaces é") };
            const auto parsed = StageState::Parse(StageState::Serialize(entries));
            Assert::AreEqual(entries.size(), parsed.size());
            for (size_t i = 0; i < entries.size(); i++)
            {
                Assert::IsTrue(entries[i] == parsed[i], std::to_wstring(i).c_str());
            }
        }

        TEST_METHOD (StateParseSkipsBrokenLinesAndForeignFiles)
        {
            Assert::IsTrue(StageState::Parse(L"").empty());
            Assert::IsTrue(StageState::Parse(L"not our file\n1 2 3\n").empty(), L"no header");
            const auto good = StageState::Serialize({ MakeEntry(1, 2, L"A") });
            const auto text = good + L"garbage line\n1 2 3\n" + StageState::Serialize({ MakeEntry(7, 8, L"B") }).substr(wcslen(StageState::Header) + 1);
            const auto parsed = StageState::Parse(text);
            Assert::AreEqual(size_t{ 2 }, parsed.size(), L"half-written lines are skipped, the rest restores");
            Assert::AreEqual(std::int64_t{ 7 }, parsed[1].hwnd);

            // CRLF line ends (a file touched by an editor)
            std::wstring crlf;
            for (const wchar_t c : good)
            {
                crlf += c == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, c);
            }

            const auto fromCrlf = StageState::Parse(crlf);
            Assert::AreEqual(size_t{ 1 }, fromCrlf.size());
            Assert::AreEqual(std::wstring(L"A"), fromCrlf[0].cls);
        }

        TEST_METHOD (StateFileIsWrittenAtomicallyAndRemovedWhenEmpty)
        {
            TempFolder folder;
            const auto file = folder.path / L"sub" / L"stage-state.txt";
            const std::vector<StageState::Entry> one = { MakeEntry(10, 5, L"Notepad") };
            Assert::IsTrue(StageState::Write(file, one), L"creates the folder");
            Assert::IsTrue(std::filesystem::exists(file));
            Assert::IsFalse(HasTempFiles(folder.path), L"the temp file was renamed over the target");
            Assert::IsTrue(StageState::Read(file).front() == one.front());

            const std::vector<StageState::Entry> two = { MakeEntry(10, 5, L"Notepad"), MakeEntry(20, 5, L"über") };
            Assert::IsTrue(StageState::Write(file, two));
            Assert::AreEqual(size_t{ 2 }, StageState::Read(file).size(), L"replaced");
            Assert::IsTrue(StageState::Read(file)[1] == two[1], L"UTF-8 round trip");
            Assert::IsFalse(HasTempFiles(folder.path));
            Assert::IsTrue(StageState::TempName(file).find(std::to_wstring(GetCurrentProcessId())) != std::wstring::npos, L"the temp name is this process's own");

            Assert::IsTrue(StageState::Write(file, {}), L"nothing to restore: the file goes");
            Assert::IsFalse(std::filesystem::exists(file));
            Assert::IsTrue(StageState::Read(file).empty());
        }

        TEST_METHOD (StateWriteFailureIsReported)
        {
            TempFolder folder;
            const auto blocker = folder.path / L"blocker";
            std::ofstream(blocker) << "a file where the folder should be";
            Assert::IsFalse(StageState::Write(blocker / L"stage-state.txt", { MakeEntry(1, 2, L"A") }), L"the caller must not move windows then");
        }

        TEST_METHOD (RestorePlan)
        {
            const std::vector<StageState::Entry> entries = {
                MakeEntry(10, 5, L"Notepad"), MakeEntry(11, 5, L"Notepad"), MakeEntry(12, 5, L"Notepad"), MakeEntry(13, 5, L"Notepad"), MakeEntry(14, 5, L"Notepad")
            };
            const auto plan = StageState::PlanRestore(entries, FakeProbe());
            Assert::IsTrue(plan[0] == StageState::RestoreAction::Restore);
            Assert::IsTrue(plan[1] == StageState::RestoreAction::Skip, L"gone");
            Assert::IsTrue(plan[2] == StageState::RestoreAction::Skip, L"handle reused by another process");
            Assert::IsTrue(plan[3] == StageState::RestoreAction::Skip, L"class changed");
            Assert::IsTrue(plan[4] == StageState::RestoreAction::Keep, L"hung: stays in the file");
        }

        TEST_METHOD (SavedRectUsableOnlyWhileOnAMonitor)
        {
            const std::vector<LRect> monitors = { LRect(0, 0, 1920, 1080), LRect(1920, 0, 3840, 1080) };
            Assert::IsTrue(StageState::RectUsable(LRect(100, 100, 900, 700), monitors));
            Assert::IsTrue(StageState::RectUsable(LRect(1800, 100, 2200, 700), monitors), L"across two monitors");
            Assert::IsFalse(StageState::RectUsable(LRect(5000, 100, 5800, 700), monitors), L"its monitor is gone");
            Assert::IsFalse(StageState::RectUsable(LRect(100, 1079, 900, 1800), monitors), L"a 1 px sliver is not visible");
            Assert::IsFalse(StageState::RectUsable(LRect(10, 10, 10, 50), monitors), L"empty");
            Assert::IsTrue(StageState::RectUsable(LRect(5000, 0, 5800, 700), {}), L"no monitor known: trust the rect");
        }

        TEST_METHOD (MinSizesAreClampedToTheWorkArea)
        {
            Assert::AreEqual(0, StageState::ClampMin(-5, 1000));
            Assert::AreEqual(1000, StageState::ClampMin(INT_MAX, 1000));
            Assert::AreEqual(1000, StageState::ClampMin(1LL << 40, 1000));
            Assert::AreEqual(0, StageState::ClampMin(500, -3), L"a negative limit");
            Assert::AreEqual(0, StageState::VisibleMin(10, 8, 8, 1000), L"smaller than its frame");
            Assert::AreEqual(1000, StageState::VisibleMin(LONG_MAX, 0, 0, 1000), L"never overflows");
            Assert::AreEqual(1000, StageState::VisibleMin(LONG_MAX, INT_MIN, 0, 1000));

            const LRect work(0, 0, 1920, 1040);
            std::vector<StageLayouts::LWin> wins = { { 1, StageLayouts::WinFlags::None, INT_MAX, INT_MAX }, { 2, StageLayouts::WinFlags::None, INT_MAX, 300 },
                                                     { 3, StageLayouts::WinFlags::None, -7, INT_MAX }, { 4, StageLayouts::WinFlags::None, 0, 0 } };
            StageState::ClampMinSizes(wins, work);
            for (const auto& w : wins)
            {
                Assert::IsTrue(w.MinW >= 0 && w.MinW <= work.W());
                Assert::IsTrue(w.MinH >= 0 && w.MinH <= work.H());
            }

            // The layout takes the clamped values and stays within sane bounds (no wrapped sums, no negative sizes)
            StageLayouts::LayoutInput in;
            in.Work = work;
            in.Wins = wins;
            const auto plan = StageLayouts::MakeMaster()->Compute(in);
            Assert::AreEqual(wins.size(), plan.Items.size());
            for (const auto& item : plan.Items)
            {
                if (item.Role == StageLayouts::Role::Main || item.Role == StageLayouts::Role::Docked)
                {
                    Assert::IsTrue(item.Rect.W() > 0 && item.Rect.H() > 0);
                    Assert::IsTrue(item.Rect.W() <= 2 * work.W() && item.Rect.H() <= 8 * work.H());
                }
            }
        }

        TEST_METHOD (AdoptionFilter)
        {
            Assert::IsTrue(StageAdopt::IsEligible(Plain()));

            const auto reject = [](const wchar_t* why, auto change) {
                auto i = Plain();
                change(i);
                Assert::IsFalse(StageAdopt::IsEligible(i), why);
            };
            reject(L"not a window", [](auto& i) { i.isWindow = false; });
            reject(L"child window", [](auto& i) { i.topLevel = false; });
            reject(L"invisible", [](auto& i) { i.visible = false; });
            reject(L"minimized", [](auto& i) { i.iconic = true; });
            reject(L"owned (a dialog)", [](auto& i) { i.hasOwner = true; });
            reject(L"hung", [](auto& i) { i.hung = true; });
            reject(L"tool window", [](auto& i) { i.exStyle = WS_EX_TOOLWINDOW; });
            reject(L"no-activate", [](auto& i) { i.exStyle = WS_EX_NOACTIVATE; });
            reject(L"topmost", [](auto& i) { i.exStyle = WS_EX_TOPMOST; });
            reject(L"not resizable", [](auto& i) { i.style = WS_POPUP | WS_CAPTION; });
            reject(L"cloaked", [](auto& i) { i.cloaked = true; });
            reject(L"Muntin's own", [](auto& i) { i.ownProcess = true; });
            reject(L"no title", [](auto& i) { i.emptyTitle = true; });
            reject(L"shell class", [](auto& i) { i.cls = L"Shell_TrayWnd"; });
            reject(L"UWP frame", [](auto& i) { i.cls = L"ApplicationFrameWindow"; });
            reject(L"no monitor with a stage", [](auto& i) { i.monitorKnown = false; });
            reject(L"too narrow", [](auto& i) { i.width = 99; });
            reject(L"too short", [](auto& i) { i.height = 59; });
            reject(L"looks parked already", [](auto& i) { i.onScreenPx = 3; });
            reject(L"parked by the scroll or arranged by StageProto", [](auto& i) { i.heldElsewhere = true; });
            reject(L"elevated", [](auto& i) { i.elevatedBlocked = true; });

            auto edge = Plain();
            edge.width = 100;
            edge.height = 60;
            edge.onScreenPx = 4;
            Assert::IsTrue(StageAdopt::IsEligible(edge), L"the limits themselves are fine");
        }

        TEST_METHOD (DialogGuard)
        {
            const auto h = [](int v) { return reinterpret_cast<HWND>(static_cast<intptr_t>(v)); };
            StageAdopt::DialogCandidate modal{ h(2), h(1), true, false, false, false, false }; // owner disabled
            StageAdopt::DialogCandidate modeless = modal;
            modeless.window = h(4);
            modeless.owner = h(3);
            modeless.ownerEnabled = true; // a Find box: the owner stays enabled
            StageAdopt::DialogCandidate tooltip = modal;
            tooltip.window = h(6);
            tooltip.owner = h(5);
            tooltip.toolOrNoActivate = true;
            StageAdopt::DialogCandidate hidden = modal;
            hidden.window = h(8);
            hidden.owner = h(7);
            hidden.visible = false;
            StageAdopt::DialogCandidate own = modal;
            own.window = h(10);
            own.owner = h(9);
            own.ownProcess = true;
            StageAdopt::DialogCandidate cloaked = modal;
            cloaked.window = h(12);
            cloaked.owner = h(11);
            cloaked.cloaked = true;

            const auto owners = StageAdopt::DialogOwners({ modal, modeless, tooltip, hidden, own, cloaked });
            Assert::AreEqual(size_t{ 1 }, owners.size());
            Assert::IsTrue(owners.contains(h(1)));
        }

        TEST_METHOD (OverlaysAndFullscreenLikeWindowsAreNotAdopted)
        {
            // The Snipping Tool's capture overlay: a XamlWindow without a title bar that covers the monitor
            auto overlay = Plain();
            overlay.cls = L"XamlWindow";
            overlay.style = WS_POPUP | WS_THICKFRAME | WS_VISIBLE;
            overlay.coversMonitor = true;
            const wchar_t* reason = StageAdopt::SkipReason(overlay);
            Assert::IsNotNull(reason);
            Assert::IsTrue(std::wstring(reason).find(L"overlay") != std::wstring::npos, L"logged as an overlay");

            auto bordered = overlay;
            bordered.style = WS_OVERLAPPEDWINDOW;
            Assert::IsNull(StageAdopt::SkipReason(bordered), L"with a title bar a window that covers the monitor (maximized) is fine");

            auto smallFrameless = Plain();
            smallFrameless.style = WS_POPUP | WS_THICKFRAME;
            Assert::IsNull(StageAdopt::SkipReason(smallFrameless), L"a frameless app that does not cover the monitor is fine");

            auto layered = Plain();
            layered.style = WS_POPUP;
            layered.exStyle = WS_EX_LAYERED;
            Assert::IsNotNull(StageAdopt::SkipReason(layered), L"a layered popup without a title bar or a resize frame is an overlay");
            layered.style = WS_POPUP | WS_THICKFRAME;
            Assert::IsNull(StageAdopt::SkipReason(layered), L"layered but resizable (a translucent terminal): an app");
            layered.style = WS_OVERLAPPEDWINDOW;
            Assert::IsNull(StageAdopt::SkipReason(layered), L"a layered window with a title bar is an app");

            auto transparent = Plain();
            transparent.exStyle = WS_EX_TRANSPARENT;
            Assert::IsNotNull(StageAdopt::SkipReason(transparent), L"click-through");

            auto excluded = Plain();
            excluded.excludedByUser = true;
            Assert::AreEqual(std::wstring(L"excluded by fancyzones_excluded_apps"), std::wstring(StageAdopt::SkipReason(excluded)));
            Assert::IsNull(StageAdopt::OverlayReason(Plain()));
            Assert::IsNotNull(StageAdopt::OverlayReason(excluded), L"a held window of an app excluded meanwhile is released");
        }

        TEST_METHOD (FullscreenIsLeftAloneButOverlaysAreReleased)
        {
            auto fullscreen = Plain();
            fullscreen.style = WS_POPUP | WS_THICKFRAME | WS_VISIBLE;
            fullscreen.coversMonitor = true;
            Assert::IsTrue(StageAdopt::FullscreenLike(fullscreen), L"F11 in a browser, a video, Alt+Enter");
            Assert::IsNull(StageAdopt::HardOverlayReason(fullscreen), L"not released and moved: only left alone");
            Assert::IsNotNull(StageAdopt::OverlayReason(fullscreen), L"but not adopted either");

            auto windowed = fullscreen;
            windowed.style = WS_OVERLAPPEDWINDOW;
            Assert::IsFalse(StageAdopt::FullscreenLike(windowed), L"back to a normal window: landed again");
            auto notCovering = fullscreen;
            notCovering.coversMonitor = false;
            Assert::IsFalse(StageAdopt::FullscreenLike(notCovering));

            auto transparent = Plain();
            transparent.exStyle = WS_EX_TRANSPARENT;
            Assert::IsNotNull(StageAdopt::HardOverlayReason(transparent));
            auto layered = Plain();
            layered.style = WS_POPUP;
            layered.exStyle = WS_EX_LAYERED;
            Assert::IsNotNull(StageAdopt::HardOverlayReason(layered));
            auto excluded = Plain();
            excluded.excludedByUser = true;
            Assert::IsNotNull(StageAdopt::HardOverlayReason(excluded));
        }

        TEST_METHOD (WezTermWithoutATitleBarIsStillAnOrdinaryWindow)
        {
            // window_decorations = "RESIZE": a resize frame, no title bar. Layered or not, docked as the master filling the work area or
            // maximized: adopted and never an overlay.
            for (const bool layered : { false, true })
            {
                auto wez = Plain();
                wez.cls = L"org.wezfurlong.wezterm";
                wez.style = WS_POPUP | WS_THICKFRAME | WS_VISIBLE | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
                wez.exStyle = layered ? WS_EX_LAYERED : 0;
                Assert::IsFalse(StageAdopt::NoFrame(wez.style), L"it has the resize frame");
                Assert::IsNull(StageAdopt::SkipReason(wez), L"adopted");
                Assert::IsNull(StageAdopt::HardOverlayReason(wez), L"not released");

                auto filling = wez;
                filling.coversMonitor = true;
                filling.zoomed = true;
                Assert::IsFalse(StageAdopt::FullscreenLike(filling), L"maximized with a resize frame: an app window");
                Assert::IsNull(StageAdopt::SkipReason(filling), L"still adopted");

                // docked as the master that fills the work area (the monitor when the taskbar is hidden), not maximized
                auto docked = wez;
                docked.coversMonitor = true;
                Assert::IsTrue(StageAdopt::FullscreenLike(docked), L"by its size alone it looks like F11");
                Assert::IsTrue(StageAdopt::IsOwnPlacement(true, true), L"but its slot fills the monitor too: the stage put it there");
                Assert::IsFalse(StageAdopt::IsOwnPlacement(true, false), L"a slot that does not fill the monitor: it really went fullscreen");
                Assert::IsFalse(StageAdopt::IsOwnPlacement(false, false), L"not docked: nothing to compare with");
            }
        }

        TEST_METHOD (BorderlessPopupCoveringTheMonitorIsAnOverlayAndAMaximizedWindowIsAdopted)
        {
            auto popup = Plain();
            popup.style = WS_POPUP | WS_VISIBLE;
            popup.coversMonitor = true;
            Assert::IsTrue(StageAdopt::NoFrame(popup.style));
            Assert::IsTrue(StageAdopt::FullscreenLike(popup));
            Assert::IsNotNull(StageAdopt::SkipReason(popup), L"a borderless popup over the monitor");

            auto layeredPopup = Plain();
            layeredPopup.style = WS_POPUP | WS_VISIBLE;
            layeredPopup.exStyle = WS_EX_LAYERED;
            Assert::IsNotNull(StageAdopt::HardOverlayReason(layeredPopup), L"layered with no frame at all");

            auto maximized = Plain();
            maximized.style = WS_OVERLAPPEDWINDOW | WS_MAXIMIZE;
            maximized.coversMonitor = true;
            maximized.zoomed = true;
            Assert::IsNull(StageAdopt::SkipReason(maximized), L"a maximized window with a title bar");

            auto f11 = Plain();
            f11.style = WS_POPUP | WS_THICKFRAME | WS_VISIBLE;
            f11.coversMonitor = true;
            f11.zoomed = false;
            Assert::IsTrue(StageAdopt::FullscreenLike(f11), L"F11 of a resizable window: fullscreen, hands off");
            Assert::IsNull(StageAdopt::HardOverlayReason(f11), L"never released and moved");
        }

        TEST_METHOD (ReleasedOverlayIsNotTakenAgainUnchangedForTenSeconds)
        {
            const StageAdopt::Backoff entry{ WS_POPUP | WS_THICKFRAME, WS_EX_LAYERED, 1000 };
            Assert::IsTrue(StageAdopt::InBackoff(&entry, WS_POPUP | WS_THICKFRAME, WS_EX_LAYERED, 1000 + 9999));
            Assert::IsFalse(StageAdopt::InBackoff(&entry, WS_POPUP | WS_THICKFRAME, WS_EX_LAYERED, 1000 + StageAdopt::BackoffMillis), L"after ten seconds: one more try");
            Assert::IsFalse(StageAdopt::InBackoff(&entry, WS_OVERLAPPEDWINDOW, WS_EX_LAYERED, 1500), L"its style changed: a new window as far as the rule goes");
            Assert::IsFalse(StageAdopt::InBackoff(&entry, WS_POPUP | WS_THICKFRAME, 0, 1500));
            Assert::IsFalse(StageAdopt::InBackoff(nullptr, 0, 0, 1500), L"never released");

            auto info = Plain();
            info.inBackoff = true;
            Assert::IsNotNull(StageAdopt::SkipReason(info));
        }

        TEST_METHOD (NewFullSizedWindowWithATitleBarIsAnOverlayOnlyForAFewSeconds)
        {
            Assert::IsTrue(StageAdopt::YoungFullSized(false, 1.0, 400));
            Assert::IsTrue(StageAdopt::YoungFullSized(false, 0.96, 4999));
            Assert::IsFalse(StageAdopt::YoungFullSized(false, 1.0, 5000), L"a few seconds old: an ordinary window");
            Assert::IsFalse(StageAdopt::YoungFullSized(true, 1.0, 400), L"maximized: an ordinary window");
            Assert::IsFalse(StageAdopt::YoungFullSized(false, 0.90, 400), L"a large window is not an overlay");
            Assert::IsFalse(StageAdopt::YoungFullSized(false, 0.60, 100));
            Assert::AreEqual(1.0, StageAdopt::MonitorShare(-8, 0, 1928, 1080, 0, 0, 1920, 1080), 1e-9, L"bigger than the monitor: all of it");
            Assert::AreEqual(0.0, StageAdopt::MonitorShare(2000, 0, 3000, 1080, 0, 0, 1920, 1080), 1e-9);
            Assert::AreEqual(0.5, StageAdopt::MonitorShare(0, 0, 960, 1080, 0, 0, 1920, 1080), 1e-9);

            auto info = Plain();
            info.youngFullSized = true;
            Assert::IsNotNull(StageAdopt::SkipReason(info), L"declined while it is new and looks like an overlay");
            info.youngFullSized = false;
            Assert::IsNull(StageAdopt::SkipReason(info));
        }

        TEST_METHOD (CoversMonitorToleratesAFewPixels)
        {
            Assert::IsTrue(StageAdopt::CoversMonitor(0, 0, 1920, 1080, 0, 0, 1920, 1080));
            Assert::IsTrue(StageAdopt::CoversMonitor(-7, -7, 1927, 1087, 0, 0, 1920, 1080), L"bigger than the monitor");
            Assert::IsTrue(StageAdopt::CoversMonitor(2, 1, 1918, 1079, 0, 0, 1920, 1080), L"within 2 px");
            Assert::IsFalse(StageAdopt::CoversMonitor(0, 0, 1920, 1040, 0, 0, 1920, 1080), L"the taskbar is left: a window, not an overlay");
            Assert::IsFalse(StageAdopt::CoversMonitor(100, 100, 1000, 800, 0, 0, 1920, 1080));
            Assert::IsTrue(StageAdopt::HasCaption(WS_OVERLAPPEDWINDOW));
            Assert::IsFalse(StageAdopt::HasCaption(WS_POPUP | WS_THICKFRAME | WS_BORDER), L"a border alone is no title bar");
        }

        TEST_METHOD (ActivatingAParkedWindowPromotesIt)
        {
            StageAdopt::ActivateFacts f;
            f.held = true;
            Assert::IsTrue(StageAdopt::ShouldPromoteOnActivate(f), L"a parked held window that got the focus");

            auto not_held = f;
            not_held.held = false;
            Assert::IsFalse(StageAdopt::ShouldPromoteOnActivate(not_held), L"shell, taskbar, our own windows, cut windows");
            auto minimized = f;
            minimized.minimized = true;
            Assert::IsFalse(StageAdopt::ShouldPromoteOnActivate(minimized), L"released and adopted again on restore");
            auto docked = f;
            docked.docked = true;
            Assert::IsFalse(StageAdopt::ShouldPromoteOnActivate(docked), L"already in a slot");
            auto guarded = f;
            guarded.guarded = true;
            Assert::IsFalse(StageAdopt::ShouldPromoteOnActivate(guarded), L"a modal dialog keeps it put");
            auto dragging = f;
            dragging.dragging = true;
            Assert::IsFalse(StageAdopt::ShouldPromoteOnActivate(dragging));
            auto grace = f;
            grace.inGrace = true;
            Assert::IsFalse(StageAdopt::ShouldPromoteOnActivate(grace), L"the stage moved the focus itself: no feedback loop");
            auto moved = f;
            moved.stillForeground = false;
            Assert::IsFalse(StageAdopt::ShouldPromoteOnActivate(moved), L"Alt-Tab went on: only the last window counts");
        }

        TEST_METHOD (ParkingAsASliverAtTheBottomOfTheMonitor)
        {
            const LRect monitor(0, 0, 1920, 1080);
            const std::vector<LRect> monitors = { monitor };
            const LRect behind(0, 0, 1152, 1040); // the master place
            const LRect window(-7, 0, 1159, 1047); // a docked window with its invisible frame
            const auto park = StagePark::Park(window, behind, monitor, monitors);
            Assert::IsTrue(park.sliver);
            Assert::AreEqual(0, park.outer.L, L"x clamped inside the monitor");
            Assert::AreEqual(1079, park.outer.T, L"one pixel left on the monitor");
            Assert::AreEqual(window.W(), park.outer.W(), L"never resized: the app keeps its last frame");
            Assert::AreEqual(window.H(), park.outer.H());
            Assert::IsTrue(StagePark::LooksParked(park.outer, monitor));
            Assert::IsTrue(StagePark::Park(park.outer, behind, monitor, monitors).outer.Same(park.outer), L"parking again changes nothing");
            Assert::IsFalse(StagePark::LooksParked(window, monitor));
        }

        TEST_METHOD (ParkingNeverReachesAnotherMonitor)
        {
            const LRect upper(0, 0, 1920, 1080), lower(0, 1080, 1920, 2160);
            const std::vector<LRect> monitors = { upper, lower };
            const LRect master(100, 0, 1000, 1040);
            const auto below = StagePark::Park(master, master, upper, monitors);
            Assert::IsFalse(below.sliver, L"a sliver would show on the monitor below");
            Assert::IsTrue(below.outer.Same(master), L"it waits behind the master (the fallback place)");

            const LRect onLower(100, 1080, 1000, 2120);
            const auto bottom = StagePark::Park(onLower, master, lower, monitors);
            Assert::IsTrue(bottom.sliver, L"nothing below the lower monitor");
            Assert::AreEqual(2159, bottom.outer.T);

            const LRect right(1920, 0, 3840, 1080);
            Assert::IsTrue(StagePark::Park(master, master, upper, { upper, right }).sliver, L"a monitor on the side does not block");
        }

        TEST_METHOD (ParkedWindowKeepsItsSizeAndStaysInsideTheMonitorHorizontally)
        {
            const LRect monitor(1920, 0, 3840, 1080), left(0, 0, 1920, 1080);
            const LRect behind(1920, 0, 3000, 1040);
            const std::vector<LRect> monitors = { left, monitor };
            // Hanging over the left edge into the other monitor: x moves in, size stays
            auto park = StagePark::Park(LRect(1800, 50, 2800, 900), behind, monitor, monitors);
            Assert::IsTrue(park.sliver);
            Assert::AreEqual(1920, park.outer.L);
            Assert::AreEqual(1000, park.outer.W());
            Assert::AreEqual(850, park.outer.H());
            // Hanging over the right edge
            park = StagePark::Park(LRect(3300, 50, 4300, 900), behind, monitor, { monitor });
            Assert::AreEqual(3840 - 1000, park.outer.L);
            Assert::AreEqual(1000, park.outer.W());
            // Wider than the monitor: starts at the left edge, accepts spilling right (nothing there)
            park = StagePark::Park(LRect(2000, 0, 4200, 900), behind, monitor, { monitor });
            Assert::IsTrue(park.sliver);
            Assert::AreEqual(1920, park.outer.L);
            Assert::AreEqual(2200, park.outer.W());
            // ... but not onto another monitor: behind the master, in the master's place and size
            const LRect behindLeft(0, 0, 1152, 1040);
            park = StagePark::Park(LRect(100, 0, 2100, 900), behindLeft, left, { left, monitor });
            Assert::IsFalse(park.sliver, L"the sliver would reach the monitor on the right");
            Assert::IsTrue(park.outer.Same(behindLeft));
        }

        TEST_METHOD (FullHeightWindowStillGetsTheSliver)
        {
            // A window taller than the monitor (a full-height docked window with its frame on a monitor without a taskbar): own size, sliver
            const LRect monitor(0, 0, 1920, 1080);
            const LRect behind(0, 0, 1152, 1040);
            const LRect tall(100, -20, 1000, 1200);
            const auto park = StagePark::Park(tall, behind, monitor, { monitor });
            Assert::IsTrue(park.sliver);
            Assert::AreEqual(1079, park.outer.T);
            Assert::AreEqual(tall.H(), park.outer.H(), L"not resized");
            Assert::AreEqual(tall.W(), park.outer.W());
            // as long as the sliver stays on this monitor
            const LRect lower(0, 1080, 1920, 2160);
            Assert::IsFalse(StagePark::Park(tall, behind, monitor, { monitor, lower }).sliver);
        }

        TEST_METHOD (WindowNotOnThisMonitorOrWithoutARectWaitsAsItAlwaysDid)
        {
            const LRect left(0, 0, 1920, 1080), right(1920, 0, 3840, 1080);
            const LRect behind(1920, 0, 3000, 1040);
            // Coming back from the left monitor: its size is of the other monitor, the master size on this one
            auto park = StagePark::Park(LRect(100, 100, 1100, 900), behind, right, { left, right });
            Assert::IsFalse(park.sliver);
            Assert::IsTrue(park.outer.Same(behind));
            // Unreadable rect
            park = StagePark::Park(LRect(), behind, right, { left, right });
            Assert::IsFalse(park.sliver);
            Assert::IsTrue(park.outer.Same(behind));
            Assert::IsTrue(StagePark::CenterOn(LRect(1800, 0, 2800, 900), right), L"the middle decides, not the edge");
            Assert::IsFalse(StagePark::CenterOn(LRect(1000, 0, 2000, 900), right));
        }

        TEST_METHOD (OuterRectStopsAtSharedMonitorEdges)
        {
            const LRect left(0, 0, 1920, 1080), right(1920, 0, 3840, 1080);
            const StagePark::Insets ins{ 7, 0, 7, 7 };
            const auto alone = StagePark::OuterRect(LRect(100, 100, 900, 700), ins, left, { left, right });
            Assert::IsTrue(alone.Same(LRect(93, 100, 907, 707)));
            const auto edge = StagePark::OuterRect(LRect(1000, 0, 1920, 1040), ins, left, { left, right });
            Assert::AreEqual(1920, edge.R, L"the invisible border does not spill onto the other monitor");
            Assert::AreEqual(993, edge.L);
            const auto single = StagePark::OuterRect(LRect(1000, 0, 1920, 1040), ins, left, { left });
            Assert::AreEqual(1927, single.R);
        }

        TEST_METHOD (HeldRegistryAndHandsOff)
        {
            const HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr); // Never shown
            Assert::IsNotNull(window);
            Assert::IsFalse(Stage::IsHeld(window));
            Stage::SetHeld({ window });
            Assert::IsTrue(Stage::IsHeld(window));
            Assert::IsFalse(Stage::IsHeld(nullptr));

            Assert::IsFalse(Arrange::HandsOff(window), L"dock mode: a held window is not hands-off");
            Settings stage;
            stage.arrangeMode = 1;
            FancyZonesSettings::instance().SetSettings(stage);
            Assert::IsTrue(Arrange::HandsOff(window), L"stage mode: the stage's windows are left alone");

            Stage::SetHeld({});
            Assert::IsFalse(Stage::IsHeld(window));
            Assert::IsFalse(Arrange::HandsOff(window));
            DestroyWindow(window);
        }

        TEST_METHOD (PersistedChoicesRoundTrip)
        {
            Settings s;
            s.stageLayout = L"master";
            s.stagePairCut = 420;
            s.stageMasterCut = 700;
            s.stageRows = L"500,300,200";
            const auto values = StageConfig::FromSettings(s);
            Assert::AreEqual(std::wstring(L"master"), values.layout);
            Assert::AreEqual(700, values.masterCut);

            auto layout = StageConfig::MakeLayout(values);
            Assert::AreEqual(std::string("master"), layout->Name());
            Assert::AreEqual(0.7, layout->Cut(), 1e-9);
            Assert::AreEqual(size_t{ 3 }, layout->Rows().size());
            Assert::IsTrue(StageConfig::Read(*layout, values) == values, L"nothing changed, nothing to save");

            layout->SetCut(0.55);
            layout->SetRows({ 0.4, 0.6 });
            const auto after = StageConfig::Read(*layout, values);
            Assert::AreEqual(550, after.masterCut);
            Assert::AreEqual(420, after.pairCut, L"the other layout's cut is left alone");
            Assert::AreEqual(std::wstring(L"400,600"), after.rows);

            const auto changes = StageConfig::Changes(values, after);
            Assert::AreEqual(size_t{ 2 }, changes.size());
            Assert::AreEqual(std::wstring(NonLocalizable::StageMasterCutID), std::wstring(changes[0].id));
            Assert::IsFalse(changes[0].text);
            Assert::AreEqual(550, changes[0].number);
            Assert::AreEqual(std::wstring(NonLocalizable::StageRowsID), std::wstring(changes[1].id));
            Assert::IsTrue(changes[1].text);
            Assert::AreEqual(std::wstring(L"400,600"), changes[1].textValue);
            Assert::IsTrue(StageConfig::Changes(values, values).empty());

            auto renamed = values;
            renamed.layout = L"pair";
            const auto layoutChange = StageConfig::Changes(values, renamed);
            Assert::AreEqual(size_t{ 1 }, layoutChange.size());
            Assert::AreEqual(std::wstring(NonLocalizable::StageLayoutID), std::wstring(layoutChange[0].id));
        }

        TEST_METHOD (PersistedValuesAreSane)
        {
            Assert::AreEqual(150, StageConfig::ToPermille(0.0));
            Assert::AreEqual(850, StageConfig::ToPermille(7.0));
            Assert::AreEqual(333, StageConfig::ToPermille(1.0 / 3));

            StageConfig::Values values;
            values.rows = L"garbage";
            Assert::IsTrue(StageConfig::MakeLayout(values)->Rows().empty(), L"bad rows: equal heights");

            // The saved choice is the layout that runs; a name nobody knows is the stage layout
            for (const auto* name : { L"stage", L"pair", L"master" })
            {
                values.layout = name;
                Assert::AreEqual(StageConfig::Narrow(name), StageConfig::MakeLayout(values)->Name());
                Assert::AreEqual(StageConfig::Narrow(name), StageConfig::EffectiveLayoutName(name));
            }

            values.layout = L"nope";
            Assert::AreEqual(std::string("master"), StageConfig::MakeLayout(values)->Name(), L"unknown: master, nobody gets the strip unasked");
            Assert::AreEqual(std::string("master"), StageConfig::EffectiveLayoutName(L""));
            Assert::AreEqual(std::wstring(L"master"), StageConfig::Values{}.layout, L"the default");
            Assert::AreEqual(std::wstring(L"master"), Settings{}.stageLayout, L"the default of the settings file");
        }

        TEST_METHOD (MonitorKeyUsesDeviceAndInstanceElseOrigin)
        {
            Assert::AreEqual(std::wstring(L"DELA0B1_5&2d1c&0&UID4352"), StageConfig::MonitorKey(L"DELA0B1", L"5&2d1c&0&UID4352", 1920, 0));
            Assert::AreEqual(std::wstring(L"@1920,-200"), StageConfig::MonitorKey(L"DELA0B1", L"", 1920, -200), L"no instance: the rect origin");
            Assert::AreEqual(std::wstring(L"@0,0"), StageConfig::MonitorKey(L"", L"", 0, 0));
            Assert::AreNotEqual(StageConfig::MonitorKey(L"DELA0B1", L"UID1", 0, 0), StageConfig::MonitorKey(L"DELA0B1", L"UID2", 0, 0), L"two monitors of one model");
        }

        TEST_METHOD (MonitorWithoutEntryFollowsGlobalValues)
        {
            StageConfig::Values global;
            global.pairCut = 420;
            global.masterCut = 700;
            global.rows = L"500,500";
            std::map<std::wstring, StageMonitorValues> monitors;
            Assert::IsTrue(StageConfig::Resolve(global, monitors, L"A") == global);

            monitors[L"A"].masterCut = 300;
            Assert::IsTrue(StageConfig::Resolve(global, monitors, L"B") == global, L"unknown / stale id: global");
            const auto a = StageConfig::Resolve(global, monitors, L"A");
            Assert::AreEqual(300, a.masterCut);
            Assert::AreEqual(420, a.pairCut, L"a field that is not set follows the global one");
            Assert::AreEqual(std::wstring(L"500,500"), a.rows);

            monitors[L"A"].rows = L"";
            monitors[L"A"].pairCut = 99999;
            const auto clamped = StageConfig::Resolve(global, monitors, L"A");
            Assert::AreEqual(std::wstring(), clamped.rows, L"an empty rows entry means equal heights, not the global rows");
            Assert::AreEqual(850, clamped.pairCut);

            global.masterCut = 650;
            monitors[L"A"].masterCut.reset();
            Assert::AreEqual(650, StageConfig::Resolve(global, monitors, L"A").masterCut, L"global changes reach monitors that do not override");
        }

        TEST_METHOD (ResizingOneMonitorLeavesTheOtherAlone)
        {
            StageConfig::Values global;
            global.layout = L"master"; // the layout that owns a cut and rows
            std::map<std::wstring, StageMonitorValues> monitors;
            const auto one = StageConfig::Resolve(global, monitors, L"ONE");
            const auto two = StageConfig::Resolve(global, monitors, L"TWO");
            auto layoutOne = StageConfig::MakeLayout(one);
            auto layoutTwo = StageConfig::MakeLayout(two);

            layoutTwo->SetCut(0.4);
            layoutTwo->SetRows({ 0.3, 0.7 });
            const auto after = StageConfig::Read(*layoutTwo, two);
            const auto change = StageConfig::MonitorChange(two, after);
            Assert::IsFalse(change.Empty());
            Assert::IsFalse(change.pairCut.has_value(), L"only what changed is saved");
            Assert::AreEqual(400, *change.masterCut);
            Assert::AreEqual(std::wstring(L"300,700"), *change.rows);
            StageConfig::Merge(monitors, L"TWO", change);

            Assert::AreEqual(size_t{ 1 }, monitors.size());
            Assert::IsTrue(StageConfig::Resolve(global, monitors, L"ONE") == one, L"monitor one is untouched");
            Assert::AreEqual(0.6, layoutOne->Cut(), 1e-9);
            Assert::AreEqual(400, StageConfig::Resolve(global, monitors, L"TWO").masterCut);
            Assert::IsTrue(StageConfig::MonitorChange(after, after).Empty(), L"nothing changed, nothing saved");

            StageMonitorValues second;
            second.masterCut = 500;
            StageConfig::Merge(monitors, L"TWO", second);
            Assert::AreEqual(std::wstring(L"300,700"), *monitors[L"TWO"].rows, L"merge keeps the other fields");
            Assert::AreEqual(500, *monitors[L"TWO"].masterCut);
            StageConfig::Merge(monitors, L"THREE", StageMonitorValues{});
            Assert::AreEqual(size_t{ 1 }, monitors.size(), L"an empty change adds no entry");
        }

        TEST_METHOD (StageMonitorsSaveAndLoadInATempFile)
        {
            TempFolder temp;
            const auto file = (temp.path / L"settings.json").wstring();
            StageMonitorValues first;
            first.masterCut = 400;
            first.rows = L"300,700";
            Assert::IsTrue(FancyZonesSettings::SaveStageMonitor(L"DELA0B1_UID1", first, file));
            StageMonitorValues other;
            other.pairCut = 300;
            Assert::IsTrue(FancyZonesSettings::SaveStageMonitor(L"@1920,0", other, file));
            StageMonitorValues more;
            more.masterCut = 450;
            Assert::IsTrue(FancyZonesSettings::SaveStageMonitor(L"DELA0B1_UID1", more, file), L"merged into the entry already there");
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageMasterCutID, json::value(555), file), L"other settings keep being written");

            const auto read = json::from_file(file);
            Assert::IsTrue(read.has_value());
            const auto properties = read->GetNamedObject(L"properties");
            Assert::AreEqual(555.0, properties.GetNamedObject(NonLocalizable::StageMasterCutID).GetNamedNumber(L"value"), L"the global value is a field of its own");
            const auto monitors = FancyZonesSettings::ParseStageMonitors(properties.GetNamedObject(NonLocalizable::StageMonitorsID).GetNamedObject(L"value"));
            Assert::AreEqual(size_t{ 2 }, monitors.size());
            Assert::AreEqual(450, *monitors.at(L"DELA0B1_UID1").masterCut);
            Assert::AreEqual(std::wstring(L"300,700"), *monitors.at(L"DELA0B1_UID1").rows);
            Assert::IsFalse(monitors.at(L"DELA0B1_UID1").pairCut.has_value());
            Assert::AreEqual(300, *monitors.at(L"@1920,0").pairCut);
            Assert::IsFalse(monitors.at(L"@1920,0").masterCut.has_value());
        }

        TEST_METHOD (LoadSettingsOldFormatAndMissingKeyClear)
        {
            // The real LoadSettings, on the unit-test settings file (test-settings.json, never settings.json), removed at the end
            const auto file = FancyZonesSettings::GetSettingsFileName();
            std::error_code ignored;
            std::filesystem::remove(file, ignored);

            StageMonitorValues entry;
            entry.masterCut = 300;
            Assert::IsTrue(FancyZonesSettings::SaveStageMonitor(L"DELA0B1_UID1", entry, file));
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(size_t{ 1 }, FancyZonesSettings::settings().stageMonitors.size());
            Assert::AreEqual(300, *FancyZonesSettings::settings().stageMonitors.at(L"DELA0B1_UID1").masterCut);

            // The old format: global values only, no monitor map. Loading it clears what an earlier load had.
            std::filesystem::remove(file, ignored);
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StagePairCutID, json::value(420), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageMasterCutID, json::value(700), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageRowsID, json::value(L"500,500"), file));
            FancyZonesSettings::instance().LoadSettings();
            const auto& s = FancyZonesSettings::settings();
            Assert::IsTrue(s.stageMonitors.empty(), L"the missing key clears the map");
            Assert::AreEqual(420, s.stagePairCut);
            Assert::AreEqual(700, s.stageMasterCut);
            const auto values = StageConfig::Resolve(StageConfig::FromSettings(s), s.stageMonitors, L"DELA0B1_UID1");
            Assert::IsTrue(values == StageConfig::FromSettings(s), L"every monitor follows the old global values");
            auto masterValues = values;
            masterValues.layout = L"master";
            Assert::AreEqual(0.7, StageConfig::MakeLayout(masterValues)->Cut(), 1e-9);

            // Huge numbers clamp to the limit instead of an undefined cast
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StageMasterCutID, json::value(3e9), file));
            Assert::IsTrue(FancyZonesSettings::SaveValue(NonLocalizable::StagePairCutID, json::value(-3e9), file));
            FancyZonesSettings::instance().LoadSettings();
            Assert::AreEqual(850, FancyZonesSettings::settings().stageMasterCut);
            Assert::AreEqual(150, FancyZonesSettings::settings().stagePairCut);
            std::filesystem::remove(file, ignored);
        }

        TEST_METHOD (SaveStageMonitorNeverWritesFromAnUnreadableFile)
        {
            TempFolder temp;
            const auto file = temp.path / L"settings.json";
            const std::string broken = "{ \"properties\": { \"fancyzones_stageMonitors\": ";
            {
                std::ofstream out(file, std::ios::binary);
                out << broken;
            }

            StageMonitorValues change;
            change.masterCut = 400;
            Assert::IsFalse(FancyZonesSettings::SaveStageMonitor(L"A", change, file.wstring()), L"the file exists but can't be read");
            std::ifstream in(file, std::ios::binary);
            const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Assert::AreEqual(broken, after, L"left as it was");

            Assert::IsTrue(FancyZonesSettings::SaveStageMonitor(L"A", change, (temp.path / L"none.json").wstring()), L"no file at all is fine");
        }

        TEST_METHOD (OnlyTheMasterLayoutOwnsRows)
        {
            StageConfig::Values pair;
            pair.layout = L"pair";
            pair.rows = L"400,600";
            auto layout = StageLayouts::MakePair();
            layout->SetCut(0.3);
            const auto after = StageConfig::Read(*layout, pair);
            Assert::AreEqual(300, after.pairCut);
            Assert::AreEqual(std::wstring(L"400,600"), after.rows, L"a pair drag keeps the rows as they were");
            Assert::IsFalse(StageConfig::MonitorChange(pair, after).rows.has_value(), L"and never saves rows");

            StageConfig::Values master;
            master.layout = L"master";
            master.rows = L"400,600";
            auto masterLayout = StageConfig::MakeLayout(master);
            masterLayout->SetRows({ 0.5, 0.5 });
            Assert::AreEqual(std::wstring(L"500,500"), StageConfig::Read(*masterLayout, master).rows);
        }

        TEST_METHOD (UnidentifiedMonitorKeepsItsKeyOrAsksForAnotherLook)
        {
            const auto real = StageConfig::ChooseKey(L"DELA0B1", L"UID1", 0, 0, L"");
            Assert::IsTrue(real.resolved);
            Assert::AreEqual(std::wstring(L"DELA0B1_UID1"), real.key);

            const auto kept = StageConfig::ChooseKey(L"\\\\.\\DISPLAY2", L"", 1920, 0, L"DELA0B1_UID1");
            Assert::IsTrue(kept.resolved, L"the lookup failed: the key it had for that rect stays");
            Assert::AreEqual(std::wstring(L"DELA0B1_UID1"), kept.key);

            const auto fresh = StageConfig::ChooseKey(L"\\\\.\\DISPLAY2", L"", 1920, 0, L"");
            Assert::IsFalse(fresh.resolved, L"nothing known: look again shortly");
            Assert::AreEqual(std::wstring(L"@1920,0"), fresh.key);

            const auto stillFallback = StageConfig::ChooseKey(L"", L"", 1920, 0, L"@1920,0");
            Assert::IsFalse(stillFallback.resolved, L"a previous fallback key is not trusted");
            Assert::IsTrue(StageConfig::ChooseKey(L"DELA0B1", L"UID9", 1920, 0, L"@1920,0").resolved, L"it upgrades once the lookup works");
        }

        TEST_METHOD (JsonNumbersClampWithoutUndefinedCasts)
        {
            Assert::AreEqual(850, ClampJsonNumber(3e9, 150, 850));
            Assert::AreEqual(150, ClampJsonNumber(-3e9, 150, 850));
            Assert::AreEqual(150, ClampJsonNumber(std::numeric_limits<double>::quiet_NaN(), 150, 850));
            Assert::AreEqual(850, ClampJsonNumber(std::numeric_limits<double>::infinity(), 150, 850));
            Assert::AreEqual(500, ClampJsonNumber(500.9, 150, 850));
            const auto parsed = FancyZonesSettings::ParseStageMonitors(json::JsonObject::Parse(L"{\"A\":{\"masterCut\":3000000000,\"pairCut\":-3000000000}}"));
            Assert::AreEqual(850, *parsed.at(L"A").masterCut);
            Assert::AreEqual(150, *parsed.at(L"A").pairCut);
        }

        TEST_METHOD (BadStageMonitorEntriesAreSkipped)
        {
            const auto parsed = FancyZonesSettings::ParseStageMonitors(json::JsonObject::Parse(
                L"{\"A\":{\"masterCut\":5000,\"rows\":\"400,600\"},\"B\":42,\"C\":{},\"D\":{\"pairCut\":\"x\"}}"));
            Assert::AreEqual(size_t{ 1 }, parsed.size());
            Assert::AreEqual(850, *parsed.at(L"A").masterCut, L"clamped like the global value");
            Assert::AreEqual(std::wstring(L"400,600"), *parsed.at(L"A").rows);
        }

        TEST_METHOD (VerifyNeedsMoreThanTheToleranceSeenTwice)
        {
            StageState::Pending pending;
            auto v = StageState::VerifyLanding(1002, 800, 1000, 800, pending);
            Assert::IsFalse(v.learnW || v.learnH || v.lookAgain, L"2 px bigger is within the tolerance");

            v = StageState::VerifyLanding(1003, 800, 1000, 800, pending);
            Assert::IsFalse(v.learnW);
            Assert::IsTrue(v.lookAgain, L"seen once: look again");
            v = StageState::VerifyLanding(1003, 800, 1000, 800, pending);
            Assert::IsTrue(v.learnW && !v.learnH && !v.lookAgain, L"seen twice: it is the minimum");

            v = StageState::VerifyLanding(1100, 800, 1000, 800, pending);
            v = StageState::VerifyLanding(1200, 800, 1000, 800, pending);
            Assert::IsFalse(v.learnW, L"a size that keeps changing (a slow move) is not believed");
            Assert::IsTrue(v.lookAgain);

            v = StageState::VerifyLanding(1000, 800, 1000, 800, pending);
            Assert::IsFalse(v.lookAgain, L"it fits now: the pending size is forgotten");
            v = StageState::VerifyLanding(1100, 800, 1000, 800, pending);
            Assert::IsTrue(v.lookAgain && !v.learnW, L"so one more sighting does not count as the second");

            v = StageState::VerifyLanding(1100, 900, 1000, 800, pending);
            v = StageState::VerifyLanding(1100, 900, 1000, 800, pending);
            Assert::IsTrue(v.learnW && v.learnH, L"both directions");
        }

        TEST_METHOD (CapacityRule)
        {
            Assert::AreEqual(8, StageConfig::MasterMaxStack);
            StageConfig::Values values;
            values.layout = L"master";
            const auto master = StageConfig::MakeLayout(values);
            Assert::AreEqual(9, StageConfig::CapacityOf(*master, StageLayouts::LRect(0, 0, 1920, 1080), StageLayouts::Spacing::MuntinDefault()), L"master + a stack of 8");
            const StageLayouts::LRect work(0, 0, 2560, 1440);
            const auto spacing = StageLayouts::Spacing::MuntinDefault();
            Assert::AreEqual(9, StageConfig::CapacityOf(*master, work, spacing));
            const int stageCap = StageConfig::CapacityOf(*StageLayouts::MakeStage(), work, spacing);
            const int pairCap = StageConfig::CapacityOf(*StageLayouts::MakePair(), work, spacing);
            Assert::AreEqual(StageConfig::StripMaxWindows, stageCap, L"the strip scrolls: every window is taken");
            Assert::AreEqual(StageConfig::StripMaxWindows, pairCap);

            Assert::IsTrue(StageConfig::HasRoom(0, 9));
            Assert::IsTrue(StageConfig::HasRoom(8, 9));
            Assert::IsFalse(StageConfig::HasRoom(9, 9), L"the tenth window is not taken");
            Assert::IsFalse(StageConfig::HasRoom(0, 0));
            Assert::IsTrue(StageConfig::HasRoom(1000, (std::numeric_limits<int>::max)()));

            // Everything that is taken is shown: with 9 windows none is Hidden (parked)
            StageLayouts::LayoutInput in;
            in.Work = LRect(0, 0, 3000, 2000);
            for (int i = 1; i <= 9; i++)
            {
                in.Wins.emplace_back(i);
            }

            for (const auto& item : master->Compute(in).Items)
            {
                Assert::IsTrue(item.Role != StageLayouts::Role::Hidden && !item.Rect.Same(LRect()));
            }
        }

        TEST_METHOD (PlacementFlags)
        {
            constexpr int async = 4, toMaximized = 2;
            auto maximized = MakeEntry(1, 2, L"A");
            maximized.showCmd = 3;
            maximized.flags = 0;
            auto normal = MakeEntry(1, 2, L"A");
            normal.showCmd = 1;
            normal.flags = 0;

            auto p = StageState::PlacementFor(normal, false, false);
            Assert::AreEqual(1, p.showCmd);
            Assert::AreEqual(async, p.flags, L"always async");
            p = StageState::PlacementFor(maximized, false, false);
            Assert::AreEqual(3, p.showCmd);
            Assert::AreEqual(async, p.flags);
            p = StageState::PlacementFor(maximized, true, false);
            Assert::AreEqual(2, p.showCmd, L"put back minimized");
            Assert::AreEqual(async | toMaximized, p.flags, L"and restores to maximized");
            p = StageState::PlacementFor(normal, true, false);
            Assert::AreEqual(async, p.flags, L"a normal window restores to normal");
            p = StageState::PlacementFor(maximized, false, true);
            Assert::IsTrue(p.usePlacement);
            Assert::AreEqual(0, p.showCmd, L"stays hidden");
            Assert::AreEqual(async | toMaximized, p.flags);
            p = StageState::PlacementFor(normal, false, true);
            Assert::IsFalse(p.usePlacement, L"hidden and normal: only the rect goes back");
        }

        TEST_METHOD (RestoreRectFromACoordinateMixUpIsBroughtDownToAMonitor)
        {
            // Monitor 0: 2560x1440 at 0 (100%), monitor 1: 3840x2160 at x=2560 (150%)
            const std::vector<LRect> monitors = { LRect(0, 0, 2560, 1440), LRect(2560, 0, 6400, 2160) };

            // What was really saved: wider than any monitor
            const auto sane = StageState::SaneRect(LRect(2561, 10, 8318, 2190), monitors);
            Assert::AreEqual(3840, sane.W(), L"as wide as the monitor it was mostly on");
            Assert::IsTrue(sane.Inside(LRect(2560, 0, 6400, 2160)));
            Assert::IsTrue(sane.W() <= 3840 && sane.H() <= 2160);

            // Real windows stay exactly as they are, maximized ones with their invisible frame too
            const LRect normal(100, 100, 1500, 900), maximized(-8, -8, 2568, 1448), spanning(2000, 100, 3200, 900);
            Assert::IsTrue(StageState::SaneRect(normal, monitors).Same(normal));
            Assert::IsTrue(StageState::SaneRect(maximized, monitors).Same(maximized));
            Assert::IsTrue(StageState::SaneRect(spanning, monitors).Same(spanning), L"across two monitors is fine");
            Assert::IsTrue(StageState::SaneRect(LRect(0, 0, 99999, 50), {}).W() == 99999, L"no monitors known: unchanged");

            const auto tall = StageState::SaneRect(LRect(-500, -500, 600, 9000), monitors);
            Assert::IsTrue(tall.H() <= 1440 && tall.Inside(LRect(0, 0, 2560, 1440)));

            auto entry = MakeEntry(1, 2, L"Chrome_WidgetWin_1");
            entry.rect = LRect(2561, 10, 8318, 2190);
            entry.normal = LRect(2561, 10, 8318, 2190);
            const auto clean = StageState::Sanitized(entry, monitors);
            Assert::IsTrue(clean.rect.Inside(LRect(2560, 0, 6400, 2160)) && clean.normal.Inside(LRect(2560, 0, 6400, 2160)));
            Assert::AreEqual(entry.pid, clean.pid);
            Assert::AreEqual(entry.cls, clean.cls);
        }

        TEST_METHOD (MaximizedWindowOnTheLargestMonitorIsNotClamped)
        {
            const std::vector<LRect> monitors = { LRect(0, 0, 2560, 1440), LRect(2560, 0, 6400, 2160) };
            const LRect maximized(2552, -8, 6408, 2168); // the largest monitor with its invisible frame
            Assert::IsTrue(StageState::SaneRect(maximized, monitors).Same(maximized));
            const LRect borderless(2560, 0, 6400, 2160);
            Assert::IsTrue(StageState::SaneRect(borderless, monitors).Same(borderless));
        }

        TEST_METHOD (UnmaximizeUsesADeadlineAndStuckWindowsStaySkipped)
        {
            using StageState::Unmaximize;
            Assert::IsTrue(StageState::UnmaximizeDecision(true, false, 100, 3100) == Unmaximize::Done, L"it restored");
            Assert::IsTrue(StageState::UnmaximizeDecision(false, true, 100, 0) == Unmaximize::UserMaximized, L"the user's: left alone");
            Assert::IsTrue(StageState::UnmaximizeDecision(true, true, 100, 3100) == Unmaximize::Wait);
            Assert::IsTrue(StageState::UnmaximizeDecision(true, true, 3099, 3100) == Unmaximize::Wait, L"every Land before the deadline just waits, however many there are");
            Assert::IsTrue(StageState::UnmaximizeDecision(true, true, 3100, 3100) == Unmaximize::GiveUp);
            Assert::IsTrue(StageState::UnmaximizeDecision(true, true, 99999, 3100) == Unmaximize::GiveUp);

            Assert::IsTrue(StageState::StillStuck(true, true));
            Assert::IsFalse(StageState::StillStuck(true, false), L"restored by itself: eligible again");
            Assert::IsFalse(StageState::StillStuck(false, true));

            auto stuck = Plain();
            stuck.stuckMaximized = true;
            Assert::AreEqual(std::wstring(L"stuck maximized (it would not restore)"), std::wstring(StageAdopt::SkipReason(stuck)));
            Assert::IsFalse(StageAdopt::IsEligible(stuck));
        }

        TEST_METHOD (RehomeShiftsTheSavedPlaceAndSlidesItInside)
        {
            const LRect a(0, 0, 2560, 1440), b(2560, 0, 6400, 2160);
            // Same relative place on the other monitor
            Assert::IsTrue(StageState::ShiftIntoMonitor(LRect(100, 100, 900, 700), a, b).Same(LRect(2660, 100, 3460, 700)));
            // Back again
            Assert::IsTrue(StageState::ShiftIntoMonitor(LRect(2660, 100, 3460, 700), b, a).Same(LRect(100, 100, 900, 700)));
            // A window that reached the right edge of the larger monitor slides back inside the smaller one
            const auto slid = StageState::ShiftIntoMonitor(LRect(2760, 200, 5160, 1400), b, a);
            Assert::IsTrue(slid.Inside(a), L"inside the smaller monitor");
            Assert::AreEqual(2560, slid.R, L"slid left until it fits");
            Assert::AreEqual(2400, slid.W(), L"its size is not touched");
            // Hanging out at the top left stays within too
            Assert::IsTrue(StageState::ShiftIntoMonitor(LRect(2500, -8, 3300, 700), b, a).T >= 0);
        }

        TEST_METHOD (RehomeJoinPlaceAndDecision)
        {
            // Docked or not on the old stage (master with a stack cap of 4: 5 docked), then the layout of the new stage places it
            auto master = StageLayouts::MakeMaster();
            const std::vector<int> seven = { 1, 2, 3, 4, 5, 6, 7 };
            Assert::IsTrue(StageState::WasDocked(*master, seven, 1), L"master");
            Assert::IsTrue(StageState::WasDocked(*master, seven, 5));
            Assert::IsFalse(StageState::WasDocked(*master, seven, 6), L"hidden by the stack cap");
            Assert::IsFalse(StageState::WasDocked(*master, seven, 99), L"not there");
            auto pair = StageLayouts::MakePair();
            Assert::IsTrue(StageState::WasDocked(*pair, { 1, 2, 3, 4 }, 2));
            Assert::IsFalse(StageState::WasDocked(*pair, { 1, 2, 3, 4 }, 3), L"strip");

            std::vector<int> order = { 10, 11, 12 };
            master->Join(order, 20, true);
            Assert::AreEqual(20, order.front(), L"master layout: a docked window arriving takes master");
            order = { 10, 11, 12 };
            master->Join(order, 20, false);
            Assert::AreEqual(20, order[1], L"master layout: otherwise the front of the stack");
            order = { 10, 11, 12 };
            pair->Join(order, 20, true);
            Assert::AreEqual(20, order.front(), L"pair layout: docked takes the left slot");
            order = { 10, 11, 12 };
            pair->Join(order, 20, false);
            Assert::AreEqual(20, order[2], L"pair layout: not docked goes to the strip, behind the two docked");

            // A scan held back by grace or a drag is armed again, at the earliest of what is due
            Assert::AreEqual(7001ULL, StageState::RearmRehome(0, 7000));
            Assert::AreEqual(5000ULL, StageState::RearmRehome(5000, 7000), L"an earlier scan stays");
            Assert::AreEqual(7001ULL, StageState::RearmRehome(9000, 7000), L"a later one is brought forward");

            StageState::RehomeFacts f;
            f.usable = true;
            f.now = 5000;
            f.grace = 1000;
            f.destinationKnown = true;
            f.destinationHasRoom = true;
            using StageState::RehomeAction;
            Assert::IsTrue(StageState::DecideRehome(f) == RehomeAction::Move);

            auto g = f;
            g.sameStage = true;
            Assert::IsTrue(StageState::DecideRehome(g) == RehomeAction::None, L"still on its own monitor");
            g = f;
            g.mouseDown = true;
            Assert::IsTrue(StageState::DecideRehome(g) == RehomeAction::None, L"a drag is going on");
            g = f;
            g.grace = 6000;
            Assert::IsTrue(StageState::DecideRehome(g) == RehomeAction::None, L"our own move is still landing");
            g = f;
            g.usable = false;
            Assert::IsTrue(StageState::DecideRehome(g) == RehomeAction::None, L"minimized, hidden or hung");
            g = f;
            g.destinationKnown = false;
            Assert::IsTrue(StageState::DecideRehome(g) == RehomeAction::None, L"on no monitor with a stage");
            g = f;
            g.destinationHasRoom = false;
            Assert::IsTrue(StageState::DecideRehome(g) == RehomeAction::Unmanage, L"no room: left where the user put it");
        }

        TEST_METHOD (SizesAreScaledFromTheAppsDpiToThePhysicalMonitor)
        {
            Assert::AreEqual(300LL, StageDpi::ScaleBetween(200, 144, 96), L"an unaware app on a 150% monitor");
            Assert::AreEqual(200LL, StageDpi::ScaleBetween(200, 144, 144), L"per-monitor aware: already physical");
            Assert::AreEqual(200LL, StageDpi::ScaleBetween(200, 0, 96), L"unknown dpi: unchanged");
            Assert::AreEqual(200LL, StageDpi::ScaleBetween(200, 144, 0));
            Assert::AreEqual(0LL, StageDpi::ScaleBetween(0, 192, 96));
            Assert::AreEqual(1LL << 41, StageDpi::ScaleBetween(1LL << 40, 192, 96), L"no overflow in 64 bit");
        }

        TEST_METHOD (EverySkipHasAReadableReason)
        {
            Assert::IsNull(StageAdopt::SkipReason(Plain()));

            const auto reason = [](auto change) {
                auto i = Plain();
                change(i);
                const auto* text = StageAdopt::SkipReason(i);
                Assert::IsNotNull(text);
                return std::wstring(text);
            };
            Assert::AreEqual(std::wstring(L"minimized"), reason([](auto& i) { i.iconic = true; }));
            Assert::AreEqual(std::wstring(L"cloaked"), reason([](auto& i) { i.cloaked = true; }));
            Assert::AreEqual(std::wstring(L"owned window (a dialog)"), reason([](auto& i) { i.hasOwner = true; }));
            Assert::AreEqual(std::wstring(L"looks parked already (under 4 px on screen)"), reason([](auto& i) { i.onScreenPx = 0; }));
            Assert::AreEqual(std::wstring(L"held by the desktop scroll or StageProto"), reason([](auto& i) { i.heldElsewhere = true; }));
            Assert::AreEqual(std::wstring(L"not resizable (no thick frame)"), reason([](auto& i) { i.style = WS_POPUP; }));
            Assert::AreEqual(std::wstring(L"shell or system class"), reason([](auto& i) { i.cls = L"Progman"; }));
            Assert::AreEqual(std::wstring(L"topmost"), reason([](auto& i) { i.exStyle = WS_EX_TOPMOST; }));
            Assert::AreEqual(std::wstring(L"no title"), reason([](auto& i) { i.emptyTitle = true; }));
        }
    };
}
