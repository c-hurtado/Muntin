#include "pch.h"
#include "Tray.h"

#include <mutex>
#include <thread>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <common/SettingsAPI/settings_helpers.h>
#include <common/logger/logger.h>
#include <common/utils/json.h>

#include <FancyZonesLib/FancyZonesData/DockLayouts.h>
#include <FancyZonesLib/Settings.h>

namespace
{
    constexpr wchar_t HelpUrl[] = L"https://github.com/c-hurtado/Muntin#readme";
    constexpr UINT IconId = 1;
    constexpr UINT WM_TRAY = WM_APP + 1;

    constexpr wchar_t AnimationsLabel[] = L"Animations"; // fancyzones_dockAnimation; every other setting is in the settings window

    // The arrange modes, one radio group: Stage mode is one of three layouts, Dock is the other mode. Choosing Dock leaves the layout as it is.
    struct Mode
    {
        const wchar_t* label;
        int arrangeMode;
        const wchar_t* stageLayout; // nullptr: not written
    };

    const Mode Modes[] = {
        { L"Dock (drag to dock)", 0, nullptr },
        { L"Master and stack", 1, L"master" },
        { L"Stage with strip", 1, L"stage" },
        { L"Pair with strip", 1, L"pair" },
    };

    // The mode the settings are in. Settings.cpp keeps no layout name it does not know, so the last line is never reached.
    size_t CurrentMode(const Settings& settings) noexcept
    {
        if (settings.arrangeMode != 1)
        {
            return 0;
        }

        for (size_t i = 1; i < std::size(Modes); ++i)
        {
            if (settings.stageLayout == Modes[i].stageLayout)
            {
                return i;
            }
        }

        return 2;
    }

    enum Command : UINT
    {
        StartAtSignIn = 1,
        OpenFolder,
        Help,
        Quit,
        SaveLayoutCommand,
        LoadLayoutCommand,
        SettingsCommand,
        AnimationsCommand,
        ModeBase = 100,
    };

    HINSTANCE s_instance{};
    HWND s_window{};
    HICON s_icon{};
    UINT s_taskbarCreated{};
    UINT s_quitMessage{};

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

    // The WinUI settings app (src\modules\fancyzones\MuntinSettings): its window class and title, see Program.cs there
    constexpr wchar_t SettingsWindowClass[] = L"WinUIDesktopWin32WindowClass";
    constexpr wchar_t SettingsWindowTitle[] = L"Muntin Settings";
    constexpr wchar_t SettingsExe[] = L"Muntin.Settings.exe";

