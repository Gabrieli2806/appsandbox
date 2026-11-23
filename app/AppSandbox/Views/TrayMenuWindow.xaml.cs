using Microsoft.UI.Composition;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;
using WinRT;
using WinRT.Interop;

namespace AppSandbox.Views;

/// <summary>
/// The notification-area menu: which VMs run, and what can be done to each
/// without opening the app.
///
/// Built as a WinUI window for the same reason as the rest of the family: a
/// classic popup menu is a list of strings, and the running state is the point.
/// </summary>
public sealed partial class TrayMenuWindow : Window
{
    private const double MenuWidth = 312;
    private const int EdgeGap = 8;

    private readonly nint _hwnd;
    private DesktopAcrylicController? _acrylic;
    private SystemBackdropConfiguration? _backdropConfig;

    public TrayMenuWindow()
    {
        InitializeComponent();
        Title = "App Sandbox menu";
        _hwnd = WindowNative.GetWindowHandle(this);

        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.SetBorderAndTitleBar(false, false);
            presenter.IsAlwaysOnTop = true;
            presenter.IsResizable = false;
            presenter.IsMinimizable = false;
            presenter.IsMaximizable = false;
        }

        AppWindow.IsShownInSwitchers = false;
        Root.Width = MenuWidth;
        Root.RequestedTheme = MainWindow.Current?.Theme ?? ElementTheme.Default;

        FloatingWindow.StripFrame(_hwnd);
        SetUpBackdrop();

        // A menu closes when you click away from it.
        Activated += (_, args) =>
        {
            if (args.WindowActivationState == WindowActivationState.Deactivated) Close();
        };

        Closed += (_, _) =>
        {
            AppState.Current.Changed -= Refresh;
            _acrylic?.Dispose();
            _acrylic = null;
            _backdropConfig = null;
        };

