using System.Text.Json;

namespace AppSandbox.Models;

internal static class JsonRead
{
    public static string Str(this JsonElement e, string name, string fallback = "")
        => e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? fallback : fallback;

    public static int Int(this JsonElement e, string name, int fallback = 0)
        => e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int n) ? n : fallback;

    public static bool Bool(this JsonElement e, string name, bool fallback = false)
        => e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v)
            ? v.ValueKind == JsonValueKind.True || (v.ValueKind != JsonValueKind.False && fallback)
            : fallback;

    public static IEnumerable<JsonElement> Items(this JsonElement e, string name)
        => e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array ? v.EnumerateArray() : [];
}

/// <summary>A branch of a snapshot (or of the base disk): a disk the VM can keep working on.</summary>
public sealed record SnapshotBranch(string Name, string Date, string SizeGb);

/// <summary>A frozen disk. Parent is the index of the snapshot it was taken on, or -2 for the base disk.</summary>
public sealed record Snapshot(string Name, string Date, int Parent, IReadOnlyList<SnapshotBranch> Branches);

public enum VmStatusKind { Stopped, Running, Busy, Stopping }

/// <summary>One VM as the host reports it in vmListChanged (build_vm_json in ui.c).</summary>
public sealed class VmInfo
{
    public string Id = "", Name = "", OsType = "", DiskDirectory = "", GpuId = "", GpuName = "", NetAdapter = "";
    public string VmFolder = "", NatIp = "", LastError = "", PendingOp = "";
    public int IconVersion, RamMb, HddGb, CpuCores, GpuMode, NetworkMode, VhdxProgress, SshPort, SshState, CopyProgress;
    public bool HasIso, Running, Starting, ShuttingDown, AgentOnline, IsTemplate, HypervVideoOff, BuildingVhdx, VhdxStaging;
    public bool InstallComplete, SshEnabled, SshDeployKey, SshKeyDeployed, CopyingDisk, UpdatingTools, GuestToolsOutdated;

    // Windows guests only.
    public string DisplayResolution = "adaptive", DisplayCodec = "auto", DisplayVideoCodec = "";
    public int DisplayHz = 60, DisplayDriver = -1, DisplayVideo = -1, DisplayBitrate;
    public bool DisplayPerformance;

    public int SnapCurrent = -1, SnapCurrentBranch = -1;
    public bool HasSnapshots;
    public List<SnapshotBranch> BaseBranches = [];
    public List<Snapshot> Snapshots = [];

    /// <summary>The raw JSON of the snapshot tree: two trees are the same when this is.</summary>
    public string SnapshotSignature = "";

