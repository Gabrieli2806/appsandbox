using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace AppSandbox;

public static class Program
{
    /// <summary>
    /// The native host keeps VM records of about 600 KB on the stack of the
    /// thread that runs it (the UI thread, or the headless daemon's), so that
    /// thread is created here with room for them instead of using the 1 MB
    /// the process starts with.
    /// </summary>
    private const int UiStackBytes = 8 * 1024 * 1024;

    [STAThread]
    public static int Main()
    {
        int exitCode = 0;
        var ui = new Thread(() => exitCode = Run(), UiStackBytes) { Name = "UI" };
        ui.SetApartmentState(ApartmentState.STA);
        ui.Start();
        ui.Join();
        return exitCode;
    }

#if DEBUG
    private static bool Demo => Environment.GetEnvironmentVariable("ASB_UI_DEMO") is { Length: > 0 };
#else
    private const bool Demo = false;
#endif

    private static int Run()
    {
        // Before anything else: the single-instance lock, the command line
        // (--headless, --vm "name") and the data folder's access list.
        switch (Demo ? HostBridge.BeginResult.Run : HostBridge.Begin())
        {
            case HostBridge.BeginResult.Headless:
                return HostBridge.RunHeadless();
            case HostBridge.BeginResult.HandedOver:
            case HostBridge.BeginResult.ServiceRunning:
                return 0;
            case HostBridge.BeginResult.RunForVm:
                // A VM shortcut: the VM's window is what was asked for.
                App.StartInTray = true;
                break;
        }

        WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(p =>
        {
            SynchronizationContext.SetSynchronizationContext(
                new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread()));
            _ = new App();
        });
        return 0;
    }
}
