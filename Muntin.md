# Muntin reference

Muntin is a fork of PowerToys FancyZones. It docks windows by drag instead of snapping them to zones drawn in advance, in the style of TrellisWM on macOS. It runs on its own, without PowerToys. This page covers how to run it, use it, configure it and build it.

## Run it

| What | Where |
|---|---|
| Executable | `Muntin.exe`, wherever you put it, for example `%LOCALAPPDATA%\Muntin` |
| Settings, layouts and logs | `%LOCALAPPDATA%\Muntin` |
| Starts at sign-in | When **Start at sign-in** is checked in the tray menu (a shortcut in the Startup folder) |
| Stop it | **Quit** in the tray menu |
| Start it again | Run the executable, or `Muntin restart` |

Keep **FancyZones turned off in PowerToys**. While FancyZones of PowerToys runs, Muntin refuses to start, because both would move the same windows. PowerToys can still start its FancyZones after Muntin is running, so turn it off there.

**Emergency exit:** Ctrl+Alt+Shift+F12 ends Muntin even when it is hung. Muntin starts a small helper of its own (a second `Muntin.exe --watchdog` process, no window, no tray icon) that listens for the keys. It asks Muntin to quit, ends it after 3 seconds if it is still there, and puts back every window that the desktop scroll was holding. It ends only the process it was started for (it checks the start time, so a process that got the same id later is safe). When it can't do its job (another program has the hotkey, or another watchdog still runs) it writes one line to `watchdog.log` in the data folder. The helper does the same when Muntin ends in any other way (a crash, or being stopped) and leaves windows parked, and it exits once Muntin has ended cleanly. **When you replace `Muntin.exe`, wait for every Muntin process to end first, the helper included** (`Get-Process Muntin`): it runs from the same file and locks it for a moment after Muntin quits.

The Muntin icon sits in the notification area. Windows may put it in the overflow menu at first; to keep it visible, drag it onto the taskbar, or turn it on under **Settings > Personalization > Taskbar > Other system tray icons**.

## Use it

### Dock with the mouse

Hold **Shift** while you drag a window (see `fancyzones_shiftDrag` to change that). A preview shows where the window lands. The preview is a rounded zone with a gradient fill (the accent color shading to a neighboring hue) that breathes slightly. As the cursor nears a screen edge, a glow along that edge strengthens until it is inside the edge band, where the window takes half of the screen. The animation holds still when Windows animation effects are off.

| Drop the window | Result |
|---|---|
| Near an edge of the screen | It takes that half of the screen, and the docked windows share the other half. An empty half closes up, so no hole is left next to the windows. |
| On the outer part of a docked window | It splits that window on that side. Splits nest. |
| On an empty half | It fills it |
| In the middle of a docked window | Nothing: the window floats |

- **Undock:** drag the window without Shift. Its neighbors take the space. Holding the title bar without moving the window leaves it docked.
- **Smallest place:** a window isn't docked where it would get less than 100 pixels in width or height.
- **Resize:** drag a border of a docked window. The neighbors follow when you release the mouse.
- **Minimize, restore, close:** the neighbors take the space. A restored window returns to its place.
- **Only minimized windows docked on a monitor:** docking a new window there works as on an empty monitor (it takes half), and the minimized windows leave the layout. Restored, they float.

### Move with the keyboard

Both shortcuts below work when `fancyzones_overrideSnapHotkeys` is on.

Win+arrows move the focus: from a docked window to the docked window on that side, on the next monitor too. Nothing moves, and nothing happens when there is no docked window on that side. On a window that isn't docked the keys keep their Windows meaning (snap, maximize, minimize); on a docked window they lose it.

Shift+Win+arrows move the window:

