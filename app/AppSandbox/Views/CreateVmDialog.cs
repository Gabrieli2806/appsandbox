using System.Text.RegularExpressions;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace AppSandbox.Views;

/// <summary>
/// New VM: a Windows 11 guest set up from its installer ISO (or copied from a
/// template), or any other system installed by hand from its ISO.
/// </summary>
internal sealed partial class CreateVmDialog
{
    private static AppState S => AppState.Current;
    private static bool _open;

    private readonly ContentDialog _dialog;
    private readonly ComboBox _os = new() { MinWidth = 260, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly TextBox _name = new() { Text = "MyAppSandbox", Width = 260, HorizontalAlignment = HorizontalAlignment.Left, IsSpellCheckEnabled = false };
    private readonly TextBox _image = new() { PlaceholderText = "Installer ISO", IsSpellCheckEnabled = false };
    private readonly Button _browseImage = new() { Content = "Browse…" };
    private readonly ComboBox _template = new() { MinWidth = 260 };
    private readonly Button _deleteTemplate = new() { Style = Forms.Res("IconButtonStyle"), Content = new FontIcon { Glyph = "", FontSize = 14 } };
    private readonly Grid _templateRow;
    private readonly NumberBox _disk = Forms.Number(64, 1);
    private readonly TextBox _diskFolder = new() { IsSpellCheckEnabled = false };
    private readonly TextBlock _diskInfo = Forms.Caption("");
    private readonly NumberBox _ram;
    private readonly NumberBox _cores;
    private readonly ComboBox _gpu = Forms.GpuCombo(1, "");
    private readonly Grid _gpuRow;
    private ComboBox _network = Forms.NetworkCombo(1);
    private readonly StackPanel _networkHost = new() { Orientation = Orientation.Horizontal, Spacing = 10 };
    private readonly ComboBox _adapter = new() { MinWidth = 220 };
    private readonly TextBox _user = new() { Text = "user", Width = 260, HorizontalAlignment = HorizontalAlignment.Left, IsSpellCheckEnabled = false };
    private readonly PasswordBox _password = new() { PlaceholderText = "Choose a password", Width = 260, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly PasswordBox _confirm = new() { PlaceholderText = "Type it again", Width = 260, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly CheckBox _testMode = new() { Content = "Test mode" };
    private readonly CheckBox _ssh = new() { Content = "SSH server" };
    private readonly CheckBox _sshKey = new() { Content = "Deploy SSH key", IsEnabled = false };
    private readonly StackPanel _accountRows;
    private readonly TextBlock _manualNote;
    private readonly TextBlock _hint = new() { TextWrapping = TextWrapping.Wrap, MinHeight = 20 };

    private string _hostError = "";
    private bool _pending, _filling;
    private int _diskRequest;
    private (string Path, int FreeGb)? _diskSpace;
    private bool _diskPending;
    private readonly DispatcherTimer _diskTimer = new() { Interval = TimeSpan.FromMilliseconds(200) };

    public static async Task ShowAsync()
    {
        if (_open || !S.CanUseVms) return;
        _open = true;
        try { await new CreateVmDialog().RunAsync(); }
        finally { _open = false; }
    }

    private string OsType => Forms.TagOf(_os, "Windows");
    private bool IsOther => OsType == "Other";

    private CreateVmDialog()
    {
        var host = S.Host;
        int ram = Math.Max(512, Math.Min(host.HostRamMb / 2, 16384));
        _ram = Forms.Number(Forms.AlignRam(ram), 512, 2);
        _cores = Forms.Number(Math.Max(1, Math.Min(host.HostCores / 2, 8)), 1);

        _os.Items.Add(new ComboBoxItem { Content = "Windows 11", Tag = "Windows" });
        _os.Items.Add(new ComboBoxItem { Content = "Other (manual install)", Tag = "Other" });
        _os.SelectedIndex = 0;

        _diskFolder.Text = host.DefaultDiskDirectory;
        _adapter.Items.Add(new ComboBoxItem { Content = "(Auto)", Tag = "" });
        foreach (string adapter in S.Adapters) _adapter.Items.Add(new ComboBoxItem { Content = adapter, Tag = adapter });
        _adapter.SelectedIndex = Math.Clamp(S.DefaultAdapter, 0, _adapter.Items.Count - 1);

        ToolTipService.SetToolTip(_testMode, "Disable Secure Boot and enable test-signing (required to load the test-signed guest drivers)");
        ToolTipService.SetToolTip(_ssh, "Exposes the VM's sshd on a port of this PC only");
        ToolTipService.SetToolTip(_sshKey, "Deploy the App Sandbox SSH public key so you can connect with key auth (requires SSH server)");
        ToolTipService.SetToolTip(_deleteTemplate, "Delete the selected template");

        // Installer: an ISO, or (Windows) a template built earlier.
        var imageLine = new Grid { ColumnSpacing = 8 };
        imageLine.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        imageLine.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        imageLine.Children.Add(_image);
        Grid.SetColumn(_browseImage, 1);
        imageLine.Children.Add(_browseImage);

        _templateRow = Forms.Row("Or a template", Forms.Line(_template, _deleteTemplate));

        var folderLine = new Grid { ColumnSpacing = 8 };
        folderLine.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        folderLine.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var browseFolder = new Button { Content = "Browse…" };
        browseFolder.Click += (_, _) => HostBridge.Send("browseDiskDirectory", ("path", _diskFolder.Text.Trim()));
        folderLine.Children.Add(_diskFolder);
        Grid.SetColumn(browseFolder, 1);
        folderLine.Children.Add(browseFolder);

        _gpuRow = Forms.Row("GPU", _gpu);
        _networkHost.Children.Add(_network);
        _networkHost.Children.Add(_adapter);

        _accountRows = Forms.Stack(14,
            Forms.Row("Username", _user),
            Forms.Row("Password", _password, _confirm,
                Forms.Caption("The administrator account in the VM. You sign in with it, and SSH uses it when you turn SSH on.")),
            Forms.Row("Options", Forms.Line(_testMode, _ssh, _sshKey)));

        _manualNote = Forms.Caption("Manual install: App Sandbox boots the ISO into an empty disk and opens the VM's console; you install the system yourself. There is no GPU acceleration, shared clipboard, audio or SSH, and the ISO stays attached until you remove it from the VM's menu.");

        var form = Forms.Stack(14,
            Forms.Row("Operating system", _os),
            Forms.Row("VM name", _name),
            Forms.Row("Installer image", imageLine),
            _templateRow,
            Forms.Row("Disk size", Forms.Line(_disk, new TextBlock { Text = "GB" })),
            Forms.Row("Disk location", folderLine, _diskInfo,
                Forms.Caption("An existing folder. Disks are stored in a subfolder named after the VM.")),
            Forms.Row("Memory", Forms.Line(_ram, new TextBlock { Text = "MB" }), Forms.Caption($"Host: {host.HostRamMb} MB | VMs using: {host.VmRamMb} MB")),
            Forms.Row("Processors", _cores, Forms.Caption($"Host: {host.HostCores} cores | VMs using: {host.VmCores}")),
            _gpuRow,
            Forms.Row("Network", _networkHost),
            _accountRows,
            _manualNote);
        form.Width = 560;

        var body = new Grid { RowSpacing = 12 };
        body.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        body.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        body.Children.Add(new ScrollViewer { Content = form, Padding = new Thickness(0, 0, 16, 0), VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        Grid.SetRow(_hint, 1);
        body.Children.Add(_hint);

        _dialog = new ContentDialog
        {
            Title = "New virtual machine",
            Content = body,
            PrimaryButtonText = "Create VM",
            SecondaryButtonText = "Build template",
            CloseButtonText = "Cancel",
            // No default button: Enter in a number box would create the VM
            // with the number that was there before the one being typed.
            DefaultButton = ContentDialogButton.None,
        };
        _dialog.Resources["ContentDialogMaxWidth"] = 680.0;

        // The dialog stays open until the host answers: a refusal is shown
        // here, with everything typed still in place.
        _dialog.PrimaryButtonClick += (_, args) => { args.Cancel = true; Submit(template: false); };
        _dialog.SecondaryButtonClick += (_, args) => { args.Cancel = true; Submit(template: true); };
        _dialog.Closing += (_, args) => { if (_pending && args.Result == ContentDialogResult.None) args.Cancel = true; };

        _os.SelectionChanged += (_, _) => ApplyOsType();
        _name.TextChanged += (_, _) => Changed();
        _image.TextChanged += (_, _) =>
        {
            // An ISO and a template are two ways to get the same disk: one at a time.
            if (_image.Text.Trim().Length > 0 && _template.SelectedIndex > 0) _template.SelectedIndex = 0;
            Changed();
        };
        _template.SelectionChanged += (_, _) =>
        {
            if (_filling) return;
            if (_template.SelectedIndex > 0) _image.Text = "";
            _deleteTemplate.IsEnabled = _template.SelectedIndex > 0;
            Changed();
        };
        _browseImage.Click += (_, _) => HostBridge.Send("browseImage");
        _disk.ValueChanged += (_, _) => Changed();
        _ram.ValueChanged += (_, _) => Changed();
        _cores.ValueChanged += (_, _) => Changed();
        _diskFolder.TextChanged += (_, _) => { AskDiskSpace(); Changed(); };
        _user.TextChanged += (_, _) => Changed();
        _password.PasswordChanged += (_, _) => Changed();
        _confirm.PasswordChanged += (_, _) => Changed();
        _ssh.Checked += (_, _) => _sshKey.IsEnabled = true;
        _ssh.Unchecked += (_, _) => { _sshKey.IsEnabled = false; _sshKey.IsChecked = false; };

        _diskTimer.Tick += (_, _) =>
        {
            _diskTimer.Stop();
            HostBridge.Send("getDiskSpace", ("path", DiskFolder), ("requestId", _diskRequest));
        };

        BuildDeleteTemplateFlyout();
        FillTemplates();
        ApplyOsType();
        AskDiskSpace();
    }

    private async Task RunAsync()
    {
        S.CreateResult += OnCreateResult;
        S.IsoPicked += OnIsoPicked;
        S.DiskDirectoryPicked += OnFolderPicked;
        S.DiskSpace += OnDiskSpace;
        S.Changed += OnStateChanged;
        try
        {
            await Dialogs.ShowAsync(_dialog);
        }
        finally
        {
            S.CreateResult -= OnCreateResult;
            S.IsoPicked -= OnIsoPicked;
            S.DiskDirectoryPicked -= OnFolderPicked;
            S.DiskSpace -= OnDiskSpace;
            S.Changed -= OnStateChanged;
            _diskTimer.Stop();

            // Passwords do not stay around once the dialog is closed.
            _password.Password = "";
            _confirm.Password = "";
        }
    }

    // ---------------------------------------------------------------- Form state

    private void OnStateChanged()
    {
        FillTemplates();
        Validate();
    }

    private void OnIsoPicked(string path)
    {
        if (path.Length > 0) _image.Text = path;
    }

    private void OnFolderPicked(string path)
    {
        if (path.Length > 0) _diskFolder.Text = path;
    }

    private void Changed()
    {
        _hostError = "";
        Validate();
    }

    private void ApplyOsType()
    {
        bool other = IsOther;
        _templateRow.Visibility = other ? Visibility.Collapsed : Visibility.Visible;
        _gpuRow.Visibility = other ? Visibility.Collapsed : Visibility.Visible;
        _accountRows.Visibility = other ? Visibility.Collapsed : Visibility.Visible;
        _manualNote.Visibility = other ? Visibility.Visible : Visibility.Collapsed;
        _dialog.SecondaryButtonText = other ? "" : "Build template";
        if (other) _template.SelectedIndex = 0;

        // NAT needs the guest agent to set the address: a manual install gets
        // an internal network instead.
        int mode = Forms.TagOf(_network, 1);
        if (other && mode == 1) mode = 3;
        var network = Forms.NetworkCombo(mode, withNat: !other);
        network.SelectionChanged += (_, _) => ApplyNetwork();
        _networkHost.Children[0] = network;
        _network = network;
        ApplyNetwork();
        Changed();
    }

    private void ApplyNetwork() => _adapter.Visibility = Forms.TagOf(_network, 1) == 2 ? Visibility.Visible : Visibility.Collapsed;

    private void FillTemplates()
    {
        string selected = Forms.TagOf(_template, "");
        var templates = S.Templates.Where(t => t.OsType == "Windows").ToList();

        // The list is refilled whenever the host reports its state; that is
        // not the user changing the form.
        _filling = true;
        try
        {
            _template.Items.Clear();
            _template.Items.Add(new ComboBoxItem { Content = templates.Count == 0 ? "(None)" : $"(None — {templates.Count} available)", Tag = "" });
            foreach (var template in templates) _template.Items.Add(new ComboBoxItem { Content = template.Name, Tag = template.Name });
            _template.SelectedIndex = 0;
            if (selected.Length > 0) Forms.SelectTag(_template, selected);
        }
        finally
        {
            _filling = false;
        }
        _template.IsEnabled = templates.Count > 0;
        _deleteTemplate.IsEnabled = _template.SelectedIndex > 0;
    }

    /// <summary>A question cannot open over this dialog, so deleting a template asks in a flyout on its button.</summary>
    private void BuildDeleteTemplateFlyout()
    {
        var text = new TextBlock { TextWrapping = TextWrapping.Wrap, MaxWidth = 280 };
        var confirm = new Button { Content = "Delete", HorizontalAlignment = HorizontalAlignment.Right };
        var flyout = new Flyout { Content = Forms.Stack(12, text, confirm) };
        flyout.Opening += (_, _) => text.Text = $"Delete template \"{Forms.TagOf(_template, "")}\"?\n\nIts disk image is deleted permanently.";
        confirm.Click += (_, _) =>
        {
            string name = Forms.TagOf(_template, "");
            flyout.Hide();
            if (name.Length > 0) HostBridge.Send("deleteTemplate", ("name", name));
        };
        _deleteTemplate.Flyout = flyout;
    }

    // ---------------------------------------------------------------- Disk space

    private string DiskFolder => _diskFolder.Text.Trim().Length > 0 ? _diskFolder.Text.Trim() : S.Host.DefaultDiskDirectory;

    private void AskDiskSpace()
    {
        _diskRequest++;
        _diskPending = true;
        _diskTimer.Stop();
        if (ValidateDiskFolder(_diskFolder.Text.Trim()) is null) _diskTimer.Start();
        else _diskPending = false;
        ShowDiskInfo();
    }

    private void OnDiskSpace(string path, int request, int freeGb)
    {
        if (request != _diskRequest) return;
        _diskPending = false;
        _diskSpace = (path, freeGb);
        ShowDiskInfo();
        Validate();
    }

    private void ShowDiskInfo()
    {
        bool known = _diskSpace is { } space && space.Path == DiskFolder;
        string free = _diskPending && !known ? "Checking…" : known ? (_diskSpace!.Value.FreeGb >= 0 ? _diskSpace.Value.FreeGb + " GB" : "unavailable") : "–";
        _diskInfo.Text = $"Free: {free} | VMs allocated: {S.Host.VmHddGb} GB";
    }

    // ---------------------------------------------------------------- Validation

    [GeneratedRegex(@"^(?:[a-zA-Z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+)")] private static partial Regex AbsolutePath();
    [GeneratedRegex("[^a-zA-Z0-9-]")] private static partial Regex NotNameCharacter();
    [GeneratedRegex(@"^\d+$")] private static partial Regex OnlyDigits();
    [GeneratedRegex(@"[\x00-\x1f￾￿""\\/\[\]:;|=,+*?<>%@]")] private static partial Regex BadUserCharacter();
    [GeneratedRegex(@"^[.\s]+$")] private static partial Regex OnlyDotsOrSpaces();

    private static readonly string[] ReservedUsers =
    [
        "NONE", "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    ];

    private static string? ValidateDiskFolder(string path)
    {
        if (path.Length == 0) return null;   // empty = the default folder
        if (path.Any(c => c < 0x20 || c == 0x7f)) return "Disk folder cannot contain control characters.";
        if (!AbsolutePath().IsMatch(path)) return "Disk folder must be an absolute path.";
        if (path.IndexOfAny(['"', '<', '>', '|', '?', '*']) >= 0 || path[2..].Contains(':')) return "Disk folder contains invalid characters.";
        return null;
    }

    private string? ValidateName(string name)
    {
        if (name.Length == 0) return "VM name is required.";
        if (IsOther) { if (name.Length > 63) return "VM name cannot exceed 63 characters."; }
        else if (name.Length > 15) return "VM name cannot exceed 15 characters (NetBIOS limit).";
        if (NotNameCharacter().IsMatch(name)) return "VM name can only contain letters, digits, and hyphens.";
        if (OnlyDigits().IsMatch(name)) return "VM name cannot be only digits.";
        if (name.StartsWith('-') || name.EndsWith('-')) return "VM name cannot start or end with a hyphen.";
        if (S.Vms.Any(vm => vm.Name.Equals(name, StringComparison.OrdinalIgnoreCase))) return "A VM with this name already exists.";
        if (S.Templates.Any(t => t.Name.Equals(name, StringComparison.OrdinalIgnoreCase))) return "A template with this name already exists.";
        return null;
    }

    private string? ValidateUser(string name, bool template)
    {
        if (name.Length == 0) return "Username is required.";
        if (name.Length > 20) return "Username cannot exceed 20 characters.";
        if (BadUserCharacter().IsMatch(name)) return "Username contains invalid characters.";
        if (OnlyDotsOrSpaces().IsMatch(name)) return "Username cannot be only dots or spaces.";
        if (name.EndsWith('.')) return "Username cannot end with a period.";
        if (ReservedUsers.Contains(name.ToUpperInvariant())) return "Username is a reserved name.";
        if (!template && name.Equals(_name.Text.Trim(), StringComparison.OrdinalIgnoreCase))
            return "Username cannot match the VM name (Windows computer name).";
        return null;
    }

    private static string? ValidatePassword(string password)
    {
        if (password.Length == 0) return "Choose a password for the VM’s administrator account.";
        if (password.Contains('\0')) return "Password cannot contain NUL characters.";
        if (password.Length > 127) return "Password is too long (max 127 characters for Windows).";
        return null;
    }

    /// <summary>The first problem in the form, in the order the fields appear, or null.</summary>
    private string? Problem(bool template)
    {
        bool hasTemplate = !IsOther && _template.SelectedIndex > 0;
        if (template && IsOther) return "Manual-install VMs cannot be templates.";
        if (template && hasTemplate) return "Select an OS image to build a new template.";

        if (ValidateName(_name.Text.Trim()) is { } nameError) return nameError;

        bool hasImage = _image.Text.Trim().Length > 0;
        if (!hasImage && (template || !hasTemplate))
            return !IsOther && !template
                ? "Choose an installer ISO (Browse…) or a template to continue."
                : "Choose an installer ISO (Browse…) to continue.";

        if (ValidateDiskFolder(_diskFolder.Text.Trim()) is { } folderError) return folderError;
        if (Forms.Whole(_disk) is not >= 1) return "Disk size must be a whole number of at least 1 GB.";
        if (Forms.Whole(_ram) is not { } ram || ram < 512 || ram % 2 != 0) return "Memory must be an even number of at least 512 MB.";
        if (Forms.Whole(_cores) is not >= 1) return "Processors must be a whole number of at least 1.";

        bool diskKnown = _diskSpace is { } space && space.Path == DiskFolder;
        if (diskKnown && _diskSpace!.Value.FreeGb < 0) return "The disk location is unavailable. Choose another one.";
        if (_diskPending && !diskKnown) return "Checking the disk location…";

        if (!IsOther)
        {
            if (ValidateUser(_user.Text.Trim(), template) is { } userError) return userError;
            if (ValidatePassword(_password.Password) is { } passwordError) return passwordError;
            if (_password.Password != _confirm.Password)
                return _confirm.Password.Length > 0 ? "The passwords don’t match." : "Type the password again in Confirm.";
        }
        return null;
    }

    private void Validate()
    {
        string? create = Problem(template: false), build = Problem(template: true);
        bool usable = !_pending && S.CanUseVms && !S.Exiting;

        _dialog.IsPrimaryButtonEnabled = usable && create is null;
        _dialog.IsSecondaryButtonEnabled = usable && build is null;
        _dialog.PrimaryButtonText = _pending ? "Creating…" : "Create VM";

        // What the host said about the last try stays until the form changes.
        bool hostError = _hostError.Length > 0;
        _hint.Text = hostError ? _hostError : _pending ? "" : create ?? "";
        _hint.Foreground = Forms.Brush(hostError ? "SystemFillColorCriticalBrush" : "TextFillColorSecondaryBrush");
    }

    // ---------------------------------------------------------------- Create

    private void Submit(bool template)
    {
        if (_pending || Problem(template) is not null) { Validate(); return; }

        var (gpuMode, gpuId) = IsOther ? (0, "") : Forms.Gpu(_gpu);
        string folder = _diskFolder.Text.Trim();
        int network = Forms.TagOf(_network, 1);

        var fields = new List<(string, object?)>
        {
            ("name", _name.Text.Trim()),
            ("osType", OsType),
            ("imagePath", _image.Text.Trim()),
            ("templateName", IsOther || template ? "" : Forms.TagOf(_template, "")),
            ("hddGb", Forms.Whole(_disk) ?? 64),
            ("ramMb", Forms.Whole(_ram) ?? 4096),
            ("cpuCores", Forms.Whole(_cores) ?? 2),
            ("gpuMode", gpuMode),
            ("gpuId", gpuId),
            ("networkMode", network),
            ("netAdapter", network == 2 ? Forms.TagOf(_adapter, "") : ""),
            // "" = the default folder, so the VM follows it if it ever moves.
            ("diskDirectory", string.Equals(folder, S.Host.DefaultDiskDirectory, StringComparison.OrdinalIgnoreCase) ? "" : folder),
            ("testMode", !IsOther && _testMode.IsChecked == true),
            ("sshEnabled", !IsOther && _ssh.IsChecked == true),
            ("sshDeployKey", !IsOther && _ssh.IsChecked == true && _sshKey.IsChecked == true),
        };
        if (!IsOther)
        {
            fields.Add(("adminUser", _user.Text.Trim()));
            fields.Add(("adminPass", _password.Password));
        }
        if (template) fields.Add(("isTemplate", true));

        _pending = true;
        _hostError = "";
        Validate();
        HostBridge.Send("createVm", [.. fields]);
    }

    private void OnCreateResult(bool ok, string message)
    {
        if (!_pending) return;
        _pending = false;

        if (ok)
        {
            _dialog.Hide();
            return;
        }

        _hostError = message.Length > 0 ? message : "The VM could not be created. Details are in Activity.";
        Validate();
    }
}
