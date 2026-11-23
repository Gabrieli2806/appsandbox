using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace AppSandbox.Views;

public sealed partial class SettingsPage : Page
{
    private static AppState S => AppState.Current;

    private bool _updating = true;
    private readonly DispatcherTimer _mcpPoll = new() { Interval = TimeSpan.FromSeconds(3) };

    public SettingsPage()
    {
        InitializeComponent();
        BuildMcpRows();

        Loaded += (_, _) =>
        {
            S.Changed += Refresh;
            Refresh();

            // What agents did is not pushed by the host: ask while the page shows.
            HostBridge.Send("getMcpStatus");
            _mcpPoll.Start();
        };
        Unloaded += (_, _) =>
        {
            S.Changed -= Refresh;
            _mcpPoll.Stop();
        };
        _mcpPoll.Tick += (_, _) => HostBridge.Send("getMcpStatus");
    }

    private void Refresh()
    {
        _updating = true;
        var ui = UiSettings.Current;
        var host = S.Host;

        Forms.SelectTag(ThemeCombo, ui.Theme);
        Forms.SelectTag(BackdropCombo, ui.Backdrop);
        Forms.SelectTag(CloseCombo, host.CloseToTrayWithoutAsking ? "tray" : "ask");
        AutoUpdateToggle.IsOn = host.AutoUpdateGuestTools;
        TitleStatsToggle.IsOn = host.ViewerTitleStats;
        ToolbarToggle.IsOn = host.ViewerToolbar;
        Forms.SelectTag(RendererCombo, host.ViewerRenderer == 12 ? "12" : "11");

        // Settings the host keeps cannot be changed before it has loaded them.
        bool ready = S.Core == CoreState.Ready;
        CloseCombo.IsEnabled = AutoUpdateToggle.IsEnabled = TitleStatsToggle.IsEnabled = ToolbarToggle.IsEnabled = RendererCombo.IsEnabled = ready;

        ComputerRows.Children.Clear();
        foreach (var (label, value) in new[]
                 {
                     ("Processors", host.HostCores > 0 ? host.HostCores + " cores" : "–"),
                     ("Memory", host.HostRamMb > 0 ? VmInfo.RamLabel(host.HostRamMb) : "–"),
                     ("GPUs", host.Gpus.Count > 0 ? string.Join("\n", host.Gpus.Select(gpu => gpu.Name)) : "–"),
                     ("Free space in the default VM folder", host.FreeGb >= 0 ? host.FreeGb + " GB" : "–"),
                 })
        {
            var grid = new Grid { ColumnSpacing = 16 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(260) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.Children.Add(new TextBlock { Text = label, Foreground = Forms.Brush("TextFillColorSecondaryBrush") });
            var text = new TextBlock { Text = value, TextWrapping = TextWrapping.Wrap };
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            ComputerRows.Children.Add(grid);
        }

        // One folder holds both when VMs live in the app's data folder.
        bool same = host.DefaultDiskDirectory.Length > 0 &&
                    string.Equals(host.DefaultDiskDirectory.TrimEnd('\\'), host.DataDirectory.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase);
        VmFolderTitle.Text = same ? "VM and app data folder" : "Default VM folder";
        VmFolderText.Text = host.DefaultDiskDirectory.Length > 0 ? host.DefaultDiskDirectory : "–";
        DataFolderCard.Visibility = same ? Visibility.Collapsed : Visibility.Visible;
        DataFolderText.Text = host.DataDirectory.Length > 0 ? host.DataDirectory : "–";
        LogFileText.Text = host.LogFile.Length > 0 ? host.LogFile : "–";

        VersionText.Text = FormatVersion(host.AppVersion);
        string project = host.ProjectRepo, upstream = host.UpstreamRepo;
        LicenseText.Text = project.Length > 0 && upstream.Length > 0 && !project.Equals(upstream, StringComparison.OrdinalIgnoreCase)
            ? $"MIT License. {project} is a fork of {upstream} by James Stringer."
            : "MIT License, by James Stringer and contributors.";
        ProjectText.Text = project.Length > 0 ? "github.com/" + project : "–";

        _updating = false;
    }

    /// <summary>"a.b.c.d" as "Version a.b.c (build d)".</summary>
    private static string FormatVersion(string version)
    {
        string[] parts = version.Split('.');
        if (parts.Length != 4) return version.Length > 0 ? "Version " + version : "";
        return $"Version {parts[0]}.{parts[1]}.{parts[2]}" + (parts[3] != "0" ? $" (build {parts[3]})" : "");
    }

    // ---------------------------------------------------------------- Changes

    private void Appearance_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        var ui = UiSettings.Current;
        ui.Theme = Forms.TagOf(ThemeCombo, ui.Theme);
        ui.Backdrop = Forms.TagOf(BackdropCombo, ui.Backdrop);
        ui.Save();
        MainWindow.Current?.ApplyAppearance();
    }

