using System.Runtime.InteropServices;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;

namespace AppSandbox.Views;

/// <summary>
/// The chrome for the small always-on-top windows: the tray menu.
/// </summary>
internal static class FloatingWindow
{
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] private static extern nint GetWindowLongPtr(nint hwnd, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] private static extern nint SetWindowLongPtr(nint hwnd, int index, nint value);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(nint hwnd, nint after, int x, int y, int cx, int cy, uint flags);
    [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(nint hwnd, int attribute, ref int value, int size);

    /// <summary>Borderless, always on top, out of Alt+Tab, rounded.</summary>
    public static void Configure(Window window)
    {
        if (window.AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.SetBorderAndTitleBar(false, false);
            presenter.IsAlwaysOnTop = true;
            presenter.IsResizable = false;
            presenter.IsMinimizable = false;
            presenter.IsMaximizable = false;
        }

        window.AppWindow.IsShownInSwitchers = false;
        StripFrame(WinRT.Interop.WindowNative.GetWindowHandle(window));
    }

    /// <summary>
    /// A borderless presenter still leaves the frame styles on the window, and
    /// they are drawn — without this the window carries a thin light rectangle.
    /// </summary>
    public static void StripFrame(nint hwnd)
    {
        long style = GetWindowLongPtr(hwnd, -16).ToInt64() & ~(0x00C00000L | 0x00040000L | 0x00800000L | 0x00400000L | 0x00080000L);
        SetWindowLongPtr(hwnd, -16, (nint)style);

        long extended = GetWindowLongPtr(hwnd, -20).ToInt64();
        SetWindowLongPtr(hwnd, -20, (nint)((extended & ~(0x100L | 0x200L | 0x20000L | 0x40000L)) | 0x80L));
        SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x37);

        int round = 2;
        DwmSetWindowAttribute(hwnd, 33, ref round, 4);
    }

    [DllImport("user32.dll")] private static extern bool SetLayeredWindowAttributes(nint hwnd, uint key, byte alpha, uint flags);

    /// <summary>A see-through sheet over a whole display: square corners, never the active window, as dark as the alpha says.</summary>
    public static void Veil(nint hwnd, byte alpha)
    {
        SetWindowLongPtr(hwnd, -20, (nint)(GetWindowLongPtr(hwnd, -20).ToInt64() | 0x08000000L | 0x00080000L));
        SetLayeredWindowAttributes(hwnd, 0, alpha, 2);
        int square = 1;
        DwmSetWindowAttribute(hwnd, 33, ref square, 4);
    }

    public static double Scale(Window window) => Math.Max(1, Services.Desktop.GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(window)) / 96.0);

    public static void Size(Window window, double width, double height)
        => window.AppWindow.Resize(new((int)Math.Ceiling(width * Scale(window)), (int)Math.Ceiling(height * Scale(window))));

    public static global::Windows.Graphics.PointInt32 Clamp(int x, int y, int width, int height)
    {
        var work = DisplayArea.GetFromPoint(new(x, y), DisplayAreaFallback.Nearest).WorkArea;
        return new(
            Math.Clamp(x, work.X + 8, Math.Max(work.X + 8, work.X + work.Width - width - 8)),
            Math.Clamp(y, work.Y + 8, Math.Max(work.Y + 8, work.Y + work.Height - height - 8)));
    }
}
