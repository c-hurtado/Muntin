using System.Text.Json.Nodes;
using Muntin.Settings;
using Xunit;

namespace Muntin.Settings.Tests;

public sealed class SettingsStoreTests : IDisposable
{
    private readonly string _folder = Path.Combine(Path.GetTempPath(), "MuntinSettingsTests", Guid.NewGuid().ToString("N"));

    public SettingsStoreTests() => Directory.CreateDirectory(_folder);

    public void Dispose() => Directory.Delete(_folder, recursive: true);

    private string FilePath => Path.Combine(_folder, "settings.json");

    [Fact]
    public void SaveKeepsOtherKeys()
    {
        File.WriteAllText(FilePath, """{"properties":{"fancyzones_stageMonitors":{"value":{"MON1":{"pairCut":420}}},"fancyzones_dockGap":{"value":4},"unknown_key":{"value":"x"}},"name":"FancyZones","version":"1.0","extra":7}""");
        var store = new SettingsStore(FilePath);

        Assert.True(store.SaveBool(SettingsSchema.DockAnimation, false));

        var root = JsonNode.Parse(File.ReadAllText(FilePath))!.AsObject();
        var properties = root["properties"]!.AsObject();
        Assert.Equal(420, properties["fancyzones_stageMonitors"]!["value"]!["MON1"]!["pairCut"]!.GetValue<int>());
        Assert.Equal(4, properties["fancyzones_dockGap"]!["value"]!.GetValue<int>());
        Assert.Equal("x", properties["unknown_key"]!["value"]!.GetValue<string>());
        Assert.False(properties["fancyzones_dockAnimation"]!["value"]!.GetValue<bool>());
        Assert.Equal(7, root["extra"]!.GetValue<int>());
        Assert.Equal("FancyZones", root["name"]!.GetValue<string>());
        Assert.Empty(Directory.GetFiles(_folder, "*.tmp"));
    }

    // Muntin's FileWatcher only hears last-write changes; a replace alone raises none
    [Fact]
    public void SaveRaisesALastWriteChange()
    {
        File.WriteAllText(FilePath, """{"properties":{}}""");
        File.SetLastWriteTimeUtc(FilePath, DateTime.UtcNow.AddMinutes(-5));
        var before = File.GetLastWriteTimeUtc(FilePath);
        using var changed = new ManualResetEventSlim();
        using var watcher = new FileSystemWatcher(_folder, "settings.json") { NotifyFilter = NotifyFilters.LastWrite };
        watcher.Changed += (_, _) => changed.Set();
        watcher.EnableRaisingEvents = true;

        Assert.True(new SettingsStore(FilePath).SaveInt(SettingsSchema.DockGap, 3));

        Assert.True(changed.Wait(TimeSpan.FromSeconds(3)), "No last-write change was raised for settings.json");
        Assert.True(File.GetLastWriteTimeUtc(FilePath) > before);
    }

    [Fact]
    public void SaveCreatesMissingFile()
    {
        var store = new SettingsStore(Path.Combine(_folder, "sub", "settings.json"));
        Assert.Equal(LoadStatus.Missing, store.Load().Status);

        Assert.True(store.SaveString(SettingsSchema.StageLayout, "pair"));

        var load = store.Load();
        Assert.Equal(LoadStatus.Ok, load.Status);
        Assert.Equal("pair", SettingsStore.ReadString(load.Properties, SettingsSchema.StageLayout).Value);
    }

    [Fact]
    public void SaveModeWritesBothKeysInOneWrite()
    {
        File.WriteAllText(FilePath, """{"properties":{"fancyzones_arrangeMode":{"value":0},"fancyzones_stageLayout":{"value":"master"},"fancyzones_dockGap":{"value":4}}}""");
        var store = new SettingsStore(FilePath);

        Assert.True(store.SaveMode(SettingsSchema.Modes[3])); // Pair with strip

        var properties = store.Load().Properties;
        Assert.Equal(1, SettingsStore.ReadInt(properties, SettingsSchema.ArrangeMode).Value);
        Assert.Equal("pair", SettingsStore.ReadString(properties, SettingsSchema.StageLayout).Value);
        Assert.Equal(4, SettingsStore.ReadInt(properties, SettingsSchema.DockGap).Value);
        Assert.Equal(3, SettingsSchema.CurrentMode(1, "pair"));

        // Dock writes the mode only: the layout stays for the next time Stage is chosen
        Assert.True(store.SaveMode(SettingsSchema.Modes[0]));
        properties = store.Load().Properties;
        Assert.Equal(0, SettingsStore.ReadInt(properties, SettingsSchema.ArrangeMode).Value);
        Assert.Equal("pair", SettingsStore.ReadString(properties, SettingsSchema.StageLayout).Value);
        Assert.Empty(Directory.GetFiles(_folder, "*.tmp"));
    }

