using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace AppSandbox.Views;

/// <summary>One VM: what it is, its hardware, its display settings and its snapshots.</summary>
public sealed partial class VmPage : Page
{
    private static AppState S => AppState.Current;

    private static readonly (string Value, string Note)[] Presets =
    [
        ("1280x720", "HD"), ("1600x900", "HD+"), ("1920x1080", "Full HD"), ("1920x1200", "16:10"),
        ("2560x1440", "QHD"), ("3440x1440", "Ultrawide"), ("3840x2160", "4K"),
    ];
    private static readonly int[] Rates = [60, 120, 144];
    private static readonly int[] Bitrates = [0, 20, 40, 60, 80, 100, 120, 150, 200, 300];

    private string _id = "";
    private string _section = "overview";
    private bool _updating;
    private string _treeSignature = "";

    // What the lists on the page were last built from. The host reports its
    // state several times a second while any VM is being built or copied; a
    // list that is open, or a row being clicked, must not be rebuilt under
    // the pointer when nothing in it changed.
    private string _displayLists = "", _treeBuilt = "";

    public VmPage()
    {
        InitializeComponent();

        Loaded += (_, _) =>
        {
            S.Changed += Refresh;
            VmIcons.Changed += OnIconChanged;
            Refresh();
        };
        Unloaded += (_, _) =>
        {
            S.Changed -= Refresh;
            VmIcons.Changed -= OnIconChanged;
        };
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        string[] parts = (e.Parameter as string ?? "").Split('|');
        _id = parts[0];
        _section = parts.Length > 1 ? parts[1] : "overview";
    }

    private void OnIconChanged(string id)
    {
        if (id == _id) Refresh();
    }

    private VmInfo? Vm => S.Find(_id);

    private void Refresh()
    {
        if (Vm is not { } vm)
        {
            // Deleted while its page was open.
            MainWindow.Current?.ShowLibrary();
            return;
        }

        _updating = true;

        NameText.Text = vm.Name;
        SubText.Text = vm.OsType + " · " + vm.GpuLabel + (vm.Running && vm.AgentOnline ? " · agent connected" : "");
        StatusBadge.Apply(vm, StatusText, StatusDot, StatusRing);

        var image = VmIcons.Get(vm);
        IconImage.Source = image;
        IconImage.Visibility = image is null ? Visibility.Collapsed : Visibility.Visible;
        OsGlyph.Glyph = VmActions.OsGlyph(vm);
        OsGlyph.Visibility = image is null ? Visibility.Visible : Visibility.Collapsed;

        var primary = VmActions.Primary(vm);
        PrimaryText.Text = primary.Label;
        PrimaryGlyph.Glyph = primary.Glyph;
        PrimaryButton.IsEnabled = primary.Enabled;
        PrimaryButton.Style = Forms.Res(vm.Running ? "DefaultButtonStyle" : "AccentButtonStyle");
        ToolTipService.SetToolTip(PrimaryButton, primary.Hint.Length > 0 ? primary.Hint : null);
        ShutDownButton.Visibility = vm.Running ? Visibility.Visible : Visibility.Collapsed;
        ShutDownButton.IsEnabled = vm.Running && !vm.Busy;

        if (vm.Progress is int percent)
        {
            Progress.Value = percent;
            Progress.Visibility = Visibility.Visible;
        }
        else
        {
            Progress.Visibility = Visibility.Collapsed;
        }

        ErrorBar.Message = vm.LastError;
        ErrorBar.IsOpen = vm.LastError.Length > 0;
        ErrorBar.Visibility = ErrorBar.IsOpen ? Visibility.Visible : Visibility.Collapsed;

        // Sections that do not apply to this VM are not offered.
        bool display = vm.DisplaySupported, snapshots = !vm.IsTemplate;
        DisplayTab.Visibility = display ? Visibility.Visible : Visibility.Collapsed;
        SnapshotsTab.Visibility = snapshots ? Visibility.Visible : Visibility.Collapsed;
        if ((_section == "display" && !display) || (_section == "snapshots" && !snapshots)) _section = "overview";
        foreach (var item in Sections.Items)
            if (item.Tag as string == _section && !item.IsSelected) Sections.SelectedItem = item;

        OverviewPanel.Visibility = _section == "overview" ? Visibility.Visible : Visibility.Collapsed;
        HardwarePanel.Visibility = _section == "hardware" ? Visibility.Visible : Visibility.Collapsed;
        DisplayPanel.Visibility = _section == "display" ? Visibility.Visible : Visibility.Collapsed;
        SnapshotsPanel.Visibility = _section == "snapshots" ? Visibility.Visible : Visibility.Collapsed;

        switch (_section)
        {
            case "hardware": RefreshHardware(vm); break;
            case "display": RefreshDisplay(vm); break;
            case "snapshots": RefreshSnapshots(vm); break;
            default: RefreshOverview(vm); break;
        }

        _updating = false;
    }

