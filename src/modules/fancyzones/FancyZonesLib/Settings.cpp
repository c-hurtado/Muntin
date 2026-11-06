#include "pch.h"
#include "Settings.h"

#include <common/logger/call_tracer.h>
#include <common/logger/logger.h>
#include <common/SettingsAPI/FileWatcher.h>
#include <common/utils/resources.h>
#include <common/utils/string_utils.h>

#include <FancyZonesLib/FancyZonesWinHookEventIDs.h>
#include <FancyZonesLib/SettingsObserver.h>
#include <FancyZonesLib/trace.h>

FancyZonesSettings::FancyZonesSettings()
{
    const std::wstring& settingsFileName = GetSettingsFileName();
    m_settingsFileWatcher = std::make_unique<FileWatcher>(settingsFileName, [&]() {
        PostMessageW(HWND_BROADCAST, WM_PRIV_SETTINGS_CHANGED, NULL, NULL);
    });
}

FancyZonesSettings& FancyZonesSettings::instance()
{
    static FancyZonesSettings instance;
    return instance;
}

void FancyZonesSettings::AddObserver(SettingsObserver& observer)
{
    m_observers.insert(&observer);
}

void FancyZonesSettings::RemoveObserver(SettingsObserver& observer)
{
    auto iter = m_observers.find(&observer);
    if (iter != m_observers.end())
    {
        m_observers.erase(iter);
    }
}

void FancyZonesSettings::SetBoolFlag(const PowerToysSettings::PowerToyValues& values, const wchar_t* id, SettingId notificationId, bool& out)
{
    if (const auto val = values.get_bool_value(id))
    {
        if (out != *val)
        {
            out = *val;
            NotifyObservers(notificationId);
        }
    }
}

