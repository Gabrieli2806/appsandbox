using System.Text.RegularExpressions;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;

namespace AppSandbox.Views;

/// <summary>The host's log lines as they arrive. The file on disk has them all; this keeps the newest.</summary>
public sealed partial class ActivityPage : Page
{
    private static AppState S => AppState.Current;

    private static readonly FontFamily Mono = new("Cascadia Mono, Consolas");
    private static readonly Regex ErrorLine = new(@"^\s*error\b|\bfailed\b", RegexOptions.IgnoreCase | RegexOptions.Compiled);

    public ActivityPage()
    {
        InitializeComponent();

        Loaded += (_, _) =>
        {
            Lines.Items.Clear();
            foreach (string line in S.Log) Lines.Items.Add(Row(line));
            S.LogAdded += OnLine;
            S.LogCleared += OnCleared;
            UpdateSummary();
            ScrollToEnd();
        };
        Unloaded += (_, _) =>
        {
            S.LogAdded -= OnLine;
            S.LogCleared -= OnCleared;
        };
    }

    private static TextBlock Row(string line)
    {
        var row = new TextBlock
        {
            Text = line,
            FontFamily = Mono,
            FontSize = 12,
            TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true,
        };
        if (ErrorLine.IsMatch(line)) row.Foreground = Forms.Brush("SystemFillColorCriticalBrush");
        return row;
    }

    private void OnLine(string line)
    {
        // Follow new lines only while the end is in view: someone reading
        // further up is not pulled away.
        bool atEnd = AtEnd();
        Lines.Items.Add(Row(line));
        while (Lines.Items.Count > AppState.MaxLogLines) Lines.Items.RemoveAt(0);
        UpdateSummary();
        if (atEnd) ScrollToEnd();
    }

    private void OnCleared()
    {
        Lines.Items.Clear();
        UpdateSummary();
    }

    private ScrollViewer? Scroller => FindChild<ScrollViewer>(Lines);

    private bool AtEnd() => Scroller is not { } scroller || scroller.VerticalOffset >= scroller.ScrollableHeight - 24;

    private void ScrollToEnd()
    {
        if (Lines.Items.Count > 0) Lines.ScrollIntoView(Lines.Items[^1]);
    }

    private void UpdateSummary()
        => SummaryText.Text = Lines.Items.Count == 0 ? "Nothing yet" : Lines.Items.Count == 1 ? "1 line" : Lines.Items.Count + " lines";

    private static T? FindChild<T>(DependencyObject parent) where T : DependencyObject
    {
        for (int i = 0; i < VisualTreeHelper.GetChildrenCount(parent); i++)
        {
            var child = VisualTreeHelper.GetChild(parent, i);
            if (child is T found) return found;
            if (FindChild<T>(child) is { } deeper) return deeper;
        }
        return null;
    }

    private void Copy_Click(object sender, RoutedEventArgs e)
    {
        var data = new DataPackage();
        data.SetText(string.Join("\r\n", S.Log));
        Clipboard.SetContent(data);
        MainWindow.Current?.ShowNotice(new("info", "", $"Activity copied ({S.Log.Count} lines)."));
    }

    private void Clear_Click(object sender, RoutedEventArgs e) => S.ClearLog();

    private void ShowFile_Click(object sender, RoutedEventArgs e) => HostBridge.Send("openFolder", ("which", "log"));
}
