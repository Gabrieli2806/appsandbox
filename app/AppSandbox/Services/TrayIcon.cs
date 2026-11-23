using System.Runtime.InteropServices;

namespace AppSandbox.Services;

/// <summary>The notification-area icon: its tooltip, and the balloons that report what happens while the window is hidden.</summary>
public sealed class TrayIcon : IDisposable
{
    public const uint CallbackMessage = 0x8000 + 24;
    public static uint TaskbarCreated { get; } = Desktop.RegisterWindowMessage("TaskbarCreated");

    private const uint NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2;
    private const uint NIF_MESSAGE = 0x01, NIF_ICON = 0x02, NIF_TIP = 0x04, NIF_INFO = 0x10;
    private const uint NIIF_INFO = 0x01, NIIF_WARNING = 0x02, NIIF_NOSOUND = 0x10;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NotifyIconData
    {
        public int Size; public nint Window; public uint Id, Flags, Message; public nint Icon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Tip;
        public uint State, StateMask;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string Info;
        public uint Version;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string InfoTitle;
        public uint InfoFlags; public Guid Guid; public nint BalloonIcon;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern bool Shell_NotifyIcon(uint message, ref NotifyIconData data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint LoadImage(nint instance, string name, uint type, int cx, int cy, uint load);
    [DllImport("user32.dll")] private static extern bool DestroyIcon(nint icon);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);

    private NotifyIconData _data;
    private string _tip = "";

    public TrayIcon(nint hwnd)
    {
        // The small-icon metric, so the icon is not a 32-pixel image squeezed
        // into a 16-pixel slot (or the other way round on a scaled display).
        int size = Math.Max(16, GetSystemMetrics(49));
        _data = new()
        {
            Size = Marshal.SizeOf<NotifyIconData>(),
            Window = hwnd,
            Id = 1,
            Flags = NIF_MESSAGE | NIF_ICON | NIF_TIP,
            Message = CallbackMessage,
            Icon = LoadImage(0, Path.Combine(AppContext.BaseDirectory, "Assets", "AppSandbox.ico"), 1, size, size, 0x10),
            Tip = "App Sandbox",
            Info = "",
            InfoTitle = "",
        };
        Add();
    }

    public void Add()
    {
        if (!Shell_NotifyIcon(NIM_ADD, ref _data)) UiSettings.Log("Shell_NotifyIcon failed");
    }

    public void Update(string status)
    {
        string tip = status.Length > 0 ? "App Sandbox · " + status : "App Sandbox";
        if (_tip == tip) return;

        _tip = tip;
        _data.Tip = tip.Length > 127 ? tip[..127] : tip;
        _data.Flags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        Shell_NotifyIcon(NIM_MODIFY, ref _data);
    }

    /// <summary>A quiet Windows notification from the tray icon — the way the rest of the family reports background events.</summary>
    public void ShowBalloon(string title, string message, bool warning)
    {
        var balloon = _data;
        balloon.Flags = NIF_INFO;
        balloon.InfoTitle = title.Length > 63 ? title[..63] : title;
        balloon.Info = message.Length > 255 ? message[..255] : message;
        balloon.InfoFlags = (warning ? NIIF_WARNING : NIIF_INFO) | NIIF_NOSOUND;
        Shell_NotifyIcon(NIM_MODIFY, ref balloon);
    }

    public void Dispose()
    {
        Shell_NotifyIcon(NIM_DELETE, ref _data);
        if (_data.Icon != 0)
        {
            DestroyIcon(_data.Icon);
            _data.Icon = 0;
        }
    }
}