void FancyZonesSettings::LoadSettings()
{
    _TRACER_;

    try
    {
        auto jsonOpt = json::from_file(GetSettingsFileName());
        if (!jsonOpt)
        {
            Logger::warn(L"Failed to read from settings file");
            return;
        }

        PowerToysSettings::PowerToyValues values = PowerToysSettings::PowerToyValues::from_json_string(jsonOpt->Stringify(), NonLocalizable::ModuleKey);

        // flags
        SetBoolFlag(values, NonLocalizable::ShiftDragID, SettingId::ShiftDrag, m_settings.shiftDrag);
        SetBoolFlag(values, NonLocalizable::MouseSwitchID, SettingId::MouseSwitch, m_settings.mouseSwitch);
        SetBoolFlag(values, NonLocalizable::MouseMiddleClickSpanningMultipleZonesID, SettingId::MouseMiddleClickSpanningMultipleZones, m_settings.mouseMiddleClickSpanningMultipleZones);
        SetBoolFlag(values, NonLocalizable::MouseWheelLayoutSwitchID, SettingId::MouseWheelLayoutSwitch, m_settings.mouseWheelLayoutSwitch);
        SetBoolFlag(values, NonLocalizable::OverrideSnapHotKeysID, SettingId::OverrideSnapHotkeys, m_settings.overrideSnapHotkeys);
        SetBoolFlag(values, NonLocalizable::MoveWindowAcrossMonitorsID, SettingId::MoveWindowAcrossMonitors, m_settings.moveWindowAcrossMonitors);
        SetBoolFlag(values, NonLocalizable::MoveWindowsBasedOnPositionID, SettingId::MoveWindowsBasedOnPosition, m_settings.moveWindowsBasedOnPosition);
        SetBoolFlag(values, NonLocalizable::DisplayOrWorkAreaChangeMoveWindowsID, SettingId::DisplayOrWorkAreaChangeMoveWindows, m_settings.displayOrWorkAreaChange_moveWindows);
        SetBoolFlag(values, NonLocalizable::ZoneSetChangeMoveWindowsID, SettingId::ZoneSetChangeMoveWindows, m_settings.zoneSetChange_moveWindows);
        SetBoolFlag(values, NonLocalizable::AppLastZoneMoveWindowsID, SettingId::AppLastZoneMoveWindows, m_settings.appLastZone_moveWindows);
        SetBoolFlag(values, NonLocalizable::OpenWindowOnActiveMonitorID, SettingId::OpenWindowOnActiveMonitor, m_settings.openWindowOnActiveMonitor);
        SetBoolFlag(values, NonLocalizable::RestoreSizeID, SettingId::RestoreWindowSize, m_settings.restoreSize);
        SetBoolFlag(values, NonLocalizable::QuickLayoutSwitch, SettingId::QuickLayoutSwitch, m_settings.quickLayoutSwitch);
        SetBoolFlag(values, NonLocalizable::FlashZonesOnQuickSwitch, SettingId::FlashZonesOnQuickSwitch, m_settings.flashZonesOnQuickSwitch);
        SetBoolFlag(values, NonLocalizable::UseCursorPosEditorStartupScreenID, SettingId::LaunchEditorOnScreenWhereCursorPlaced, m_settings.use_cursorpos_editor_startupscreen);
        SetBoolFlag(values, NonLocalizable::ShowOnAllMonitorsID, SettingId::ShowOnAllMonitors, m_settings.showZonesOnAllMonitors);
        SetBoolFlag(values, NonLocalizable::SpanZonesAcrossMonitorsID, SettingId::SpanZonesAcrossMonitors, m_settings.spanZonesAcrossMonitors);
        SetBoolFlag(values, NonLocalizable::MakeDraggedWindowTransparentID, SettingId::MakeDraggedWindowsTransparent, m_settings.makeDraggedWindowTransparent);
        SetBoolFlag(values, NonLocalizable::WindowSwitchingToggleID, SettingId::WindowSwitching, m_settings.windowSwitching);
        SetBoolFlag(values, NonLocalizable::MonitorRotationID, SettingId::MonitorRotation, m_settings.monitorRotation);
        SetBoolFlag(values, NonLocalizable::SystemThemeID, SettingId::SystemTheme, m_settings.systemTheme);
        SetBoolFlag(values, NonLocalizable::ShowZoneNumberID, SettingId::ShowZoneNumber, m_settings.showZoneNumber);
        SetBoolFlag(values, NonLocalizable::AllowChildWindowSnapID, SettingId::AllowSnapChildWindows, m_settings.allowSnapChildWindows);
        SetBoolFlag(values, NonLocalizable::DisableRoundCornersOnSnapping, SettingId::DisableRoundCornersOnSnapping, m_settings.disableRoundCorners);

        // colors
        if (auto val = values.get_string_value(NonLocalizable::ZoneColorID))
        {
            if (m_settings.zoneColor != *val)
            {
                m_settings.zoneColor = std::move(*val);
                NotifyObservers(SettingId::ZoneColor);
            }
        }

        if (auto val = values.get_string_value(NonLocalizable::ZoneBorderColorID))
        {
            if (m_settings.zoneBorderColor != *val)
            {
                m_settings.zoneBorderColor = std::move(*val);
                NotifyObservers(SettingId::ZoneBorderColor);
            }
        }

        if (auto val = values.get_string_value(NonLocalizable::ZoneHighlightColorID))
        {
            if (m_settings.zoneHighlightColor != *val)
            {
                m_settings.zoneHighlightColor = std::move(*val);
                NotifyObservers(SettingId::ZoneHighlightColor);
            }
        }

        if (auto val = values.get_string_value(NonLocalizable::ZoneNumberColorID))
        {
            if (m_settings.zoneNumberColor != *val)
            {
                m_settings.zoneNumberColor = std::move(*val);
                NotifyObservers(SettingId::ZoneNumberColor);
            }
        }

        if (auto val = values.get_int_value(NonLocalizable::ZoneHighlightOpacityID))
        {
            if (m_settings.zoneHighlightOpacity != *val)
            {
                m_settings.zoneHighlightOpacity = std::move(*val);
                NotifyObservers(SettingId::ZoneHighlightOpacity);
            }
        }

        // layout name label
        SetBoolFlag(values, NonLocalizable::LayoutNameLabelEnabledID, SettingId::LayoutNameLabelEnabled, m_settings.layoutNameLabelEnabled);

        // Read when a window is docked or the layout is applied: no observer needs to hear about them
        if (auto val = values.get_bool_value(NonLocalizable::DockCaptionButtonsID))
        {
            m_settings.dockCaptionButtons = *val;
        }

        if (auto val = values.get_bool_value(NonLocalizable::DockAutoTileID))
        {
            m_settings.dockAutoTile = *val;
        }

        if (auto val = values.get_bool_value(NonLocalizable::DesktopScrollOverviewID))
        {
            m_settings.desktopScrollOverview = *val;
        }

        if (auto val = values.get_int_value(NonLocalizable::DesktopScrollPagesID))
        {
            m_settings.desktopScrollPages = *val > 0 ? (std::min)(*val, MaxDesktopScrollPages) : MaxDesktopScrollPages; // 0 meant no limit before
        }

        if (auto val = values.get_bool_value(NonLocalizable::DockAnimationID))
        {
            m_settings.dockAnimation = *val;
        }

        if (auto val = values.get_bool_value(NonLocalizable::DockMiddleClickExpandID))
        {
            m_settings.dockMiddleClickExpand = *val;
        }

        if (auto val = values.get_bool_value(NonLocalizable::DockEdgeSnapID))
        {
            m_settings.dockEdgeSnap = *val;
        }

        if (auto val = values.get_int_value(NonLocalizable::DockEdgeSnapBandID))
        {
            m_settings.dockEdgeSnapBand = std::clamp(*val, 1, 100);
        }

        if (auto val = values.get_int_value(NonLocalizable::DockGapID))
        {
            m_settings.dockGap = std::clamp(*val, 0, 100);
        }

        if (auto val = values.get_int_value(NonLocalizable::DockEdgeBandID))
        {
            m_settings.dockEdgeBand = std::clamp(*val, 8, 500);
        }

        if (auto val = values.get_int_value(NonLocalizable::DockWindowEdgePercentID))
        {
            m_settings.dockWindowEdgePercent = std::clamp(*val, 5, 50);
        }

        if (auto val = values.get_int_value(NonLocalizable::LayoutNameLabelPlacementID))
        {
            // Avoid undefined behavior
            if (*val >= 0 && *val < static_cast<int>(LayoutNameLabelPlacement::EnumElements))
            {
                auto placement = (LayoutNameLabelPlacement)*val;
                if (m_settings.layoutNameLabelPlacement != placement)
                {
                    m_settings.layoutNameLabelPlacement = placement;
                    NotifyObservers(SettingId::LayoutNameLabelPlacement);
                }
            }
        }

        if (auto val = values.get_string_value(NonLocalizable::LayoutNameLabelTextColorID))
        {
            if (m_settings.layoutNameLabelTextColor != *val)
            {
                m_settings.layoutNameLabelTextColor = std::move(*val);
                NotifyObservers(SettingId::LayoutNameLabelTextColor);
            }
        }

        if (auto val = values.get_string_value(NonLocalizable::LayoutNameLabelBackgroundColorID))
        {
            if (m_settings.layoutNameLabelBackgroundColor != *val)
            {
                m_settings.layoutNameLabelBackgroundColor = std::move(*val);
                NotifyObservers(SettingId::LayoutNameLabelBackgroundColor);
            }
        }

        if (auto val = values.get_int_value(NonLocalizable::LayoutNameLabelFontSizeID))
        {
            auto fontSize = std::clamp(*val, 8, 128);
            if (m_settings.layoutNameLabelFontSize != fontSize)
            {
                m_settings.layoutNameLabelFontSize = fontSize;
                NotifyObservers(SettingId::LayoutNameLabelFontSize);
            }
        }

        if (auto val = values.get_int_value(NonLocalizable::LayoutNameLabelPaddingID))
        {
            auto padding = std::clamp(*val, 0, 64);
            if (m_settings.layoutNameLabelPadding != padding)
            {
                m_settings.layoutNameLabelPadding = padding;
                NotifyObservers(SettingId::LayoutNameLabelPadding);
            }
        }

        if (auto val = values.get_int_value(NonLocalizable::LayoutNameLabelDurationID))
        {
            auto duration = std::clamp(*val, 300, 10000);
            if (m_settings.layoutNameLabelDuration != duration)
            {
                m_settings.layoutNameLabelDuration = duration;
                NotifyObservers(SettingId::LayoutNameLabelDuration);
            }
        }

        // hotkeys
        if (const auto val = values.get_json(NonLocalizable::EditorHotkeyID))
        {
            auto hotkey = PowerToysSettings::HotkeyObject::from_json(*val);
            if (m_settings.editorHotkey.get_modifiers() != hotkey.get_modifiers() || m_settings.editorHotkey.get_key() != hotkey.get_key() || m_settings.editorHotkey.get_code() != hotkey.get_code())
            {
                m_settings.editorHotkey = std::move(hotkey);
                NotifyObservers(SettingId::EditorHotkey);
            }
        }

        if (const auto val = values.get_json(NonLocalizable::NextTabHotkeyID))
        {
            auto hotkey = PowerToysSettings::HotkeyObject::from_json(*val);
            if (m_settings.nextTabHotkey.get_modifiers() != hotkey.get_modifiers() || m_settings.nextTabHotkey.get_key() != hotkey.get_key() || m_settings.nextTabHotkey.get_code() != hotkey.get_code())
            {
                m_settings.nextTabHotkey = PowerToysSettings::HotkeyObject::from_json(*val);
                NotifyObservers(SettingId::NextTabHotkey);
            }
        }

        if (const auto val = values.get_json(NonLocalizable::PrevTabHotkeyID))
        {
            auto hotkey = PowerToysSettings::HotkeyObject::from_json(*val);
            if (m_settings.prevTabHotkey.get_modifiers() != hotkey.get_modifiers() || m_settings.prevTabHotkey.get_key() != hotkey.get_key() || m_settings.prevTabHotkey.get_code() != hotkey.get_code())
            {
                m_settings.prevTabHotkey = PowerToysSettings::HotkeyObject::from_json(*val);
                NotifyObservers(SettingId::PrevTabHotkey);
            }
        }

        if (const auto val = values.get_json(NonLocalizable::MonitorRotationHotkeyID))
        {
            auto hotkey = PowerToysSettings::HotkeyObject::from_json(*val);
            if (m_settings.monitorRotationHotkey.get_modifiers() != hotkey.get_modifiers() || m_settings.monitorRotationHotkey.get_key() != hotkey.get_key() || m_settings.monitorRotationHotkey.get_code() != hotkey.get_code())
            {
                m_settings.monitorRotationHotkey = PowerToysSettings::HotkeyObject::from_json(*val);
                NotifyObservers(SettingId::MonitorRotationHotkey);
            }
        }

        // excluded apps
        if (auto val = values.get_string_value(NonLocalizable::ExcludedAppsID))
        {
            std::wstring apps = std::move(*val);
            std::vector<std::wstring> excludedApps;
            auto excludedUppercase = apps;
            CharUpperBuffW(excludedUppercase.data(), static_cast<DWORD>(excludedUppercase.length()));
            std::wstring_view view(excludedUppercase);
            view = left_trim<wchar_t>(trim<wchar_t>(view));

            while (!view.empty())
            {
                auto pos = (std::min)(view.find_first_of(L"\r\n"), view.length());
                excludedApps.emplace_back(view.substr(0, pos));
                view.remove_prefix(pos);
                view = left_trim<wchar_t>(trim<wchar_t>(view));
            }

            if (m_settings.excludedAppsArray != excludedApps)
            {
                m_settings.excludedApps = apps;
                m_settings.excludedAppsArray = excludedApps;
                NotifyObservers(SettingId::ExcludedApps);
            }
        }

        // algorithms
        if (auto val = values.get_int_value(NonLocalizable::OverlappingZonesAlgorithmID))
        {
            // Avoid undefined behavior
            if (*val >= 0 && *val < static_cast<int>(OverlappingZonesAlgorithm::EnumElements))
            {
                auto algorithm = (OverlappingZonesAlgorithm)*val;
                if (m_settings.overlappingZonesAlgorithm != algorithm)
                {
                    m_settings.overlappingZonesAlgorithm = algorithm;
                    NotifyObservers(SettingId::OverlappingZonesAlgorithm);
                }
            }
        }
    }
    catch (const winrt::hresult_error& e)
    {
        // Failure to load settings does not break FancyZones functionality. Display error message and continue with default settings.
        Logger::error(L"Failed to read settings. {}", e.message());
        MessageBox(NULL,
                   GET_RESOURCE_STRING(IDS_FANCYZONES_SETTINGS_LOAD_ERROR).c_str(),
                   GET_RESOURCE_STRING(IDS_POWERTOYS_FANCYZONES).c_str(),
                   MB_OK);
    }
}

