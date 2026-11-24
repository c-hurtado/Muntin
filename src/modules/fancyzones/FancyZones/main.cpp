#include "pch.h"

#include <common/utils/window.h>
#include <common/utils/UnhandledExceptionHandler.h>

#include <FancyZonesLib/trace.h>
#include <FancyZonesLib/Generated Files/resource.h>

#include <common/utils/logger_helper.h>
#include <common/hooks/LowlevelKeyboardEvent.h>
#include <common/utils/resources.h>


#include <FancyZonesLib/DesktopScroll.h>
#include <FancyZonesLib/FancyZones.h>
#include <FancyZonesLib/FancyZonesWinHookEventIDs.h>
#include <FancyZonesLib/ModuleConstants.h>
#include <FancyZonesLib/FancyZonesData/DockLayouts.h>
#include <FancyZonesLib/Settings.h>
#include <FancyZonesLib/Tray.h>
#include <FancyZonesLib/StageMode.h>
#include <FancyZonesLib/Watchdog.h>

#include <filesystem>
#include <fstream>

#include <KnownFolders.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <wil/resource.h>
#include <common/utils/json.h>

#include <FancyZonesApp.h>

// Non-localizable
const std::wstring moduleName = L"FancyZones";
const std::wstring internalPath = L"";
const std::wstring instanceMutexName = L"Local\\Muntin_InstanceMutex";
const std::wstring watchdogMutexName = L"Local\\Muntin_WatchdogMutex";
const std::wstring stockMutexName = L"Local\\PowerToys_FancyZones_InstanceMutex"; // FancyZones of PowerToys, which can't run alongside