        // A VM that stops while the menu is open leaves it.
        AppState.Current.Changed += Refresh;
        Refresh();
    }

    /// <summary>Places the menu by the pointer, above it when there is room, and shows it.</summary>
    public void ShowNear(int cursorX, int cursorY)
    {
        Activate();

        Root.UpdateLayout();
        Root.Measure(new global::Windows.Foundation.Size(MenuWidth, double.PositiveInfinity));

        double scale = FloatingWindow.Scale(this);
        int width = (int)Math.Ceiling(MenuWidth * scale);
        int height = (int)Math.Ceiling(Math.Max(Root.DesiredSize.Height, 120) * scale);
        AppWindow.Resize(new SizeInt32(width, height));

        var work = DisplayArea.GetFromPoint(new PointInt32(cursorX, cursorY), DisplayAreaFallback.Nearest).WorkArea;
        int x = Math.Clamp(cursorX - width / 2, work.X + EdgeGap, Math.Max(work.X + EdgeGap, work.X + work.Width - width - EdgeGap));
        int y = cursorY - height - EdgeGap;
        if (y < work.Y + EdgeGap) y = Math.Min(cursorY + EdgeGap, work.Y + work.Height - height - EdgeGap);

        AppWindow.Move(new PointInt32(x, Math.Max(y, work.Y + EdgeGap)));
        Desktop.SetForegroundWindow(_hwnd);
    }

    private void Refresh()
    {
        var state = AppState.Current;
        var running = state.Vms.Where(vm => vm.Running).ToList();
        var busy = state.Vms.Count(vm => !vm.Running && vm.Busy);

        var tint = Forms.Brush(running.Count > 0 ? "SystemFillColorSuccessBrush" : "TextFillColorSecondaryBrush");
        StatusGlyph.Foreground = tint;
        StatusHalo.Fill = tint;

        if (state.Exiting)
        {
            StatusTitle.Text = "Exiting";
            StatusDetail.Text = "Waiting for the VMs to shut down";
        }
        else if (state.Core != CoreState.Ready)
        {
            StatusTitle.Text = state.Core == CoreState.Starting ? "Starting…" : "Not ready";
            StatusDetail.Text = state.Core == CoreState.Starting ? "Finding your virtual machines" : "Virtual Machine Platform is off";
        }
        else
        {
            StatusTitle.Text = running.Count switch { 0 => "No VM running", 1 => "1 VM running", _ => running.Count + " VMs running" };
            StatusDetail.Text = busy > 0
                ? busy == 1 ? "1 more is being built, started or copied" : busy + " more are being built, started or copied"
                : state.Vms.Count switch { 0 => "No virtual machines yet", 1 => "1 virtual machine", _ => state.Vms.Count + " virtual machines" };
        }

        VmRows.Children.Clear();
        foreach (var vm in running)
        {
            VmRows.Children.Add(new Border { Height = 1, Margin = new Thickness(0, 4, 0, 6), Background = Forms.Brush("DividerStrokeColorDefaultBrush") });

            var heading = new Grid { Margin = new Thickness(10, 2, 10, 4), ColumnSpacing = 8 };
            heading.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            heading.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            heading.Children.Add(new TextBlock
            {
                Text = vm.Name,
                Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
                TextTrimming = TextTrimming.CharacterEllipsis,
            });
            var status = new TextBlock
            {
                Text = vm.Status.Label,
                Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
                Foreground = Forms.Brush("TextFillColorSecondaryBrush"),
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(status, 1);
            heading.Children.Add(status);
            VmRows.Children.Add(heading);

            // While the app exits, or while something is being done to the VM,
            // its rows stay but cannot be used.
            bool usable = !vm.Busy && !state.Exiting;
            string id = vm.Id;
            VmRows.Children.Add(Item("", "Connect", usable && !vm.IsTemplate, () => HostBridge.Send("connectIddVm", ("vmId", id))));
            VmRows.Children.Add(Item("", "Shut down", usable && !vm.ShuttingDown, () => HostBridge.Send("shutdownVm", ("vmId", id))));
            VmRows.Children.Add(Item("", "Force stop…", usable, () => HostBridge.Send("confirmForceStop", ("vmId", id))));
        }
    }

    private Button Item(string glyph, string text, bool enabled, Action action)
    {
        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(new FontIcon { Glyph = glyph, FontSize = 16 });
        var label = new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(label, 1);
        grid.Children.Add(label);

        var button = new Button
        {
            Style = (Style)Application.Current.Resources["TrayMenuItemStyle"],
            Content = grid,
            IsEnabled = enabled,
        };
        button.Click += (_, _) =>
        {
            Close();
            action();
        };
        return button;
    }

    // ---------------------------------------------------------------- Actions

    private void Open_Click(object sender, RoutedEventArgs e) { Close(); MainWindow.Current?.ShowPage("library"); }

    private void Settings_Click(object sender, RoutedEventArgs e) { Close(); MainWindow.Current?.ShowPage("settings"); }

    private void Exit_Click(object sender, RoutedEventArgs e) { Close(); MainWindow.Current?.RequestExit(); }

    // ---------------------------------------------------------------- Chrome

    private void SetUpBackdrop()
    {
        if (!DesktopAcrylicController.IsSupported()) return;

        try
        {
            _backdropConfig = new SystemBackdropConfiguration
            {
                IsInputActive = true,
                Theme = UiSettings.Current.Theme switch
                {
                    "Dark" => SystemBackdropTheme.Dark,
                    "Light" => SystemBackdropTheme.Light,
                    _ => SystemBackdropTheme.Default,
                },
            };

            _acrylic = new DesktopAcrylicController { Kind = DesktopAcrylicKind.Base };
            _acrylic.SetSystemBackdropConfiguration(_backdropConfig);
            _acrylic.AddSystemBackdropTarget(this.As<ICompositionSupportsSystemBackdrop>());
        }
        catch (Exception ex)
        {
            UiSettings.Log("Tray menu backdrop: " + ex.Message);
            _acrylic = null;
        }
    }
}
