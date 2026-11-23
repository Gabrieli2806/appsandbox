using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace AppSandbox.Services;

/// <summary>
/// What can be done to a VM, with the questions that go first. Cards, the VM
/// page and menus all come here, so each action asks the same thing wherever
/// it is started from.
/// </summary>
public static class VmActions
{
    private static AppState S => AppState.Current;

    /// <summary>Connect when it runs, Start when it does not: the button a VM leads with.</summary>
    public static (string Label, string Glyph, bool Enabled, string Hint) Primary(VmInfo vm)
    {
        if (vm.Running) return ("Connect", "", !vm.Busy && !vm.IsTemplate, "Open the VM's window");
        bool enabled = !vm.Busy && !S.Exiting && S.CanUseVms;
        return ("Start", "", enabled, S.Exiting ? "App Sandbox is shutting its VMs down to exit" : "");
    }

    public static void RunPrimary(VmInfo vm)
    {
        if (vm.Running) Connect(vm);
        else _ = Start(vm);
    }

    public static void Connect(VmInfo vm) => HostBridge.Send("connectIddVm", ("vmId", vm.Id));

    /// <summary>
    /// Starts the VM from the disk picked in its snapshot list. A frozen disk
    /// (the base or a snapshot) is never written to: the VM starts on a new
    /// branch of it, which gets a name first.
    /// </summary>
    public static async Task Start(VmInfo vm)
    {
        if (vm.Running || vm.BuildingVhdx) return;

        var choice = S.DiskChoices.TryGetValue(vm.Id, out var picked) ? picked : DiskChoice.Current;
        if (!choice.StartsNewBranch)
        {
            HostBridge.Send("startVm", ("vmId", vm.Id), ("snapIndex", choice.SnapIndex), ("branchIndex", choice.BranchIndex));
            return;
        }

        string tree = vm.SnapshotSignature;
        string parent = choice.SnapIndex == -2 ? "Base" : choice.SnapshotOf(vm)?.Name ?? "the snapshot";
        string? name = await Dialogs.Prompt("New branch",
            $"A new branch will be created from {parent}. Branches are independent working copies — changes in one branch don’t affect others or modify the base snapshot.",
            "Branch name", DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"), "Boot");
        if (name is null) return;

        // The question was open for a while: the VM may have changed under it.
        var now = S.Find(vm.Id);
        if (now is null || now.Running || now.BuildingVhdx) return;
        if (now.SnapshotSignature != tree)
        {
            await Dialogs.Alert("Snapshots changed", "Select the disk again before starting the VM.");
            return;
        }

        S.DiskChoices.Remove(vm.Id);
        HostBridge.Send("startVm", ("vmId", vm.Id), ("snapIndex", choice.SnapIndex), ("branchIndex", choice.BranchIndex), ("branchName", name));
    }

    public static void ShutDown(VmInfo vm) => HostBridge.Send("shutdownVm", ("vmId", vm.Id));

    public static async Task ForceStop(VmInfo vm)
    {
        if (vm.IsTemplate)
        {
            if (await Dialogs.Confirm("Cancel the template build?",
                    $"Stopping a template build deletes the incomplete template \"{vm.Name}\".", "Stop and delete", destructive: true))
            {
                // The host turns a running VM off as part of deleting it.
                HostBridge.Send("deleteVm", ("vmId", vm.Id));
            }
            return;
        }

        if (!UiSettings.Current.SkipForceStopWarning)
        {
            var (confirmed, dontAsk) = await Dialogs.ConfirmWithOptOut("Force stop?",
                $"Force stop turns \"{vm.Name}\" off at once, like pulling the plug. Unsaved work in it is lost and its disk can be damaged. Use Shut down when you can.",
                "Force stop");
            if (!confirmed) return;
            if (dontAsk)
            {
                UiSettings.Current.SkipForceStopWarning = true;
                UiSettings.Current.Save();
            }
        }

        HostBridge.Send("stopVm", ("vmId", vm.Id));
    }

    public static async Task Delete(VmInfo vm)
    {
        if (await Dialogs.Confirm("Delete VM?",
                $"Delete \"{vm.Name}\" with its disk and snapshots? This cannot be undone." +
                (vm.Running ? "\n\nThe VM is running; it is turned off first." : ""), "Delete", destructive: true))
        {
            HostBridge.Send("deleteVm", ("vmId", vm.Id));
        }
    }

    public static async Task UpdateGuestTools(VmInfo vm)
    {
        if (await Dialogs.Confirm("Update guest components",
                $"App Sandbox will open the disk of \"{vm.Name}\" and copy in this version's agent, helpers and display driver. The display driver installs during the next start.\n\nTip: take a snapshot first if you want a way back.",
                "Update"))
        {
            HostBridge.Send("updateGuestTools", ("vmId", vm.Id));
        }
    }

    public static void OpenSsh(VmInfo vm) => HostBridge.Send("sshConnect", ("vmId", vm.Id));

    /// <summary>The "more" menu of a VM: the same entries on its card and on its page.</summary>
    public static MenuFlyout Menu(VmInfo vm, bool onVmPage)
    {
        var menu = new MenuFlyout();

        void Add(string text, string glyph, bool enabled, Action run)
        {
            var item = new MenuFlyoutItem { Text = text, Icon = new FontIcon { Glyph = glyph }, IsEnabled = enabled };
            item.Click += (_, _) => run();
            menu.Items.Add(item);
        }

        bool busy = vm.Busy, running = vm.Running;

        if (!onVmPage) Add("Open settings", "", true, () => MainWindow.Current?.OpenVm(vm.Id));
        Add("Shut down", "", running && !busy, () => ShutDown(vm));
        Add("Force stop…", "", running && !busy, () => _ = ForceStop(vm));
        if (!vm.IsTemplate) Add("Snapshots…", "", !busy, () => MainWindow.Current?.OpenVm(vm.Id, "snapshots"));
        menu.Items.Add(new MenuFlyoutSeparator());

        Add("Export…", "", !running && !busy && !vm.IsTemplate, () => HostBridge.Send("exportVm", ("vmId", vm.Id)));
        if (vm.IsOther && vm.HasIso)
            Add("Remove installer ISO", "", !running && !busy, () => HostBridge.Send("ejectIso", ("vmId", vm.Id)));

        if (!vm.IsTemplate)
        {
            Add("Change icon…", "", true, () => HostBridge.Send("setVmIcon", ("vmId", vm.Id)));
            if (vm.IconVersion != 0) Add("Use default icon", "", true, () => HostBridge.Send("clearVmIcon", ("vmId", vm.Id)));
            Add("Create desktop shortcut", "", true, () => HostBridge.Send("createShortcut", ("vmId", vm.Id), ("where", "desktop")));
            Add("Add to Start menu", "", true, () => HostBridge.Send("createShortcut", ("vmId", vm.Id), ("where", "start")));
        }

        if (vm.VmFolder.Length > 0)
        {
            Add("Open VM folder", "", vm.PendingOp != "deleting", () => HostBridge.Send("openVmFolder", ("vmId", vm.Id)));
            Add("Copy VM folder path", "", true, () => HostBridge.Send("copyVmPath", ("vmId", vm.Id)));
        }

        if (vm.SshEnabled) Add("Open SSH terminal", "", vm.SshReady, () => OpenSsh(vm));

        menu.Items.Add(new MenuFlyoutSeparator());
        Add("Delete VM…", "", !busy, () => _ = Delete(vm));
        return menu;
    }

    /// <summary>The glyph a VM is known by when it has no picture of its own.</summary>
    public static string OsGlyph(VmInfo vm) => vm.IsWindows ? "" : "";
}