| Window | Shift+Win+arrow does |
|---|---|
| Floating, Left or Right | Docks it on that side of its monitor |
| Floating, Up or Down | Carries it to the page above or below, where it stays floating. On the first page Up docks it at the top. |
| Docked, with a neighbor on that side | Swaps the two. Also moves it into an empty half. |
| Docked, with nothing on that side | Moves it to the monitor on that side, docked on the edge it came in from. An empty space along that edge is filled and the windows there keep their size; a space kept for a closed app is taken only when there is no other. |
| Docked, Up or Down, with nothing on that side and no monitor there | Scrolls to the page above or below and takes the window along, docked on the same side, or in the empty half if the page has one. It stops at the last page (see `fancyzones_desktopScrollPages`). |

They replace the Windows meaning of Shift+Win+arrows (move to the other monitor, stretch vertically) on ordinary app windows. On a window that can't be docked (one that runs as administrator, or any window while "span zones across monitors" is on), the keys then do nothing.

A middle click on a title bar expands the window into the open space instead of maximizing it. A docked window takes the empty spaces beside it, including ones kept for a closed app, and the other windows keep their size. A floating window docks into the largest empty space on its monitor (an open one before one kept for an app). When nothing is docked there, it fills the work area (not maximized). But when no docked window shows on the monitor and another floating window is on it, the clicked window docks on the half it sits on and the other one takes the other half, and the clicked window comes to the front. Minimized docked windows and slots kept for apps don't count, and they leave the layout: a minimized one floats when it comes back, and a closed app no longer has a slot kept. With several floating windows, the one nearest the top of the z-order goes; always-on-top windows and the clicked window's own dialogs don't count. When there is no space to expand into, or the window can't be docked, nothing happens. Middle clicks on tabs still reach the app and don't expand. The click is not taken from the app, so an app that acts on a middle click in its empty tab strip does both (Firefox opens a new tab). It works whatever `fancyzones_overrideSnapHotkeys` is. Turn it off with `fancyzones_dockMiddleClickExpand` or the tray menu.

Three more shortcuts act on a docked window, and on any other window the keys keep their Windows meaning:

| Keys | Action |
|---|---|
| Win+J | Turns the split around the window: side by side becomes one above the other, and back |
| Win+plus, Win+minus | Make the window wider or narrower by 100 pixels. The neighbor follows. They repeat while held. |
| Add Shift | Taller or shorter instead |
| Add Alt | 25 pixels |
| Add Ctrl | 300 pixels |

Nothing happens when no split runs in that direction next to the window. Win+plus and Win+minus are the Windows Magnifier keys: on a docked window Muntin takes them.

### Dock new windows by themselves

With `fancyzones_dockAutoTile` on, a new window docks on the monitor it opens on, as in Hyprland's dwindle layout:

1. If the layout has an empty place, the window takes it. A place kept for an app waits for that app.
2. Otherwise it splits the docked window last in use on that monitor, across the longer side of that window, and takes the right or bottom half.
3. With nothing docked it takes the left half of its monitor.

A window stays floating when it opens maximized, can't be resized, belongs to another window (most dialogs), is in `fancyzones_excluded_apps`, or takes more than 5 seconds to appear. Only a newly created window docks: one that comes back from the tray or from another virtual desktop doesn't, and a window you undock stays undocked.

### Scroll the desktop

Hold **Shift+Win** and roll the mouse wheel to scroll the windows of the monitor under the cursor up and down a tall canvas. Six notches scroll one screen. The windows glide for a moment after the last notch, and a click stops them. They stay wherever the scroll stops, cut off by the edge of the monitor if need be, and they work normally there. You can scroll down to the first full page past the lowest window, which is an empty screen, and a new window opens where you are.