    private void Close_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_updating) return;
        HostBridge.Send("setCloseBehavior", ("toTray", Forms.TagOf(CloseCombo, "ask") == "tray"));
    }

    private void AutoUpdate_Toggled(object sender, RoutedEventArgs e)
    {
        if (_updating) return;
        HostBridge.Send("setAutoUpdateGuestTools", ("enabled", AutoUpdateToggle.IsOn));
    }

    private void Viewer_Changed(object sender, object e)
    {
        if (_updating) return;
        HostBridge.Send("setViewerPrefs",
            ("titleStats", TitleStatsToggle.IsOn),
            ("toolbar", ToolbarToggle.IsOn),
            ("renderer", Forms.TagOf(RendererCombo, "11") == "12" ? 12 : 11));
    }

    private void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is string which) HostBridge.Send("openFolder", ("which", which));
    }

    private void CopyDiagnostics_Click(object sender, RoutedEventArgs e) => HostBridge.Send("copyDiagnostics");

    private void Document_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is string which) HostBridge.Send("showDocument", ("which", which));
    }

    private void Link_Click(object sender, RoutedEventArgs e)
    {
        if ((sender as FrameworkElement)?.Tag is string which) HostBridge.Send("openLink", ("which", which));
    }

    // ---------------------------------------------------------------- AI agents (MCP)

    /// <summary>AI agents (MCP): the rows every G2806 app shows, fed with what the host says about its endpoint.</summary>
    private void BuildMcpRows()
    {
        var readOnly = new ToggleSwitch { VerticalAlignment = VerticalAlignment.Center };
        bool settingReadOnly = false;
        readOnly.Toggled += (_, _) =>
        {
            if (settingReadOnly || readOnly.IsOn == S.Mcp.ReadOnly) return;
            HostBridge.Send("setMcpReadOnly", ("readOnly", readOnly.IsOn));
        };

        var readOnlyRow = new Grid { ColumnSpacing = 12 };
        readOnlyRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        readOnlyRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        readOnlyRow.Children.Add(Forms.Stack(0,
            new TextBlock { Text = "Read-only" },
            Forms.Caption("Agents can look (list VMs, screenshots, logs) but not start, change or delete anything.")));
        Grid.SetColumn(readOnly, 1);
        readOnlyRow.Children.Add(readOnly);

        McpRows.Children.Add(new McpSettingsCard(new McpCardHost
        {
            AppName = "App Sandbox",
            ServerName = "appsandbox",
            About = "App Sandbox runs Windows virtual machines with GPU acceleration on this PC. Use it as a disposable test machine: create or start a VM, run commands and copy files in it, install and test drivers, take screenshots, and go back to a snapshot.",
            FirstCalls = "Once connected, call list_vms and get_host and tell me what you can work with.",
            Url = () => S.Mcp.Url,
            Enabled = () => S.Mcp.Enabled,
            SetEnabled = on => HostBridge.Send("setMcpEnabled", ("enabled", on)),
            Running = () => S.Mcp.Enabled && S.Mcp.Listening,
            // On but not listening yet, with nothing wrong: the host is still starting it.
            Error = () => S.Mcp.Error.Length > 0 ? S.Mcp.Error : "still starting",
            Client = () => S.Mcp.LastClient.Length > 0 ? S.Mcp.LastClient : null,
            Activity = () => [.. S.Mcp.Activity.Select(call => (
                DateTime.TryParseExact(call.Time, "HH:mm:ss", CultureInfo.InvariantCulture, DateTimeStyles.None, out var at) ? at : DateTime.Now,
                call.Vm.Length > 0 ? $"{call.Tool}  {call.Vm}" : call.Tool,
                call.Ok))],
            Subscribe = handler =>
            {
                void Changed()
                {
                    settingReadOnly = true;
                    readOnly.IsOn = S.Mcp.ReadOnly;
                    settingReadOnly = false;
                    handler();
                }
                S.McpChanged += Changed;
                return () => S.McpChanged -= Changed;
            },
            Permissions = [readOnlyRow],
        }));
    }
}
