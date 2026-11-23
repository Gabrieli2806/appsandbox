using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace AppSandbox.Views;

/// <summary>Memory, processors, GPU and network of a stopped VM.</summary>
internal static class EditVmDialog
{
    private const int MinimumRamMb = 4000;

    public static async Task ShowAsync(string vmId)
    {
        var state = AppState.Current;
        if (state.Find(vmId) is not { } vm) return;

        var ram = Forms.Number(vm.RamMb, 512, 2);
        var cores = Forms.Number(vm.CpuCores, 1);
        var gpu = Forms.GpuCombo(vm.GpuMode, vm.GpuId);
        var shownGpu = Forms.Gpu(gpu);
        var network = Forms.NetworkCombo(vm.NetworkMode);
        var warning = new TextBlock { Foreground = Forms.Brush("SystemFillColorCriticalBrush"), TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };

        var host = state.Host;
        var panel = Forms.Stack(14,
            Forms.Row("Memory", Forms.Line(ram, new TextBlock { Text = "MB" }), Forms.Caption($"This PC has {host.HostRamMb} MB; running VMs use {host.VmRamMb} MB.")),
            Forms.Row("Processors", cores, Forms.Caption($"This PC has {host.HostCores} cores; running VMs use {host.VmCores}.")),
            Forms.Row("GPU", gpu),
            Forms.Row("Network", network),
            warning);
        panel.MinWidth = 440;

        // A manual-install VM has no display driver to use a GPU with.
        if (vm.IsOther) gpu.IsEnabled = false;

        var dialog = new ContentDialog
        {
            Title = "Edit " + vm.Name,
            Content = panel,
            PrimaryButtonText = "Save",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        // What the VM had when the dialog opened. Hardware can also change
        // through the local API or an AI agent while it is open.
        int ram0 = vm.RamMb, cores0 = vm.CpuCores, net0 = vm.NetworkMode, gpuMode0 = vm.GpuMode;
        string gpuId0 = vm.GpuId;

        string? Problem()
        {
            var now = state.Find(vmId);
            if (now is null) return "This VM no longer exists.";
            if (now.Running || now.BuildingVhdx || now.Starting) return "Stop the VM before editing its configuration.";
            if (now.RamMb != ram0 || now.CpuCores != cores0 || now.NetworkMode != net0 || now.GpuMode != gpuMode0 || now.GpuId != gpuId0)
                return "These settings changed in another interface. Reopen this editor to review the latest values.";
            if (Forms.Whole(cores) is not >= 1) return "CPU cores must be a whole number of at least 1.";
            if (Forms.Whole(ram) is not { } mb || (mb != ram0 && mb < MinimumRamMb)) return $"RAM must be a whole number of at least {MinimumRamMb} MB.";
            return null;
        }

        void Validate()
        {
            string? problem = Problem();
            warning.Text = problem ?? "";
            warning.Visibility = problem is null ? Visibility.Collapsed : Visibility.Visible;
            dialog.IsPrimaryButtonEnabled = problem is null;
        }

        ram.ValueChanged += (_, _) => Validate();
        cores.ValueChanged += (_, _) => Validate();
        state.Changed += Validate;
        Validate();

        try
        {
            if (await Dialogs.ShowAsync(dialog) != ContentDialogResult.Primary || Problem() is not null) return;
        }
        finally
        {
            state.Changed -= Validate;
        }

        // One command per value that changed: the host checks each on its own.
        int newRam = Forms.Whole(ram) ?? ram0, newCores = Forms.Whole(cores) ?? cores0;
        if (newRam != ram0) HostBridge.Send("editVm", ("vmId", vmId), ("field", "ramMb"), ("value", Forms.AlignRam(newRam).ToString()));
        if (newCores != cores0) HostBridge.Send("editVm", ("vmId", vmId), ("field", "cpuCores"), ("value", newCores.ToString()));

        int newNet = Forms.TagOf(network, net0);
        if (newNet != net0) HostBridge.Send("editVm", ("vmId", vmId), ("field", "networkMode"), ("value", newNet.ToString()));

        // Against what the list showed at first: a VM whose GPU is no longer in
        // this PC shows "Default GPU", and that alone is not a change.
        var (newGpuMode, newGpuId) = Forms.Gpu(gpu);
        if (newGpuMode != shownGpu.Mode || !string.Equals(newGpuId, shownGpu.Id, StringComparison.OrdinalIgnoreCase))
            HostBridge.Send("editVm", ("vmId", vmId), ("field", "gpuMode"), ("value", newGpuMode.ToString()), ("gpuId", newGpuId));
    }
}
