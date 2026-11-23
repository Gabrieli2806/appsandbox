using Microsoft.UI.Xaml;

namespace AppSandbox;

public partial class App : Application
{
    /// <summary>Started by a VM shortcut: the manager's window stays out of the way.</summary>
    public static bool StartInTray { get; set; }

    private MainWindow? _window;

    public App()
    {
        InitializeComponent();
#if DEBUG
        // Debug builds only: every exception, also the ones something catches.
        AppDomain.CurrentDomain.FirstChanceException += (_, e) =>
            UiSettings.Log("First chance: " + e.Exception.GetType().Name + ": " + e.Exception.Message + " @ " + (e.Exception.StackTrace?.Split('\n').FirstOrDefault()?.Trim() ?? ""));
#endif
        UnhandledException += (_, e) =>
        {
            // The VMs live in this process: a page that fails must not take them down.
            UiSettings.Log("Unhandled: " + e.Exception);
            e.Handled = true;
        };
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        // One instance only: Program.Main has already handed a second launch
        // over to the first (the host's lock is machine-wide).
        _window = new MainWindow();
        if (!StartInTray) _window.Activate();
    }
}