bool FancyZonesSettings::SaveValue(const wchar_t* id, const json::JsonValue& value, const std::wstring& file)
{
    // A file that can't be read, say after a typo while editing it by hand, isn't replaced: the other settings would be lost
    auto read = json::from_file(file);
    if (!read && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        Logger::error(L"{} can't be read, {} isn't saved", file, id);
        return false;
    }

    auto settings = read.value_or(json::JsonObject{});
    if (!settings.HasKey(L"properties"))
    {
        settings.SetNamedValue(L"properties", json::JsonObject{});
    }
    else if (!json::has(settings, L"properties", json::JsonValueType::Object))
    {
        Logger::error(L"{} has no list of properties, {} isn't saved", file, id);
        return false;
    }

    json::JsonObject property;
    property.SetNamedValue(L"value", value);
    settings.GetNamedObject(L"properties").SetNamedValue(id, property);
    settings.SetNamedValue(L"name", json::value(NonLocalizable::ModuleKey));
    settings.SetNamedValue(L"version", json::value(L"1.0"));

    // The file watcher reloads the settings
    json::to_file(file, settings);
    return true;
}

void FancyZonesSettings::NotifyObservers(SettingId id) const
{
    for (auto observer : m_observers)
    {
        if (observer->WantsToBeNotified(id))
        {
            observer->SettingsUpdate(id);
        }
    }
}