    private static void Fill(StackPanel rows, IEnumerable<(string Label, string Value)> facts)
    {
        rows.Children.Clear();
        foreach (var (label, value) in facts)
        {
            var grid = new Grid { ColumnSpacing = 16 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(160) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.Children.Add(new TextBlock { Text = label, Foreground = Forms.Brush("TextFillColorSecondaryBrush") });
            var text = new TextBlock { Text = value, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            rows.Children.Add(grid);
        }
    }

    // ---------------------------------------------------------------- Overview

    private void RefreshOverview(VmInfo vm)
    {
        var facts = new List<(string, string)>
        {
            ("Status", vm.Status.Label),
            ("Operating system", vm.OsType + (vm.IsTemplate ? " (template)" : "")),
            ("In-VM agent", vm.IsOther ? "Not used (manual install)" : !vm.Running ? "VM is off" : vm.AgentOnline ? "Connected" : "Not connected"),
            ("Network", vm.NetworkLabel),
        };

        // NAT: the address the VM gets. A manual-install VM needs it set by
        // hand; the gateway is the .1 of its /24.
        if (vm.NatIp.Length > 0)
        {
            int dot = vm.NatIp.LastIndexOf('.');
            facts.Add(("IP address", vm.NatIp + "/24" + (dot > 0 ? " · gateway " + vm.NatIp[..dot] + ".1" : "")));
        }
        if (vm.SshEnabled && vm.SshPort > 0) facts.Add(("SSH", "localhost:" + vm.SshPort + (vm.SshState == 4 ? " (key deployed)" : "")));
        if (vm.HasSnapshots) facts.Add(("Boots from", DiskChoiceOf(vm).Path(vm)));
        if (vm.DiskDirectory.Length > 0) facts.Add(("Disk folder", vm.DiskDirectory));
        Fill(OverviewRows, facts);

        FilesCard.Visibility = vm.VmFolder.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        FolderText.Text = vm.VmFolder;

        bool tools = vm.IsWindows && !vm.IsTemplate;
        ToolsCard.Visibility = tools ? Visibility.Visible : Visibility.Collapsed;
        if (!tools) return;

        bool auto = S.Host.AutoUpdateGuestTools;
        ToolsText.Text =
            vm.UpdatingTools ? "Copying this version’s agent, helpers and display driver into the VM’s disk…"
            : !vm.InstallComplete ? "Installed with the VM. Available to update once Windows setup has finished."
            : vm.GuestToolsOutdated
                ? "This version of App Sandbox has newer components than this VM (agent, helpers, display driver). " +
                  (auto ? "They are copied in automatically the next time the VM starts, or now: " : "Update them now: ") +
                  "the VM must be stopped. The display driver installs during the next start."
            : "Up to date with this version of App Sandbox.";
        ToolsButton.IsEnabled = !vm.Running && !vm.Busy && vm.InstallComplete;
        ToolsButton.Style = Forms.Res(vm.GuestToolsOutdated ? "AccentButtonStyle" : "DefaultButtonStyle");
        ToolTipService.SetToolTip(ToolsButton, vm.Running ? "Shut the VM down first" : null);
    }

    // ---------------------------------------------------------------- Hardware

    private void RefreshHardware(VmInfo vm)
    {
        Fill(HardwareRows,
        [
            ("Processors", vm.CpuCores + (vm.CpuCores == 1 ? " core" : " cores")),
            ("Memory", $"{VmInfo.RamLabel(vm.RamMb)} ({vm.RamMb} MB)"),
            ("Disk", vm.HddGb + " GB"),
            ("GPU", vm.GpuLabel),
            ("Network", vm.NetworkLabel),
        ]);
        EditButton.IsEnabled = !vm.Running && !vm.Busy;
        ToolTipService.SetToolTip(EditButton, EditButton.IsEnabled ? null : "Stop the VM to change its hardware");
    }

    // ---------------------------------------------------------------- Display

    private void RefreshDisplay(VmInfo vm)
    {
        int driver = vm.DisplayDriver;
        bool auto = S.Host.AutoUpdateGuestTools;

        DisplayUpdateButton.Visibility = driver == 0 && !vm.Running && !vm.Busy ? Visibility.Visible : Visibility.Collapsed;
        if (driver == 0)
        {
            DisplayNote.Severity = InfoBarSeverity.Warning;
            DisplayNote.Title = "This VM has the older display driver";
            DisplayNote.Message = "It stays at 1920 × 1080 at 60 Hz. " + (vm.Running
                ? auto ? "Shut the VM down and start it again: App Sandbox updates its components before it starts."
                       : "Shut the VM down, then use Update guest components."
                : "Update its guest components, then start the VM.");
        }
        else if (driver >= 2)
        {
            DisplayNote.Severity = InfoBarSeverity.Success;
            DisplayNote.Title = "";
            DisplayNote.Message = "The display driver in this VM supports these settings. Changes apply in a few seconds.";
        }
        else
        {
            DisplayNote.Severity = InfoBarSeverity.Informational;
            DisplayNote.Title = "";
            DisplayNote.Message = vm.Running
                ? "Open the VM window to apply changes and check the VM’s display driver."
                : "Saved now; applied when the VM window opens.";
        }

        string lists = $"{vm.DisplayResolution}|{vm.DisplayHz}|{vm.DisplayBitrate}";
        if (lists != _displayLists)
        {
            _displayLists = lists;
            FillDisplayLists(vm);
        }

        bool performance = vm.DisplayPerformance;
        Forms.SelectTag(StreamCombo, performance ? "performance" : "quality");
        int video = vm.DisplayVideo;
        StreamNote.Text =
            performance && video == 1 ? "Active: the VM’s GPU is encoding the picture" + (vm.DisplayVideoCodec.Length > 0 ? $" ({vm.DisplayVideoCodec})." : ".")
            : performance && driver is >= 0 and < 3 ? "Needs the updated display driver: shut the VM down and update its guest components."
            : performance && video == 2 ? "Not active: the VM’s GPU has no hardware video encoder (GPU partitioning off?) or this PC cannot decode H.264. Showing exact pixels instead; the VM window’s log has details."
            : performance && video == 0 ? "Starting…"
            : performance ? "Applied when the VM window is open. Needs GPU partitioning so the VM has a hardware video encoder."
            : "";
        StreamNote.Visibility = StreamNote.Text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        StreamOptions.Visibility = performance ? Visibility.Visible : Visibility.Collapsed;

        Forms.SelectTag(CodecCombo, vm.DisplayCodec);
        string inUse = vm.DisplayVideoCodec.Length > 0 ? $"In use: {vm.DisplayVideoCodec}. " : "";
        CodecNote.Text = inUse + (
            vm.DisplayCodec != "h264" && !S.Host.HevcDecoder ? "HEVC needs “HEVC Video Extensions” from the Microsoft Store on this PC; until then H.264 is used."
            : vm.DisplayCodec == "auto" ? "Auto uses HEVC (sharper at the same bit rate) when this PC and the VM’s GPU support it, otherwise H.264."
            : "More bit rate gives a sharper picture in fast motion.");
    }

    /// <summary>Resolution (Adaptive, the presets, and whatever else is saved), refresh rate and bit rate.</summary>
    private void FillDisplayLists(VmInfo vm)
    {
        ResolutionCombo.Items.Clear();
        ResolutionCombo.Items.Add(new ComboBoxItem { Content = "Adaptive (follows the window)", Tag = "adaptive" });
        foreach (var (value, note) in Presets)
            ResolutionCombo.Items.Add(new ComboBoxItem { Content = value.Replace("x", " × ") + "  ·  " + note, Tag = value });
        if (vm.DisplayResolution != "adaptive" && Presets.All(p => p.Value != vm.DisplayResolution))
            ResolutionCombo.Items.Add(new ComboBoxItem { Content = vm.DisplayResolution.Replace("x", " × "), Tag = vm.DisplayResolution });
        Forms.SelectTag(ResolutionCombo, vm.DisplayResolution);

        RateCombo.Items.Clear();
        foreach (int rate in Rates.Contains(vm.DisplayHz) ? Rates : [.. Rates, vm.DisplayHz])
            RateCombo.Items.Add(new ComboBoxItem { Content = rate + " Hz", Tag = rate });
        Forms.SelectTag(RateCombo, vm.DisplayHz);

        BitrateCombo.Items.Clear();
        foreach (int rate in Bitrates.Contains(vm.DisplayBitrate) ? Bitrates : [.. Bitrates, vm.DisplayBitrate])
            BitrateCombo.Items.Add(new ComboBoxItem { Content = rate == 0 ? "Auto" : rate + " Mbit/s", Tag = rate });
        Forms.SelectTag(BitrateCombo, vm.DisplayBitrate);
    }

    private void Resolution_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || Vm is not { } vm || ResolutionCombo.SelectedItem is not ComboBoxItem { Tag: string resolution }) return;
        HostBridge.Send("setDisplayMode", ("vmId", vm.Id), ("resolution", resolution), ("hz", vm.DisplayHz));
    }

