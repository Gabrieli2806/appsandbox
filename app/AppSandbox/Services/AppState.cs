using System.Text.Json;

namespace AppSandbox.Services;

/// <summary>Whether VMs can be used yet: the host starts on a worker thread and may find Virtual Machine Platform off.</summary>
public enum CoreState { Starting, Ready, PrereqMissing, PrereqEnabling, PrereqReboot, PrereqFailed }

public sealed record HostNotice(string Kind, string Title, string Message);

/// <summary>
/// What the host has told the shell, kept for every page: the VMs, the PC, the
/// templates, the activity lines. Pages read it and listen to its events; they
/// never keep copies. Everything here lives on the UI thread.
/// </summary>
public sealed class AppState
{
    public static AppState Current { get; } = new();

    public const int MaxLogLines = 1000;

    public List<VmInfo> Vms { get; private set; } = [];
    public HostInfo Host { get; private set; } = new();
    public List<TemplateInfo> Templates { get; private set; } = [];
    public List<string> Adapters { get; private set; } = [];

    /// <summary>1-based index into Adapters of the one to offer first; 0 = automatic.</summary>
    public int DefaultAdapter { get; private set; }

    public CoreState Core { get; private set; } = CoreState.Starting;
    public int PrereqProgress { get; private set; }

    /// <summary>The app is shutting its VMs down in order to exit.</summary>
    public bool Exiting { get; private set; }
    public int ExitRemaining { get; private set; }
    public bool ExitForSession { get; private set; }

    public McpStatus Mcp { get; private set; } = new();
    public List<string> Log { get; } = [];

    /// <summary>The disk each VM starts from next, when the user picked one other than its current disk.</summary>
    public Dictionary<string, DiskChoice> DiskChoices { get; } = [];

    /// <summary>VMs, host figures, templates or the core state changed.</summary>
    public event Action? Changed;
    public event Action? ExitChanged;
    public event Action? McpChanged;
    public event Action<string>? LogAdded;
    public event Action? LogCleared;

    /// <summary>A passing message: shown in the notice bar.</summary>
    public event Action<HostNotice>? Notice;

    /// <summary>Something the user has to read: shown as a dialog.</summary>
    public event Action<HostNotice>? Alert;

    /// <summary>The answer to createVm: ok, or why not.</summary>
    public event Action<bool, string>? CreateResult;

    /// <summary>The licence or the third-party notices, to read in the app: kind, title, text.</summary>
    public event Action<string, string, string>? Document;

    /// <summary>A VM's custom icon as a data URL ("" = none).</summary>
    public event Action<string, string>? VmIcon;

    /// <summary>Free space of a folder asked for with getDiskSpace: path, request id, GB (-1 unknown).</summary>
    public event Action<string, int, int>? DiskSpace;

    public event Action<string>? IsoPicked;
    public event Action<string>? DiskDirectoryPicked;

    /// <summary>The host asks for the window to be shown, or hidden to the tray.</summary>
    public event Action<bool>? WindowRequested;

    /// <summary>The host has shut down: the process ends.</summary>
    public event Action? ExitRequested;

    public VmInfo? Find(string id) => Vms.FirstOrDefault(vm => vm.Id == id);

    public bool CanUseVms => Core == CoreState.Ready;

    public void Attach() => HostBridge.Message += OnMessage;

    /// <summary>The user asked for Virtual Machine Platform to be turned on: show it at once, before Windows reports progress.</summary>
    public void BeginPrereq()
    {
        PrereqProgress = 0;
        SetCore(CoreState.PrereqEnabling);
    }

    public void ClearLog()
    {
        Log.Clear();
        LogCleared?.Invoke();
    }