    public static VmInfo Parse(JsonElement e)
    {
        var vm = new VmInfo
        {
            Id = e.Str("id"), Name = e.Str("name"), OsType = e.Str("osType"),
            IconVersion = e.Int("iconVersion"), HasIso = e.Bool("hasIso"), DiskDirectory = e.Str("diskDirectory"),
            Running = e.Bool("running"), Starting = e.Bool("starting"), ShuttingDown = e.Bool("shuttingDown"),
            AgentOnline = e.Bool("agentOnline"), RamMb = e.Int("ramMb"), HddGb = e.Int("hddGb"), CpuCores = e.Int("cpuCores"),
            GpuMode = e.Int("gpuMode"), GpuId = e.Str("gpuId"), GpuName = e.Str("gpuName"),
            NetworkMode = e.Int("networkMode"), NetAdapter = e.Str("netAdapter"), IsTemplate = e.Bool("isTemplate"),
            HypervVideoOff = e.Bool("hypervVideoOff"), BuildingVhdx = e.Bool("buildingVhdx"), VhdxStaging = e.Bool("vhdxStaging"),
            VhdxProgress = e.Int("vhdxProgress"), InstallComplete = e.Bool("installComplete"),
            SshEnabled = e.Bool("sshEnabled"), SshPort = e.Int("sshPort"), SshState = e.Int("sshState"),
            SshDeployKey = e.Bool("sshDeployKey"), SshKeyDeployed = e.Bool("sshKeyDeployed"),
            CopyingDisk = e.Bool("copyingDisk"), CopyProgress = e.Int("copyProgress"),
            VmFolder = e.Str("vmFolder"), NatIp = e.Str("natIp"), LastError = e.Str("lastError"), PendingOp = e.Str("pendingOp"),
            DisplayResolution = e.Str("displayResolution", "adaptive"), DisplayHz = e.Int("displayHz", 60),
            DisplayDriver = e.Int("displayDriver", -1), DisplayPerformance = e.Bool("displayPerformance"),
            DisplayVideo = e.Int("displayVideo", -1), DisplayCodec = e.Str("displayCodec", "auto"),
            DisplayVideoCodec = e.Str("displayVideoCodec"), DisplayBitrate = e.Int("displayBitrate"),
            UpdatingTools = e.Bool("updatingTools"), GuestToolsOutdated = e.Bool("guestToolsOutdated"),
            SnapCurrent = e.Int("snapCurrent", -1), SnapCurrentBranch = e.Int("snapCurrentBranch", -1),
            HasSnapshots = e.Bool("hasSnapshots"),
        };

        static List<SnapshotBranch> Branches(JsonElement owner, string name)
            => [.. owner.Items(name).Select(b => new SnapshotBranch(b.Str("name"), b.Str("date"), b.Str("sizeGb")))];

        vm.BaseBranches = Branches(e, "baseBranches");
        vm.Snapshots = [.. e.Items("snapshots").Select(s => new Snapshot(s.Str("name"), s.Str("date"), s.Int("parent", -2) >= 0 ? s.Int("parent") : -2, Branches(s, "branches")))];

        string Raw(string name) => e.TryGetProperty(name, out var v) ? v.GetRawText() : "";
        vm.SnapshotSignature = vm.HasSnapshots + "|" + Raw("baseBranches") + "|" + Raw("snapshots");
        return vm;
    }

    // ---- What the old page derived from these fields ----

    public bool IsWindows => OsType == "Windows";
    public bool IsOther => OsType == "Other";

    /// <summary>Something is being done to the VM that its buttons have to wait for.</summary>
    public bool Busy => BuildingVhdx || Starting || UpdatingTools || CopyingDisk || PendingOp.Length > 0;

    public (string Label, VmStatusKind Kind) Status
    {
        get
        {
            if (PendingOp == "deleting") return ("Deleting", VmStatusKind.Busy);
            if (PendingOp == "stopping") return ("Stopping", VmStatusKind.Stopping);
            if (PendingOp == "shuttingDown") return ("Shutting down", VmStatusKind.Stopping);
            if (CopyingDisk) return ((InstallComplete ? "Exporting " : "Importing ") + CopyProgress + "%", VmStatusKind.Busy);
            if (UpdatingTools) return ("Updating components", VmStatusKind.Busy);
            if (Starting) return ("Starting", VmStatusKind.Busy);
            if (BuildingVhdx) return (VhdxStaging ? "Staging files" : "Building disk " + VhdxProgress + "%", VmStatusKind.Busy);
            if (Running && ShuttingDown) return ("Shutting down", VmStatusKind.Stopping);
            if (Running && IsTemplate) return ("Building template", VmStatusKind.Busy);
            if (Running && !InstallComplete) return ("Installing Windows", VmStatusKind.Busy);
            if (Running) return ("Running", VmStatusKind.Running);
            return (IsTemplate ? "Template" : "Stopped", VmStatusKind.Stopped);
        }
    }

    /// <summary>Building or copying a disk: there is a percentage to show.</summary>
    public int? Progress => CopyingDisk ? CopyProgress : BuildingVhdx && !VhdxStaging ? VhdxProgress : null;

    public string GpuLabel => GpuName.Length > 0 ? GpuName : GpuMode == 1 ? "Default GPU" : "No GPU";

