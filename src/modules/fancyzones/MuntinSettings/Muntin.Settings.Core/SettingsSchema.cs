// Mirrors the Muntin native code: key names from FancyZonesLib\SettingsConstants.h (NonLocalizable IDs),
// defaults from FancyZonesLib\Settings.h, clamps from FancyZonesSettings::LoadSettings (Settings.cpp),
// the arrange modes and the Animations label from FancyZonesLib\Tray.cpp (Modes[], AnimationsLabel).
// The other settings left the tray for this app: their labels and choice tables live here only.
// When one of those changes, change this file too (NativeParityTests checks them).
namespace Muntin.Settings;

public enum SettingKind
{
    Bool,
    Int,
    String,
}

public sealed record Choice(int Value, string Label);

/// <summary>One of the arrange modes (Tray.cpp Modes[]): Dock, or Stage with one of its layouts. StageLayout null: Dock, which leaves the layout as it is.</summary>
public sealed record ModeDef(string Label, int ArrangeMode, string? StageLayout);

public sealed record SettingDef(string Key, SettingKind Kind, object Default, string Label, int Min = 0, int Max = 0)
{
    public int Clamp(int value) => Math.Clamp(value, Min, Max);
}

public static class SettingsSchema
{
    public const string ModuleName = "FancyZones";
    public const string FileVersion = "1.0";
    public const string HelpUrl = "https://github.com/c-hurtado/Muntin#readme";
    public const int MaxDesktopScrollPages = 4;

    // Settings.h stageMonitors: written by Muntin itself (strip/cut drags); the app never touches it
    public const string StageMonitorsKey = "fancyzones_stageMonitors";

    // On/off settings (the order the tray once listed them in; only Animations is still in the tray)
    public static readonly SettingDef ShiftDrag = new("fancyzones_shiftDrag", SettingKind.Bool, true, "Dock only while Shift is held");
    public static readonly SettingDef DockEdgeSnap = new("fancyzones_dockEdgeSnap", SettingKind.Bool, true, "Dock at the left and right screen edges");
    public static readonly SettingDef DockAutoTile = new("fancyzones_dockAutoTile", SettingKind.Bool, false, "Dock new windows by themselves");
    public static readonly SettingDef OverrideSnapHotkeys = new("fancyzones_overrideSnapHotkeys", SettingKind.Bool, false, "Win+arrow keys move docked windows");
    public static readonly SettingDef DockCaptionButtons = new("fancyzones_dockCaptionButtons", SettingKind.Bool, true, "< > buttons on docked windows");
    public static readonly SettingDef DockMiddleClickExpand = new("fancyzones_dockMiddleClickExpand", SettingKind.Bool, true, "Middle click on a title bar expands the window");
    public static readonly SettingDef DockAnimation = new("fancyzones_dockAnimation", SettingKind.Bool, true, "Animations");
    public static readonly SettingDef DockAnticipation = new("fancyzones_dockAnticipation", SettingKind.Bool, false, "Pull back slightly before a move");
    public static readonly SettingDef StageAnimatePromote = new("fancyzones_stageAnimatePromote", SettingKind.Bool, false, "Animate promoting from the strip");
    public static readonly SettingDef DesktopScrollOverview = new("fancyzones_desktopScrollOverview", SettingKind.Bool, false, "Zoom out while scrolling the desktop");

    public static readonly SettingDef[] Toggles =
    [
        ShiftDrag, DockEdgeSnap, DockAutoTile, OverrideSnapHotkeys, DockCaptionButtons, DockMiddleClickExpand,
        DockAnimation, DockAnticipation, StageAnimatePromote, DesktopScrollOverview,
    ];

    // Choices (Settings.cpp clamps each to its table's range)
    public static readonly SettingDef DockMotionCurve = new("fancyzones_dockMotionCurve", SettingKind.Int, 0, "Motion curve", 0, 1);
    public static readonly SettingDef StagePromoteStyle = new("fancyzones_stagePromoteStyle", SettingKind.Int, 1, "Promote animation", 0, 1);
    public static readonly SettingDef StageMoveStyle = new("fancyzones_stageMoveStyle", SettingKind.Int, 0, "Move animation", 0, 1);
    public static readonly SettingDef StageStripRadius = new("fancyzones_stageStripRadius", SettingKind.Int, 12, "Strip corner radius", 0, 40);
    public static readonly SettingDef ArrangeMode = new("fancyzones_arrangeMode", SettingKind.Int, 1, "Arrange", 0, 1);
    public static readonly SettingDef StageLayout = new("fancyzones_stageLayout", SettingKind.String, "stage", "Stage layout");
    public static readonly SettingDef DesktopScrollPages = new("fancyzones_desktopScrollPages", SettingKind.Int, 2, "Desktop pages per monitor", 1, MaxDesktopScrollPages);

