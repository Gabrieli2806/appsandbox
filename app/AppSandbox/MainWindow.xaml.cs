using System.Runtime.InteropServices;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using WinRT.Interop;

namespace AppSandbox;

/// <summary>
/// The shell: title bar, navigation, the notice bar, and what has to outlive
/// any one page — the notification-area icon and its menu, and the link to
/// the native host that runs the VMs. Pages are XAML (Views/*Page.xaml) and
/// read their state from <see cref="AppState"/>.
/// </summary>
public sealed partial class MainWindow : Window
{
    public static new MainWindow? Current { get; private set; }

    private const uint WM_GETMINMAXINFO = 0x0024;

    private readonly nint _hwnd;
    private readonly Desktop.SubclassProc _subclass;
    private readonly TrayIcon _tray;
    private readonly DispatcherTimer _noticeTimer = new() { Interval = TimeSpan.FromSeconds(7) };
    private TrayMenuWindow? _trayMenu;
    private bool _allowClose, _selectingWithoutNavigation;
    private long _trayClicksIgnoredUntil;

    private static AppState S => AppState.Current;

    public nint Handle => _hwnd;

    /// <summary>The theme picked in Settings, for windows and dialogs that are not inside this one.</summary>
    public ElementTheme Theme => UiSettings.Current.Theme switch
    {
        "Dark" => ElementTheme.Dark,
        "Light" => ElementTheme.Light,
        _ => ElementTheme.Default,
    };

    public MainWindow()
    {
        InitializeComponent();
        Current = this;
        Title = "App Sandbox";
        _hwnd = WindowNative.GetWindowHandle(this);

        SizeToWorkArea();
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "AppSandbox.ico"));
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        _subclass = WndProc;
        Desktop.SetWindowSubclass(_hwnd, _subclass, 1, 0);

        // This process is elevated and Explorer is not: its "the taskbar is
        // back" broadcast has to be let through, or the icon is lost when
        // Explorer restarts.
        Desktop.AllowFromLowerIntegrity(_hwnd, TrayIcon.TaskbarCreated);
        _tray = new TrayIcon(_hwnd);

        ApplyAppearance();

        _noticeTimer.Tick += (_, _) =>
        {
            _noticeTimer.Stop();
            NoticeBar.IsOpen = false;
        };

        AppWindow.Closing += OnClosing;
        Closed += OnClosed;
        NavView.PaneOpening += (_, _) => HostCard.Visibility = Visibility.Visible;
        NavView.PaneClosing += (_, _) => HostCard.Visibility = Visibility.Collapsed;

        S.Attach();
        S.Changed += OnStateChanged;
        S.ExitChanged += OnExitChanged;
        S.Notice += ShowNotice;
        S.Alert += notice => _ = Dialogs.Alert(notice);
        S.Document += (kind, title, text) => _ = Dialogs.Document(kind, title, text);
        S.WindowRequested += show => { if (show) ShowPage(CurrentPageTag(), navigate: false); else HideToTray(); };
        S.ExitRequested += ExitNow;

        _selectingWithoutNavigation = true;
        NavView.SelectedItem = NavView.MenuItems[0];
        _selectingWithoutNavigation = false;
        NavigateTo(typeof(LibraryPage));

        // The host starts last: from here on it may show its window-less
        // dialogs over this window and send messages to every page.
        if (HostBridge.Start(_hwnd, DispatcherQueue))
        {
            HostBridge.Send("uiReady");
        }
        else
        {
            ShowNotice(new("error", "", "The App Sandbox engine could not start. Reinstall the app; details are in %LOCALAPPDATA%\\AppSandbox\\shell.log."));
            UiSettings.Log("asb_host_start failed");
        }

        OnStateChanged();
