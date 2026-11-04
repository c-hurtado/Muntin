#include "pch.h"
#include "Tray.h"

#include <KnownFolders.h>
#include <ShlObj.h>

#include <common/SettingsAPI/settings_helpers.h>
#include <common/logger/logger.h>
#include <common/utils/json.h>

#include <FancyZonesLib/FancyZonesData/DockLayouts.h>
#include <FancyZonesLib/Settings.h>

namespace
{
    constexpr wchar_t WindowClassName[] = L"MuntinTray";
    constexpr wchar_t HelpUrl[] = L"https://github.com/c-hurtado/Muntin#readme";
    constexpr UINT IconId = 1;
    constexpr UINT WM_TRAY = WM_APP + 1;

    struct Toggle
    {
        const wchar_t* id;
        const wchar_t* label;
        bool Settings::*value;
    };

    const Toggle Toggles[] = {
        { NonLocalizable::ShiftDragID, L"Dock only while Shift is held", &Settings::shiftDrag },
        { NonLocalizable::DockEdgeSnapID, L"Dock at the left and right screen edges", &Settings::dockEdgeSnap },
        { NonLocalizable::DockAutoTileID, L"Dock new windows by themselves", &Settings::dockAutoTile },
        { NonLocalizable::OverrideSnapHotKeysID, L"Win+arrow keys move docked windows", &Settings::overrideSnapHotkeys },
        { NonLocalizable::DockCaptionButtonsID, L"< > buttons on docked windows", &Settings::dockCaptionButtons },
        { NonLocalizable::DockAnimationID, L"Animations", &Settings::dockAnimation },
        { NonLocalizable::DesktopScrollOverviewID, L"Zoom out while scrolling the desktop", &Settings::desktopScrollOverview },
    };

    struct Choice
    {
        int value;
        const wchar_t* label;
    };

    const Choice GapChoices[] = { { 0, L"None" }, { 4, L"4 pixels" }, { 8, L"8 pixels" }, { 12, L"12 pixels" }, { 16, L"16 pixels" }, { 24, L"24 pixels" } };
    const Choice PageChoices[] = { { 1, L"1 (no scrolling)" }, { 2, L"2" }, { 3, L"3" }, { MaxDesktopScrollPages, L"4" } };

    enum Command : UINT
    {
        StartAtSignIn = 1,
        OpenFolder,
        Help,
        Quit,
        SaveLayoutCommand,
        ToggleBase = 100,
        GapBase = 200,
        PagesBase = 300,
        LayoutBase = 400,
    };

    HINSTANCE s_instance{};
    HWND s_window{};
    HICON s_icon{};
    UINT s_taskbarCreated{};

    bool AddIcon() noexcept
    {
        NOTIFYICONDATAW data{ .cbSize = sizeof(NOTIFYICONDATAW), .hWnd = s_window, .uID = IconId, .uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP, .uCallbackMessage = WM_TRAY, .hIcon = s_icon };
        wcscpy_s(data.szTip, L"Muntin");
        if (!Shell_NotifyIconW(NIM_ADD, &data))
        {
            return false;
        }

        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        return true;
    }