    private void Rate_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || Vm is not { } vm || RateCombo.SelectedItem is not ComboBoxItem { Tag: int hz }) return;
        HostBridge.Send("setDisplayMode", ("vmId", vm.Id), ("resolution", vm.DisplayResolution), ("hz", hz));
    }

    private void Stream_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || Vm is not { } vm || StreamCombo.SelectedItem is not ComboBoxItem { Tag: string mode }) return;
        HostBridge.Send("setPerformanceMode", ("vmId", vm.Id), ("enabled", mode == "performance"));
    }

    private void Codec_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || Vm is not { } vm || CodecCombo.SelectedItem is not ComboBoxItem { Tag: string codec }) return;
        HostBridge.Send("setVideoCodec", ("vmId", vm.Id), ("codec", codec));
    }

    private void Bitrate_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || Vm is not { } vm || BitrateCombo.SelectedItem is not ComboBoxItem { Tag: int mbps }) return;
        HostBridge.Send("setVideoBitrate", ("vmId", vm.Id), ("mbps", mbps));
    }

    // ---------------------------------------------------------------- Snapshots

    private static DiskChoice DiskChoiceOf(VmInfo vm) => S.DiskChoices.TryGetValue(vm.Id, out var picked) ? picked : DiskChoice.Current;

    private void RefreshSnapshots(VmInfo vm)
    {
        bool locked = vm.Running || vm.BuildingVhdx || vm.Busy;
        var choice = DiskChoiceOf(vm);
        var current = DiskChoice.Current.Resolve(vm);
        var selected = choice.Resolve(vm);

        if (_treeSignature != vm.SnapshotSignature)
        {
            _treeSignature = vm.SnapshotSignature;
            SnapshotWarn.Visibility = Visibility.Collapsed;
        }

        SnapshotCurrent.Text = "Current: " + DiskChoice.Current.Path(vm);
        SnapshotHelp.Text = locked ? "Stop the VM to select a different disk or manage snapshots."
            : !vm.HasSnapshots ? "No snapshots. This VM uses its original disk."
            : choice.IsCurrent ? "The next start resumes the current disk. A frozen disk will start in a new branch."
            : selected.BranchIndex >= 0 ? "The next start resumes this branch."
            : "The next start creates a new branch from this disk.";

        bool editable = selected.SnapshotOf(vm) is not null || selected.BranchOf(vm) is not null;
        SnapshotNew.IsEnabled = !locked;
        SnapshotRename.IsEnabled = !locked && editable;
        SnapshotDelete.IsEnabled = !locked && editable;

        string built = $"{vm.SnapshotSignature}|{current}|{selected}|{locked}";
        if (built == _treeBuilt) return;
        _treeBuilt = built;

        SnapshotTree.Children.Clear();

        void Add(DiskChoice value, int depth, string name, string meta)
        {
            var text = new StackPanel { Spacing = 1 };
            text.Children.Add(new TextBlock { Text = name + (value == current ? " (current)" : "") });
            if (meta.Length > 0) text.Children.Add(Forms.Caption(meta));

            var row = new RadioButton
            {
                GroupName = "disk",
                Content = text,
                IsChecked = value == selected,
                IsEnabled = !locked,
                Margin = new Thickness(depth * 28, 0, 0, 0),
            };
            row.Checked += (_, _) =>
            {
                if (_updating || Vm is not { } now) return;
                SnapshotWarn.Visibility = Visibility.Collapsed;

                // Picking the disk the VM is already on is "no choice": the next
                // start just resumes it.
                if (!now.HasSnapshots || value == DiskChoice.Current.Resolve(now)) S.DiskChoices.Remove(now.Id);
                else S.DiskChoices[now.Id] = value;
                Refresh();
            };
            SnapshotTree.Children.Add(row);
        }

        void AddBranches(IReadOnlyList<SnapshotBranch> branches, int snapIndex, int depth)
        {
            for (int b = 0; b < branches.Count; b++)
            {
                string meta = branches[b].Date.Length > 0 ? "Modified " + branches[b].Date : "";
                if (branches[b].SizeGb.Length > 0) meta += (meta.Length > 0 ? " · " : "") + branches[b].SizeGb + " GB including parent disks";
                Add(new(snapIndex, b), depth, branches[b].Name.Length > 0 ? branches[b].Name : "branch " + (b + 1), meta);
            }
        }

        // A snapshot sits under the one it was taken on, after that one's branches.
        void AddSnapshots(int parent, int depth)
        {
            for (int i = 0; i < vm.Snapshots.Count; i++)
            {
                if (vm.Snapshots[i].Parent != parent || depth > 64) continue;
                var snapshot = vm.Snapshots[i];
                Add(new(i, -1), depth, snapshot.Name, (snapshot.Date.Length > 0 ? "Created " + snapshot.Date + " · " : "") + "New branch on start");
                AddBranches(snapshot.Branches, i, depth + 1);
                AddSnapshots(i, depth + 1);
            }
        }

        Add(DiskChoice.Base, 0, "Base", vm.HasSnapshots ? "New branch on start" : "Original disk");
        if (vm.HasSnapshots)
        {
            AddBranches(vm.BaseBranches, -2, 1);
            AddSnapshots(-2, 1);
        }
    }

    /// <summary>The VM again after a question was open, if its snapshots are still the ones that were asked about.</summary>
    private VmInfo? Unchanged(string tree)
    {
        if (Vm is not { } vm || vm.Running || vm.BuildingVhdx) return null;
        if (vm.SnapshotSignature == tree) return vm;

        SnapshotWarn.Text = "Snapshots changed. Select the disk again.";
        SnapshotWarn.Visibility = Visibility.Visible;
        return null;
    }

    private async void SnapshotNew_Click(object sender, RoutedEventArgs e)
    {
        if (Vm is not { } vm || vm.Running || vm.BuildingVhdx) return;
        string tree = vm.SnapshotSignature;

        string? name = await Dialogs.Prompt("New snapshot",
            "Freeze the current disk as a new snapshot; the VM continues on a new branch of it. Snapshots are frozen points in time that you can create independent branches from.",
            "Snapshot name", "Snapshot " + (vm.Snapshots.Count + 1), "Create");
        if (name is null || Unchanged(tree) is not { } now) return;
        HostBridge.Send("snapTake", ("vmId", now.Id), ("name", name));
    }

    private async void SnapshotRename_Click(object sender, RoutedEventArgs e)
    {
        if (Vm is not { } vm || vm.Running || vm.BuildingVhdx) return;
        var selected = DiskChoiceOf(vm).Resolve(vm);
        string? old = selected.BranchOf(vm)?.Name ?? selected.SnapshotOf(vm)?.Name;
        if (old is null) return;
        string tree = vm.SnapshotSignature;

        string? name = await Dialogs.Prompt("Rename", "Enter a new name:", "Name", old, "Rename");
        if (name is null || name == old || Unchanged(tree) is not { } now) return;

        if (selected.BranchIndex >= 0)
            HostBridge.Send("snapRename", ("vmId", now.Id), ("snapIndex", selected.SnapIndex), ("name", name), ("branchIndex", selected.BranchIndex));
        else
            HostBridge.Send("snapRename", ("vmId", now.Id), ("snapIndex", selected.SnapIndex), ("name", name));
    }

    private async void SnapshotDelete_Click(object sender, RoutedEventArgs e)
    {
        if (Vm is not { } vm || vm.Running || vm.BuildingVhdx) return;
        var selected = DiskChoiceOf(vm).Resolve(vm);
        var branch = selected.BranchOf(vm);
        var snapshot = selected.SnapshotOf(vm);
        if (branch is null && snapshot is null) return;
        string tree = vm.SnapshotSignature;

        if (branch is null)
        {
            // A snapshot other snapshots were taken on holds their parent disk.
            var child = vm.Snapshots.FirstOrDefault(s => s.Parent == selected.SnapIndex);
            if (child is not null)
            {
                await Dialogs.Alert("Snapshot in use",
                    $"Snapshot \"{child.Name}\" was taken on \"{snapshot!.Name}\" and depends on its disk. Delete \"{child.Name}\" first.");
                return;
            }
        }

        bool confirmed = await Dialogs.Confirm(branch is not null ? "Delete branch?" : "Delete snapshot?",
            branch is not null ? $"Delete branch \"{branch.Name}\"? Its parent disk will be kept."
                               : $"Delete snapshot \"{snapshot!.Name}\" and all its branches?",
            "Delete", destructive: true);
        if (!confirmed || Unchanged(tree) is not { } now) return;

        S.DiskChoices.Remove(now.Id);
        if (branch is not null)
            HostBridge.Send("snapDeleteBranch", ("vmId", now.Id), ("snapIndex", selected.SnapIndex), ("branchIndex", selected.BranchIndex));
        else
            HostBridge.Send("snapDelete", ("vmId", now.Id), ("snapIndex", selected.SnapIndex));
    }

    // ---------------------------------------------------------------- Actions

    private void Sections_SelectionChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        if (_updating || sender.SelectedItem?.Tag is not string section) return;
        _section = section;
        _displayLists = _treeBuilt = "";
        Refresh();
    }

    private void Back_Click(object sender, RoutedEventArgs e) => MainWindow.Current?.ShowLibrary();

    private void Primary_Click(object sender, RoutedEventArgs e) { if (Vm is { } vm) VmActions.RunPrimary(vm); }

    private void ShutDown_Click(object sender, RoutedEventArgs e) { if (Vm is { } vm) VmActions.ShutDown(vm); }

    private void More_Click(object sender, RoutedEventArgs e)
    {
        if (Vm is { } vm)
            VmActions.Menu(vm, onVmPage: true).ShowAt(MoreButton, new Microsoft.UI.Xaml.Controls.Primitives.FlyoutShowOptions { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedRight });
    }

    private void Edit_Click(object sender, RoutedEventArgs e) => _ = EditVmDialog.ShowAsync(_id);

    private void UpdateTools_Click(object sender, RoutedEventArgs e) { if (Vm is { } vm) _ = VmActions.UpdateGuestTools(vm); }

    private void OpenFolder_Click(object sender, RoutedEventArgs e) => HostBridge.Send("openVmFolder", ("vmId", _id));

    private void CopyPath_Click(object sender, RoutedEventArgs e) => HostBridge.Send("copyVmPath", ("vmId", _id));

    private void ErrorBar_Close(InfoBar sender, object args) => HostBridge.Send("dismissVmError", ("vmId", _id));
}