    // %LOCALAPPDATA%\Muntin\Settings (tools\deploy-settings-app.ps1) first, then next to Muntin.exe
    void OpenSettings() noexcept
    {
        if (const HWND open = FindWindowW(SettingsWindowClass, SettingsWindowTitle))
        {
            if (IsIconic(open))
            {
                ShowWindow(open, SW_RESTORE);
            }

            SetForegroundWindow(open);
            return;
        }

        wchar_t muntin[MAX_PATH]{};
        GetModuleFileNameW(nullptr, muntin, ARRAYSIZE(muntin));
        std::wstring candidates[2];
        try
        {
            candidates[0] = PTSettingsHelper::get_root_save_folder_location() + L"\\Settings\\" + SettingsExe;
        }
        catch (...)
        {
        }

        const std::wstring_view self{ muntin };
        if (const auto slash = self.find_last_of(L'\\'); slash != std::wstring_view::npos)
        {
            candidates[1] = std::wstring{ self.substr(0, slash + 1) } + SettingsExe;
        }

        for (const auto& exe : candidates)
        {
            if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                continue;
            }

            // The app makes the start at sign-in shortcut to this Muntin.exe
            const std::wstring parameters = std::wstring{ L"--muntin \"" } + muntin + L"\"";
            const std::wstring directory = exe.substr(0, exe.find_last_of(L'\\'));
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(), parameters.c_str(), directory.c_str(), SW_SHOWNORMAL)) <= 32)
            {
                Logger::error(L"{} couldn't be started", exe);
                Tray::Notify(L"Muntin.Settings.exe couldn't be started.");
            }

            return;
        }

        Tray::Notify(L"Muntin.Settings.exe was not found in %LOCALAPPDATA%\\Muntin\\Settings or next to Muntin.exe.");
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

    // A dialog template built in memory: the module has no dialog resources
    struct DialogBuilder
    {
        std::vector<WORD> buffer;

        void Align()
        {
            if (buffer.size() % 2)
            {
                buffer.push_back(0);
            }
        }

        void Add(const void* data, size_t bytes)
        {
            const auto words = static_cast<const WORD*>(data);
            buffer.insert(buffer.end(), words, words + bytes / sizeof(WORD));
        }

        void AddText(const wchar_t* text) { Add(text, (wcslen(text) + 1) * sizeof(wchar_t)); }

        // In the taskbar, so that it can be found again once covered
        DialogBuilder(const wchar_t* title, WORD items, short cx, short cy)
        {
            DLGTEMPLATE header{ .style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU, .dwExtendedStyle = WS_EX_APPWINDOW, .cdit = items, .cx = cx, .cy = cy };
            Add(&header, sizeof(header));
            buffer.push_back(0); // No menu
            buffer.push_back(0); // The standard dialog class
            AddText(title);
            buffer.push_back(9);
            AddText(L"Segoe UI");
        }

        // A control of a standard class: 0x0080 button, 0x0081 edit, 0x0082 static
        void AddItem(DWORD style, short x, short y, short cx, short cy, WORD id, WORD kind, const wchar_t* text)
        {
            BeginItem(style, x, y, cx, cy, id);
            buffer.push_back(0xFFFF);
            buffer.push_back(kind);
            EndItem(text);
        }

        // A control of a registered class, by name
        void AddItem(DWORD style, short x, short y, short cx, short cy, WORD id, const wchar_t* className, const wchar_t* text)
        {
            BeginItem(style, x, y, cx, cy, id);
            AddText(className);
            EndItem(text);
        }

        void BeginItem(DWORD style, short x, short y, short cx, short cy, WORD id)
        {
            Align();
            DLGITEMTEMPLATE item{ .style = style | WS_CHILD | WS_VISIBLE, .x = x, .y = y, .cx = cx, .cy = cy, .id = id };
            Add(&item, sizeof(item));
        }

        void EndItem(const wchar_t* text)
        {
            AddText(text);
            buffer.push_back(0); // No creation data
        }

        INT_PTR Run(DLGPROC proc, LPARAM param) { return DialogBoxIndirectParamW(s_instance, reinterpret_cast<DLGTEMPLATE*>(buffer.data()), s_window, proc, param); }
    };

    // A dialog asking for a layout name. Empty when cancelled.
    std::wstring AskLayoutName()
    {
        DialogBuilder dialog(L"Save layout", 4, 220, 72);
        dialog.AddItem(0, 8, 8, 204, 10, static_cast<WORD>(-1), 0x0082, L"Name for the windows docked now, on every monitor:");
        dialog.AddItem(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 22, 204, 14, NameEditId, 0x0081, L"");
        dialog.AddItem(WS_TABSTOP | BS_DEFPUSHBUTTON, 108, 48, 50, 14, IDOK, 0x0080, L"Save");
        dialog.AddItem(WS_TABSTOP | BS_PUSHBUTTON, 162, 48, 50, 14, IDCANCEL, 0x0080, L"Cancel");

        std::wstring name;
        if (dialog.Run(NameDialogProc, reinterpret_cast<LPARAM>(&name)) != IDOK)
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

    // The layout picker: a card per saved layout, each monitor drawn as its tree of tiles holding the apps' icons
    constexpr wchar_t PickerClassName[] = L"MuntinLayoutPicker";
    constexpr int TileW = 128, TileH = 72, Pad = 12, NameH = 22, Between = 8, CardGap = 12, MaxRow = 760, CloseSize = 18;
    constexpr DWORD PickerStyle = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    constexpr DWORD PickerExStyle = WS_EX_APPWINDOW;

    struct Card
    {
        std::wstring name;
        std::vector<Dock::Tree> monitors;
        RECT rect{};
    };

    // Icons come from a worker thread: the shell can be slow (a network drive, a cold packaged app), and this thread
    // services the mouse and keyboard hooks, which Windows removes when they stall. Shared with the worker, which can outlive the picker.
    struct IconStore
    {
        std::mutex lock;
        std::map<std::wstring, HICON> icons; // by what starts the app
        bool closed{};
    };

    struct Picker
    {
        std::vector<Card> cards;
        std::shared_ptr<IconStore> store;
        HWND window{};
        HFONT font{};
        int dpi{ 96 };
        int hover{ -1 };
        bool overClose{};
        bool tracking{};
        bool open{};
        std::wstring choice;
    } s_picker;

    int Scale(int value) noexcept { return MulDiv(value, s_picker.dpi, 96); }

    const std::wstring& StartOf(const Dock::Node& leaf) noexcept { return leaf.launch.empty() ? leaf.app : leaf.launch; }

    HICON IconFor(const Dock::Node& leaf)
    {
        std::lock_guard guard(s_picker.store->lock);
        const auto entry = s_picker.store->icons.find(StartOf(leaf));
        return entry == s_picker.store->icons.end() ? nullptr : entry->second;
    }

    void LoadIcons(std::shared_ptr<IconStore> store, std::vector<std::wstring> paths, HWND window)
    {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
        for (const auto& path : paths)
        {
            HICON icon = nullptr;
            PIDLIST_ABSOLUTE pidl{};
            if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)))
            {
                SHFILEINFOW info{};
                if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof(info), SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON))
                {
                    icon = info.hIcon;
                }

                CoTaskMemFree(pidl);
            }

            if (!icon)
            {
                continue;
            }

            {
                std::lock_guard guard(store->lock);
                if (store->closed)
                {
                    DestroyIcon(icon);
                    continue;
                }

                store->icons[path] = icon;
            }

            PostMessageW(window, WM_APP, 0, 0); // Repaint
        }

        if (com)
        {
            CoUninitialize();
        }
    }

    // Sets the card rects, cards wrap at MaxRow. Returns the client size. ponytail: no scrolling, add when layouts outgrow the screen
    SIZE LayoutCards()
    {
        int x = Scale(CardGap), y = Scale(CardGap), rowHeight = 0, width = 0;
        for (auto& card : s_picker.cards)
        {
            const int count = static_cast<int>(card.monitors.size());
            const int w = Scale(2 * Pad + count * TileW + (count - 1) * Between);
            const int h = Scale(2 * Pad + TileH + NameH);
            if (x > Scale(CardGap) && x + w > Scale(MaxRow))
            {
                x = Scale(CardGap);
                y += rowHeight + Scale(CardGap);
                rowHeight = 0;
            }

            card.rect = { x, y, x + w, y + h };
            x += w + Scale(CardGap);
            rowHeight = (std::max)(rowHeight, h);
            width = (std::max)(width, x);
        }

        return { (std::max)(width, static_cast<int>(Scale(280))), (std::max)(y + rowHeight + Scale(CardGap), static_cast<int>(Scale(100))) };
    }

    RECT MonitorRect(const Card& card, size_t index)
    {
        const int left = card.rect.left + Scale(Pad) + static_cast<int>(index) * Scale(TileW + Between);
        const int top = card.rect.top + Scale(Pad);
        return { left, top, left + Scale(TileW), top + Scale(TileH) };
    }

    RECT CloseRect(const Card& card)
    {
        const int size = Scale(CloseSize);
        const int right = card.rect.right - Scale(6);
        const int top = card.rect.bottom - Scale(Pad) - Scale(NameH) + (Scale(NameH) - size) / 2 + Scale(4);
        return { right - size, top, right, top + size };
    }

    void DrawNode(HDC dc, const Dock::Node& node, RECT rect)
    {
        if (!node.IsLeaf())
        {
            RECT a = rect, b = rect;
            if (node.sideBySide)
            {
                a.right = b.left = rect.left + static_cast<LONG>((rect.right - rect.left) * node.ratio);
            }
            else
            {
                a.bottom = b.top = rect.top + static_cast<LONG>((rect.bottom - rect.top) * node.ratio);
            }

            DrawNode(dc, *node.first, a);
            DrawNode(dc, *node.second, b);
            return;
        }

        InflateRect(&rect, -Scale(1), -Scale(1)); // Two neighbors leave a gap of two pixels
        FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW));
        if (const HICON icon = IconFor(node))
        {
            const int size = (std::min)({ Scale(32), static_cast<int>(rect.right - rect.left) - Scale(4), static_cast<int>(rect.bottom - rect.top) - Scale(4) });
            if (size >= Scale(12))
            {
                DrawIconEx(dc, (rect.left + rect.right - size) / 2, (rect.top + rect.bottom - size) / 2, icon, size, size, 0, nullptr, DI_NORMAL);
            }
        }
    }

    void PaintPicker(HWND window)
    {
        PAINTSTRUCT paint;
        const HDC target = BeginPaint(window, &paint);
        RECT client;
        GetClientRect(window, &client);

        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        const HGDIOBJ oldFont = SelectObject(dc, s_picker.font);
        SetBkMode(dc, TRANSPARENT);
        FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

        if (s_picker.cards.empty())
        {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            DrawTextW(dc, L"No layouts saved yet", -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        for (size_t i = 0; i < s_picker.cards.size(); ++i)
        {
            const Card& card = s_picker.cards[i];
            const bool hot = static_cast<int>(i) == s_picker.hover;
            FillRect(dc, &card.rect, GetSysColorBrush(hot ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
            for (size_t m = 0; m < card.monitors.size(); ++m)
            {
                RECT screen = MonitorRect(card, m);
                FillRect(dc, &screen, GetSysColorBrush(COLOR_3DSHADOW));
                InflateRect(&screen, -Scale(2), -Scale(2));
                DrawNode(dc, *card.monitors[m].root, screen);
            }

            const RECT close = CloseRect(card);
            RECT text{ card.rect.left + Scale(Pad), card.rect.top + Scale(Pad + TileH + 4), close.left - Scale(4), card.rect.bottom - Scale(4) };
            SetTextColor(dc, GetSysColor(hot ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            DrawTextW(dc, card.name.c_str(), -1, &text, DT_END_ELLIPSIS | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            if (hot)
            {
                RECT x = close;
                if (s_picker.overClose)
                {
                    FillRect(dc, &x, GetSysColorBrush(COLOR_WINDOW));
                    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                }

                DrawTextW(dc, L"×", -1, &x, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &paint);
    }

    // The card under the point, or -1
    int HitCard(POINT at, bool& onClose)
    {
        for (size_t i = 0; i < s_picker.cards.size(); ++i)
        {
            if (PtInRect(&s_picker.cards[i].rect, at))
            {
                const RECT close = CloseRect(s_picker.cards[i]);
                onClose = PtInRect(&close, at);
                return static_cast<int>(i);
            }
        }

        onClose = false;
        return -1;
    }

    void ResizePicker(HWND window)
    {
        const SIZE client = LayoutCards();
        RECT frame{ 0, 0, client.cx, client.cy };
        AdjustWindowRectExForDpi(&frame, PickerStyle, FALSE, PickerExStyle, s_picker.dpi);
        SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window, nullptr, FALSE);
    }

    // The same as "Muntin.exe delete <name>" (see main.cpp), but with the result at hand
    void DeleteCard(HWND window, size_t index)
    {
        const std::wstring name = s_picker.cards[index].name;
        if (MessageBoxW(window, (L"Delete the layout " + name + L"?").c_str(), L"Muntin", MB_YESNO | MB_ICONQUESTION) != IDYES)
        {
            return;
        }

        const HWND fancyZones = FindWindowW(DockCommands::WindowClassName, nullptr);
        std::wstring text = L"delete\n" + name;
        COPYDATASTRUCT data{ .dwData = DockCommands::CopyDataId, .cbData = static_cast<DWORD>(text.size() * sizeof(wchar_t)), .lpData = text.data() };
        if (!fancyZones || !SendMessageW(fancyZones, WM_COPYDATA, reinterpret_cast<WPARAM>(s_window), reinterpret_cast<LPARAM>(&data)))
        {
            Tray::Notify(L"The layout couldn't be deleted.");
            return;
        }

        wil::unique_cotaskmem_string programs;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
        {
            DeleteFileW((std::wstring{ programs.get() } + L"\\Muntin\\" + name + L".lnk").c_str()); // The Start menu shortcut
        }

        s_picker.cards.erase(s_picker.cards.begin() + index);
        s_picker.hover = -1;
        ResizePicker(window);
    }

    LRESULT CALLBACK PickerProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept
    {
        switch (message)
        {
        case WM_PAINT:
            PaintPicker(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_APP: // An icon arrived
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
        {
            if (!s_picker.tracking)
            {
                TRACKMOUSEEVENT track{ .cbSize = sizeof(track), .dwFlags = TME_LEAVE, .hwndTrack = window };
                s_picker.tracking = TrackMouseEvent(&track);
            }

            bool onClose = false;
            const int hover = HitCard(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }, onClose);
            if (hover != s_picker.hover || onClose != s_picker.overClose)
            {
                s_picker.hover = hover;
                s_picker.overClose = onClose;
                InvalidateRect(window, nullptr, FALSE);
            }

            return 0;
        }
        case WM_MOUSELEAVE:
            s_picker.tracking = false;
            s_picker.hover = -1;
            s_picker.overClose = false;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && s_picker.hover >= 0)
            {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }

            break;
        case WM_LBUTTONUP:
        {
            bool onClose = false;
            const int index = HitCard(POINT{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) }, onClose);
            if (index >= 0 && onClose)
            {
                DeleteCard(window, index);
            }
            else if (index >= 0)
            {
                s_picker.choice = s_picker.cards[index].name;
                DestroyWindow(window);
            }

            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE)
            {
                DestroyWindow(window);
            }
            else if (wparam == VK_DELETE && s_picker.hover >= 0)
            {
                DeleteCard(window, s_picker.hover);
            }

            return 0;
        case WM_DESTROY:
            s_picker.open = false;
            return 0;
        }

        return DefWindowProcW(window, message, wparam, lparam);
    }

    // Opens the picker, runs it until it closes, then loads the layout that was chosen
    void PickLayout()
    {
        s_picker = {};
        s_picker.store = std::make_shared<IconStore>();
        s_picker.dpi = static_cast<int>(GetDpiForSystem());
        NONCLIENTMETRICSW metrics{ .cbSize = sizeof(metrics) };
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, s_picker.dpi);
        s_picker.font = CreateFontIndirectW(&metrics.lfMessageFont);

        for (const auto& name : LayoutNames())
        {
            Card card{ .name = name };
            for (auto& [key, tree] : DockLayouts::instance().Named(name))
            {
                if (tree.root)
                {
                    card.monitors.push_back(std::move(tree));
                }
            }

            if (!card.monitors.empty())
            {
                s_picker.cards.push_back(std::move(card));
            }
        }

        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wcex{ .cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = PickerProc, .hInstance = s_instance, .hCursor = LoadCursorW(nullptr, IDC_ARROW), .lpszClassName = PickerClassName };
            registered = RegisterClassExW(&wcex) != 0;
        }

        const SIZE client = LayoutCards();
        RECT frame{ 0, 0, client.cx, client.cy };
        AdjustWindowRectExForDpi(&frame, PickerStyle, FALSE, PickerExStyle, s_picker.dpi);

        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{ .cbSize = sizeof(monitor) };
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width = frame.right - frame.left, height = frame.bottom - frame.top;
        // Kept on screen, so that the title bar stays reachable
        const int x = (std::max)(static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2));
        const int y = (std::max)(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2));

        const HWND window = registered ? CreateWindowExW(PickerExStyle, PickerClassName, L"Load layout", PickerStyle, x, y, width, height, s_window, nullptr, s_instance, nullptr) : nullptr;
        if (window)
        {
            s_picker.open = true;
            s_picker.window = window;
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);

            std::vector<std::wstring> paths;
            for (const auto& card : s_picker.cards)
            {
                for (const auto& monitorTree : card.monitors)
                {
                    for (const Dock::Node* leaf : monitorTree.Leaves())
                    {
                        if (const auto& path = StartOf(*leaf); !path.empty() && std::find(paths.begin(), paths.end(), path) == paths.end())
                        {
                            paths.push_back(path);
                        }
                    }
                }
            }

            std::thread(LoadIcons, s_picker.store, std::move(paths), window).detach();

            MSG msg;
            while (s_picker.open)
            {
                const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
                if (got <= 0)
                {
                    if (got == 0)
                    {
                        PostQuitMessage(static_cast<int>(msg.wParam)); // Not ours to swallow
                    }

                    break;
                }

                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }

            if (IsWindow(window))
            {
                DestroyWindow(window);
            }
        }

        {
            // The worker destroys the icons that arrive from now on
            std::lock_guard guard(s_picker.store->lock);
            s_picker.store->closed = true;
            for (const auto& [path, icon] : s_picker.store->icons)
            {
                DestroyIcon(icon);
            }
        }

        s_picker.window = nullptr;
        DeleteObject(s_picker.font);
        if (!s_picker.choice.empty())
        {
            LoadLayout(s_picker.choice);
        }
    }

    void ShowMenu(POINT at) noexcept
    {
        const Settings& settings = FancyZonesSettings::settings();
        const auto startup = StartupShortcut();
        const bool startsAtSignIn = !startup.empty() && GetFileAttributesW(startup.c_str()) != INVALID_FILE_ATTRIBUTES;

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Muntin");
        AppendMenuW(menu, MF_STRING, SettingsCommand, L"Settings...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        for (size_t i = 0; i < std::size(Modes); ++i)
        {
            AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(ModeBase) + i, Modes[i].label);
        }

        const UINT current = static_cast<UINT>(ModeBase + CurrentMode(settings));
        CheckMenuRadioItem(menu, ModeBase, static_cast<UINT>(ModeBase + std::size(Modes) - 1), current, MF_BYCOMMAND);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (settings.dockAnimation ? MF_CHECKED : 0), AnimationsCommand, AnimationsLabel);
        AppendMenuW(menu, MF_STRING, SaveLayoutCommand, L"Save layout...");
        AppendMenuW(menu, MF_STRING, LoadLayoutCommand, L"Load layout...");
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
        if (command == SettingsCommand)
        {
            OpenSettings();
        }
        else if (command == LoadLayoutCommand)
        {
            PickLayout();
        }
        else if (command >= ModeBase && command - ModeBase < std::size(Modes))
        {
            // Both keys in one write: the reload never sees the new mode with the old layout
            const Mode& mode = Modes[command - ModeBase];
            std::vector<std::pair<const wchar_t*, json::JsonValue>> values;
            if (mode.stageLayout)
            {
                values.emplace_back(NonLocalizable::StageLayoutID, json::value(std::wstring{ mode.stageLayout }));
            }

            values.emplace_back(NonLocalizable::ArrangeModeID, json::value(mode.arrangeMode));
            saved = FancyZonesSettings::SaveValues(values);
        }
        else if (command == AnimationsCommand)
        {
            saved = FancyZonesSettings::SaveValue(NonLocalizable::DockAnimationID, json::value(!settings.dockAnimation));
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
            // A picker lost behind other windows
            if (s_picker.open && s_picker.window && (event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT))
            {
                SetForegroundWindow(s_picker.window);
                return 0;
            }

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

        // From the watchdog, which asks for a clean exit before it ends the process
        if (message == s_quitMessage && s_quitMessage != 0)
        {
            PostQuitMessage(0);
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
    s_quitMessage = RegisterWindowMessageW(QuitMessageName);

    WNDCLASSEXW wcex{ .cbSize = sizeof(WNDCLASSEXW), .lpfnWndProc = WndProc, .hInstance = instance, .lpszClassName = WindowClassName };
    RegisterClassExW(&wcex);

    // A hidden top-level window rather than a message-only one, which wouldn't hear about TaskbarCreated
    s_window = CreateWindowExW(WS_EX_TOOLWINDOW, WindowClassName, L"Muntin", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);

    // Run as administrator, the window would otherwise not hear from Explorer
    ChangeWindowMessageFilterEx(s_window, s_taskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(s_window, s_quitMessage, MSGFLT_ALLOW, nullptr);
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
        wcscpy_s(data.szInfo, L"Right-click this icon to choose how windows are arranged, or to open Settings.");
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