    std::wstring StartupShortcut()
    {
        wil::unique_cotaskmem_string folder;
        return SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &folder)) ? std::wstring{ folder.get() } + L"\\Muntin.lnk" : std::wstring{};
    }

    void SetStartAtSignIn(bool start) noexcept
    {
        const auto path = StartupShortcut();
        if (path.empty())
        {
            return;
        }

        if (!start)
        {
            DeleteFileW(path.c_str());
            return;
        }

        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        const auto link = winrt::try_create_instance<IShellLinkW>(CLSID_ShellLink);
        const auto file = link ? link.try_as<IPersistFile>() : nullptr;
        if (!file || FAILED(link->SetPath(exe)) || FAILED(link->SetIconLocation(exe, 0)) || FAILED(file->Save(path.c_str(), TRUE)))
        {
            Logger::error(L"Startup shortcut {} couldn't be written", path);
        }
    }

    // Read from the file, which the running layouts keep up to date
    std::vector<std::wstring> LayoutNames()
    {
        std::vector<std::wstring> names;
        const auto data = json::from_file(DockLayouts::DockLayoutsFileName());
        if (data && json::has(data.value(), NonLocalizable::DockLayoutsIds::NamedID, json::JsonValueType::Object))
        {
            for (const auto& entry : data->GetNamedObject(NonLocalizable::DockLayoutsIds::NamedID))
            {
                names.emplace_back(entry.Key());
            }
        }

        return names;
    }

    // The same command as "Muntin.exe load <name>", see main.cpp
    void LoadLayout(const std::wstring& name) noexcept
    {
        if (const HWND fancyZones = FindWindowW(DockCommands::WindowClassName, nullptr))
        {
            std::wstring text = L"load\n" + name;
            COPYDATASTRUCT data{ .dwData = DockCommands::CopyDataId, .cbData = static_cast<DWORD>(text.size() * sizeof(wchar_t)), .lpData = text.data() };
            SendMessageW(fancyZones, WM_COPYDATA, reinterpret_cast<WPARAM>(s_window), reinterpret_cast<LPARAM>(&data));
        }
    }

    constexpr int NameEditId = 100;

    INT_PTR CALLBACK NameDialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        if (message == WM_INITDIALOG)
        {
            SetWindowLongPtrW(dialog, DWLP_USER, lparam);
            SendDlgItemMessageW(dialog, NameEditId, EM_LIMITTEXT, 100, 0);
            EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
            SetForegroundWindow(dialog);
            SetFocus(GetDlgItem(dialog, NameEditId));
            return FALSE; // The focus is set
        }

        if (message != WM_COMMAND)
        {
            return FALSE;
        }

        wchar_t text[101]{};
        GetDlgItemTextW(dialog, NameEditId, text, ARRAYSIZE(text));
        if (LOWORD(wparam) == NameEditId && HIWORD(wparam) == EN_CHANGE)
        {
            EnableWindow(GetDlgItem(dialog, IDOK), DockCommands::IsValidName(text));
        }
        else if (LOWORD(wparam) == IDOK && DockCommands::IsValidName(text))
        {
            *reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, DWLP_USER)) = text;
            EndDialog(dialog, IDOK);
        }
        else if (LOWORD(wparam) == IDCANCEL)
        {
            EndDialog(dialog, IDCANCEL);
        }

        return TRUE;
    }

    // A dialog asking for a layout name, built in memory: the module has no dialog resources. Empty when cancelled.
    std::wstring AskLayoutName()
    {
        std::vector<WORD> buffer;
        const auto align = [&] {
            if (buffer.size() % 2)
            {
                buffer.push_back(0);
            }
        };
        const auto add = [&](const void* data, size_t bytes) {
            const auto words = static_cast<const WORD*>(data);
            buffer.insert(buffer.end(), words, words + bytes / sizeof(WORD));
        };
        const auto addText = [&](const wchar_t* text) { add(text, (wcslen(text) + 1) * sizeof(wchar_t)); };
        const auto addItem = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD kind, const wchar_t* text) {
            align();
            DLGITEMTEMPLATE item{ .style = style | WS_CHILD | WS_VISIBLE, .x = x, .y = y, .cx = cx, .cy = cy, .id = id };
            add(&item, sizeof(item));
            buffer.push_back(0xFFFF);
            buffer.push_back(kind);
            addText(text);
            buffer.push_back(0); // No creation data
        };

        // In the taskbar, so that it can be found again once covered
        DLGTEMPLATE header{ .style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU, .dwExtendedStyle = WS_EX_APPWINDOW, .cdit = 4, .cx = 220, .cy = 72 };
        add(&header, sizeof(header));
        buffer.push_back(0); // No menu
        buffer.push_back(0); // The standard dialog class
        addText(L"Save layout");
        buffer.push_back(9);
        addText(L"Segoe UI");
        addItem(0, 8, 8, 204, 10, static_cast<WORD>(-1), 0x0082, L"Name for the windows docked now, on every monitor:");
        addItem(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 22, 204, 14, NameEditId, 0x0081, L"");
        addItem(WS_TABSTOP | BS_DEFPUSHBUTTON, 108, 48, 50, 14, IDOK, 0x0080, L"Save");
        addItem(WS_TABSTOP | BS_PUSHBUTTON, 162, 48, 50, 14, IDCANCEL, 0x0080, L"Cancel");

        std::wstring name;
        if (DialogBoxIndirectParamW(s_instance, reinterpret_cast<DLGTEMPLATE*>(buffer.data()), s_window, NameDialogProc, reinterpret_cast<LPARAM>(&name)) != IDOK)
        {
            name.clear();
        }

        return name;
    }

    // Runs "Muntin.exe save <name>", which also makes the Start menu shortcut, see main.cpp. Names can't hold quotes.
    void SaveLayout()
    {
        const auto name = AskLayoutName();
        if (name.empty())
        {
            return;
        }

        const auto existing = LayoutNames(); // Read now: the dialog can stay open a long time
        const bool taken = std::any_of(existing.begin(), existing.end(), [&](const std::wstring& other) { return _wcsicmp(other.c_str(), name.c_str()) == 0; });
        if (taken && MessageBoxW(s_window, (L"Replace the layout " + name + L"?").c_str(), L"Muntin", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES)
        {
            return;
        }

        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        // ponytail: a second process only to reuse main.cpp's Start menu shortcut code; move that into the lib if it grows
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe, (L"save \"" + name + L"\"").c_str(), nullptr, SW_HIDE)) <= 32)
        {
            Tray::Notify(L"The layout couldn't be saved.");
        }
    }

    HMENU ChoiceMenu(const Choice* choices, size_t count, int current, UINT base)
    {
        HMENU menu = CreatePopupMenu();
        for (size_t i = 0; i < count; ++i)
        {
            AppendMenuW(menu, MF_STRING | (choices[i].value == current ? MF_CHECKED : 0), static_cast<UINT_PTR>(base) + i, choices[i].label);
        }

        return menu;
    }

    void ShowMenu(POINT at) noexcept
    {
        const Settings& settings = FancyZonesSettings::settings();
        const auto layouts = LayoutNames();
        const auto startup = StartupShortcut();
        const bool startsAtSignIn = !startup.empty() && GetFileAttributesW(startup.c_str()) != INVALID_FILE_ATTRIBUTES;

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Muntin");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < std::size(Toggles); ++i)
        {
            AppendMenuW(menu, MF_STRING | (settings.*Toggles[i].value ? MF_CHECKED : 0), static_cast<UINT_PTR>(ToggleBase) + i, Toggles[i].label);
        }

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(ChoiceMenu(GapChoices, std::size(GapChoices), settings.dockGap, GapBase)), L"Gap between windows");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(ChoiceMenu(PageChoices, std::size(PageChoices), settings.desktopScrollPages, PagesBase)), L"Desktop pages per monitor");

        HMENU layoutMenu = CreatePopupMenu();
        for (size_t i = 0; i < layouts.size(); ++i)
        {
            std::wstring label;
            for (const wchar_t letter : layouts[i])
            {
                label += letter == L'&' ? L"&&" : std::wstring(1, letter); // A single & would underline the next letter
            }

            AppendMenuW(layoutMenu, MF_STRING, static_cast<UINT_PTR>(LayoutBase) + i, label.c_str());
        }

        if (layouts.empty())
        {
            AppendMenuW(layoutMenu, MF_STRING | MF_GRAYED, 0, L"None saved yet");
        }

        AppendMenuW(menu, MF_STRING, SaveLayoutCommand, L"Save layout...");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), L"Load layout");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (startsAtSignIn ? MF_CHECKED : 0), StartAtSignIn, L"Start at sign-in");
        AppendMenuW(menu, MF_STRING, OpenFolder, L"Open settings folder");
        AppendMenuW(menu, MF_STRING, Help, L"Help");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Quit, L"Quit");

        // Without this the menu stays open when the user clicks elsewhere
        SetForegroundWindow(s_window);
        const UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, s_window, nullptr);
        PostMessageW(s_window, WM_NULL, 0, 0);
        DestroyMenu(menu);

        bool saved = true;
        if (command >= LayoutBase && command - LayoutBase < layouts.size())
        {
            LoadLayout(layouts[command - LayoutBase]);
        }
        else if (command >= PagesBase && command - PagesBase < std::size(PageChoices))
        {
            saved = FancyZonesSettings::SaveValue(NonLocalizable::DesktopScrollPagesID, json::value(PageChoices[command - PagesBase].value));
        }
        else if (command >= GapBase && command - GapBase < std::size(GapChoices))
        {
            saved = FancyZonesSettings::SaveValue(NonLocalizable::DockGapID, json::value(GapChoices[command - GapBase].value));
        }
        else if (command >= ToggleBase && command - ToggleBase < std::size(Toggles))
        {
            const auto& toggle = Toggles[command - ToggleBase];
            saved = FancyZonesSettings::SaveValue(toggle.id, json::value(!(settings.*toggle.value)));
        }
        else if (command == SaveLayoutCommand)
        {
            SaveLayout();
        }
        else if (command == StartAtSignIn)
        {
            SetStartAtSignIn(!startsAtSignIn);
        }
        else if (command == OpenFolder)
        {
            ShellExecuteW(nullptr, L"open", PTSettingsHelper::get_root_save_folder_location().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (command == Help)
        {
            ShellExecuteW(nullptr, L"open", HelpUrl, nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (command == Quit)
        {
            PostQuitMessage(0);
        }

        if (!saved)
        {
            Tray::Notify(L"settings.json can't be read, so the change wasn't saved. Fix the file or delete it, then try again.");
        }
    }

    LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        if (message == WM_TRAY)
        {
            const UINT event = LOWORD(lparam);
            // The icon still hears clicks while the menu's dialogs run their own loops: one menu at a time
            static bool open = false;
            if ((event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT) && !open)
            {
                open = true;
                ShowMenu(POINT{ GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam) });
                open = false;
            }

            return 0;
        }

        // Explorer started again: the icon is gone with the old taskbar
        if (message == s_taskbarCreated && s_taskbarCreated != 0)
        {
            AddIcon();
            return 0;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void Tray::Start(HINSTANCE instance, bool welcome) noexcept
{
    s_instance = instance;
    s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wcex{ .cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = WndProc, .hInstance = instance, .lpszClassName = WindowClassName };
    RegisterClassExW(&wcex);

    // A hidden top-level window rather than a message-only one, which wouldn't hear about TaskbarCreated
    s_window = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClassName, L"Muntin", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    // Run as administrator, the window would otherwise not hear from Explorer
    ChangeWindowMessageFilterEx(s_window, s_taskbarCreated, MSGFLT_ALLOW, nullptr);
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    s_icon = static_cast<HICON>(LoadImageW(instance, L"IDI_ICON1", IMAGE_ICON, size, size, 0));
    if (!s_window || !AddIcon())
    {
        Logger::error(L"The tray icon couldn't be added");
        return;
    }

    if (welcome)
    {
        NOTIFYICONDATAW data{ .cbSize = sizeof(NOTIFYICONDATAW), .hWnd = s_window, .uID = IconId, .uFlags = NIF_INFO };
        wcscpy_s(data.szInfoTitle, L"Muntin is running");
        wcscpy_s(data.szInfo, L"Hold Shift while you drag a window to dock it. Right-click this icon for settings.");
        data.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }
}

void Tray::Stop() noexcept
{
    if (s_window)
    {
        NOTIFYICONDATAW data{ .cbSize = sizeof(NOTIFYICONDATAW), .hWnd = s_window, .uID = IconId };
        Shell_NotifyIconW(NIM_DELETE, &data);
        DestroyWindow(s_window);
        s_window = nullptr;
    }

    if (s_icon)
    {
        DestroyIcon(s_icon);
        s_icon = nullptr;
    }

    UnregisterClassW(WindowClassName, s_instance);
}

bool Tray::Notify(const wchar_t* text) noexcept
{
    const HWND window = s_window;
    if (!window)
    {
        return false;
    }

    NOTIFYICONDATAW data{ .cbSize = sizeof(NOTIFYICONDATAW), .hWnd = window, .uID = IconId, .uFlags = NIF_INFO };
    wcscpy_s(data.szInfoTitle, L"Muntin");
    wcsncpy_s(data.szInfo, text, _TRUNCATE);
    data.dwInfoFlags = NIIF_INFO;
    return Shell_NotifyIconW(NIM_MODIFY, &data);
}
