using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace AppSandbox.Services;

/// <summary>
/// The app's dialogs. WinUI shows one ContentDialog at a time, so they queue:
/// an alert from the host waits for the question that is open.
/// </summary>
public static class Dialogs
{
    private static readonly SemaphoreSlim Gate = new(1, 1);

    private static XamlRoot? Root => MainWindow.Current?.Content?.XamlRoot;

    /// <summary>Shows a dialog when no other is open, in the app's theme.</summary>
    public static async Task<ContentDialogResult> ShowAsync(ContentDialog dialog)
    {
        await Gate.WaitAsync();
        try
        {
            // Started in the tray (a VM shortcut), the window's content has no
            // XamlRoot until it has been shown once: wait for it instead of
            // dropping what the host has to say.
            if (MainWindow.Current?.Content is FrameworkElement { XamlRoot: null } content)
            {
                var loaded = new TaskCompletionSource();
                void OnLoaded(object sender, RoutedEventArgs e) => loaded.TrySetResult();
                content.Loaded += OnLoaded;
                try { await loaded.Task; }
                finally { content.Loaded -= OnLoaded; }
            }

            if (Root is not { } root) return ContentDialogResult.None;
            dialog.XamlRoot = root;
            dialog.RequestedTheme = MainWindow.Current!.Theme;
            return await dialog.ShowAsync();
        }
        catch (Exception ex)
        {
            UiSettings.Log("Dialog: " + ex.Message);
            return ContentDialogResult.None;
        }
        finally
        {
            Gate.Release();
        }
    }

    private static TextBlock Body(string text) => new() { Text = text, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };

    public static Task Alert(string title, string message)
        => ShowAsync(new ContentDialog { Title = title, Content = Body(message), CloseButtonText = "OK", DefaultButton = ContentDialogButton.Close });

    /// <summary>An alert from the host: its own title, or one that fits its kind.</summary>
    public static Task Alert(HostNotice notice)
        => Alert(notice.Title.Length > 0 ? notice.Title : notice.Kind switch
        {
            "error" => "Something went wrong",
            "warning" => "Check this first",
            _ => "App Sandbox",
        }, notice.Message);

    public static async Task<bool> Confirm(string title, string message, string confirm, bool destructive = false)
    {
        var dialog = new ContentDialog
        {
            Title = title,
            Content = Body(message),
            PrimaryButtonText = confirm,
            CloseButtonText = "Cancel",
            DefaultButton = destructive ? ContentDialogButton.Close : ContentDialogButton.Primary,
        };
        return await ShowAsync(dialog) == ContentDialogResult.Primary;
    }

    /// <summary>A question with "Don't show this again"; the second value is whether it was ticked.</summary>
    public static async Task<(bool Confirmed, bool DontAskAgain)> ConfirmWithOptOut(string title, string message, string confirm)
    {
        var check = new CheckBox { Content = "Don't show this again" };
        var panel = new StackPanel { Spacing = 12 };
        panel.Children.Add(Body(message));
        panel.Children.Add(check);
        var dialog = new ContentDialog
        {
            Title = title,
            Content = panel,
            PrimaryButtonText = confirm,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        bool confirmed = await ShowAsync(dialog) == ContentDialogResult.Primary;
        return (confirmed, confirmed && check.IsChecked == true);
    }

    /// <summary>Asks for a name. Null when cancelled. Names go into vms.cfg lines: no brackets, backslashes or line breaks.</summary>
    public static async Task<string?> Prompt(string title, string message, string label, string value, string confirm)
    {
        var box = new TextBox { Header = label, Text = value, MaxLength = 127 };
        box.SelectAll();
        var panel = new StackPanel { Spacing = 12, MinWidth = 380 };
        panel.Children.Add(Body(message));
        panel.Children.Add(box);
        var dialog = new ContentDialog
        {
            Title = title,
            Content = panel,
            PrimaryButtonText = confirm,
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Primary,
        };

        string Clean() => new string([.. box.Text.Where(c => c is not ('\n' or '\r' or '\t' or '[' or ']' or '\\'))]).Trim();
        dialog.IsPrimaryButtonEnabled = Clean().Length > 0;
        box.TextChanged += (_, _) => dialog.IsPrimaryButtonEnabled = Clean().Length > 0;

        return await ShowAsync(dialog) == ContentDialogResult.Primary ? Clean() : null;
    }

    /// <summary>The licence or the notices: long text to read and scroll.</summary>
    public static async Task Document(string kind, string title, string text)
    {
        var dialog = new ContentDialog
        {
            Title = title,
            Content = new ScrollViewer
            {
                MaxHeight = 460,
                Content = new TextBlock
                {
                    Text = text,
                    TextWrapping = TextWrapping.Wrap,
                    IsTextSelectionEnabled = true,
                    FontFamily = new Microsoft.UI.Xaml.Media.FontFamily("Cascadia Mono, Consolas"),
                    FontSize = 12,
                },
            },
            PrimaryButtonText = "Open on GitHub",
            CloseButtonText = "Close",
            DefaultButton = ContentDialogButton.Close,
        };
        dialog.Resources["ContentDialogMaxWidth"] = 760.0;
        if (await ShowAsync(dialog) == ContentDialogResult.Primary) HostBridge.Send("openLink", ("which", kind));
    }
}
