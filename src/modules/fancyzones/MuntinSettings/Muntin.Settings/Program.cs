using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace Muntin.Settings.App;

public static partial class Program
{
    public const string WindowTitle = "Muntin Settings";

    // The class every WinUI 3 desktop window gets; Tray.cpp looks for the same class and title
    public const string WindowClass = "WinUIDesktopWin32WindowClass";

    public static string? MuntinExe { get; private set; }

    [STAThread]
    private static int Main(string[] args)
    {
        // One window at a time: a second start brings the first to the front
        using var mutex = new Mutex(true, @"Local\Muntin.Settings.SingleInstance", out var created);
        if (!created)
        {
            var existing = FindWindowW(WindowClass, WindowTitle);
            if (existing != IntPtr.Zero)
            {
                ShowWindow(existing, 9 /* SW_RESTORE */);
                SetForegroundWindow(existing);
            }

            return 0;
        }

        MuntinExe = ParseMuntinExe(args);

        WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(callback =>
        {
            var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            _ = new App();
        });
        return 0;
    }

    // "--muntin <path>": Tray.cpp passes the path of the running Muntin.exe
    private static string? ParseMuntinExe(string[] args)
    {
        for (var i = 0; i + 1 < args.Length; i++)
        {
            if (string.Equals(args[i], "--muntin", StringComparison.OrdinalIgnoreCase) && File.Exists(args[i + 1]))
            {
                return args[i + 1];
            }
        }

        return null;
    }

    [LibraryImport("user32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial IntPtr FindWindowW(string? className, string? windowName);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetForegroundWindow(IntPtr window);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool ShowWindow(IntPtr window, int command);
}