    [Fact]
    public void SaveModeNeverOverwritesAnUnreadableFile()
    {
        const string broken = """{"properties":{"fancyzones_dockGap":{"value":4},""";
        File.WriteAllText(FilePath, broken);
        Assert.False(new SettingsStore(FilePath).SaveMode(SettingsSchema.Modes[1]));
        Assert.Equal(broken, File.ReadAllText(FilePath));
    }

    [Fact]
    public void SaveClampsInts()
    {
        var store = new SettingsStore(FilePath);
        Assert.True(store.SaveInt(SettingsSchema.StageStripRadius, 99));
        Assert.True(store.SaveInt(SettingsSchema.DesktopScrollPages, 0));

        var properties = store.Load().Properties;
        Assert.Equal(40, SettingsStore.ReadInt(properties, SettingsSchema.StageStripRadius).Value);
        Assert.Equal(1, SettingsStore.ReadInt(properties, SettingsSchema.DesktopScrollPages).Value);
    }

    [Fact]
    public void UnreadableFileIsNeverOverwritten()
    {
        const string broken = """{"properties":{"fancyzones_dockGap":{"value":4},""";
        File.WriteAllText(FilePath, broken);
        var store = new SettingsStore(FilePath);

        Assert.Equal(LoadStatus.Unreadable, store.Load().Status);
        Assert.False(store.SaveBool(SettingsSchema.DockAnimation, true));
        Assert.Equal(broken, File.ReadAllText(FilePath));
    }

    [Fact]
    public void PropertiesThatAreNotAnObjectAreNotReplaced()
    {
        const string odd = """{"properties":[1,2]}""";
        File.WriteAllText(FilePath, odd);
        var store = new SettingsStore(FilePath);

        Assert.False(store.SaveInt(SettingsSchema.DockGap, 3));
        Assert.Equal(odd, File.ReadAllText(FilePath));
    }

    [Fact]
    public void OddValuesAreFlaggedNotRewritten()
    {
        const string text = """{"properties":{"fancyzones_stageStripRadius":{"value":77},"fancyzones_dockAnimation":{"value":"yes"},"fancyzones_stageLayout":{"value":"spiral"}}}""";
        File.WriteAllText(FilePath, text);
        var store = new SettingsStore(FilePath);
        var properties = store.Load().Properties;

        var radius = SettingsStore.ReadInt(properties, SettingsSchema.StageStripRadius);
        Assert.True(radius.Odd);
        Assert.Equal(40, radius.Value);

        var animation = SettingsStore.ReadBool(properties, SettingsSchema.DockAnimation);
        Assert.True(animation.Odd);
        Assert.True(animation.Value); // default

        var layout = SettingsStore.ReadString(properties, SettingsSchema.StageLayout);
        Assert.False(SettingsSchema.IsKnownStageLayout(layout.Value));

        var pages = SettingsStore.ReadInt(new JsonObject { ["fancyzones_desktopScrollPages"] = new JsonObject { ["value"] = 0 } }, SettingsSchema.DesktopScrollPages);
        Assert.True(pages.Odd);
        Assert.Equal(SettingsSchema.MaxDesktopScrollPages, pages.Value); // Settings.cpp: 0 meant no limit

        var missing = SettingsStore.ReadInt(properties, SettingsSchema.DockGap);
        Assert.False(missing.Present);
        Assert.False(missing.Odd);
        Assert.Equal(8, missing.Value);

        Assert.Equal(text, File.ReadAllText(FilePath)); // Loading never writes
    }
}

public sealed class ReadValueTests
{
    // SettingsView keeps a local copy with values made in code after each save, not parsed from text
    [Fact]
    public void IntsMadeInCodeReadBack()
    {
        var properties = new JsonObject { ["fancyzones_stageStripRadius"] = new JsonObject { ["value"] = JsonValue.Create(20) } };
        var read = SettingsStore.ReadInt(properties, SettingsSchema.StageStripRadius);
        Assert.False(read.Odd);
        Assert.Equal(20, read.Value);
    }
}

public sealed class SettingsSchemaTests
{
    [Fact]
    public void KeysAreUniqueAndPrefixed()
    {
        var keys = SettingsSchema.All.Select(s => s.Key).ToList();
        Assert.Equal(keys.Count, keys.Distinct().Count());
        Assert.All(keys, k => Assert.StartsWith("fancyzones_", k));
        Assert.DoesNotContain(SettingsSchema.StageMonitorsKey, keys);
    }