    private void OnMessage(JsonElement m)
    {
        switch (m.Str("type"))
        {
            case "vmListChanged":
                ReadVms(m);
                if (m.TryGetProperty("hostInfo", out var info)) Host = HostInfo.Parse(info);
                Changed?.Invoke();
                break;

            case "fullState":
                ReadVms(m);
                if (m.TryGetProperty("hostInfo", out var full)) Host = HostInfo.Parse(full);
                Adapters = [.. m.Items("adapters").Where(a => a.ValueKind == JsonValueKind.String).Select(a => a.GetString() ?? "")];
                DefaultAdapter = m.Int("defaultAdapter");
                ReadTemplates(m);
                Changed?.Invoke();
                break;

            case "hostInfo":
                Host = HostInfo.Parse(m);
                Changed?.Invoke();
                break;

            case "templates":
                ReadTemplates(m);
                Changed?.Invoke();
                break;

            case "coreLoading": SetCore(CoreState.Starting); break;
            case "coreReady": SetCore(CoreState.Ready); break;
            case "prereqRequired": SetCore(CoreState.PrereqMissing); break;
            case "prereqReboot": SetCore(CoreState.PrereqReboot); break;

            case "prereqProgress":
                PrereqProgress = m.Int("pct");
                SetCore(CoreState.PrereqEnabling);
                break;

            case "prereqResult":
                // ok without a restart: the host starts the core and says coreLoading itself.
                if (!m.Bool("ok")) SetCore(CoreState.PrereqFailed);
                else if (m.Bool("reboot")) SetCore(CoreState.PrereqReboot);
                break;

            case "log":
                string line = m.Str("message");
                Log.Add(line);
                if (Log.Count > MaxLogLines) Log.RemoveRange(0, Log.Count - MaxLogLines);
                LogAdded?.Invoke(line);
                break;

            case "alert": Alert?.Invoke(new(m.Str("kind", "error"), m.Str("title"), m.Str("message"))); break;
            case "notice": Notice?.Invoke(new(m.Str("kind", "info"), m.Str("title"), m.Str("message"))); break;
            case "createResult": CreateResult?.Invoke(m.Str("kind") == "ok", m.Str("message")); break;
            case "document": Document?.Invoke(m.Str("kind"), m.Str("title"), m.Str("message")); break;
            case "vmIcon": VmIcon?.Invoke(m.Str("vmId"), m.Str("data")); break;
            case "diskSpace": DiskSpace?.Invoke(m.Str("path"), m.Int("requestId"), m.Int("freeGb", -1)); break;
            case "browseResult": IsoPicked?.Invoke(m.Str("path")); break;
            case "diskDirectoryBrowseResult": DiskDirectoryPicked?.Invoke(m.Str("path")); break;

            case "exitProgress":
                Exiting = m.Bool("active");
                ExitRemaining = m.Int("remaining");
                ExitForSession = m.Bool("session");
                ExitChanged?.Invoke();
                Changed?.Invoke();
                break;

            case "mcpStatus":
                if (m.TryGetProperty("mcp", out var mcp)) Mcp = McpStatus.Parse(mcp);
                McpChanged?.Invoke();
                break;

            case "window": WindowRequested?.Invoke(m.Bool("show")); break;
            case "exit": ExitRequested?.Invoke(); break;
        }
    }

    private void SetCore(CoreState state)
    {
        Core = state;
        Changed?.Invoke();
    }

    private void ReadVms(JsonElement m)
    {
        var before = Vms.ToDictionary(vm => vm.Id, vm => vm.SnapshotSignature);
        Vms = [.. m.Items("vms").Select(VmInfo.Parse)];

        // A choice of disk is only good for the tree it was made in: once a
        // snapshot or branch is added or removed (here, or by an agent), the
        // same numbers name another disk.
        foreach (string id in DiskChoices.Keys.ToList())
        {
            var vm = Find(id);
            if (vm is null || !vm.HasSnapshots || !before.TryGetValue(id, out string? tree) || tree != vm.SnapshotSignature)
                DiskChoices.Remove(id);
        }
    }

    private void ReadTemplates(JsonElement m)
        => Templates = [.. m.Items("templates").Select(t => new TemplateInfo(t.Str("name"), t.Str("osType")))];
}
