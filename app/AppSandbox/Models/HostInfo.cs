using System.Text.Json;

namespace AppSandbox.Models;

public sealed record GpuInfo(string Id, string Name, string Location);

public sealed record TemplateInfo(string Name, string OsType);

/// <summary>The PC and the app's own settings, as the host reports them (build_host_info_json in ui.c).</summary>
public sealed class HostInfo
{
    public int HostCores, HostRamMb, VmCores, VmRamMb, FreeGb = -1, VmHddGb, ViewerRenderer = 11;
    public string DefaultDiskDirectory = "", AppVersion = "", ProjectRepo = "", UpstreamRepo = "";
    public string DataDirectory = "", LogFile = "", AppDirectory = "";
    public bool CloseToTrayWithoutAsking, AutoUpdateGuestTools = true, ViewerTitleStats, ViewerToolbar, HevcDecoder, Packaged;
    public List<GpuInfo> Gpus = [];

    public static HostInfo Parse(JsonElement e) => new()
    {
        HostCores = e.Int("hostCores"), HostRamMb = e.Int("hostRamMb"), VmCores = e.Int("vmCores"), VmRamMb = e.Int("vmRamMb"),
        FreeGb = e.Int("freeGb", -1), VmHddGb = e.Int("vmHddGb"), DefaultDiskDirectory = e.Str("defaultDiskDirectory"),
        CloseToTrayWithoutAsking = e.Bool("closeToTrayWithoutAsking"), AutoUpdateGuestTools = e.Bool("autoUpdateGuestTools", true),
        ViewerTitleStats = e.Bool("viewerTitleStats"), ViewerToolbar = e.Bool("viewerToolbar"),
        ViewerRenderer = e.Int("viewerRenderer", 11), HevcDecoder = e.Bool("hevcDecoder"),
        AppVersion = e.Str("appVersion"), Packaged = e.Bool("packaged"),
        ProjectRepo = e.Str("projectRepo"), UpstreamRepo = e.Str("upstreamRepo"),
        DataDirectory = e.Str("dataDirectory"), LogFile = e.Str("logFile"), AppDirectory = e.Str("appDirectory"),
        Gpus = [.. e.Items("gpus").Select(g => new GpuInfo(g.Str("id"), g.Str("name"), g.Str("location")))],
    };
}

public sealed record McpCall(string Time, string Tool, string Vm, bool Ok);

/// <summary>The MCP server for AI agents (mcpStatus).</summary>
public sealed class McpStatus
{
    public bool Enabled, ReadOnly, Listening;
    public string Name = "appsandbox", Url = "http://127.0.0.1:47841/mcp", Error = "", LastClient = "";
    public List<McpCall> Activity = [];

    public static McpStatus Parse(JsonElement e) => new()
    {
        Enabled = e.Bool("enabled"), ReadOnly = e.Bool("readOnly"), Listening = e.Bool("listening"),
        Name = e.Str("name", "appsandbox"), Url = e.Str("url", "http://127.0.0.1:47841/mcp"),
        Error = e.Str("error"), LastClient = e.Str("lastClient"),
        Activity = [.. e.Items("activity").Select(a => new McpCall(a.Str("time"), a.Str("tool"), a.Str("vm"), a.Bool("ok", true)))],
    };
}
