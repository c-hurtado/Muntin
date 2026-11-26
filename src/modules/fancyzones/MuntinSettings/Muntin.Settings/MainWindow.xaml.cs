using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Muntin.Settings.App;

public sealed partial class MainWindow : Microsoft.UI.Xaml.Window
{
    private readonly SettingsStore _store = new(SettingsStore.DefaultPath);
    private readonly SettingsView _view;
    private readonly FileSystemWatcher? _watcher;
    private readonly DispatcherQueueTimer _reloadTimer;

    public MainWindow(string? muntinExe)
    {
        InitializeComponent();
        Title = Program.WindowTitle;
        SystemBackdrop = new MicaBackdrop();
        AppWindow.Resize(new Windows.Graphics.SizeInt32(980, 760));

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
            _view.Refresh(load.Properties);
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
        _view.Refresh(load.Properties);
    }

    private void Nav_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem { Tag: string tag })
        {
            Host.Children.Clear();
            Host.Children.Add(_view.Section(tag));
            Scroller.ChangeView(null, 0, null, disableAnimation: true);
        }
    }
}
