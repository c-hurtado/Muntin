using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Muntin.Settings;

public enum LoadStatus
{
    Ok,
    Missing,
    Unreadable,
}

public sealed record LoadResult(LoadStatus Status, JsonObject? Properties, string? Error = null);

/// <summary>A value read from settings.json: Raw is what the file holds (null when absent), Odd when it can't be used as is.</summary>
public sealed record ReadValue<T>(T Value, JsonNode? Raw, bool Odd)
{
    public bool Present => Raw is not null;
}

/// <summary>
/// Reads and writes %LOCALAPPDATA%\Muntin\settings.json one key at a time, like FancyZonesSettings::SaveValue (Settings.cpp):
/// read the file, replace one property, keep every other key, write. Muntin's FileWatcher hot-reloads the file.
/// </summary>
public sealed class SettingsStore
{
    private static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = false, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }; // Compact, like json::to_file

    public SettingsStore(string path)
    {
        Path = path;
    }

    public string Path { get; }

    public static string DefaultPath =>
        System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Muntin", "settings.json");

    public LoadResult Load()
    {
        string text;
        try
        {
            if (!File.Exists(Path))
            {
                return new LoadResult(LoadStatus.Missing, null);
            }

            text = ReadAllTextShared(Path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return new LoadResult(LoadStatus.Unreadable, null, e.Message);
        }

        try
        {
            if (JsonNode.Parse(text) is not JsonObject root)
            {
                return new LoadResult(LoadStatus.Unreadable, null, "The file is not a JSON object.");
            }

            if (!root.ContainsKey("properties"))
            {
                return new LoadResult(LoadStatus.Ok, new JsonObject());
            }

            return root["properties"] is JsonObject properties
                ? new LoadResult(LoadStatus.Ok, properties)
                : new LoadResult(LoadStatus.Unreadable, null, "\"properties\" is not an object.");
        }
        catch (JsonException e)
        {
            return new LoadResult(LoadStatus.Unreadable, null, e.Message);
        }
    }

    public static JsonNode? RawValue(JsonObject? properties, string key) =>
        properties?[key] is JsonObject property ? property["value"] : null;

    public static ReadValue<bool> ReadBool(JsonObject? properties, SettingDef def)
    {
        var raw = RawValue(properties, def.Key);
        if (raw is JsonValue v && v.TryGetValue(out bool b))
        {
            return new(b, raw, false);
        }

        return new((bool)def.Default, raw, raw is not null);
    }

    public static ReadValue<int> ReadInt(JsonObject? properties, SettingDef def)
    {
        var raw = RawValue(properties, def.Key);
        // Through the text: TryGetValue<double> fails on a value made in code from an int (JsonValue.Create(4))
        if (raw is JsonValue v && v.GetValueKind() == JsonValueKind.Number && double.TryParse(v.ToJsonString(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var d))
        {
            if (d >= def.Min && d <= def.Max && d == Math.Floor(d))
            {
                return new((int)d, raw, false);
            }

            // Settings.cpp: 0 or less pages meant "no limit" before, Muntin reads it as the most pages
            if (def.Key == SettingsSchema.DesktopScrollPages.Key && d <= 0)
            {
                return new(SettingsSchema.MaxDesktopScrollPages, raw, true);
            }

            // Muntin clamps it on load: show the clamped value, flag the raw one
            var clamped = double.IsNaN(d) ? (int)def.Default : (int)Math.Clamp(Math.Floor(d), def.Min, def.Max);
            return new(clamped, raw, true);
        }

        return new((int)def.Default, raw, raw is not null);
    }

    public static ReadValue<string> ReadString(JsonObject? properties, SettingDef def)
    {
        var raw = RawValue(properties, def.Key);
        if (raw is JsonValue v && v.TryGetValue(out string? s) && s is not null)
        {
            return new(s, raw, false);
        }

        return new((string)def.Default, raw, raw is not null);
    }

    public bool SaveBool(SettingDef def, bool value) => SaveValue(def.Key, JsonValue.Create(value));

    public bool SaveInt(SettingDef def, int value) => SaveValue(def.Key, JsonValue.Create(def.Clamp(value)));

    public bool SaveString(SettingDef def, string value) => SaveValue(def.Key, JsonValue.Create(value));

    /// <summary>
    /// Replaces one property and keeps everything else. False when the file exists but can't be parsed (it is never
    /// overwritten then: the other settings would be lost) or can't be written.
    /// </summary>
    public bool SaveValue(string key, JsonNode? value)
    {
        JsonObject root;
        try
        {
            if (File.Exists(Path))
            {
                if (JsonNode.Parse(ReadAllTextShared(Path)) is not JsonObject parsed)
                {
                    return false;
                }

                root = parsed;
            }
            else
            {
                root = new JsonObject();
            }
        }
        catch (Exception e) when (e is JsonException or IOException or UnauthorizedAccessException)
        {
            return false;
        }

        if (!root.ContainsKey("properties"))
        {
            root["properties"] = new JsonObject();
        }
        else if (root["properties"] is not JsonObject)
        {
            return false;
        }

        var properties = (JsonObject)root["properties"]!;
        properties[key] = new JsonObject { ["value"] = value?.DeepClone() };
        root["name"] = SettingsSchema.ModuleName;
        root["version"] = SettingsSchema.FileVersion;

        return WriteAtomic(root.ToJsonString(WriteOptions));
    }

    private bool WriteAtomic(string text)
    {
        var folder = System.IO.Path.GetDirectoryName(Path)!;
        for (var attempt = 0; attempt < 5; attempt++)
        {
            var temp = System.IO.Path.Combine(folder, $"settings.json.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp");
            try
            {
                Directory.CreateDirectory(folder);
                File.WriteAllText(temp, text, new UTF8Encoding(false));
                if (File.Exists(Path))
                {
                    File.Replace(temp, Path, null, ignoreMetadataErrors: true);
                }
                else
                {
                    File.Move(temp, Path);
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                TryDelete(temp);
                Thread.Sleep(40 * (attempt + 1)); // Muntin may be reading the file
                continue;
            }

            TouchLastWrite();
            return true;
        }

        return false;
    }

    // Muntin's FileWatcher (common\SettingsAPI\FileWatcher.cpp) only listens for last-write changes, and a replace or move
    // raises none. The file is already in place, so a failure here retries the stamp only, never the swap.
    private void TouchLastWrite()
    {
        for (var attempt = 0; attempt < 5; attempt++)
        {
            try
            {
                File.SetLastWriteTimeUtc(Path, DateTime.UtcNow);
                return;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                Thread.Sleep(20 * (attempt + 1));
            }
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
        }
    }

    private static string ReadAllTextShared(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        return reader.ReadToEnd();
    }
}