    public static readonly Choice[] MotionChoices = [new(0, "Spring"), new(1, "Ease")];
    public static readonly Choice[] PromoteChoices = [new(0, "Grow from the tile"), new(1, "Crossfade")];
    public static readonly Choice[] MoveChoices = [new(0, "Slide"), new(1, "Crossfade")];
    public static readonly Choice[] RadiusChoices = [new(0, "Square"), new(6, "Small"), new(12, "Medium"), new(20, "Large"), new(30, "Round")];
    public static readonly Choice[] PageChoices = [new(1, "1 (no scrolling)"), new(2, "2"), new(3, "3"), new(MaxDesktopScrollPages, "4")];

    // SliderMax is the slider reach; settings.json allows more (Settings.cpp clamps gap 0..100, margins 0..500)
    public sealed record SpacingDef(SettingDef Setting, int SliderMax);

    public static readonly SettingDef DockGap = new("fancyzones_dockGap", SettingKind.Int, 8, "Gap", 0, 100);
    public static readonly SettingDef DockMarginLeft = new("fancyzones_dockMarginLeft", SettingKind.Int, 0, "Left", 0, 500);
    public static readonly SettingDef DockMarginTop = new("fancyzones_dockMarginTop", SettingKind.Int, 0, "Top", 0, 500);
    public static readonly SettingDef DockMarginRight = new("fancyzones_dockMarginRight", SettingKind.Int, 0, "Right", 0, 500);
    public static readonly SettingDef DockMarginBottom = new("fancyzones_dockMarginBottom", SettingKind.Int, 0, "Bottom", 0, 500);

    public static readonly SpacingDef[] Spacings =
    [
        new(DockGap, 30), new(DockMarginLeft, 15), new(DockMarginTop, 15), new(DockMarginRight, 15), new(DockMarginBottom, 15),
    ];

    // Advanced (no tray item): ranges from Settings.cpp
    public static readonly SettingDef DockEdgeSnapBand = new("fancyzones_dockEdgeSnapBand", SettingKind.Int, 12, "Edge docking band (pixels)", 1, 100);
    public static readonly SettingDef DockEdgeBand = new("fancyzones_dockEdgeBand", SettingKind.Int, 48, "Work area edge band (pixels)", 8, 500);
    public static readonly SettingDef DockWindowEdgePercent = new("fancyzones_dockWindowEdgePercent", SettingKind.Int, 30, "Window edge share (percent)", 5, 50);
    public static readonly SettingDef StagePairCut = new("fancyzones_stagePairCut", SettingKind.Int, 500, "Pair split (thousandths)", 150, 850);
    public static readonly SettingDef StageMasterCut = new("fancyzones_stageMasterCut", SettingKind.Int, 600, "Master split (thousandths)", 150, 850);

    public static readonly SettingDef[] All =
    [
        .. Toggles,
        DockMotionCurve, StagePromoteStyle, StageMoveStyle, StageStripRadius, ArrangeMode, StageLayout, DesktopScrollPages,
        DockGap, DockMarginLeft, DockMarginTop, DockMarginRight, DockMarginBottom,
        DockEdgeSnapBand, DockEdgeBand, DockWindowEdgePercent, StagePairCut, StageMasterCut,
    ];

    // Settings.cpp: a layout name nobody knows is ignored (Muntin keeps the one it had)
    public static bool IsKnownStageLayout(string? value) => value is "master" or "stage" or "pair";

    // The arrange modes, one choice (Tray.cpp Modes[], same order and labels): fancyzones_arrangeMode and fancyzones_stageLayout together
    public static readonly ModeDef[] Modes =
    [
        new("Dock (drag to dock)", 0, null),
        new("Master and stack", 1, "master"),
        new("Stage with strip", 1, "stage"),
        new("Pair with strip", 1, "pair"),
    ];

    /// <summary>
    /// The index into Modes of what the two keys say; -1 for Stage with a layout name Muntin does not know (it ignores the name and
    /// keeps the layout it had, which the file can't tell).
    /// </summary>
    public static int CurrentMode(int arrangeMode, string? stageLayout)
    {
        if (arrangeMode != 1)
        {
            return 0;
        }

        return Array.FindIndex(Modes, m => m.StageLayout is not null && m.StageLayout == stageLayout);
    }

    /// <summary>
    /// Settings.h ArrangeFallback: the mode and layout when settings.json doesn't give them. No file: a fresh install, Stage with
    /// the stage layout (the defaults above). A file without them, or one that can't be read: an install from before, Dock and master.
    /// </summary>
    public static (int ArrangeMode, string StageLayout) ArrangeFallback(bool fileExists) =>
        fileExists ? (0, "master") : ((int)ArrangeMode.Default, (string)StageLayout.Default);
}