    [Fact]
    public void IntDefaultsAndChoicesFitTheirRanges()
    {
        foreach (var def in SettingsSchema.All.Where(s => s.Kind == SettingKind.Int))
        {
            Assert.InRange((int)def.Default, def.Min, def.Max);
        }

        void Fits(SettingDef def, Choice[] choices) => Assert.All(choices, c => Assert.InRange(c.Value, def.Min, def.Max));
        Fits(SettingsSchema.DockMotionCurve, SettingsSchema.MotionChoices);
        Fits(SettingsSchema.StagePromoteStyle, SettingsSchema.PromoteChoices);
        Fits(SettingsSchema.StageMoveStyle, SettingsSchema.MoveChoices);
        Fits(SettingsSchema.StageStripRadius, SettingsSchema.RadiusChoices);
        Assert.All(SettingsSchema.Modes, m => Assert.InRange(m.ArrangeMode, SettingsSchema.ArrangeMode.Min, SettingsSchema.ArrangeMode.Max));
        Fits(SettingsSchema.DesktopScrollPages, SettingsSchema.PageChoices);
        Assert.All(SettingsSchema.Spacings, s => Assert.InRange(s.SliderMax, s.Setting.Min, s.Setting.Max));
        Assert.All(SettingsSchema.Modes.Where(m => m.ArrangeMode == 1), m => Assert.True(SettingsSchema.IsKnownStageLayout(m.StageLayout)));
    }

    [Fact]
    public void ModesAreFlatAndCoverEveryLayout()
    {
        Assert.Equal(["Dock (drag to dock)", "Master and stack", "Stage with strip", "Pair with strip"], SettingsSchema.Modes.Select(m => m.Label));
        Assert.Null(SettingsSchema.Modes[0].StageLayout);
        Assert.Equal(0, SettingsSchema.Modes[0].ArrangeMode);
        Assert.Equal(["master", "stage", "pair"], SettingsSchema.Modes.Skip(1).Select(m => m.StageLayout!));

        Assert.Equal(0, SettingsSchema.CurrentMode(0, "pair")); // Dock whatever the layout
        Assert.Equal(1, SettingsSchema.CurrentMode(1, "master"));
        Assert.Equal(2, SettingsSchema.CurrentMode(1, "stage"));
        Assert.Equal(-1, SettingsSchema.CurrentMode(1, "spiral")); // Muntin ignores it and keeps the layout it had: not in the file
    }

    // Settings.cpp ArrangeFallback: Stage only when there is no settings.json at all
    [Fact]
    public void ModeOfAnExistingFileWithoutTheKeysIsDockOrMaster()
    {
        Assert.Equal(0, SettingsStore.ReadMode(null, fileExists: true).Index); // Unreadable: Dock
        Assert.Equal(0, SettingsStore.ReadMode(new JsonObject(), fileExists: true).Index); // No keys: Dock

        var stageOnly = new JsonObject { ["fancyzones_arrangeMode"] = new JsonObject { ["value"] = 1 } };
        Assert.Equal("Master and stack", SettingsSchema.Modes[SettingsStore.ReadMode(stageOnly, fileExists: true).Index].Label);

        var unknown = new JsonObject
        {
            ["fancyzones_arrangeMode"] = new JsonObject { ["value"] = 1 },
            ["fancyzones_stageLayout"] = new JsonObject { ["value"] = "spiral" },
        };
        var (index, note) = SettingsStore.ReadMode(unknown, fileExists: true);
        Assert.Equal(-1, index);
        Assert.Contains("keeps the layout it had", note);
    }

    [Fact]
    public void FreshInstallIsStageWithStripAndCrossfade()
    {
        var empty = new JsonObject();
        var (mode, note) = SettingsStore.ReadMode(null, fileExists: false);
        Assert.Equal("Stage with strip", SettingsSchema.Modes[mode].Label);
        Assert.Null(note);
        Assert.Equal(1, SettingsStore.ReadInt(empty, SettingsSchema.StagePromoteStyle).Value);
        Assert.Equal("Crossfade", SettingsSchema.PromoteChoices.Single(c => c.Value == (int)SettingsSchema.StagePromoteStyle.Default).Label);
    }

    [Fact]
    public void TogglesMatchTheTrayOrder()
    {
        Assert.Equal(10, SettingsSchema.Toggles.Length);
        Assert.Equal("fancyzones_shiftDrag", SettingsSchema.Toggles[0].Key);
        Assert.Equal("fancyzones_desktopScrollOverview", SettingsSchema.Toggles[^1].Key);
    }
}
