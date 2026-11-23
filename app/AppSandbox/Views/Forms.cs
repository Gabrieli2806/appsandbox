using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace AppSandbox.Views;

/// <summary>Pieces the dialogs and pages build their forms from, so they all line up the same way.</summary>
internal static class Forms
{
    public static Style Res(string key) => (Style)Application.Current.Resources[key];

    /// <summary>
    /// A theme brush by name, for the theme picked in Settings. A plain
    /// lookup in the application's resources answers for the Windows theme,
    /// which is another one when Settings pins Light or Dark.
    /// </summary>
    public static Brush Brush(string key)
    {
        string theme = (MainWindow.Current?.Theme ?? ElementTheme.Default) switch
        {
            ElementTheme.Light => "Light",
            ElementTheme.Dark => "Default",   // WinUI keeps its dark brushes under "Default"
            _ => "",
        };
        if (theme.Length == 0) return (Brush)Application.Current.Resources[key];

        // Looked up once per theme and name: the search walks every merged dictionary.
        if (!Themed.TryGetValue((theme, key), out Brush? brush))
        {
            var watch = System.Diagnostics.Stopwatch.StartNew();
            brush = Find(Application.Current.Resources, key, theme, 0) as Brush ?? (Brush)Application.Current.Resources[key];
            Themed[(theme, key)] = brush;
            if (watch.ElapsedMilliseconds > 50) UiSettings.Log($"Brush {key} ({theme}) took {watch.ElapsedMilliseconds} ms");
        }
        return brush;
    }

    private static readonly Dictionary<(string Theme, string Key), Brush> Themed = [];

    private static object? Find(ResourceDictionary dictionary, string key, string theme, int depth)
    {
        if (depth > 8) return null;
        if (dictionary.ThemeDictionaries.TryGetValue(theme, out object? found) && found is ResourceDictionary themed &&
            themed.TryGetValue(key, out object? value)) return value;
        foreach (var merged in dictionary.MergedDictionaries)
            if (Find(merged, key, theme, depth + 1) is { } deeper) return deeper;
        return null;
    }

    public static TextBlock Caption(string text) => new()
    {
        Text = text,
        Style = Res("CaptionTextBlockStyle"),
        Foreground = Brush("TextFillColorSecondaryBrush"),
        TextWrapping = TextWrapping.Wrap,
    };

    /// <summary>A label on the left, its control (and anything said about it) on the right.</summary>
    public static Grid Row(string label, params FrameworkElement[] content)
    {
        var grid = new Grid { ColumnSpacing = 16 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(120) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.Children.Add(new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 6, 0, 0) });

        FrameworkElement right = content.Length == 1 ? content[0] : Stack(6, content);
        Grid.SetColumn(right, 1);
        grid.Children.Add(right);
        return grid;
    }

    public static StackPanel Stack(double spacing, params FrameworkElement[] content)
    {
        var panel = new StackPanel { Spacing = spacing };
        foreach (var element in content) panel.Children.Add(element);
        return panel;
    }

    public static StackPanel Line(params FrameworkElement[] content)
    {
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        foreach (var element in content)
        {
            element.VerticalAlignment = VerticalAlignment.Center;
            panel.Children.Add(element);
        }
        return panel;
    }

    public static NumberBox Number(int value, int minimum, int step = 1) => new()
    {
        Value = value,
        Minimum = minimum,
        SmallChange = step,
        LargeChange = step * 4,
        SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact,
        Width = 150,
        HorizontalAlignment = HorizontalAlignment.Left,
    };

    /// <summary>A whole number from a NumberBox, or null when it holds nothing usable.</summary>
    public static int? Whole(NumberBox box)
        => double.IsNaN(box.Value) || box.Value != Math.Floor(box.Value) || box.Value > int.MaxValue || box.Value < int.MinValue ? null : (int)box.Value;

    /// <summary>The GPU choices: none, whichever Windows picks, or one by name. Tag is (mode, id).</summary>
    public static ComboBox GpuCombo(int mode, string id)
    {
        var combo = new ComboBox { MinWidth = 260, HorizontalAlignment = HorizontalAlignment.Left };
        combo.Items.Add(new ComboBoxItem { Content = "None", Tag = (0, "") });
        combo.Items.Add(new ComboBoxItem { Content = "Default GPU", Tag = (1, "") });
        foreach (var gpu in AppState.Current.Host.Gpus)
            combo.Items.Add(new ComboBoxItem { Content = gpu.Name, Tag = (1, gpu.Id) });

        combo.SelectedIndex = mode == 0 ? 0 : 1;
        if (mode != 0 && id.Length > 0)
        {
            for (int i = 2; i < combo.Items.Count; i++)
                if (combo.Items[i] is ComboBoxItem { Tag: (int, string known) } && string.Equals(known, id, StringComparison.OrdinalIgnoreCase))
                    combo.SelectedIndex = i;
        }
        return combo;
    }

    public static (int Mode, string Id) Gpu(ComboBox combo)
        => combo.SelectedItem is ComboBoxItem { Tag: (int mode, string id) } ? (mode, id) : (1, "");

    /// <summary>Network choices by their mode number (0 None, 1 NAT, 2 External, 3 Internal).</summary>
    public static ComboBox NetworkCombo(int mode, bool withNat = true)
    {
        var combo = new ComboBox { MinWidth = 160, HorizontalAlignment = HorizontalAlignment.Left };
        foreach (int value in new[] { 0, 1, 2, 3 })
        {
            if (value == 1 && !withNat) continue;
            combo.Items.Add(new ComboBoxItem { Content = VmInfo.NetworkName(value), Tag = value });
        }
        SelectTag(combo, mode);
        if (combo.SelectedIndex < 0) combo.SelectedIndex = 0;
        return combo;
    }

    public static void SelectTag(ComboBox combo, object tag)
    {
        for (int i = 0; i < combo.Items.Count; i++)
            if (combo.Items[i] is ComboBoxItem item && Equals(item.Tag, tag)) { combo.SelectedIndex = i; return; }
    }

    public static T TagOf<T>(ComboBox combo, T fallback) => combo.SelectedItem is ComboBoxItem { Tag: T value } ? value : fallback;

    /// <summary>Memory is given to VMs in even megabytes.</summary>
    public static int AlignRam(int mb) => mb + (mb & 1);
}