- **Overview:** with `fancyzones_desktopScrollOverview` on, the scroll zooms out to half size, so you see the pages stacked with a gap between them. A thin rounded border in the Windows accent color fades in as it zooms out and marks the whole canvas, from the first page to the last. Keep Shift+Win down and roll the wheel to move through them. Let go and it zooms back in on the nearest page. A window can't be picked or dragged in the overview, and the pages are not drawn with a wallpaper of their own.
- **Which windows scroll:** the ordinary windows on that monitor. Always-on-top, minimized and administrator windows stay where they are.
- **A window out of sight:** Alt+Tab or a taskbar click scrolls to the page that holds it.
- **Pages and docking:** every stretch of the canvas as tall as the monitor is a page with its own dock layout. A scroll that stops very close to a page rests exactly on it. The layout is in force while the monitor shows exactly that page. Docking a window or using a dock key between two pages scrolls to the nearest page first. Otherwise docked windows just scroll between pages, and resizing one doesn't move its neighbors until the monitor is back on that page. Only the layout of the first page is saved. The layouts of other pages last until Muntin closes or every window comes back to the first page.
- **Maximized windows:** cut off by the scroll, a window isn't maximized anymore but keeps its size. It is maximized again when it is back in its place.
- **Back to the top:** a change of virtual desktop or of the monitor setup brings every window back to its place on the first page, and each page's windows pile up there. Closing Muntin does too, and after a crash the next start does.

A window fully out of sight waits as a one pixel strip at the bottom of the monitor, under the taskbar. On a monitor without a taskbar that strip is visible. There is no scrolling on a monitor with another monitor right below it.

### Save and recall layouts

| Keys | Action |
|---|---|
| Win+Ctrl+Alt+Shift+digit | Save the layout of the monitor under the cursor to that digit |
| Win+Ctrl+Alt+digit | Recall it on the monitor under the cursor |

Saving a monitor with nothing docked changes nothing: the layout already saved under that digit is kept. The keys are ignored while you drag a window.

### Named layouts that start your apps

A named layout covers every monitor, and loading it starts the apps that aren't open.

1. Dock the windows the way you want them, on any monitors.
2. Right-click the tray icon, choose **Save layout...** and type a name, such as `DevLayout`. Or, in a terminal or the Run box (Win+R): `Muntin save DevLayout` (with the folder of `Muntin.exe` on your PATH; otherwise give the whole path of the exe)
3. Load it from Command Palette or Windows search: type `DevLayout` and run it. The same works with `Muntin.exe load DevLayout`.

| Command | What it does |
|---|---|
| `list` | Prints the names of the saved layouts, one per line |
| `save <name>` | Saves the docked windows of every monitor under that name, and adds a Start menu entry with that name (in a `Muntin` folder) |
| `load <name>` | Puts the layout back on the monitors it was saved on. Open windows of its apps take their places, the other apps are started and dock as their windows appear. Starts Muntin first if it isn't running. |
| `delete <name>` | Removes the layout and its Start menu entry |
| `restart` | Ends the running Muntin and starts it again (or just starts it). Layouts are kept. |

- Saving again under the same name replaces the layout. Names ignore case.
- The exit code is 0 on success and 1 on failure, with the reason printed. Muntin is a window app, so PowerShell doesn't wait for it: pipe it to see the output in order, e.g. `Muntin.exe list | Write-Output`.
- If Muntin runs as administrator, loading doesn't start apps.
- Store apps (Terminal, Claude, Notepad) are started by their app id, other apps by their exe. Arguments aren't kept: VS Code reopens its last folder by itself.
- Monitors that aren't connected are skipped.
- A name can't contain `\ / : * ? " < > |`.

### What it remembers

Layouts are kept per monitor and per virtual desktop in `dock-layouts.json`, and they survive a restart. When a docked window is gone at startup, its place is kept for that app, and the next window of the app takes it. After a reboot, apps return to their places as you open them. A kept place can also be filled by hand: Shift-drag any window onto it. Layouts of virtual desktops that were deleted are dropped.

To forget all layouts and saved digits, quit Muntin, delete `%LOCALAPPDATA%\Muntin\dock-layouts.json`, and start it again.

### Stage mode

