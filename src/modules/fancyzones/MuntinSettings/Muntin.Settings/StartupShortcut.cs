using System.Runtime.InteropServices;

namespace Muntin.Settings.App;

/// <summary>Start at sign-in, the same shortcut Tray.cpp StartupShortcut/SetStartAtSignIn makes: Startup\Muntin.lnk to Muntin.exe.</summary>
internal static class StartupShortcut
{
    public static string ShortcutPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Startup), "Muntin.lnk");

    public static bool Exists() => File.Exists(ShortcutPath);

    // Deployed as %LOCALAPPDATA%\Muntin\Settings\Muntin.Settings.exe, or next to Muntin.exe
    public static string? FindMuntinExe()
    {
        var here = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
        string?[] candidates =
        [
            Path.GetDirectoryName(here) is { } parent ? Path.Combine(parent, "Muntin.exe") : null,
            Path.Combine(here, "Muntin.exe"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Muntin", "Muntin.exe"),
        ];
        return candidates.FirstOrDefault(c => c is not null && File.Exists(c));
    }

    public static bool Set(bool start, string? muntinExe, out string error)
    {
        error = string.Empty;
        try
        {
            if (!start)
            {
                File.Delete(ShortcutPath);
                return true;
            }

            if (muntinExe is null)
            {
                error = "Muntin.exe wasn't found, so the startup shortcut wasn't made.";
                return false;
            }

            var shellType = Type.GetTypeFromProgID("WScript.Shell") ?? throw new COMException("WScript.Shell is not available");
            dynamic shell = Activator.CreateInstance(shellType)!;
            try
            {
                dynamic link = shell.CreateShortcut(ShortcutPath);
                link.TargetPath = muntinExe;
                link.IconLocation = muntinExe + ",0";
                link.Save();
                Marshal.FinalReleaseComObject(link);
            }
            finally
            {
                Marshal.FinalReleaseComObject(shell);
            }

            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or COMException or Microsoft.CSharp.RuntimeBinder.RuntimeBinderException)
        {
            error = $"The startup shortcut couldn't be changed: {e.Message}";
            return false;
        }
    }
}