    public static string RamLabel(int mb) => mb >= 1024 ? (Math.Round(mb / 102.4) / 10).ToString("0.#") + " GB" : mb + " MB";

    public static string NetworkName(int mode) => mode switch { 1 => "NAT", 2 => "External", 3 => "Internal", _ => "None" };

    public string NetworkLabel => NetworkMode == 2 && NetAdapter.Length > 0 ? "External · " + NetAdapter : NetworkName(NetworkMode);

    public bool SshReady => SshEnabled && SshState is 2 or 4 && Running && !Busy;

    public string SshHint => SshState switch
    {
        1 => "Installing OpenSSH in the guest...",
        4 => $"Open an SSH terminal (localhost:{SshPort}, key deployed)",
        2 => $"Open an SSH terminal (localhost:{SshPort})",
        3 => "SSH install failed",
        _ => "SSH: waiting for the in-VM agent",
    };

    /// <summary>Display settings exist for VMs with the App Sandbox display driver.</summary>
    public bool DisplaySupported => IsWindows && !IsTemplate;
}

/// <summary>
/// Which disk of a VM the next start uses: the current one, the base disk, a
/// snapshot (a new branch is made from it) or a branch. SnapIndex and
/// BranchIndex are what startVm takes: -1 current, -2 base.
/// </summary>
public readonly record struct DiskChoice(int SnapIndex, int BranchIndex)
{
    public static readonly DiskChoice Current = new(-1, -1);
    public static readonly DiskChoice Base = new(-2, -1);

    public bool IsCurrent => SnapIndex == -1;

    /// <summary>A frozen disk (the base or a snapshot): starting from it makes a new branch.</summary>
    public bool StartsNewBranch => !IsCurrent && BranchIndex < 0;

    /// <summary>The same disk named without "current": what the VM is on now.</summary>
    public DiskChoice Resolve(VmInfo vm) => IsCurrent ? Normalize(vm, new(vm.SnapCurrent, vm.SnapCurrentBranch)) : Normalize(vm, this);

    private static DiskChoice Normalize(VmInfo vm, DiskChoice c)
    {
        var snapshot = c.SnapIndex >= 0 && c.SnapIndex < vm.Snapshots.Count ? vm.Snapshots[c.SnapIndex] : null;
        var branches = snapshot?.Branches ?? vm.BaseBranches;
        bool branch = c.BranchIndex >= 0 && c.BranchIndex < branches.Count;
        if (branch) return new(snapshot is null ? -2 : c.SnapIndex, c.BranchIndex);
        return snapshot is null ? Base : new(c.SnapIndex, -1);
    }

    public Snapshot? SnapshotOf(VmInfo vm) => SnapIndex >= 0 && SnapIndex < vm.Snapshots.Count ? vm.Snapshots[SnapIndex] : null;

    public SnapshotBranch? BranchOf(VmInfo vm)
    {
        var branches = SnapshotOf(vm)?.Branches ?? (SnapIndex == -2 ? vm.BaseBranches : null);
        return branches is not null && BranchIndex >= 0 && BranchIndex < branches.Count ? branches[BranchIndex] : null;
    }

    /// <summary>"Base → Snapshot 1 → branch", as shown on the card and in the snapshot list.</summary>
    public string Path(VmInfo vm)
    {
        var resolved = Resolve(vm);
        string path = "Base";
        if (resolved.SnapshotOf(vm) is not null)
        {
            var chain = new List<string>();
            for (int s = resolved.SnapIndex; s >= 0 && s < vm.Snapshots.Count && chain.Count < vm.Snapshots.Count; s = vm.Snapshots[s].Parent)
                chain.Insert(0, vm.Snapshots[s].Name);
            path += " → " + string.Join(" → ", chain);
        }
        if (resolved.BranchOf(vm) is { } branch) path += " → " + (branch.Name.Length > 0 ? branch.Name : "branch " + (resolved.BranchIndex + 1));
        if (StartsNewBranch) path += " [new branch]";
        return path;
    }
}
