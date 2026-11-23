using System.Text.Json;

namespace AppSandbox.Services;

/// <summary>
/// How the window looks, and the answers to "don't ask again". Everything
/// about VMs is kept by the host in %ProgramData%\AppSandbox\vms.cfg; this is
/// only the shell's own, per user.
/// </summary>
public sealed class UiSettings
{
    public string Theme { get; set; } = "System";
    public string Backdrop { get; set; } = "Mica";
    public bool SkipForceStopWarning { get; set; }

    private static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AppSandbox");
    private static string File => Path.Combine(Folder, "shell.json");

    public static UiSettings Current { get; } = Load();

    private static UiSettings Load()
    {
        UiSettings value;
        try { value = JsonSerializer.Deserialize<UiSettings>(System.IO.File.ReadAllText(File)) ?? new(); }
        catch { value = new(); }

        if (value.Theme is not ("Light" or "Dark")) value.Theme = "System";
        if (value.Backdrop is not ("Mica" or "MicaAlt" or "Acrylic" or "None")) value.Backdrop = "Mica";
        return value;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Folder);
            System.IO.File.WriteAllText(File + ".tmp", JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
            System.IO.File.Move(File + ".tmp", File, true);
        }
        catch (Exception ex)
        {
            Log("Settings not saved: " + ex.Message);
        }
    }

    /// <summary>Problems of the shell itself (the VM log is the host's appsandbox.log).</summary>
    public static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(Folder);
            string path = Path.Combine(Folder, "shell.log");
            if (System.IO.File.Exists(path) && new FileInfo(path).Length > 1024 * 1024) System.IO.File.Delete(path);
            System.IO.File.AppendAllText(path, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {message}\n");
        }
        catch { }
    }
}
