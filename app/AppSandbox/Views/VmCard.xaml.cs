using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace AppSandbox.Views;

public sealed partial class VmCard : UserControl
{
    private VmInfo _vm = new();

    public string VmId => _vm.Id;

    public VmCard(VmInfo vm)
    {
        InitializeComponent();
        Update(vm);
    }

    /// <summary>Brings the card in line with the VM, in place: a progress tick must not rebuild its buttons.</summary>
    public void Update(VmInfo vm)
    {
        _vm = vm;
        var state = AppState.Current;

        NameText.Text = vm.Name;
        SubText.Text = vm.OsType + " · " + vm.GpuLabel;

        var image = VmIcons.Get(vm);
        IconImage.Source = image;
        IconImage.Visibility = image is null ? Visibility.Collapsed : Visibility.Visible;
        OsGlyph.Glyph = VmActions.OsGlyph(vm);
        OsGlyph.Visibility = image is null ? Visibility.Visible : Visibility.Collapsed;

        StatusBadge.Apply(vm, StatusText, StatusDot, StatusRing);

        Chips.Children.Clear();
        foreach (string fact in new[]
                 {
                     vm.CpuCores + " vCPU",
                     VmInfo.RamLabel(vm.RamMb),
                     vm.HddGb + " GB disk",
                     VmInfo.NetworkName(vm.NetworkMode) + (vm.NetworkMode == 0 ? "" : " network"),
                 })
        {
            Chips.Children.Add(new Border
            {
                Style = (Style)Application.Current.Resources["ChipStyle"],
                Child = new TextBlock { Text = fact, Style = (Style)Application.Current.Resources["ChipTextStyle"] },
            });
        }

        var primary = VmActions.Primary(vm);
        PrimaryText.Text = primary.Label;
        PrimaryGlyph.Glyph = primary.Glyph;
        PrimaryButton.IsEnabled = primary.Enabled;
        PrimaryButton.Style = (Style)Application.Current.Resources[vm.Running ? "DefaultButtonStyle" : "AccentButtonStyle"];
        ToolTipService.SetToolTip(PrimaryButton, primary.Hint.Length > 0 ? primary.Hint : null);

        SshButton.Visibility = vm.SshEnabled ? Visibility.Visible : Visibility.Collapsed;
        SshButton.IsEnabled = vm.SshReady;
        ToolTipService.SetToolTip(SshButton, vm.SshHint);

        EditButton.IsEnabled = !vm.Running && !vm.Busy;

        if (vm.HasSnapshots)
        {
            var choice = state.DiskChoices.TryGetValue(vm.Id, out var picked) ? picked : DiskChoice.Current;
            DiskText.Text = "Disk: " + choice.Path(vm);
            DiskText.Visibility = Visibility.Visible;
        }
        else
        {
            DiskText.Visibility = Visibility.Collapsed;
        }

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
    }

    private void Open_Click(object sender, RoutedEventArgs e) => MainWindow.Current?.OpenVm(_vm.Id);

    private void Primary_Click(object sender, RoutedEventArgs e) => VmActions.RunPrimary(_vm);

    private void Ssh_Click(object sender, RoutedEventArgs e) => VmActions.OpenSsh(_vm);

    private void Edit_Click(object sender, RoutedEventArgs e) => _ = EditVmDialog.ShowAsync(_vm.Id);

    private void More_Click(object sender, RoutedEventArgs e)
        => VmActions.Menu(_vm, onVmPage: false).ShowAt(MoreButton, new Microsoft.UI.Xaml.Controls.Primitives.FlyoutShowOptions { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedRight });

    private void ErrorBar_Close(InfoBar sender, object args) => HostBridge.Send("dismissVmError", ("vmId", _vm.Id));
}

/// <summary>The state of a VM as a pill: a dot (or a ring while something is under way) and the words.</summary>
internal static class StatusBadge
{
    public static void Apply(VmInfo vm, TextBlock text, Microsoft.UI.Xaml.Shapes.Ellipse dot, ProgressRing ring)
    {
        var (label, kind) = vm.Status;
        text.Text = label;

        bool busy = kind == VmStatusKind.Busy;
        ring.IsActive = busy;
        ring.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        dot.Visibility = busy ? Visibility.Collapsed : Visibility.Visible;
        dot.Fill = Forms.Brush(kind switch
        {
            VmStatusKind.Running => "SystemFillColorSuccessBrush",
            VmStatusKind.Stopping => "SystemFillColorCautionBrush",
            _ => "TextFillColorTertiaryBrush",
        });
    }
}
