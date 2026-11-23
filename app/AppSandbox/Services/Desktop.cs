using System.Runtime.InteropServices;

namespace AppSandbox.Services;

/// <summary>The Win32 calls the shell needs around its windows.</summary>
public static class Desktop
{
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MinMaxInfo { public Point Reserved, MaxSize, MaxPosition, MinTrackSize, MaxTrackSize; }

    [DllImport("user32.dll", EntryPoint = "IsIconic")] public static extern bool IsMinimized(nint window);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(nint window);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(nint window);
    [DllImport("user32.dll")] public static extern uint GetDoubleClickTime();
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out global::Windows.Graphics.PointInt32 point);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint RegisterWindowMessage(string message);
    [DllImport("user32.dll")] public static extern bool ChangeWindowMessageFilterEx(nint window, uint message, uint action, nint change);

    public delegate nint SubclassProc(nint window, uint message, nuint wp, nint lp, nuint id, nuint data);
    [DllImport("comctl32.dll")] public static extern bool SetWindowSubclass(nint window, SubclassProc proc, nuint id, nuint data);
    [DllImport("comctl32.dll")] public static extern bool RemoveWindowSubclass(nint window, SubclassProc proc, nuint id);
    [DllImport("comctl32.dll")] public static extern nint DefSubclassProc(nint window, uint message, nuint wp, nint lp);

    /// <summary>
    /// Lets a message from a process that is not elevated reach this (elevated)
    /// window. Explorer is one: without this the tray icon would not come back
    /// after Explorer restarts.
    /// </summary>
    public static void AllowFromLowerIntegrity(nint window, uint message) => ChangeWindowMessageFilterEx(window, message, 1, 0);
}