Stage mode is a second way to arrange windows, ported from StageProto: Muntin takes the ordinary windows of each monitor and keeps them in a list, one monitor at a time. The first window is the main one, and the others sit beside it. Dock-by-drag stays the default. **Stage mode has not been tried live yet: read "Stage mode status and known limitations" before you turn it on.** The checklist for the first live run is in [doc/muntin/STAGE-LIVE-TEST.md](doc/muntin/STAGE-LIVE-TEST.md).

**Switch:** right-click the tray icon and choose **Arrange: Dock / Stage** (checked means Stage), or set `fancyzones_arrangeMode` in `settings.json` to `1` (Stage) or `0` (Dock). The change applies at once: switching to Stage starts after about 0.7 seconds, and switching back puts every window back where it was before Stage took it (size, position, minimized or maximized).

**Layouts:** three layouts, the same on every monitor. Set one with `fancyzones_stageLayout`, or press **Win+Shift+L** to go to the next one (Stage, then Pair, then Master, then Stage again). The choice is saved.

| Layout | What you get |
|---|---|
| Master (the default) | The main window on the left, the other windows stacked on its right with equal heights. Up to 9 windows per monitor (the main one and 8 in the stack). Windows beyond that are not taken by Stage and stay where they are, until room appears. |
| Stage | The main window large, a strip of live thumbnails of the other windows on the right |
| Pair | Two windows side by side, and a strip of thumbnails on the right from the third window on |

In Stage and Pair the strip shows as many tiles as fit; the rest are reached with the wheel or the keys. The strip is an overlay of its own. It hides while a window is maximized or a window of another app covers the monitor, such as a fullscreen game or video.

**Keys** (they work on the window Stage manages that has the focus; while the focus is on the taskbar, Start or an overlay they act on the last managed window; on any other window they keep their Windows meaning). They don't need `fancyzones_overrideSnapHotkeys`. Muntin also keeps the Start menu from opening when you press Win for these.

