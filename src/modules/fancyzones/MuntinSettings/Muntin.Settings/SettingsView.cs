using System.Diagnostics;
using System.Text.Json.Nodes;
using CommunityToolkit.WinUI.Controls;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Rectangle = Microsoft.UI.Xaml.Shapes.Rectangle;
using S = Muntin.Settings.SettingsSchema;

namespace Muntin.Settings.App;

/// <summary>
/// Builds the sections from SettingsSchema and keeps them in step with settings.json. Every change writes one key at once
/// (the arrange mode: its two keys in one write; Muntin hot-reloads the file); loading never writes, so odd values in the file
/// stay until the user changes them.
/// </summary>
internal sealed class SettingsView
{
    private readonly SettingsStore _store;
    private readonly DispatcherQueue _dispatcher;
    private readonly string? _muntinExe;
    private readonly Dictionary<string, FrameworkElement> _sections = [];
    private readonly List<Action> _refreshers = [];
    private JsonObject? _properties;
    private bool _fileExists = true;
    private bool _updating;
    private bool _editable = true;

    public SettingsView(SettingsStore store, DispatcherQueue dispatcher, string? muntinExe)
    {
        _store = store;
        _dispatcher = dispatcher;
        _muntinExe = muntinExe ?? StartupShortcut.FindMuntinExe();
    }

    public event Action<string>? SaveFailed;

    public void SetEditable(bool editable) => _editable = editable;

    // fileExists: settings.json is there (readable or not); a missing arrange key means something else without it
    public void Refresh(JsonObject? properties, bool fileExists)
    {
        _properties = properties?.DeepClone().AsObject();
        _fileExists = fileExists;
        RefreshControls();
    }

    public FrameworkElement Section(string tag)
    {
        if (!_sections.TryGetValue(tag, out var section))
        {
            section = tag switch
            {
                "Arrange" => BuildArrange(),
                "Stage" => BuildStage(),
                "Dock" => BuildDock(),
                "Animations" => BuildAnimations(),
                "Spacing" => BuildSpacing(),
                _ => BuildStartup(),
            };
            _sections[tag] = section;
            RefreshControls();
        }

        return section;
    }

    private bool Animations => SettingsStore.ReadBool(_properties, S.DockAnimation).Value;

    private void RefreshControls()
    {
        _updating = true;
        try
        {
            foreach (var refresh in _refreshers)
            {
                refresh();
            }
        }
        finally
        {
            _updating = false;
        }
    }

    // Writes one key, then updates the local copy so dependent controls follow before the file watcher reports back
    private void Save(SettingDef def, JsonNode value)
    {
        if (_updating)
        {
            return;
        }

        if (!_editable)
        {
            _dispatcher.TryEnqueue(RefreshControls); // The keyboard still reaches the controls: show the value in effect again
            return;
        }

        var saved = def.Kind switch
        {
            SettingKind.Bool => _store.SaveBool(def, value.GetValue<bool>()),
            SettingKind.Int => _store.SaveInt(def, value.GetValue<int>()),
            _ => _store.SaveString(def, value.GetValue<string>()),
        };

        if (!saved)
        {
            SaveFailed?.Invoke($"{def.Label} wasn't saved: settings.json can't be read or written.");
            _dispatcher.TryEnqueue(RefreshControls); // Back to the value that is really in effect
            return;
        }

        _properties ??= [];
        var stored = def.Kind == SettingKind.Int ? JsonValue.Create(def.Clamp(value.GetValue<int>())) : value.DeepClone();
        _properties[def.Key] = new JsonObject { ["value"] = stored };
        _dispatcher.TryEnqueue(RefreshControls);
    }

    // The arrange mode: both keys in one write, then the local copy, as Save does
    private void SaveMode(ModeDef mode)
    {
        if (_updating)
        {
            return;
        }

        if (!_editable)
        {
            _dispatcher.TryEnqueue(RefreshControls); // The keyboard still reaches the controls: show the value in effect again
            return;
        }

        if (!_store.SaveMode(mode))
        {
            SaveFailed?.Invoke($"{mode.Label} wasn't saved: settings.json can't be read or written.");
            _dispatcher.TryEnqueue(RefreshControls);
            return;
        }

        _properties ??= [];
        if (mode.StageLayout is not null)
        {
            _properties[S.StageLayout.Key] = new JsonObject { ["value"] = mode.StageLayout };
        }

        _properties[S.ArrangeMode.Key] = new JsonObject { ["value"] = JsonValue.Create(mode.ArrangeMode) };
        _fileExists = true;
        _dispatcher.TryEnqueue(RefreshControls);
    }