namespace
{
    ULONGLONG FileTimeValue(const FILETIME& time) noexcept
    {
        return (static_cast<ULONGLONG>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    constexpr wchar_t Usage[] = L"Usage: Muntin.exe list | save <name> | load <name> | delete <name> | restart\n"
                                L"A name has at most 100 characters and none of \\ / : * ? \" < > | %\n";

    // A GUI app has no console of its own: write to the redirected output, or to the console of the shell that started it
    void Print(const std::wstring& text)
    {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        wil::unique_hfile console;
        if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
        {
            console.reset(CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
            out = console.get();
        }

        DWORD written = 0;
        if (!out || out == INVALID_HANDLE_VALUE)
        {
            return;
        }

        if (GetFileType(out) == FILE_TYPE_CHAR)
        {
            WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        }
        else
        {
            const auto utf8 = winrt::to_string(text);
            WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }

    // Builds before November 2026 kept their files with those of PowerToys FancyZones. They are copied over once, never
    // over a file that is already there, and a marker file says it was done: deleting the layouts later to start afresh
    // doesn't bring the old ones back. The originals stay, PowerToys may still use them.
    // Returns true on the first run ever: no layouts here, and none from an earlier build either.
    bool TakeOverPowerToysFiles()
    {
        namespace fs = std::filesystem;
        std::error_code error;
        const fs::path folder = PTSettingsHelper::get_root_save_folder_location();
        const fs::path marker = folder / L"imported-from-powertoys";
        if (fs::exists(marker, error))
        {
            return false;
        }

        bool firstRun = !fs::exists(folder / L"dock-layouts.json", error);
        wil::unique_cotaskmem_string localAppData;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
        {
            const fs::path old = fs::path{ localAppData.get() } / L"Microsoft" / L"PowerToys" / L"FancyZones";
            firstRun = firstRun && !fs::exists(old / L"dock-layouts.json", error);
            for (const auto name : { L"settings.json", L"dock-layouts.json" })
            {
                fs::copy_file(old / name, folder / name, fs::copy_options::skip_existing, error);
            }

            // Only an earlier build writes this one: where scrolled windows belong, after a crash. Moved, so that it is used once.
            if (!fs::exists(folder / L"desktop-scroll.txt", error))
            {
                fs::rename(old / L"desktop-scroll.txt", folder / L"desktop-scroll.txt", error);
            }
        }

        std::ofstream{ marker };
        return firstRun;
    }

    // Names of the saved layouts, one per line. Read from the file: no running instance is needed.
    std::wstring LayoutNames()
    {
        std::wstring names;
        const auto data = json::from_file(DockLayouts::DockLayoutsFileName());
        if (data && json::has(data.value(), NonLocalizable::DockLayoutsIds::NamedID))
        {
            for (const auto& entry : data->GetNamedObject(NonLocalizable::DockLayoutsIds::NamedID))
            {
                names += std::wstring{ entry.Key() } + L"\n";
            }
        }

        return names;
    }

    // Muntin.exe list | save|load|delete <name>: "<verb>\n<name>" for the running instance, see FancyZones::DockCommand
    std::optional<std::pair<std::wstring, std::wstring>> DockCommandFromCommandLine()
    {
        int count = 0;
        wil::unique_hlocal_ptr<LPWSTR> args(CommandLineToArgvW(GetCommandLineW(), &count));
        if (!args || count < 2)
        {
            return std::nullopt;
        }

        const std::wstring verb = args.get()[1];
        if (verb == L"list" || verb == L"restart")
        {
            // These take no name: anything more gets the usage
            return std::pair{ count == 2 ? verb : L"help", std::wstring{} };
        }

        if (verb != L"save" && verb != L"load" && verb != L"delete" && verb != L"list" && verb != L"restart" && verb != L"help" && verb != L"/?")
        {
            return std::nullopt;
        }

        // A command that can't be run fails here, instead of starting FancyZones
        const std::wstring name = count == 3 ? args.get()[2] : L"";
        return std::pair{ verb, DockCommands::IsValidName(name) ? name : std::wstring{} };
    }

    // Muntin.exe --watchdog <pid> <creation time>, for the helper that Muntin starts for itself
    std::optional<Watchdog::Parent> WatchdogParentFromCommandLine()
    {
        int count = 0;
        wil::unique_hlocal_ptr<LPWSTR> args(CommandLineToArgvW(GetCommandLineW(), &count));
        return args ? Watchdog::ParseCommandLine(count, args.get()) : std::nullopt;
    }

    // watchdog.log in the data folder: one line each time the helper can't do its job. It has no logger, which would share Muntin's log file.
    void WatchdogNote(const wchar_t* text) noexcept
    {
        try
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            wchar_t stamp[32]{};
            swprintf_s(stamp, L"%04d-%02d-%02d %02d:%02d:%02d ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            std::wofstream{ PTSettingsHelper::get_module_save_folder_location(NonLocalizable::ModuleKey) + L"\\watchdog.log", std::ios::app } << stamp << text << L'\n';
        }
        catch (const std::exception&)
        {
        }
    }

    // A Muntin of another process: the one that took over, when this one ended by being stopped
    bool OtherInstanceRuns(DWORD process)
    {
        DWORD other = 0;
        const HWND window = FindWindowW(Tray::WindowClassName, nullptr);
        return window && GetWindowThreadProcessId(window, &other) && other != process;
    }

    // The process of that id, if it was created at that time: not another that got the id since
    wil::unique_handle OpenParent(const Watchdog::Parent& parent)
    {
        wil::unique_handle process(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent.process));
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!process || !GetProcessTimes(process.get(), &created, &exited, &kernel, &user) || FileTimeValue(created) != parent.created)
        {
            return {};
        }

        return process;
    }

    // A process of its own, so that it still works when Muntin hangs. It has no window and takes no lock of Muntin's.
    // Ctrl+Alt+Shift+F12 ends Muntin: it is asked to quit, and ended after a few seconds if it doesn't. Then, or when Muntin
    // ends any other way, the windows that the desktop scroll or stage mode still hold go back to their places.
    int RunWatchdog(const Watchdog::Parent& parentProcess)
    {
        constexpr DWORD QuitWaitMillis = 3000;

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        // One at a time. A restart can leave the old one a moment longer, while it puts the windows back.
        wil::unique_handle only(CreateMutexW(nullptr, FALSE, watchdogMutexName.c_str()));
        const DWORD owned = only ? WaitForSingleObject(only.get(), 5000) : WAIT_FAILED;
        if (owned != WAIT_OBJECT_0 && owned != WAIT_ABANDONED)
        {
            WatchdogNote(L"Another watchdog still runs, or the lock failed: this one gives up");
            return 0;
        }

        wil::unique_handle parent = OpenParent(parentProcess);
        if (!parent)
        {
            return 0; // Muntin is gone already, or it isn't the process that started this
        }

        const UINT quitMessage = RegisterWindowMessageW(Tray::QuitMessageName);
        if (!RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12))
        {
            WatchdogNote(L"RegisterHotKey for Ctrl+Alt+Shift+F12 failed: another program has it. Crash recovery still works.");
        }

        for (;;)
        {
            const HANDLE handles[] = { parent.get() };
            const DWORD woke = MsgWaitForMultipleObjects(1, handles, FALSE, INFINITE, QS_ALLINPUT);
            if (woke == WAIT_FAILED)
            {
                WatchdogNote(L"Waiting for Muntin failed: the watchdog exits without touching any window");
                return 0;
            }

            if (woke == WAIT_OBJECT_0)
            {
                break;
            }

            bool pressed = false;
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                pressed = pressed || message.message == WM_HOTKEY;
            }

            if (pressed)
            {
                DWORD owner = 0;
                const HWND window = FindWindowW(Tray::WindowClassName, nullptr);
                if (window && GetWindowThreadProcessId(window, &owner) && owner == parentProcess.process)
                {
                    PostMessageW(window, quitMessage, 0, 0);
                }

                if (WaitForSingleObject(parent.get(), QuitWaitMillis) != WAIT_OBJECT_0)
                {
                    TerminateProcess(parent.get(), 1); // The handle was checked against the creation time
                    WaitForSingleObject(parent.get(), 2000);
                }

                break;
            }
        }

        if (!OtherInstanceRuns(parentProcess.process))
        {
            DesktopScroll::RecoverParkedFile();
            StageMode::RecoverStateFile([&] { return OtherInstanceRuns(parentProcess.process); }); // Stage mode: every window it moved goes back to where stage-state.txt says
        }

        return 0;
    }