| Keys | Action |
|---|---|
| Win+Left | Rotate: the main window goes to the end of the list, the next one becomes main. Repeats while held. |
| Win+Right | Rotate the other way: the last window becomes main. Repeats while held. |
| Win+Up | Promote: the focused window becomes the main one |
| Win+Down | Demote: the focused window goes to the end of the list |
| Win+Shift+Left, Win+Shift+Right | Move the focused window to the monitor on that side (the monitors are ordered by screen position, and the last wraps to the first) |
| Win+Shift+L | Next layout (Win+L locks Windows, so it can't be used) |

A minimized window keeps its place in the list but takes no part in the rotation and doesn't get the focus.

**Mouse:**

- Drag a border that two windows share to move it. The neighbors follow while you drag, and the new cut is saved when you let go. In Master it moves the edge between the main window and the stack, and the heights in the stack. In Pair it moves the edge between the two. Stage has no movable edge.
- Drag a window by its title bar and drop it on the place of another window: the two swap. A drop anywhere else, or Esc, snaps the window back. A drop that Windows reads as snap (Aero Snap, Snap Layouts) also snaps back and changes no cut.
- Drag a window to another monitor: it joins the stage of that monitor.
- Click a tile of the strip: that window becomes the main one.
- Roll the wheel over the strip: the tiles scroll. This needs the Windows setting "Scroll inactive windows when hovering" to be on (it is by default).

**Per monitor:** the cut and the heights of the stack are kept per monitor, so a drag on one screen leaves the others alone. See `fancyzones_stageMonitors` below. The layout name is the same everywhere.

**Which windows Stage takes:** ordinary top-level windows that are visible, not minimized, have a title and a resizable frame, and are at least 100 by 60 pixels. It leaves alone dialogs (owned windows), tool windows, topmost windows, shell and system windows, Muntin's own windows, windows of apps that run as administrator, windows that don't answer, and windows another program has marked (see below). A window that can't get small enough for Master's slots (its minimum size) is released and floats until there is room. A dialog of a window that is waiting in the strip or parked brings that window forward. New windows are taken as they appear, once they have existed for about 0.4 seconds (a short-lived overlay is never tiled). Stage also leaves alone overlays and fullscreen-like windows: click-through windows, layered windows without a title bar, and windows without a title bar that cover their whole monitor (borderless fullscreen apps, the Snipping Tool's capture overlay). A held window that turns into one of these goes back where it was. Apps in `fancyzones_excluded_apps` are never taken by Stage either, the same list the dock mode uses: it is the way out for an app that does not behave. The log line of every decision shows the window's style, extended style and rectangle.

**Files and recovery:** before Stage moves a window, it writes the place the window had to `stage-state.txt` in `%LOCALAPPDATA%\Muntin`. The file is deleted when Stage mode ends cleanly (switching back to Dock, or **Quit**). If Muntin dies while in Stage mode, the helper process (`Muntin.exe --watchdog`, see "Emergency exit") puts every window back from that file, and so does the next start of Muntin. If something still looks wrong after a crash:

1. Check `Get-Process Muntin`. If a Muntin runs, switch to Dock from the tray and back if needed. If none runs, start `Muntin.exe`: it restores from `stage-state.txt` when it starts.
2. Look at `watchdog.log` in the data folder for a line saying the helper could not do its job, and at the Muntin log for lines that start with `Stage:` (each window taken or skipped is logged with its reason).
3. Windows parked as a one pixel strip at the bottom of a monitor are the usual leftovers: they come back with step 1.

Don't delete `stage-state.txt` by hand while windows are still arranged, because it is the only record of where they were.

**Another program that arranges windows (StageProto):** a window that carries the window property `Muntin.HandsOff`, whose value is the process id of a running `pwsh.exe` or `powershell.exe`, is left alone by Muntin in both modes. A stale id (that process is gone, or the id now belongs to another program) does not count. The StageProto side of this (setting the property) is not part of this repository; whether StageProto sets it yet is unverified.

## Configure it

The tray menu (right-click the Muntin icon) turns the most used settings on and off. All settings are in one file:

`%LOCALAPPDATA%\Muntin\settings.json`

**Open settings folder** in the tray menu opens its folder. Edit the file in a text editor and save it. Muntin reloads it right away, with no restart. Each setting is an entry under `properties` with a `value`:

```json
{
  "properties": {
    "fancyzones_dockGap": { "value": 8 },
    "fancyzones_shiftDrag": { "value": true }
  },
  "name": "FancyZones",
  "version": "1.0"
}
```

### Docking

These settings exist only in the fork.

| Setting | Meaning | Default | Allowed |
|---|---|---|---|
| `fancyzones_dockGap` | Pixels left between docked windows. A change is applied to the docked windows at once. | 8 | 0 to 100 |
| `fancyzones_dockMarginLeft`, `fancyzones_dockMarginTop`, `fancyzones_dockMarginRight`, `fancyzones_dockMarginBottom` | Pixels kept free between docked windows and that edge of the screen's work area. The tray menu's "Gap and margins..." has sliders (margins 0 to 15, gap 0 to 30) that apply as you drag; the file allows more. A change is applied at once. Margins that leave no room are ignored. | 0 | 0 to 500 |
| `fancyzones_dockEdgeBand` | How close to a screen edge, in pixels, the cursor must be for the window to take half of the screen | 48 | 8 to 500 |
| `fancyzones_dockWindowEdgePercent` | How far into a docked window, in percent from each of its edges, the cursor splits that window. The rest of the window is the middle, where nothing happens. | 30 | 5 to 50 |

A value outside the allowed range is brought back to the nearest allowed value.

### Behavior

| Setting | Meaning | Default |
|---|---|---|
| `fancyzones_shiftDrag` | `true`: dock only while Shift is held. `false`: dock on every drag, and Shift turns docking off. | `true` |
| `fancyzones_mouseSwitch` | A right click during a drag toggles docking | `false` |
| `fancyzones_overrideSnapHotkeys` | Win+arrows move the focus between docked windows, Shift+Win+arrows move windows in the layout | `false` |
| `fancyzones_excluded_apps` | Apps that are never docked: one name or path fragment per line (`\r` or `\n` between entries in the JSON string) | empty |
| `fancyzones_allowChildWindowSnap` | Also dock windows that have an owner window, such as dialogs | `false` |
| `fancyzones_dockAutoTile` | New windows dock by themselves (see "Dock new windows by themselves") | `false` |
| `fancyzones_dockAnimation` | Windows moved with Shift+Win+arrows or the < > buttons spring to their new places | `true` |
| `fancyzones_dockMotionCurve` | How moved windows travel (Shift+Win+arrows, the < > buttons, and going to a desktop page): `0` spring, which overshoots slightly and keeps the speed of a move in flight, or `1` ease, one 0.3 second curve without overshoot, which a new target restarts from where the window is. Also in the tray menu, under "Motion curve". | 0 |
| `fancyzones_dockAnticipation` | Windows pull back slightly before they move. Moves to a desktop page pull back only with the ease curve. Nothing moves without `fancyzones_dockAnimation`, so the tray item is grayed out then. | `false` |
| `fancyzones_dockMiddleClickExpand` | A middle click on a title bar expands its window into the open space | `true` |
| `fancyzones_desktopScrollOverview` | Scrolling the desktop zooms out to the pages (see "Scroll the desktop") | `false` |
| `fancyzones_desktopScrollPages` | How many pages each monitor scrolls through, the first included, empty pages too: 1 to 4. 1 turns scrolling off, and Shift+Win+Down doesn't take a window past the last page. | 2 |
| `fancyzones_dockEdgeSnap` | A window dragged to the left or right edge of a screen docks there without Shift, like Windows snap. The top edge is left to Windows (it maximizes), and the bottom edge is the taskbar. Only with `fancyzones_shiftDrag` on. | `true` |
| `fancyzones_dockEdgeSnapBand` | How close to that edge, in pixels, the cursor must be (1 to 100) | 12 |
| `fancyzones_restoreSize` | A window that is undocked gets back the size it had before | `false` |
| `fancyzones_dockCaptionButtons` | Show < > buttons left of the minimize button of every docked window. They move the window to the neighbor on that side, like Shift+Win+Left and Shift+Win+Right. | `true` |

### Look

| Setting | Meaning | Default |
|---|---|---|
| `fancyzones_systemTheme` | `true`: the drop preview takes the Windows accent color, and the two colors below are ignored | `true` |
| `fancyzones_zoneHighlightColor` | Color of the drop preview, as `#RRGGBB` | `#008CFF` |
| `fancyzones_highlight_opacity` | Opacity of the drop preview, 0 to 100 | 50 |
| `fancyzones_zoneBorderColor` | Border color of the drop preview | `#FFFFFF` |
| `fancyzones_makeDraggedWindowTransparent` | The dragged window is see-through while docking is active | `true` |
| `fancyzones_layoutNameLabelEnabled` | Show a label when a layout is saved or recalled | `true` |

### Stage mode

These settings exist only in the fork. See "Stage mode" above.

| Setting | Meaning | Default | Allowed |
|---|---|---|---|
| `fancyzones_arrangeMode` | `0`: Dock (dock-by-drag), `1`: Stage. Also the tray item "Arrange: Dock / Stage". | 0 | 0 or 1 |
| `fancyzones_stageLayout` | The layout of every monitor. Win+Shift+L changes it. | `"master"` | `"stage"`, `"pair"`, `"master"` (anything else is ignored) |
| `fancyzones_stagePairCut` | Where the vertical edge of the Pair layout sits, in thousandths of the usable width | 500 | 150 to 850 |
| `fancyzones_stageMasterCut` | The same for the Master layout (the main window's share) | 600 | 150 to 850 |
| `fancyzones_stageRows` | Master layout: the heights of the stack windows as shares in thousandths, comma separated (for example `"500,300,200"`). Empty means equal heights. | `""` | |
| `fancyzones_stageMonitors` | What single monitors override: an object from monitor key to `pairCut`, `masterCut` and `rows` (each optional, a missing one follows the global setting above) | none | cuts 150 to 850 |

The three global values are the defaults. Dragging a shared edge saves into `fancyzones_stageMonitors` for that monitor only. The key of a monitor is its display device id and instance id joined with an underscore (`<device id>_<instance id>`, from the display lookup, no serial number). When the lookup can't identify a monitor, the key is `@left,top` of its screen position, for example `@0,0`. The easiest way to get a real key is to drag an edge on that monitor once and read `settings.json`. Example:

```json
"fancyzones_stageMonitors": {
  "value": {
    "<device id>_<instance id>": { "masterCut": 700, "rows": "600,400" },
    "@2560,0": { "pairCut": 450 }
  }
}
```

A monitor that is plugged in again gets its entry back through its key. Removing the entry (or the whole setting) makes the monitor follow the global values.

### Settings that no longer do anything

The fork ignores the zone settings: the zone editor hotkey, zone numbers, "show zones on all monitors", "move newly created windows to their last known zone", "move windows based on their position", and the overlapping zones choice.

### The first run

Early builds kept their files in the folder of PowerToys FancyZones, `%LOCALAPPDATA%\Microsoft\PowerToys\FancyZones`. When its own folder has neither `settings.json` nor `dock-layouts.json`, it copies those two files from there, so settings and layouts carry over. The originals stay where they are. From then on PowerToys Settings no longer changes the settings of Muntin.

## Known limits

- Windows that are not aware of per-monitor scaling sit a few pixels inside the outer screen edges when the monitors use different scaling. Stock FancyZones does the same.
- A window that crosses between monitors of different scaling can land about 4 pixels off until the layout changes again.
- Layouts are stored per monitor. When a monitor is turned off or its identity changes, the windows docked on it stay where they are but are no longer in a layout. Dock them again.
- The hotkeys for saved layouts are fixed in code.
- Windows of apps that run as administrator can't be docked unless the fork runs as administrator too.
- Docking is off when "span zones across monitors" is on.

## Stage mode status and known limitations

Stage mode is code-complete and unit tested, but **nothing that touches real windows has been verified live**. What the tests cover is the pure logic: the layouts, the state file format, the key table, the mouse decisions, the strip plan, per-monitor settings. What was never run on a real desktop:

- Entering and leaving Stage mode on real windows, the restore of size, position, maximized and minimized state, and DPI behavior between monitors of different scaling (a bug of this kind was found and fixed by reading, not by a live test).
- Recovery by the helper process after a crash, including the change that starts it as a child of the shell so that `taskkill /T` on Muntin doesn't take it along. The helper is not started that way when Muntin runs as administrator.
- The overlay of the strip: thumbnails, rounded tiles, hover, wheel scrolling, the animation, and hiding over fullscreen apps.
- Tray switching on a real run: an early report said the tray toggle did not seem to enter Stage mode, and no cause was found by reading. Logging was added; read the log after a toggle.
- Keys: the Win mask that keeps Start from opening, and the swallowing of the chord's key-up.
- Mouse: title bar swap, shared edge resize, drop on another monitor, and moves made by other means (Win+Shift+Arrow of Windows, app moves), which are noticed by a one second poll.
- Several monitors: unplug and replug (windows move to the remaining monitor and return), a new monitor taking the key of a gone one, and the per-monitor cut on a real second display.
- The `Muntin.HandsOff` handshake with a running StageProto: only the Muntin side exists.

Known limitations, found while reviewing and left as they are:

- The strip is clipped to the work area: tiles that don't fit are reached by the wheel or keys. The wheel needs "Scroll inactive windows when hovering"; with it off, those windows are reached by keys only. Win+Shift+wheel is not handled.
- A tile that scrolls in snaps to its place while the one leaving still eases out, so two thumbnails can overlap for up to 0.3 seconds. Hover can stay stale under a still cursor after a scroll.
- The scroll position of the strip is forgotten when the strip hides and shows again.
- While you drag an edge, windows that wait to be taken (new ones) wait until the drag ends.
- Master holds 9 windows per monitor; beyond that windows are not taken. A window whose minimum size doesn't fit is released and floats.
- A window that is stuck maximized (it would not restore) is skipped, and one that spans more than one monitor may be clamped to the size of a single monitor when it is put back.
- The order of the list is not saved: after a restart, windows are taken in z-order again. Rotating does not write the state file.
- Flashing windows (apps asking for attention) are not brought forward.
- `fancyzones_excluded_apps` is honoured in Stage mode too: such apps are never taken, and a held one is released within about a second of being added to the list.
- Dock-by-drag and the desktop scroll leave the windows that Stage holds alone: they don't act on them in Stage mode.

## Install it on another computer

The release build is one file, `Muntin.exe`. It needs Windows 10 or 11 (64-bit) and nothing else: no PowerToys, no Visual Studio, no runtime to install.

1. Copy `Muntin.exe` to the other computer, for example to `%LOCALAPPDATA%\Muntin\`.
2. If PowerToys is installed there, turn FancyZones off in it.
3. Run it, right-click its tray icon, and check **Start at sign-in**.
4. Optional: copy your `settings.json` to `%LOCALAPPDATA%\Muntin\` on the other computer. Don't copy `dock-layouts.json`: layouts belong to the monitors of one computer.

Things to expect:

- The file isn't signed. If you download it instead of copying it from a drive, Windows SmartScreen may ask before the first run: choose "More info", then "Run anyway".
- To update, quit Muntin from its icon, replace the file, and start it again.
## Rebuild it

Only the C++ part of PowerToys builds with Visual Studio 2022, and that is all the fork needs. The installed exe is locked while it runs, so stop the fork before copying the new build over it.

```powershell
# From the root of the repository
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  src\modules\fancyzones\FancyZones\FancyZones.vcxproj -restore -p:RestorePackagesConfig=true `
  -p:Platform=x64 -p:Configuration=Release "-p:SolutionDir=$PWD\\" -m
Stop-Process -Name Muntin -ErrorAction SilentlyContinue
Copy-Item x64\Release\Muntin.exe "$env:LOCALAPPDATA\Muntin\" -Force
Start-Process "$env:LOCALAPPDATA\Muntin\Muntin.exe"
```

The build lands in `x64\Release`, which is disposable (about 15 GB of intermediates with a Debug build, so delete `x64` freely). Point shortcuts at an installed copy, never at the build folder. Use `-p:Configuration=Debug` only for debugging: the debug exe only runs where Visual Studio is installed. If the Release link fails with LNK1201 while writing the PDB, set `$env:_LINK_ = '/DEBUG:NONE'` first.

Unit tests: build `src\modules\fancyzones\FancyZonesTests\UnitTests\UnitTests.vcxproj` the same way with `-p:Configuration=Debug`, then run `vstest.console.exe x64\Debug\tests\FancyZones.UnitTests\FancyZones.UnitTests.dll /TestCaseFilter:FullyQualifiedName~DockTree`.

`tools\dock-live-test\docktest.ps1 -Monitor 0` (or `-Monitor 1`) checks the whole feature with real test windows. It takes over the mouse and keyboard for about a minute. `-Only keys` (or `dock`, `resize`, `slots`, `monitors`, `close`, several separated by commas) runs only those sections, after docking two windows side by side; sections that build on earlier ones bring them along.
