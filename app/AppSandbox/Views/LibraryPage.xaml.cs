using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace AppSandbox.Views;

/// <summary>The list of VMs: the page the app opens on.</summary>
public sealed partial class LibraryPage : Page
{
    private static AppState S => AppState.Current;

    private string _filter = "all";
    private bool _listening;

    public LibraryPage()
    {
        InitializeComponent();

        // The page is kept (NavigationCacheMode), so it only listens while shown.
        Loaded += (_, _) =>
        {
            if (_listening) return;
            _listening = true;
            S.Changed += Refresh;
            VmIcons.Changed += OnIconChanged;
            Refresh();
        };
        Unloaded += (_, _) =>
        {
            // Leaving and coming straight back can raise Loaded before this.
            if (IsLoaded) return;
            _listening = false;
            S.Changed -= Refresh;
            VmIcons.Changed -= OnIconChanged;
        };
    }

    private void OnIconChanged(string id) => Refresh();

    private bool Matches(VmInfo vm)
    {
        string query = SearchBox.Text.Trim();
        if (query.Length > 0 &&
            !vm.Name.Contains(query, StringComparison.OrdinalIgnoreCase) &&
            !vm.OsType.Contains(query, StringComparison.OrdinalIgnoreCase)) return false;

        return _filter switch
        {
            "running" => vm.Running || vm.Busy,
            "stopped" => !vm.Running && !vm.Busy,
            _ => true,
        };
    }

    private void Refresh()
    {
        var vms = S.Vms;
        int running = vms.Count(vm => vm.Running);

        SummaryText.Text = S.Core switch
        {
            CoreState.Starting => "Starting…",
            CoreState.PrereqReboot => "Restart Windows to finish setup",
            CoreState.PrereqMissing or CoreState.PrereqEnabling or CoreState.PrereqFailed => "Virtual Machine Platform is off",
            _ => vms.Count == 0 ? "No virtual machines yet" : $"{running} running · {vms.Count - running} stopped",
        };

        bool usable = S.CanUseVms && !S.Exiting;
        NewButton.IsEnabled = usable;
        ImportButton.IsEnabled = usable;
        ToolTipService.SetToolTip(NewButton, S.CanUseVms ? null : "Virtual Machine Platform must be on first");

        FilterRow.Visibility = vms.Count > 0 ? Visibility.Visible : Visibility.Collapsed;

        RefreshPrereq();

        // Cards are kept and updated in place, in the host's order; only VMs
        // that came or went change the list itself.
        var shown = vms.Where(Matches).ToList();
        var existing = Cards.Children.OfType<VmCard>().ToDictionary(card => card.VmId);
        bool sameOrder = shown.Count == Cards.Children.Count &&
                         shown.Select(vm => vm.Id).SequenceEqual(Cards.Children.OfType<VmCard>().Select(card => card.VmId));
        if (sameOrder)
        {
            foreach (var vm in shown) existing[vm.Id].Update(vm);
        }
        else
        {
            Cards.Children.Clear();
            foreach (var vm in shown)
            {
                if (existing.TryGetValue(vm.Id, out var card)) card.Update(vm);
                else card = new VmCard(vm);
                Cards.Children.Add(card);
            }
        }

        RefreshEmpty(shown.Count);
    }