    // Starts the watchdog for this process
    void StartWatchdog() noexcept
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        {
            return;
        }

        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        std::wstring commandLine = L"\"" + std::wstring{ exe } + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(FileTimeValue(created));
        STARTUPINFOW startup{ .cb = sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION info{};
        // Out of the job of Muntin if it is allowed to, so that whatever ends the job leaves the watchdog to put the windows back
        const bool started = CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &startup, &info) ||
                             CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
        if (started)
        {
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
        else
        {
            Logger::warn(L"The watchdog couldn't be started: {}", get_last_error_or_default(GetLastError()));
        }
    }

    // Ends the running instance. Layouts are saved on every change, nothing is lost.
    bool EndInstance(HWND window)
    {
        DWORD processId = 0;
        GetWindowThreadProcessId(window, &processId);
        wil::unique_handle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId));
        return process && TerminateProcess(process.get(), 0) && WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0;
    }

    // Starts an instance as a process of its own, and returns its main window once it is up
    HWND StartInstance()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        STARTUPINFOW startup{ .cb = sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info))
        {
            return nullptr;
        }

        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        for (int i = 0; i < 100; ++i)
        {
            if (const HWND window = FindWindowW(DockCommands::WindowClassName, nullptr))
            {
                return window;
            }

            Sleep(100);
        }

        return nullptr;
    }

    bool SendDockCommand(HWND window, const std::wstring& verb, const std::wstring& name)
    {
        std::wstring text = verb + L"\n" + name;
        COPYDATASTRUCT data{ .dwData = DockCommands::CopyDataId, .cbData = static_cast<DWORD>(text.size() * sizeof(wchar_t)), .lpData = text.data() };
        DWORD_PTR result = FALSE;
        return SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 10000, &result) && result;
    }

    // Start menu shortcut "<name>" that loads the layout, so that Command Palette and Windows search find it by name
    void UpdateLayoutShortcut(const std::wstring& verb, const std::wstring& name)
    {
        wil::unique_cotaskmem_string programs;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
        {
            Logger::error(L"Layout shortcut: no Start menu folder");
            return;
        }

        const std::wstring folder = std::wstring{ programs.get() } + L"\\Muntin";
        const std::wstring path = folder + L"\\" + name + L".lnk";
        if (verb == L"delete")
        {
            DeleteFileW(path.c_str());
            return;
        }

        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
        CreateDirectoryW(folder.c_str(), nullptr);

        auto link = winrt::try_create_instance<IShellLinkW>(CLSID_ShellLink);
        if (!link)
        {
            return;
        }

        link->SetPath(exe);
        link->SetArguments((L"load \"" + name + L"\"").c_str());
        link->SetDescription((L"Muntin layout " + name).c_str());
        link->SetIconLocation(exe, 0);
        const auto file = link.try_as<IPersistFile>();
        if (!file || FAILED(file->Save(path.c_str(), TRUE)))
        {
            Logger::error(L"Layout shortcut {} couldn't be written", path);
        }
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR lpCmdLine, _In_ int nCmdShow)
{
    // The watchdog uses no logger: it must not touch the log file of Muntin
    if (const auto parent = WatchdogParentFromCommandLine())
    {
        return RunWatchdog(parent.value());
    }

    // No telemetry: the PowerToys events and trace sessions are left out
    winrt::init_apartment();
    LoggerHelpers::init_logger(moduleName, internalPath, LogSettings::fancyZonesLoggerName);
    InitUnhandledExceptionHandler();

    // A command for the running instance. A command process always exits: a shell waits for it when its output is piped.
    const auto command = DockCommandFromCommandLine();
    if (command)
    {
        const auto& [verb, name] = command.value();
        if (verb == L"list")
        {
            Print(LayoutNames());
            return 0;
        }

        if (verb != L"restart" && name.empty())
        {
            Print(Usage);
            return 1;
        }

        HWND running = FindWindowW(DockCommands::WindowClassName, nullptr);
        if (verb == L"restart" && running && !EndInstance(running))
        {
            Print(L"Couldn't end the running Muntin\n");
            return 1;
        }

        if (verb == L"restart" || (verb == L"load" && !running))
        {
            running = StartInstance();
            if (!running)
            {
                Print(L"Muntin didn't start\n");
                return 1;
            }

            if (verb == L"restart")
            {
                return 0;
            }
        }

        if (!running)
        {
            Print(L"Muntin isn't running\n");
            return 1;
        }

        const bool done = SendDockCommand(running, verb, name);
        if (done || verb == L"delete")
        {
            UpdateLayoutShortcut(verb, name);
        }

        if (!done)
        {
            Print(verb == L"save" ? L"Nothing docked to save\n" : L"No layout named " + name + L"\n");
        }

        return done ? 0 : 1;
    }

    auto mutex = CreateMutex(nullptr, true, instanceMutexName.c_str());
    if (mutex == nullptr)
    {
        Logger::error(L"Failed to create mutex. {}", get_last_error_or_default(GetLastError()));
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Logger::warn(L"Muntin is already running");
        return 0;
    }

    // Both would dock the same windows. A PowerToys that runs as administrator denies access to its lock, which also means it's there.
    SetLastError(ERROR_SUCCESS);
    wil::unique_handle stock{ OpenMutexW(SYNCHRONIZE, FALSE, stockMutexName.c_str()) };
    if (stock || GetLastError() == ERROR_ACCESS_DENIED)
    {
        MessageBoxW(nullptr, L"FancyZones of PowerToys is running, and the two can't run together. Turn FancyZones off in PowerToys Settings, then start Muntin again.", L"Muntin", MB_OK | MB_ICONWARNING);
        return 0;
    }

    // Holding the lock of stock FancyZones keeps it from starting while Muntin runs
    wil::unique_handle stockBlock{ CreateMutexW(nullptr, FALSE, stockMutexName.c_str()) };

    // After the checks, so that a run that gave up doesn't use up the welcome
    const bool firstRun = TakeOverPowerToysFiles();

    auto process = GetCurrentProcess();
    if (!SetPriorityClass(process, NORMAL_PRIORITY_CLASS))
    {
        std::wstring err = get_last_error_or_default(GetLastError());
        Logger::warn(L"Failed to set priority to FancyZones: {}", err);
    }

    FancyZonesApp app(GET_RESOURCE_STRING(IDS_FANCYZONES), NonLocalizable::ModuleKey);
    app.Run();

    // A settings file of its own, so that the welcome is said once
    if (firstRun)
    {
        FancyZonesSettings::SaveValue(NonLocalizable::ShiftDragID, json::value(FancyZonesSettings::settings().shiftDrag));
    }

    StartWatchdog();
    Tray::Start(hInstance, firstRun);
    run_message_loop();
    Tray::Stop();

    return 0;
}
