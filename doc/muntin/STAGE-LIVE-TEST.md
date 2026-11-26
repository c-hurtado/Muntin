# Stage mode: manual live-test checklist

Nothing of Stage mode has been run on real windows yet (see "Stage mode status and known limitations" in `Muntin.md`). Do this on a spare desktop, with a few harmless windows (Notepad, Explorer, a browser), and keep a PowerShell window open outside Stage's reach (run it elevated: Stage skips windows of apps that run as administrator). Release build, installed with the Muntin deploy steps. Keep the Muntin log open: lines that start with `Stage:` say what was taken or skipped and why.

## 1. Enter and leave restore

1. Open 4 or 5 windows, in different sizes and places. Maximize one, minimize one.
2. Tray, **Arrange: Dock / Stage**. After about a second the windows should line up in the Master layout (main window left, stack right). Check the log for one line per window.
3. Switch back. Every window must return to its size and place, the maximized one maximized, the minimized one minimized. `stage-state.txt` is gone from `%LOCALAPPDATA%\Muntin`.
4. Repeat with `fancyzones_arrangeMode` edited in `settings.json` instead of the tray (the tray toggle is the path an early report doubted).

## 2. Crash recovery

1. Enter Stage mode with 4 windows. Confirm `stage-state.txt` exists.
2. End only the main process: `taskkill /PID <id of the Muntin.exe without --watchdog> /F` (no `/T`). Find it with `Get-CimInstance Win32_Process -Filter "Name='Muntin.exe'"`.
3. Within a few seconds the helper (`--watchdog`) must put the windows back and the state file must go. If not, read `watchdog.log`, then start Muntin: it must restore at start.
4. Repeat once with `taskkill /T` on the main process: the helper must survive it (shell parent).

## 3. Keys

Win+Left and Win+Right rotate (hold to repeat); Win+Up promotes the focused window; Win+Down demotes it; Win+Shift+L goes Master, Stage, Pair (check the layout name saved in `settings.json`). The Start menu must not open at any of these. Minimize the main window, then Win+Left: focus must go to a visible window. Focus the taskbar and press Win+Up: it acts on the last managed window.

## 4. Mouse

- Drag the shared edge: neighbors follow live; on release the cut is saved (under `fancyzones_stageMonitors` for that monitor). Esc during the drag puts it back.
- Drag a title bar onto another window's place: they swap. Drop on empty desktop: snaps back.
- Snap-to-edge must not change the cut: drag a window to a screen edge so Windows snaps it (Aero Snap, Snap Layouts): it snaps back to its slot and `settings.json` shows no new cut.
- Strip width (Stage and Pair layouts, 3 or more windows in Pair, 2 or more in Stage): drag the right edge of the rightmost docked window (the one next to the strip). The docked windows and the strip tiles follow live, between 10% and 40% of the usable width; Esc puts it back; on release `settings.json` has `stripCut` under that monitor only. The same drag of the right edge in Master, of the right edge with no strip showing (Pair with two windows), or of any other outer edge snaps back.
- In Stage or Pair, click a strip tile: that window becomes the main one.

## 5. Cross monitor

Drag a window to the other monitor: it joins that stage and the old stage closes up. Win+Shift+Left and Win+Shift+Right do the same with keys. Resize an edge on monitor 2 only: monitor 1 keeps its cut. Check the monitors have different DPI scaling and that nothing lands off by the scale ratio.

## 6. Many windows and the strip

Open 12 or more windows. In Stage, the strip must show what fits, and the wheel over it scrolls the tiles (with "Scroll inactive windows when hovering" on). In Master, windows past 9 are not taken (log reason) and come in when room appears.

## 7. Fullscreen and maximize

Put a video or game in fullscreen: the strip must hide, and show again when it leaves. Maximize a managed window: the strip hides while it is maximized.

## 8. Unplug and replug a monitor

With windows on both monitors, turn off or unplug monitor 2. Its windows must move to monitor 1 without being lost. Plug it in: they must return to monitor 2. Check the per-monitor cut still applies, and `settings.json` has no stray `@x,y` entry afterwards.

## 9. Other checks

- A dialog of a window waiting in the strip brings the window forward.
- Quit from the tray in Stage mode: all windows go back and the state file is deleted.
- With StageProto running (it must set `Muntin.HandsOff` first), its windows are not touched by Muntin.
