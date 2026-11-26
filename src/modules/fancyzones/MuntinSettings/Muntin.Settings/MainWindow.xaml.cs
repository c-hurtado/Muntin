using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

namespace Muntin.Settings.App;

public sealed partial class MainWindow : Microsoft.UI.Xaml.Window
{
    // In DIPs: the first size, and the smallest the window can be made (every page still lays out, with the pane as a menu button)
    private const int StartWidth = 980, StartHeight = 760, MinWidth = 400, MinHeight = 420;

    private readonly SettingsStore _store = new(SettingsStore.DefaultPath);
    private readonly SettingsView _view;
    private readonly FileSystemWatcher? _watcher;
    private readonly DispatcherQueueTimer _reloadTimer;

    public MainWindow(string? muntinExe)
    {
        InitializeComponent();
        Title = Program.WindowTitle;
        AppTitleBar.Title = Program.WindowTitle; // Program finds the running window by this title
        SystemBackdrop = new MicaBackdrop();

        // The XAML title bar replaces the system one; caption buttons stay the system's
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;

        // AppWindow sizes are physical pixels
        var scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new Windows.Graphics.SizeInt32(Px(StartWidth, scale), Px(StartHeight, scale)));
        ApplyMinimumSize(scale);

        // On another monitor's DPI the minimum is scaled again
        if (Content is FrameworkElement root)
        {
            root.Loaded += (_, _) => root.XamlRoot.Changed += (sender, _) => ApplyMinimumSize(sender.RasterizationScale);
        }

        // Muntin.exe's own icon (FancyZonesLib\icon.ico, copied next to the exe): title bar, taskbar and Alt+Tab
        var icon = Path.Combine(AppContext.BaseDirectory, "Muntin.ico");
        if (File.Exists(icon))
        {
            AppWindow.SetIcon(icon);
            AppTitleBar.IconSource = new ImageIconSource { ImageSource = new BitmapImage(new Uri(icon)) };
        }

        _view = new SettingsView(_store, DispatcherQueue, muntinExe);
        _view.SaveFailed += message =>
        {
            FileBar.Severity = InfoBarSeverity.Warning;
            FileBar.Title = "Not saved";
            FileBar.Message = message;
            FileBar.IsClosable = true;
            FileBar.IsOpen = true;
        };
        _reloadTimer = DispatcherQueue.CreateTimer();
        _reloadTimer.Interval = TimeSpan.FromMilliseconds(200);
        _reloadTimer.IsRepeating = false;
        _reloadTimer.Tick += (_, _) => Reload();

        // Changes from the tray, from Muntin itself (strip drags) or from a text editor show up here
        var folder = Path.GetDirectoryName(_store.Path)!;
        try
        {
            Directory.CreateDirectory(folder);
            _watcher = new FileSystemWatcher(folder, Path.GetFileName(_store.Path))
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size,
            };
            _watcher.Changed += (_, _) => QueueReload();
            _watcher.Created += (_, _) => QueueReload();
            _watcher.Deleted += (_, _) => QueueReload();
            _watcher.Renamed += (_, _) => QueueReload();
            _watcher.EnableRaisingEvents = true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
        {
            _watcher = null; // Changes made elsewhere don't show until the window reopens; editing still works
        }

        Closed += (_, _) => _watcher?.Dispose();
        Reload();
        Nav.SelectedItem = Nav.MenuItems[0];
    }

    // Watcher events arrive on a pool thread, several per save
    private void QueueReload() => DispatcherQueue.TryEnqueue(() =>
    {
        _reloadTimer.Stop();
        _reloadTimer.Start();
    });

    private void Reload()
    {
        var load = _store.Load();
        var unreadable = load.Status == LoadStatus.Unreadable;
        if (!unreadable && FileBar.IsOpen && FileBar.Severity == InfoBarSeverity.Warning)
        {
            _view.SetEditable(true);
            _view.Refresh(load.Properties, load.Status != LoadStatus.Missing);
            return; // Keep a save warning up until the user closes it
        }

        FileBar.Severity = InfoBarSeverity.Error;
        FileBar.IsClosable = false;
        FileBar.Title = unreadable ? "settings.json can't be read" : string.Empty;
        FileBar.Message = unreadable ? $"Nothing is saved until the file is fixed or deleted. {load.Error}" : string.Empty;
        FileBar.IsOpen = unreadable;
        _view.SetEditable(!unreadable);
        Host.IsHitTestVisible = !unreadable;
        Host.Opacity = unreadable ? 0.5 : 1;
        _view.Refresh(load.Properties, load.Status != LoadStatus.Missing);
    }

    private static int Px(int dips, double scale) => (int)Math.Round(dips * (scale > 0 ? scale : 1));

    private void ApplyMinimumSize(double scale)
    {
        if (AppWindow.Presenter is OverlappedPresenter presenter && presenter.PreferredMinimumWidth != Px(MinWidth, scale))
        {
            presenter.PreferredMinimumWidth = Px(MinWidth, scale);
            presenter.PreferredMinimumHeight = Px(MinHeight, scale);
        }
    }

    // The pane button lives in the title bar: it opens the pane as an overlay (Minimal, Compact) or folds it (Expanded)
    private void AppTitleBar_PaneToggleRequested(TitleBar sender, object args) => Nav.IsPaneOpen = !Nav.IsPaneOpen;

    // Narrow windows keep more room for the cards
    private void Nav_DisplayModeChanged(NavigationView sender, NavigationViewDisplayModeChangedEventArgs args)
    {
        Column.Padding = args.DisplayMode == NavigationViewDisplayMode.Minimal ? new Thickness(16, 16, 16, 24) : new Thickness(36, 24, 36, 36);
    }

    private void Nav_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem { Tag: string tag })
        {
            Host.Content = _view.Section(tag);
            Scroller.ChangeView(null, 0, null, disableAnimation: true);
        }
    }

    [LibraryImport("user32.dll")]
    private static partial uint GetDpiForWindow(IntPtr window);
}