    private void RefreshPrereq()
    {
        bool show = S.Core is CoreState.PrereqMissing or CoreState.PrereqEnabling or CoreState.PrereqReboot or CoreState.PrereqFailed;
        PrereqCard.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        if (!show) return;

        PrereqProgress.Visibility = Visibility.Collapsed;
        PrereqButton.Visibility = Visibility.Visible;
        PrereqButton.IsEnabled = true;

        switch (S.Core)
        {
            case CoreState.PrereqMissing:
                PrereqTitle.Text = "Turn on Virtual Machine Platform";
                PrereqText.Text = "App Sandbox runs its VMs on this Windows feature. Turning it on takes a minute and may need a restart.";
                PrereqButton.Content = "Turn on";
                break;
            case CoreState.PrereqEnabling:
                PrereqTitle.Text = "Turning on Virtual Machine Platform…";
                PrereqText.Text = S.PrereqProgress > 0 ? S.PrereqProgress + "%" : "Windows is adding the feature.";
                PrereqProgress.Value = S.PrereqProgress;
                PrereqProgress.Visibility = Visibility.Visible;
                PrereqButton.Visibility = Visibility.Collapsed;
                break;
            case CoreState.PrereqReboot:
                PrereqTitle.Text = "Restart Windows to finish setup";
                PrereqText.Text = "Virtual Machine Platform is installed and starts working after a restart. Save your work in other apps first.";
                PrereqButton.Content = "Restart now…";
                break;
            default:
                PrereqTitle.Text = "Virtual Machine Platform could not be turned on";
                PrereqText.Text = "Turn it on yourself: open \"Turn Windows features on or off\", tick Virtual Machine Platform, restart Windows, then open App Sandbox again. Details are in Activity.";
                PrereqButton.Content = "Try again";
                break;
        }
    }

    private void RefreshEmpty(int shown)
    {
        bool prereq = PrereqCard.Visibility == Visibility.Visible;
        bool show = shown == 0 && !prereq;
        EmptyCard.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        if (!show) return;

        bool starting = S.Core == CoreState.Starting;
        EmptyRing.IsActive = starting;
        EmptyRing.Visibility = starting ? Visibility.Visible : Visibility.Collapsed;
        EmptyGlyph.Visibility = starting ? Visibility.Collapsed : Visibility.Visible;
        EmptyButton.Visibility = starting ? Visibility.Collapsed : Visibility.Visible;

        if (starting)
        {
            EmptyTitle.Text = "Starting App Sandbox…";
            EmptyText.Text = "Checking Windows features and your GPUs, and finding your virtual machines.";
        }
        else if (S.Vms.Count == 0)
        {
            EmptyTitle.Text = "Create your first virtual machine";
            EmptyText.Text = "Windows 11, set up automatically from an installer image, or any other system you install yourself from its ISO.";
            EmptyButton.Content = "New VM";
            EmptyButton.Tag = "new";
            EmptyButton.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
            EmptyButton.IsEnabled = S.CanUseVms && !S.Exiting;
        }
        else
        {
            string query = SearchBox.Text.Trim();
            EmptyTitle.Text = query.Length > 0 ? $"No VMs match \"{query}\"" : "No VMs here";
            EmptyText.Text = "";
            EmptyButton.Content = "Show all VMs";
            EmptyButton.Tag = "all";
            EmptyButton.Style = (Style)Application.Current.Resources["DefaultButtonStyle"];
            EmptyButton.IsEnabled = true;
        }
    }

    private void Search_TextChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args) => Refresh();

    private void Filter_SelectionChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        _filter = sender.SelectedItem?.Tag?.ToString() ?? "all";
        if (_listening) Refresh();
    }

    private void Empty_Click(object sender, RoutedEventArgs e)
    {
        if (EmptyButton.Tag as string == "new")
        {
            _ = CreateVmDialog.ShowAsync();
            return;
        }

        SearchBox.Text = "";
        FilterBar.SelectedItem = FilterBar.Items[0];
        Refresh();
    }

    private void New_Click(object sender, RoutedEventArgs e) => _ = CreateVmDialog.ShowAsync();

    private void Import_Click(object sender, RoutedEventArgs e) => HostBridge.Send("importVm");

    private async void Prereq_Click(object sender, RoutedEventArgs e)
    {
        if (S.Core == CoreState.PrereqReboot)
        {
            if (await Dialogs.Confirm("Restart Windows now?", "Windows closes every app and restarts. Save your work first.", "Restart now"))
                HostBridge.Send("enableFeatureReboot");
            return;
        }

        // Shown as under way at once: a second click must not start it twice.
        S.BeginPrereq();
        HostBridge.Send("enableFeature");
    }
}