#if DEBUG
        // Debug builds only: open on a given page, for looking at the shell.
        // ASB_UI_PAGE = settings | activity | create | tray | vm:<id>|<section>
        if (Environment.GetEnvironmentVariable("ASB_UI_PAGE") is { Length: > 0 } page)
        {
            DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, async () =>
            {
                await Task.Delay(600);
                if (page.StartsWith("vm:", StringComparison.Ordinal)) ContentFrame.Navigate(typeof(VmPage), page[3..]);
                else if (page == "create") _ = CreateVmDialog.ShowAsync();
                else if (page == "tray") ShowTrayMenu();
                else if (page.StartsWith("edit:", StringComparison.Ordinal)) _ = EditVmDialog.ShowAsync(page[5..]);
                else ShowPage(page);
            });
        }
#endif
    }

    // ---------------------------------------------------------------- Navigation

    private void NavView_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (_selectingWithoutNavigation) return;

        if (args.IsSettingsSelected)
        {
            NavigateTo(typeof(SettingsPage));
            return;
        }

        switch ((args.SelectedItem as NavigationViewItem)?.Tag?.ToString())
        {
            case "activity": NavigateTo(typeof(ActivityPage)); break;
            default: NavigateTo(typeof(LibraryPage)); break;
        }
    }

    /// <summary>With a VM's page open the list's item is already the selected one: clicking it goes back to the list.</summary>
    private void NavView_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
    {
        if (!args.IsSettingsInvoked && args.InvokedItemContainer?.Tag as string == "library") NavigateTo(typeof(LibraryPage));
    }

    private void NavigateTo(Type page, object? parameter = null)
    {
        if (ContentFrame.Content?.GetType() == page && parameter is null) return;
        ContentFrame.Navigate(page, parameter);
    }

    /// <summary>Opens one VM's own page, from its card.</summary>
    public void OpenVm(string id, string section = "overview") => ContentFrame.Navigate(typeof(VmPage), id + "|" + section);

    /// <summary>Back to the list, as from a VM that was deleted.</summary>
    public void ShowLibrary() => NavigateTo(typeof(LibraryPage));

    private string CurrentPageTag() => ContentFrame.Content switch
    {
        SettingsPage => "settings",
        ActivityPage => "activity",
        _ => "library",
    };

    /// <summary>Brings the window back from the tray or from behind other windows, on the page asked for.</summary>
    public void ShowPage(string page, bool navigate = true)
    {
        AppWindow.Show();
        if (AppWindow.Presenter is OverlappedPresenter presenter && presenter.State == OverlappedPresenterState.Minimized) presenter.Restore();
        Activate();
        Desktop.SetForegroundWindow(_hwnd);
        if (!navigate) return;

        _selectingWithoutNavigation = true;
        NavView.SelectedItem = page switch
        {
            "settings" => NavView.SettingsItem,
            "activity" => NavView.MenuItems[1],
            _ => NavView.MenuItems[0],
        };
        _selectingWithoutNavigation = false;

        NavigateTo(page switch
        {
            "settings" => typeof(SettingsPage),
            "activity" => typeof(ActivityPage),
            _ => typeof(LibraryPage),
        });
    }

    private void TitleBar_PaneToggleRequested(TitleBar sender, object args) => NavView.IsPaneOpen = !NavView.IsPaneOpen;

    // ---------------------------------------------------------------- State

    private void OnStateChanged()
    {
        int running = S.Vms.Count(vm => vm.Running);
        RunningBadge.Visibility = running > 0 ? Visibility.Visible : Visibility.Collapsed;
        RunningText.Text = running == 1 ? "1 VM running" : running + " VMs running";

        VmCountBadge.Visibility = S.Vms.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        VmCountBadge.Value = S.Vms.Count;

        var host = S.Host;
        HostCpuText.Text = host.HostCores > 0 ? $"{host.VmCores} / {host.HostCores} cores" : "–";
        HostCpuBar.Value = host.HostCores > 0 ? Math.Min(1.0, (double)host.VmCores / host.HostCores) : 0;
        HostRamText.Text = host.HostRamMb > 0 ? $"{VmInfo.RamLabel(host.VmRamMb)} / {VmInfo.RamLabel(host.HostRamMb)}" : "–";
        HostRamBar.Value = host.HostRamMb > 0 ? Math.Min(1.0, (double)host.VmRamMb / host.HostRamMb) : 0;
        HostDiskText.Text = host.FreeGb >= 0 ? host.FreeGb + " GB" : "–";
        HostGpuText.Text = host.Gpus.Count == 0 ? "–" : host.Gpus[0].Name + (host.Gpus.Count > 1 ? " +" + (host.Gpus.Count - 1) : "");
        ToolTipService.SetToolTip(HostGpuText, host.Gpus.Count > 0 ? string.Join("\n", host.Gpus.Select(gpu => gpu.Name)) : null);

        _tray.Update(running == 0 ? "" : running == 1 ? "1 VM running" : running + " VMs running");
    }

    private void OnExitChanged()
    {
        ExitBar.IsOpen = S.Exiting;
        if (!S.Exiting) return;

        ExitBar.Title = "Exiting";
        ExitBar.Message = S.ExitRemaining > 0
            ? $"Shutting down {S.ExitRemaining} VM{(S.ExitRemaining == 1 ? "" : "s")}; App Sandbox exits when {(S.ExitRemaining == 1 ? "it has" : "they have")} stopped."
            : "The VMs have stopped. App Sandbox is exiting…";

        // When Windows itself is signing out or restarting there is nothing to cancel.
        ExitCancelButton.Visibility = S.ExitForSession ? Visibility.Collapsed : Visibility.Visible;
    }

    private void ExitCancel_Click(object sender, RoutedEventArgs e) => HostBridge.Send("cancelExit");

    /// <summary>News from the host. Errors stay until closed; the rest leaves after a few seconds.</summary>
    public void ShowNotice(HostNotice notice)
    {
        // An error waits to be read: something lesser does not take its place.
        if (NoticeBar.IsOpen && NoticeBar.Severity == InfoBarSeverity.Error && notice.Kind != "error")
        {
            if (!AppWindow.IsVisible) _tray.ShowBalloon("App Sandbox", notice.Message, false);
            return;
        }

        NoticeBar.Severity = notice.Kind switch
        {
            "error" => InfoBarSeverity.Error,
            "warning" => InfoBarSeverity.Warning,
            "success" => InfoBarSeverity.Success,
            _ => InfoBarSeverity.Informational,
        };
        NoticeBar.Title = notice.Title;
        NoticeBar.Message = notice.Message;
        NoticeBar.IsOpen = true;

        _noticeTimer.Stop();
        if (notice.Kind != "error") _noticeTimer.Start();

        // With the window in the tray the bar is not seen: say it from the icon.
        if (!AppWindow.IsVisible) _tray.ShowBalloon("App Sandbox", notice.Message, notice.Kind is "error" or "warning");
    }

    // ---------------------------------------------------------------- Appearance

    public void ApplyAppearance()
    {
        var settings = UiSettings.Current;
        var theme = Theme;

        Root.RequestedTheme = theme;
        SystemBackdrop = settings.Backdrop switch
        {
            "Mica" => new MicaBackdrop { Kind = MicaKind.Base },
            "MicaAlt" => new MicaBackdrop { Kind = MicaKind.BaseAlt },
            "Acrylic" => new DesktopAcrylicBackdrop(),
            _ => null,
        };
        Root.Background = settings.Backdrop == "None" ? Forms.Brush("ApplicationPageBackgroundThemeBrush") : null;

        // The caption buttons are drawn by Windows and do not follow the
        // app theme on their own.
        AppWindow.TitleBar.PreferredTheme = theme switch
        {
            ElementTheme.Dark => TitleBarTheme.Dark,
            ElementTheme.Light => TitleBarTheme.Light,
            _ => TitleBarTheme.UseDefaultAppMode,
        };
    }

    private void SizeToWorkArea()
    {
        var work = DisplayArea.Primary.WorkArea;
        double scale = FloatingWindow.Scale(this);
        int width = Math.Min(work.Width - 40, (int)(1180 * scale));
        int height = Math.Min(work.Height - 40, (int)(840 * scale));
        AppWindow.MoveAndResize(new global::Windows.Graphics.RectInt32(
            work.X + (work.Width - width) / 2, work.Y + (work.Height - height) / 2, width, height));
    }

    // ---------------------------------------------------------------- Tray

    private nint WndProc(nint window, uint message, nuint wp, nint lp, nuint id, nuint data)
    {
        if (message == WM_GETMINMAXINFO)
        {
            // The family's minimum: below it the pages have nowhere to go.
            double scale = Math.Max(1, Desktop.GetDpiForWindow(window) / 96.0);
            var info = Marshal.PtrToStructure<Desktop.MinMaxInfo>(lp);
            info.MinTrackSize.X = (int)(760 * scale);
            info.MinTrackSize.Y = (int)(520 * scale);
            Marshal.StructureToPtr(info, lp, false);
            return 0;
        }

        if (message == TrayIcon.TaskbarCreated) _tray.Add();
        else if (message == TrayIcon.CallbackMessage)
        {
            // The family convention: one click opens the menu, a double click the
            // window. A double click ends with a second button-up, which must not
            // bring the menu back over the window that just opened.
            uint mouse = (uint)(lp.ToInt64() & 0xffff);
            bool endOfDoubleClick = mouse == 0x0202 && Environment.TickCount64 < _trayClicksIgnoredUntil;
            if (mouse == 0x0203)
            {
                _trayClicksIgnoredUntil = Environment.TickCount64 + Desktop.GetDoubleClickTime();
                DispatcherQueue.TryEnqueue(() =>
                {
                    _trayMenu?.Close();
                    ShowPage(CurrentPageTag(), navigate: false);
                });
            }
            else if (mouse is 0x0202 or 0x0205 or 0x007B && !endOfDoubleClick)
            {
                DispatcherQueue.TryEnqueue(ShowTrayMenu);
            }
        }

        return Desktop.DefSubclassProc(window, message, wp, lp);
    }

    private void ShowTrayMenu()
    {
        _trayMenu?.Close();

        var menu = new TrayMenuWindow();
        _trayMenu = menu;
        menu.Closed += (_, _) =>
        {
            if (ReferenceEquals(_trayMenu, menu)) _trayMenu = null;
        };

        Desktop.GetCursorPos(out var cursor);
        menu.ShowNear(cursor.X, cursor.Y);
    }

    // ---------------------------------------------------------------- Closing

    /// <summary>
    /// Closing the window never ends the app by itself: ending the process
    /// turns every VM off. The host decides with the user (keep running in the
    /// tray, shut the VMs down first, or exit) and answers with "window" or "exit".
    /// </summary>
    private void OnClosing(AppWindow sender, AppWindowClosingEventArgs args)
    {
        if (_allowClose) return;
        args.Cancel = true;

        if (HostBridge.Started) HostBridge.Send("requestExit", ("fromClose", true));
        else ExitNow();
    }

    /// <summary>Quit from the tray menu: the same question, without the option to just hide.</summary>
    public void RequestExit()
    {
        if (HostBridge.Started) HostBridge.Send("requestExit", ("fromClose", false));
        else ExitNow();
    }

    private void HideToTray()
    {
        _trayMenu?.Close();
        AppWindow.Hide();
    }

    private void ExitNow()
    {
        if (_allowClose) return;
        _allowClose = true;
        Close();
    }

    private void OnClosed(object sender, WindowEventArgs args)
    {
        _noticeTimer.Stop();
        _tray.Dispose();
        _trayMenu?.Close();
        Desktop.RemoveWindowSubclass(_hwnd, _subclass, 1);
        Application.Current.Exit();
    }
}
