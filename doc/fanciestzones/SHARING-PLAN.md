# Making FanciestZones shareable

This is the plan for presenting FanciestZones on GitHub for free, so that other people can install it, trust it and report problems. The first part lists what is done. The rest lists what is still needed, most important first.

## Done

| Area | Change |
|---|---|
| Separation from PowerToys | Settings, layouts and logs live in `%LOCALAPPDATA%\FanciestZones`. The first run copies settings and layouts from the PowerToys folder. |
| Separation from PowerToys | Own instance lock and window class. FanciestZones refuses to start while FancyZones of PowerToys runs, and no longer quits when FancyZones is turned off in PowerToys. |
| Separation from PowerToys | No PowerToys group policy check, no PowerToys toast notifications, no zone editor hotkey (it took Win+Shift+\` and started an editor that isn't shipped) |
| Settings UI | Tray icon with a menu: the main switches, gap, desktop pages per monitor, named layouts, start at sign-in, settings folder, help, quit |
| New setting | `fancyzones_desktopScrollPages` limits how many pages each monitor scrolls through |
| Privacy | PowerToys telemetry left out: no ETW trace sessions, no events under the Microsoft provider |
| Legal | The exe says FanciestZones, not Microsoft and PowerToys, in its version information. `LICENSE` keeps the Microsoft notice and adds one for the FanciestZones changes. README states the MIT license, the third-party code, the trademark notice, and that Microsoft doesn't endorse it. |
| Repository files | New `README.md` with the promo GIF; `SECURITY.md`, `SUPPORT.md`, `DATA_AND_PRIVACY.md`, `CONTRIBUTING.md` and `CODE_OF_CONDUCT.md` rewritten for FanciestZones, so that nobody reports FanciestZones problems to Microsoft |
| Icon | New FanciestZones icon in the exe and the tray |

## Before you make it public

1. **Commit and push.** Nothing above is committed.
2. **Release builds.** Add a GitHub Actions workflow that builds `FancyZones.vcxproj` in Release on a `windows-2022` runner and attaches `FanciestZones.exe`, `LICENSE` and `NOTICE.md` to a GitHub Release. The MIT license requires the copyright notices to travel with the binary, so always ship `LICENSE` and `NOTICE.md` beside the exe (a zip is simplest). GitHub Actions and Releases are free for public repositories.
3. **Remove the PowerToys automation.** `.github/workflows` holds 10 PowerToys workflows (issue triage, Store submissions, spelling) and `.github/ISSUE_TEMPLATE` holds PowerToys issue forms. Forks have Actions turned off until you enable them, so they do no harm yet, but they will fail once Actions is on, and the issue forms ask about PowerToys. Replace them with one bug form and one idea form.
4. **A version of its own.** The exe reports the PowerToys version it was forked from (`src/common/version/version.h`). Give FanciestZones its own number, for example 1.0.0, in the FanciestZones resource file, and tag releases with it.
5. **Repository page.** Set the description, the topics (`windows`, `window-manager`, `tiling-window-manager`, `fancyzones`), the website link, and a social preview image (a still from the promo).
6. **Keep personal tooling out.** `tools/promo/capture.ps1` refers to a private tool and a path on this computer. Leave `tools/promo` out of the repository, or remove those lines before committing it.

## Soon after

1. **Installer.** `installer\FanciestZones.iss` (Inno Setup, free) installs per user to `%LOCALAPPDATA%\FanciestZones` without administrator rights: the exe, `LICENSE` and `NOTICE.md`, a Start menu entry and an optional sign-in shortcut. It asks a running copy to quit on upgrade and uninstall, and keeps settings and layouts. Build it with `ISCC.exe installer\FanciestZones.iss` after the Release exe; it writes `x64\Release\installer\FanciestZones-<version>-setup.exe`. Not yet run end to end. Build it in the release workflow, and sign both the exe and the setup.
2. **Code signing.** Unsigned downloads get a SmartScreen warning. [SignPath Foundation](https://signpath.org) signs open-source projects for free; Azure Trusted Signing costs about 10 USD a month.
3. **winget.** After a signed release, add a manifest to `microsoft/winget-pkgs` so that `winget install FanciestZones` works.
4. **A smaller repository.** A GitHub fork stays tied to PowerToys: issues are off by default, code search skips it, and a clone downloads all of PowerToys. Either ask GitHub Support to detach the fork, or move to a new repository that holds only `src/modules/fancyzones`, the parts of `src/common` it links, and the build files. The second also makes CI faster.
5. **Name check.** FancyZones is a Microsoft product name. FanciestZones plays on it and says clearly that it isn't Microsoft's, which is common for forks, but if Microsoft ever asks, rename it.

## Quality fixes

### Known bugs

These came out of earlier reviews and are still open:

- A maximized window that arrives on a page during a scroll is activated, and can change the z-order.
- Focus changes during a slide are dropped.
- A change of the monitor setup brings every window back to the first page and piles the pages up there.
- Recalling a saved layout on a scrolled page can put a window in two layouts.
- When Windows renumbers the displays (DISPLAY2 becomes DISPLAY7 after a restart or driver reset), each monitor starts with an empty layout, and the old one stays in `dock-layouts.json` under the old key. Named layouts already ignore the number. Fix it by re-keying the trees, the pages and the page trees together once the work areas are rebuilt, not on lookup.

### Behavior

- **Win+arrow keys are off by default.** Most of the keyboard features need them. Ask on the first run, or turn them on by default.
- **Stock zone code still runs.** FanciestZones still loads zone layouts, applied layouts and app zone history, writes their files, and registers Win+PgUp and Win+PgDn to cycle windows in a zone, which does nothing useful without zones and takes the keys from other apps. Removing the zone engine would shrink the program and remove these conflicts.
- **Leftovers of the zone editor.** `ToggleEditor`, the editor process handling, the editor hotkey setting and the Trace calls are dead code now. The zone files (`applied-layouts.json`, `custom-layouts.json` and others) weren't copied from the PowerToys folder, because FanciestZones doesn't use zones.
- **Tray icon size.** It is chosen for the system DPI at start and isn't reloaded when the scaling changes.
- **Fixed layout hotkeys.** The digit hotkeys for saved layouts are fixed in code.
- **Apps that run as administrator** can't be docked unless FanciestZones runs as administrator too. Offer a "run as administrator at sign-in" choice (a scheduled task), as PowerToys does.
- **Numeric settings** other than the gap and the page count (edge band, split zone, edge snap band) are only in `settings.json`.

### Performance

Measured on this computer with the screen locked: 0.36% of one core while idle, 66 MB working set, 137 MB private memory, 49 threads.

- The < > caption buttons are checked every 100 ms while the setting is on, even when nothing is docked. Follow window moves with `EVENT_OBJECT_LOCATIONCHANGE` instead, or stop the timer while nothing is docked.
- A settings change is broadcast to every top-level window (`HWND_BROADCAST`). Post it to FanciestZones' own window.
- 49 threads and 137 MB are a lot for a tray tool. Most come from the PowerToys plumbing (a file watcher per data file, the zone engine). Removing the zone engine would cut them.

### Tests and checks

- The full unit-test suite crashes with an access violation, on unmodified PowerToys too. Only the `DockTree` tests run (64, all passing). Fix or prune the zone tests, then run them in CI.
- The live test (`tools/dock-live-test`) drives the real mouse and keyboard, so it can't run in CI. Keep it as a manual check before each release.
- The tray menu was built tonight and seen to register its icon, but nobody has clicked through it yet.
