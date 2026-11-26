using System.Text.RegularExpressions;
using Muntin.Settings;
using Xunit;

namespace Muntin.Settings.Tests;

/// <summary>SettingsSchema.cs against the native sources it mirrors: SettingsConstants.h, Settings.h, Tray.cpp (arrange modes, Animations).</summary>
public sealed partial class NativeParityTests
{
    private static readonly string Lib = FindLib();

    private static string FindLib()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var lib = Path.Combine(dir.FullName, "FancyZonesLib");
            if (File.Exists(Path.Combine(lib, "SettingsConstants.h")))
            {
                return lib;
            }
        }

        throw new DirectoryNotFoundException("FancyZonesLib not found above the test folder");
    }

    private static string Read(string file) => File.ReadAllText(Path.Combine(Lib, file));

    private static string Member(SettingDef def) => def.Key["fancyzones_".Length..];

    [GeneratedRegex("""const wchar_t \w+\[\] = L"(fancyzones_\w+)";""")]
    private static partial Regex ConstantId();

    [GeneratedRegex("""^\s*(bool|int|std::wstring) (\w+) = (.+?);""", RegexOptions.Multiline)]
    private static partial Regex Default();

    // Tray.cpp Modes[]: { L"label", arrangeMode, nullptr | L"layout" }
    [GeneratedRegex("""\{ L"([^"]+)", (\d), (?:nullptr|L"(\w+)") \}""")]
    private static partial Regex TrayMode();

    [Fact]
    public void EveryKeyIsDefinedInSettingsConstants()
    {
        var ids = ConstantId().Matches(Read("SettingsConstants.h")).Select(m => m.Groups[1].Value).ToHashSet();
        Assert.All(SettingsSchema.All.Select(s => s.Key).Append(SettingsSchema.StageMonitorsKey), key => Assert.Contains(key, ids));
    }

    [Fact]
    public void DefaultsMatchSettingsH()
    {
        var text = Read("Settings.h");
        var defaults = Default().Matches(text).ToDictionary(m => m.Groups[2].Value, m => m.Groups[3].Value.Trim());
        foreach (var def in SettingsSchema.All)
        {
            Assert.True(defaults.TryGetValue(Member(def), out var native), $"{Member(def)} is not in Settings.h");
            var expected = def.Kind switch
            {
                SettingKind.Bool => (bool)def.Default ? "true" : "false",
                SettingKind.Int => ((int)def.Default).ToString(System.Globalization.CultureInfo.InvariantCulture),
                _ => $"L\"{def.Default}\"",
            };
            Assert.True(expected == native, $"{def.Key}: schema {expected}, Settings.h {native}");
        }

        Assert.Contains($"constexpr int MaxDesktopScrollPages = {SettingsSchema.MaxDesktopScrollPages};", text);
    }

    [Fact]
    public void ModesAndAnimationsMatchTray()
    {
        var tray = Read("Tray.cpp");
        var modes = TrayMode().Matches(tray).Select(m => new ModeDef(m.Groups[1].Value, int.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture), m.Groups[3].Success ? m.Groups[3].Value : null)).ToArray();
        Assert.Equal(SettingsSchema.Modes, modes);

        Assert.Contains($"AnimationsLabel[] = L\"{SettingsSchema.DockAnimation.Label}\"", tray);
        Assert.Contains("NonLocalizable::DockAnimationID", tray);
        Assert.Contains($"HelpUrl[] = L\"{SettingsSchema.HelpUrl}\"", tray);
    }

    // The settings that left the tray: none of their keys is written from there any more (the app is their only editor besides the file)
    [Fact]
    public void TrayWritesOnlyTheModeAndAnimations()
    {
        var tray = Read("Tray.cpp");
        var constants = Read("SettingsConstants.h");
        var written = Regex.Matches(tray, @"NonLocalizable::(\w+ID)").Select(m => m.Groups[1].Value).Distinct().Order().ToArray();
        Assert.Equal(["ArrangeModeID", "DockAnimationID", "StageLayoutID"], written);
        Assert.All(written, id => Assert.Contains($"const wchar_t {id}[]", constants));
    }
}
