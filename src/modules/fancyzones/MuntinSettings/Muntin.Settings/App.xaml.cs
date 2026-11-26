using Microsoft.UI.Xaml;

namespace Muntin.Settings.App;

public partial class App : Application
{
    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        _window = new MainWindow(Program.MuntinExe);
        _window.Activate();
    }
}
