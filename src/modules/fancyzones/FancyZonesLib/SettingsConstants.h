#pragma once

enum class SettingId
{
    ShiftDrag = 0,
    MouseSwitch,
    MouseMiddleClickSpanningMultipleZones,
    MouseWheelLayoutSwitch,
    OverrideSnapHotkeys,
    MoveWindowAcrossMonitors,
    MoveWindowsBasedOnPosition,
    OverlappingZonesAlgorithm,
    DisplayOrWorkAreaChangeMoveWindows,
    ZoneSetChangeMoveWindows,
    AppLastZoneMoveWindows,
    OpenWindowOnActiveMonitor,
    RestoreWindowSize,
    QuickLayoutSwitch,
    FlashZonesOnQuickSwitch,
    LaunchEditorOnScreenWhereCursorPlaced,
    ShowOnAllMonitors,
    SpanZonesAcrossMonitors,
    MakeDraggedWindowsTransparent,
    ZoneHighlightColor,
    ZoneHighlightOpacity,
    LayoutNameLabelEnabled,
    LayoutNameLabelPlacement,
    LayoutNameLabelTextColor,
    LayoutNameLabelBackgroundColor,
    LayoutNameLabelFontSize,
    LayoutNameLabelPadding,
    LayoutNameLabelDuration,
    ZoneBorderColor,
    ZoneColor,
    ZoneNumberColor,
    SystemTheme,
    ShowZoneNumber,
    EditorHotkey,
    WindowSwitching,
    NextTabHotkey,
    PrevTabHotkey,
    MonitorRotation,
    MonitorRotationHotkey,
    ExcludedApps,
    AllowSnapChildWindows,
    DisableRoundCornersOnSnapping,
};

// The names of the settings in settings.json
namespace NonLocalizable
{
    // FancyZones settings descriptions are localized, but underlying toggle (spinner, color picker) names are not.
    const wchar_t ShiftDragID[] = L"fancyzones_shiftDrag";
    const wchar_t MouseSwitchID[] = L"fancyzones_mouseSwitch";
    const wchar_t MouseMiddleClickSpanningMultipleZonesID[] = L"fancyzones_mouseMiddleClickSpanningMultipleZones";
    const wchar_t MouseWheelLayoutSwitchID[] = L"fancyzones_mouseWheelLayoutSwitch";
    const wchar_t OverrideSnapHotKeysID[] = L"fancyzones_overrideSnapHotkeys";
    const wchar_t MoveWindowAcrossMonitorsID[] = L"fancyzones_moveWindowAcrossMonitors";
    const wchar_t MoveWindowsBasedOnPositionID[] = L"fancyzones_moveWindowsBasedOnPosition";
    const wchar_t OverlappingZonesAlgorithmID[] = L"fancyzones_overlappingZonesAlgorithm";
    const wchar_t DisplayOrWorkAreaChangeMoveWindowsID[] = L"fancyzones_displayOrWorkAreaChange_moveWindows";
    const wchar_t ZoneSetChangeMoveWindowsID[] = L"fancyzones_zoneSetChange_moveWindows";
    const wchar_t AppLastZoneMoveWindowsID[] = L"fancyzones_appLastZone_moveWindows";
    const wchar_t OpenWindowOnActiveMonitorID[] = L"fancyzones_openWindowOnActiveMonitor";
    const wchar_t RestoreSizeID[] = L"fancyzones_restoreSize";
    const wchar_t QuickLayoutSwitch[] = L"fancyzones_quickLayoutSwitch";
    const wchar_t FlashZonesOnQuickSwitch[] = L"fancyzones_flashZonesOnQuickSwitch";
    const wchar_t UseCursorPosEditorStartupScreenID[] = L"use_cursorpos_editor_startupscreen";
    const wchar_t ShowOnAllMonitorsID[] = L"fancyzones_show_on_all_monitors";
    const wchar_t SpanZonesAcrossMonitorsID[] = L"fancyzones_span_zones_across_monitors";
    const wchar_t MakeDraggedWindowTransparentID[] = L"fancyzones_makeDraggedWindowTransparent";
    const wchar_t AllowChildWindowSnapID[] = L"fancyzones_allowChildWindowSnap";
    const wchar_t DisableRoundCornersOnSnapping[] = L"fancyzones_disableRoundCornersOnSnap";

    const wchar_t SystemThemeID[] = L"fancyzones_systemTheme";
    const wchar_t ZoneColorID[] = L"fancyzones_zoneColor";
    const wchar_t ZoneBorderColorID[] = L"fancyzones_zoneBorderColor";
    const wchar_t ZoneHighlightColorID[] = L"fancyzones_zoneHighlightColor";
    const wchar_t ZoneNumberColorID[] = L"fancyzones_zoneNumberColor";
    const wchar_t EditorHotkeyID[] = L"fancyzones_editor_hotkey";
    const wchar_t WindowSwitchingToggleID[] = L"fancyzones_windowSwitching";
    const wchar_t NextTabHotkeyID[] = L"fancyzones_nextTab_hotkey";
    const wchar_t PrevTabHotkeyID[] = L"fancyzones_prevTab_hotkey";
    const wchar_t MonitorRotationID[] = L"fancyzones_monitorRotation";
    const wchar_t MonitorRotationHotkeyID[] = L"fancyzones_monitorRotation_hotkey";
    const wchar_t ExcludedAppsID[] = L"fancyzones_excluded_apps";
    const wchar_t ZoneHighlightOpacityID[] = L"fancyzones_highlight_opacity";
    const wchar_t DockCaptionButtonsID[] = L"fancyzones_dockCaptionButtons";
    const wchar_t DockAutoTileID[] = L"fancyzones_dockAutoTile";
    const wchar_t DesktopScrollOverviewID[] = L"fancyzones_desktopScrollOverview";
    const wchar_t DesktopScrollPagesID[] = L"fancyzones_desktopScrollPages";
    const wchar_t DockAnimationID[] = L"fancyzones_dockAnimation";
    const wchar_t DockMiddleClickExpandID[] = L"fancyzones_dockMiddleClickExpand";
    const wchar_t DockEdgeSnapID[] = L"fancyzones_dockEdgeSnap";
    const wchar_t DockEdgeSnapBandID[] = L"fancyzones_dockEdgeSnapBand";
    const wchar_t DockGapID[] = L"fancyzones_dockGap";
    const wchar_t DockEdgeBandID[] = L"fancyzones_dockEdgeBand";
    const wchar_t DockWindowEdgePercentID[] = L"fancyzones_dockWindowEdgePercent";
    const wchar_t ShowZoneNumberID[] = L"fancyzones_showZoneNumber";
    const wchar_t LayoutNameLabelEnabledID[] = L"fancyzones_layoutNameLabelEnabled";
    const wchar_t LayoutNameLabelPlacementID[] = L"fancyzones_layoutNameLabelPlacement";
    const wchar_t LayoutNameLabelTextColorID[] = L"fancyzones_layoutNameLabelTextColor";
    const wchar_t LayoutNameLabelBackgroundColorID[] = L"fancyzones_layoutNameLabelBackgroundColor";
    const wchar_t LayoutNameLabelFontSizeID[] = L"fancyzones_layoutNameLabelFontSize";
    const wchar_t LayoutNameLabelPaddingID[] = L"fancyzones_layoutNameLabelPadding";
    const wchar_t LayoutNameLabelDurationID[] = L"fancyzones_layoutNameLabelDuration";
}