    private static string OddNote(JsonNode? raw, object used) =>
        $"settings.json has {raw?.ToJsonString() ?? "nothing"} here; Muntin uses {used}. It stays until you change this.";

    private static string Join(string? description, string? extra) =>
        string.IsNullOrEmpty(extra) ? description ?? string.Empty : string.IsNullOrEmpty(description) ? extra : $"{description}\n{extra}";

    // ---- Controls

    private SettingsCard Toggle(SettingDef def, string? description = null, Func<bool>? enabled = null, string? disabledNote = null)
    {
        var toggle = new ToggleSwitch();
        var card = new SettingsCard { Header = def.Label, Content = toggle };
        toggle.Toggled += (_, _) => Save(def, JsonValue.Create(toggle.IsOn));
        _refreshers.Add(() =>
        {
            var read = SettingsStore.ReadBool(_properties, def);
            var on = enabled?.Invoke() ?? true;
            toggle.IsOn = read.Value;
            card.IsEnabled = on;
            card.Description = Join(description, read.Odd ? OddNote(read.Raw, read.Value ? "on" : "off") : !on ? disabledNote : null);
        });
        return card;
    }

    private SettingsCard Combo(SettingDef def, Choice[] choices, string? description = null, Func<bool>? enabled = null, string? disabledNote = null)
    {
        var combo = new ComboBox { MinWidth = 200 };
        foreach (var choice in choices)
        {
            combo.Items.Add(choice.Label);
        }

        var card = new SettingsCard { Header = def.Label, Content = combo };
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex >= 0)
            {
                Save(def, JsonValue.Create(choices[combo.SelectedIndex].Value));
            }
        };
        _refreshers.Add(() =>
        {
            var read = SettingsStore.ReadInt(_properties, def);
            var on = enabled?.Invoke() ?? true;
            combo.SelectedIndex = Array.FindIndex(choices, c => c.Value == read.Value);
            card.IsEnabled = on;
            card.Description = Join(description, read.Odd ? OddNote(read.Raw, read.Value) : !on ? disabledNote : null);
        });
        return card;
    }

    private SettingsCard Number(SettingDef def, string? description = null)
    {
        var box = new NumberBox
        {
            Minimum = def.Min,
            Maximum = def.Max,
            SmallChange = 1,
            LargeChange = 10,
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact,
            ValidationMode = NumberBoxValidationMode.InvalidInputOverwritten,
            MinWidth = 120,
        };
        var card = new SettingsCard { Header = def.Label, Content = box };
        box.ValueChanged += (_, args) =>
        {
            if (!double.IsNaN(args.NewValue))
            {
                Save(def, JsonValue.Create((int)Math.Round(args.NewValue)));
            }
        };
        _refreshers.Add(() =>
        {
            var read = SettingsStore.ReadInt(_properties, def);
            box.Value = read.Value;
            card.Description = Join(Join(description, $"{def.Min} to {def.Max}, default {def.Default}."), read.Odd ? OddNote(read.Raw, read.Value) : null);
        });
        return card;
    }

    // Saves a moment after the thumb stops, not on every step of a drag
    private SettingsCard Spacing(S.SpacingDef spacing)
    {
        var def = spacing.Setting;
        var slider = new Slider { Minimum = 0, Maximum = spacing.SliderMax, StepFrequency = 1, MinWidth = 260 };
        var card = new SettingsCard { Header = def.Label, Content = slider };
        var timer = _dispatcher.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(150);
        timer.IsRepeating = false;
        timer.Tick += (_, _) => Save(def, JsonValue.Create((int)slider.Value));
        slider.ValueChanged += (_, _) =>
        {
            if (!_updating)
            {
                timer.Stop();
                timer.Start();
            }
        };
        _refreshers.Add(() =>
        {
            if (timer.IsRunning)
            {
                return; // The user is still moving it
            }

            var read = SettingsStore.ReadInt(_properties, def);
            slider.Maximum = Math.Max(spacing.SliderMax, read.Value); // A bigger value set by hand shows as is
            slider.Value = read.Value;
            card.Description = Join(
                read.Value > spacing.SliderMax ? $"{read.Value} pixels, set in settings.json (the slider goes to {spacing.SliderMax})." : $"{read.Value} pixels",
                read.Odd ? OddNote(read.Raw, read.Value) : null);
        });
        return card;
    }

    private static SettingsExpander Advanced(string description, params SettingsCard[] cards)
    {
        var expander = new SettingsExpander
        {
            Header = "Advanced",
            Description = description,
            Margin = new Thickness(0, 20, 0, 0), // Apart from the group above it
            HeaderIcon = new FontIcon { Glyph = "" },
        };
        foreach (var card in cards)
        {
            expander.Items.Add(card);
        }

        return expander;
    }

    private static T Resource<T>(string key) => (T)Application.Current.Resources[key];

    // Brushes set in code keep the theme they were read in; parsed {ThemeResource} ones follow theme, contrast and accent changes
    private static T Themed<T>(string xaml) =>
        (T)XamlReader.Load(xaml.Replace("NS", "xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'", StringComparison.Ordinal));

    private static TextBlock SecondaryText() => Themed<TextBlock>(
        "<TextBlock NS Style='{StaticResource CaptionTextBlockStyle}' Foreground='{ThemeResource TextFillColorSecondaryBrush}' TextWrapping='WrapWholeWords' />");

    // A card's leading icon (Segoe Fluent Icons), as in Windows Settings
    private static T Icon<T>(T card, string glyph)
        where T : SettingsCard
    {
        card.HeaderIcon = new FontIcon { Glyph = glyph };
        return card;
    }

    // A heading over a run of related cards
    private static TextBlock Group(string title) =>
        new() { Text = title, Style = Resource<Style>("BodyStrongTextBlockStyle"), Margin = new Thickness(1, 24, 0, 4) };

    private static StackPanel Page(string title, params UIElement[] items)
    {
        var page = new StackPanel { Spacing = 4 };
        page.Children.Add(new TextBlock { Text = title, Style = Resource<Style>("TitleTextBlockStyle"), Margin = new Thickness(0, 0, 0, 12) });
        foreach (var item in items)
        {
            page.Children.Add(item);
        }

        // A heading right under the title needs no extra space above it
        if (items.Length > 0 && items[0] is TextBlock first)
        {
            first.Margin = new Thickness(1, 0, 0, 4);
        }

        return page;
    }

    private static SettingsCard Note(string header, string text) => new() { Header = header, Description = text };

    // ---- Sections

    // One line under each mode's picture (S.Modes order; the labels themselves match the tray)
    private static readonly string[] ModeBlurbs =
    [
        "Drag windows against the screen edges and each other.",
        "One large window, the others stacked beside it.",
        "One window in front, the others waiting in a strip.",
        "Two windows side by side, the others in a strip.",
    ];

    // Each mode's picture on a 156 x 96 screen: (x, y, width, height, a main window), the strip on the right as in Muntin
    private static readonly (double X, double Y, double W, double H, bool Main)[][] ModeShapes =
    [
        [(6, 6, 70, 84, true), (80, 6, 70, 40, false), (80, 50, 70, 40, false)],
        [(6, 6, 88, 84, true), (98, 6, 52, 26, false), (98, 35, 52, 26, false), (98, 64, 52, 26, false)],
        [(6, 6, 114, 84, true), (124, 6, 26, 26, false), (124, 35, 26, 26, false), (124, 64, 26, 26, false)],
        [(6, 6, 55, 84, true), (65, 6, 55, 84, true), (124, 6, 26, 26, false), (124, 35, 26, 26, false), (124, 64, 26, 26, false)],
    ];

    // mains: gets the tile's main windows, which light up only on the selected mode
    private static FrameworkElement ModeTile(ModeDef mode, string blurb, (double X, double Y, double W, double H, bool Main)[] shapes, List<UIElement> mains)
    {
        var screen = new Canvas { Width = 156, Height = 96 };
        foreach (var (x, y, w, h, main) in shapes)
        {
            var window = Themed<Rectangle>($"<Rectangle NS RadiusX='3' RadiusY='3' Fill='{{ThemeResource {(main ? "AccentFillColorDefaultBrush" : "ControlStrongFillColorDisabledBrush")}}}' />");
            window.Width = w;
            window.Height = h;
            Canvas.SetLeft(window, x);
            Canvas.SetTop(window, y);
            screen.Children.Add(window);
            if (main)
            {
                mains.Add(window);
            }
        }

        var tile = new StackPanel { Width = 176, Spacing = 2, Padding = new Thickness(10, 10, 10, 12) };
        var frame = Themed<Border>("<Border NS HorizontalAlignment='Left' CornerRadius='6' BorderThickness='1' Margin='0,0,0,8' "
            + "BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}' Background='{ThemeResource ControlFillColorDefaultBrush}' />");
        frame.Child = screen;
        tile.Children.Add(frame);
        tile.Children.Add(new TextBlock { Text = mode.Label, Style = Resource<Style>("BodyStrongTextBlockStyle") });
        var text = SecondaryText();
        text.Text = blurb;
        tile.Children.Add(text);
        AutomationProperties.SetName(tile, mode.Label);
        AutomationProperties.SetHelpText(tile, blurb);
        return tile;
    }

    // One choice for the mode and the Stage layout, as in the tray: Dock, or Stage with one of its three layouts
    private FrameworkElement BuildArrange()
    {
        // Tiles whose text wraps to fewer lines stay top-aligned with the others
        var container = new Style(typeof(GridViewItem));
        container.Setters.Add(new Setter(Control.VerticalContentAlignmentProperty, VerticalAlignment.Top));
        var modes = new GridView { SelectionMode = ListViewSelectionMode.Single, Margin = new Thickness(-4, 0, 0, 0), ItemContainerStyle = container };
        AutomationProperties.SetName(modes, "Arrange mode");
        var mains = new List<UIElement>[S.Modes.Length];
        for (var i = 0; i < S.Modes.Length; i++)
        {
            mains[i] = [];
            modes.Items.Add(ModeTile(S.Modes[i], ModeBlurbs[i], ModeShapes[i], mains[i]));
        }

        void Light(int selected)
        {
            for (var i = 0; i < mains.Length; i++)
            {
                foreach (var window in mains[i])
                {
                    window.Opacity = i == selected ? 1 : 0.35;
                }
            }
        }

        var note = SecondaryText();
        note.Margin = new Thickness(1, 8, 0, 0);

        // The list can report a selection after the refresh is over (when it loads): only a different mode is written
        modes.SelectionChanged += (_, _) =>
        {
            var index = modes.SelectedIndex;
            Light(index);
            if (index >= 0 && index != SettingsStore.ReadMode(_properties, _fileExists).Index)
            {
                SaveMode(S.Modes[index]);
            }
        };
        _refreshers.Add(() =>
        {
            var (index, odd) = SettingsStore.ReadMode(_properties, _fileExists);
            modes.SelectedIndex = index; // -1: none, the layout Muntin keeps isn't in the file
            Light(index);
            note.Text = Join("Win+Shift+L cycles the Stage layouts.", odd);
        });

        return Page("Arrange", Group("How windows are arranged"), modes, note);
    }

    private FrameworkElement BuildStage()
    {
        const string needsAnimations = "Turn on Animations first.";
        var promote = Toggle(S.StageAnimatePromote, "A window promoted out of the strip glides to its place.", () => Animations, needsAnimations);
        var promoteStyle = Combo(S.StagePromoteStyle, S.PromoteChoices, null,
            () => Animations && SettingsStore.ReadBool(_properties, S.StageAnimatePromote).Value, "Turn on Animations and Animate promoting first.");
        var move = Combo(S.StageMoveStyle, S.MoveChoices, "How a window moves between slots.", () => Animations, needsAnimations);

        // Strip corner radius: the tray's choices plus any whole number in Muntin's range
        var def = S.StageStripRadius;
        var radiusCombo = new ComboBox { MinWidth = 140, PlaceholderText = "Custom" };
        foreach (var choice in S.RadiusChoices)
        {
            radiusCombo.Items.Add($"{choice.Label} ({choice.Value})");
        }

        var radiusBox = new NumberBox
        {
            Minimum = def.Min,
            Maximum = def.Max,
            SmallChange = 1,
            SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact,
            ValidationMode = NumberBoxValidationMode.InvalidInputOverwritten,
            MinWidth = 100,
        };
        var radius = new SettingsCard
        {
            Header = def.Label,
            Content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { radiusCombo, radiusBox } },
        };
        radiusCombo.SelectionChanged += (_, _) =>
        {
            if (radiusCombo.SelectedIndex >= 0)
            {
                Save(def, JsonValue.Create(S.RadiusChoices[radiusCombo.SelectedIndex].Value));
            }
        };
        radiusBox.ValueChanged += (_, args) =>
        {
            if (!double.IsNaN(args.NewValue))
            {
                Save(def, JsonValue.Create((int)Math.Round(args.NewValue)));
            }
        };
        _refreshers.Add(() =>
        {
            var read = SettingsStore.ReadInt(_properties, def);
            radiusCombo.SelectedIndex = Array.FindIndex(S.RadiusChoices, c => c.Value == read.Value);
            radiusBox.Value = read.Value;
            radius.Description = Join($"Corners of the strip tiles, {def.Min} to {def.Max} pixels at 100% scaling (larger on scaled monitors).", read.Odd ? OddNote(read.Raw, read.Value) : null);
        });

        var width = Note("Strip width", "Drag the inner edge of the strip to make it wider or narrower.");
        const string perMonitor = "A split dragged on a monitor is kept for that monitor and wins over this.";
        var advanced = Advanced("Default splits for Pair and Master", Number(S.StagePairCut, perMonitor), Number(S.StageMasterCut, perMonitor));

        return Page(
            "Stage",
            Group("Motion"),
            Icon(promote, ""),
            Icon(promoteStyle, ""),
            Icon(move, ""),
            Group("Strip"),
            Icon(radius, ""),
            Icon(width, ""),
            advanced);
    }

    private FrameworkElement BuildDock()
    {
        return Page(
            "Dock",
            Group("Docking"),
            Icon(Toggle(S.ShiftDrag), ""),
            Icon(Toggle(S.DockEdgeSnap), ""),
            Icon(Toggle(S.DockAutoTile), ""),
            Group("Keys and buttons"),
            Icon(Toggle(S.OverrideSnapHotkeys), ""),
            Icon(Toggle(S.DockCaptionButtons), ""),
            Icon(Toggle(S.DockMiddleClickExpand), ""),
            Group("Desktop"),
            Icon(Combo(S.DesktopScrollPages, S.PageChoices, "Pages each monitor scrolls through, the first included."), ""),
            Advanced(
                "Distances that start docking",
                Number(S.DockEdgeSnapBand, "Distance from the left or right screen edge within which a window docks."),
                Number(S.DockEdgeBand, "Distance from a work area edge within which a window docks against the whole work area."),
                Number(S.DockWindowEdgePercent, "Share of a docked window, from each edge, within which a window docks against it.")));
    }

    private FrameworkElement BuildAnimations()
    {
        const string needsAnimations = "Turn on Animations first.";
        return Page(
            "Animations",
            Icon(Toggle(S.DockAnimation, "Windows glide to their new places."), ""),
            Icon(Toggle(S.DockAnticipation, null, () => Animations, needsAnimations), ""),
            Icon(Combo(S.DockMotionCurve, S.MotionChoices, "Spring overshoots a little; Ease settles without overshoot."), ""), // Not greyed, like the tray
            Icon(Toggle(S.DesktopScrollOverview), ""));
    }

    private FrameworkElement BuildSpacing()
    {
        var gap = S.Spacings[0];
        var items = new List<UIElement> { Group("Between windows"), Icon(Spacing(gap), ""), Group("Screen edges") };
        foreach (var margin in S.Spacings.Skip(1))
        {
            items.Add(Icon(Spacing(margin), ""));
        }

        items.Add(Advanced("Exact values, beyond the slider's reach", [.. S.Spacings.Select(s => Number(s.Setting, "The exact value; settings.json allows more than the slider reaches."))]));
        return Page("Gap and margins", [.. items]);
    }

    private FrameworkElement BuildStartup()
    {
        var toggle = new ToggleSwitch();
        var signIn = new SettingsCard { Header = "Start at sign-in", Content = toggle };
        toggle.Toggled += (_, _) =>
        {
            if (_updating)
            {
                return;
            }

            if (!StartupShortcut.Set(toggle.IsOn, _muntinExe, out var error))
            {
                SaveFailed?.Invoke(error);
            }

            _dispatcher.TryEnqueue(RefreshControls);
        };
        _refreshers.Add(() =>
        {
            var exists = StartupShortcut.Exists();
            toggle.IsOn = exists;
            signIn.IsEnabled = exists || _muntinExe is not null; // Turning it off needs no Muntin.exe
            signIn.Description = _muntinExe is null
                ? "Muntin.exe wasn't found, so the shortcut can't be made from here. Use the tray menu."
                : "Adds Muntin.lnk to the Startup folder.";
        });

        var folder = new Button { Content = "Open" };
        folder.Click += (_, _) =>
        {
            var path = Path.GetDirectoryName(_store.Path)!;
            Directory.CreateDirectory(path);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{path}\"") { UseShellExecute = true });
        };

        var help = new HyperlinkButton { Content = "Open help", NavigateUri = new Uri(S.HelpUrl) };

        return Page(
            "Startup",
            Icon(signIn, ""),
            Group("Files and help"),
            Icon(new SettingsCard { Header = "Settings folder", Description = Path.GetDirectoryName(_store.Path) ?? _store.Path, Content = folder }, ""),
            Icon(new SettingsCard { Header = "Help", Description = S.HelpUrl, Content = help }, ""));
    }
}
