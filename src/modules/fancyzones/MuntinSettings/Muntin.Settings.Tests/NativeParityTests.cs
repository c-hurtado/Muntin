using System.Text.RegularExpressions;
using Muntin.Settings;
using Xunit;

namespace Muntin.Settings.Tests;

/// <summary>SettingsSchema.cs against the native sources it mirrors: SettingsConstants.h, Settings.h, Tray.cpp.</summary>
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

    [GeneratedRegex("""\{ NonLocalizable::\w+, L"([^"]+)", &Settings::(\w+)""")]
    private static partial Regex TrayRow();

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
    public void TogglesAndSpacingsMatchTray()
    {
        var tray = Read("Tray.cpp");
        var labels = TrayRow().Matches(tray).ToDictionary(m => m.Groups[2].Value, m => m.Groups[1].Value);
        foreach (var toggle in SettingsSchema.Toggles)
        {
            Assert.True(labels.TryGetValue(Member(toggle), out var label), $"{Member(toggle)} is not a tray toggle");
            Assert.Equal(label, toggle.Label);
        }

        foreach (var spacing in SettingsSchema.Spacings)
        {
            Assert.Contains($"L\"{spacing.Setting.Label}\", &Settings::{Member(spacing.Setting)}, {spacing.SliderMax} }}", tray);
        }

        Assert.Contains($"HelpUrl[] = L\"{SettingsSchema.HelpUrl}\"", tray);
    }
}
