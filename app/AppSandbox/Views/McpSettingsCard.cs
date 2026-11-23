using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;

namespace AppSandbox.Views;

/// <summary>What the "AI agents (MCP)" settings rows need to know about the app they are in.</summary>
public sealed class McpCardHost
{
    /// <summary>The app's name as people read it ("GCapture").</summary>
    public required string AppName { get; init; }

    /// <summary>The server's name in an agent's configuration ("gcapture").</summary>
    public required string ServerName { get; init; }

    /// <summary>One or two sentences for the agent: what the app is and what it can do with it. English.</summary>
    public required string About { get; init; }

    /// <summary>How the connect prompt ends: what to call first. English.</summary>
    public required string FirstCalls { get; init; }

    public required Func<string> Url { get; init; }
    public required Func<bool> Enabled { get; init; }
    public required Action<bool> SetEnabled { get; init; }
    public required Func<bool> Running { get; init; }

    /// <summary>Why it is not running although it is on.</summary>
    public required Func<string?> Error { get; init; }

    /// <summary>The agent that connected last, as it named itself.</summary>
    public required Func<string?> Client { get; init; }

    /// <summary>The latest tool calls, newest first.</summary>
    public required Func<IReadOnlyList<(DateTime At, string Tool, bool Ok)>> Activity { get; init; }

    /// <summary>Hooks a handler to "something above changed" and returns how to unhook it. The handler may be called on any thread.</summary>
    public required Func<Action, Action> Subscribe { get; init; }

    /// <summary>A string by key, or the English given when the app has no translation for it.</summary>
    public Func<string, string, string> Text { get; init; } = (_, english) => english;

    /// <summary>The app's own permission rows (read-only, saving files…), shown under Details.</summary>
    public IReadOnlyList<UIElement> Permissions { get; init; } = [];

    /// <summary>Anything else the app has to say about agents (how they work here), shown last.</summary>
    public IReadOnlyList<UIElement> Extra { get; init; } = [];
}

/// <summary>
/// What every G2806 app shows under "AI agents (MCP)", with the same words
/// (the "MCP" section of the apps' shared CLAUDE.md): one row that stays
/// folded (the name, the state and the switch) and opens onto the prompt that
/// connects an agent, the address, the setup per client and what agents did. Taken from the other G2806 apps
/// (GCapture, GPixelArt); here the brushes follow the theme picked in Settings and the switch waits for the
/// host's answer.
/// </summary>
public sealed class McpSettingsCard : StackPanel
{
    private static readonly string[] Clients = ["Claude Code", "Codex", "Claude Desktop", ""];

    private readonly McpCardHost _host;
    private readonly ToggleSwitch _toggle = new() { VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _status;
    private readonly Button _copyPrompt, _copyUrl, _copySnippet;
    private readonly StackPanel _details = new() { Spacing = 16, Margin = new Thickness(36, 0, 0, 4) };
    private readonly TextBlock _url, _snippetHelp, _client;
    private readonly TextBox _snippet;
    private readonly ComboBox _clientCombo = new() { MinWidth = 170 };
    private readonly StackPanel _activity = new() { Spacing = 2 };
    private Action? _unsubscribe;
    private bool _refreshing;

    public McpSettingsCard(McpCardHost host)
    {
        _host = host;
        Spacing = 12;

        // The header is all there is until it is opened: the name, the state, the switch.
        _status = Caption("");
        string enableTitle = string.Format(T("McpEnableTitle", "Let AI agents use {0}"), host.AppName);
        AutomationProperties.SetName(_toggle, enableTitle);
        _toggle.Toggled += (_, _) =>
        {
            if (_refreshing || _toggle.IsOn == _host.Enabled()) return;

            // The app answers by reporting its new state, which refreshes the row.
            _host.SetEnabled(_toggle.IsOn);
        };
        var header = Row("\uE99A", enableTitle, [_status], _toggle);
        header.Margin = new Thickness(0, 12, 0, 12);

        _details.Children.Add(Caption(T("McpEnableDesc", "Claude Code, Codex, Cursor and other MCP clients on this PC can then use this app. Nothing outside this PC can reach it.")));

        // Connect one.
        _copyPrompt = new Button();
        _copyPrompt.Click += (_, _) => Copy(_copyPrompt, T("McpCopyPrompt", "Copy prompt"), ConnectPrompt());
        _details.Children.Add(Section(T("McpConnectTitle", "Connect an agent"),
            Line(Caption(T("McpConnectDesc", "Paste this prompt in your agent's chat and it connects itself.")), _copyPrompt)));

        var expander = new Expander
        {
            Header = header,
            Content = _details,
            IsExpanded = false,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
        };
        AutomationProperties.SetName(expander, T("McpSectionName", "AI agents (MCP)"));
        Children.Add(expander);

        // Address
        _url = new TextBlock { FontFamily = Mono, IsTextSelectionEnabled = true, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
        _copyUrl = new Button();
        _copyUrl.Click += (_, _) => Copy(_copyUrl, T("McpCopy", "Copy"), _host.Url());
        _details.Children.Add(Section(T("McpAddress", "Address"),
            Line(_url, _copyUrl),
            Caption(T("McpLocalOnly", "Only programs on this PC can connect. It needs no token."))));

        // Setup per client
        foreach (string client in Clients)
            _clientCombo.Items.Add(client.Length > 0 ? client : T("McpOtherClients", "Other clients"));
        _clientCombo.SelectedIndex = 0;
        _clientCombo.SelectionChanged += (_, _) => Refresh();
        AutomationProperties.SetName(_clientCombo, T("McpSetup", "Manual setup"));
        _snippetHelp = Caption("");
        _snippet = new TextBox { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, FontFamily = Mono, FontSize = 12 };
        _copySnippet = new Button { HorizontalAlignment = HorizontalAlignment.Left };
        _copySnippet.Click += (_, _) => Copy(_copySnippet, T("McpCopy", "Copy"), Snippet(_clientCombo.SelectedIndex));
        _details.Children.Add(Section(T("McpSetup", "Manual setup"), _clientCombo, _snippetHelp, _snippet, _copySnippet));

        // The app's own permissions
        if (host.Permissions.Count > 0)
            _details.Children.Add(Section(T("McpPermissions", "What agents may do"), [.. host.Permissions]));
        foreach (var element in host.Extra) _details.Children.Add(element);

        // Activity
        _client = Caption("");
        _details.Children.Add(Section(T("McpActivity", "Recent activity"), _client, _activity));

        Loaded += (_, _) =>
        {
            _unsubscribe ??= _host.Subscribe(() => DispatcherQueue?.TryEnqueue(Refresh));
            Refresh();
        };
        Unloaded += (_, _) =>
        {
            _unsubscribe?.Invoke();
            _unsubscribe = null;
        };

        Refresh();
    }

    /// <summary>Bring every row in line with the app's state.</summary>
    public void Refresh()
    {
        _refreshing = true;
        bool enabled = _host.Enabled(), running = _host.Running();
        _toggle.IsOn = enabled;

        _status.Text = !enabled ? T("McpStateOff", "Off")
            : running ? string.Format(T("McpStateOn", "On · {0}"), _host.Url())
            : string.Format(T("McpStateFailed", "Could not start: {0}"), _host.Error() ?? "");
        _status.Foreground = Themed(!enabled ? "TextFillColorSecondaryBrush" : running ? "SystemFillColorSuccessBrush" : "SystemFillColorCriticalBrush");

        _copyPrompt.IsEnabled = running;
        if (_copyPrompt.Tag is null) _copyPrompt.Content = T("McpCopyPrompt", "Copy prompt");
        if (_copyUrl.Tag is null) _copyUrl.Content = T("McpCopy", "Copy");
        if (_copySnippet.Tag is null) _copySnippet.Content = T("McpCopy", "Copy");

        _url.Text = _host.Url();
        int client = Math.Max(0, _clientCombo.SelectedIndex);
        // Only when it changes: rewriting the same text drops a selection being made in it.
        if (_snippet.Text != Snippet(client)) _snippet.Text = Snippet(client);
        _snippetHelp.Text = client switch
        {
            0 => T("McpSetupClaudeCode", "Run this in a terminal. The app is then available in every project."),
            1 => T("McpSetupCodex", "Run this in a terminal."),
            2 => T("McpSetupClaudeDesktop", "Add this to claude_desktop_config.json (Settings > Developer > Edit Config) and restart Claude Desktop. It needs Node.js."),
            _ => T("McpSetupOther", "Add this to the client's MCP configuration (mcp.json or similar). In VS Code the top-level key is \"servers\"."),
        };

        string? lastClient = _host.Client();
        _client.Text = lastClient is null
            ? T("McpNoClient", "No agent has connected yet.")
            : string.Format(T("McpLastClient", "Last connected: {0}"), lastClient);

        _activity.Children.Clear();
        foreach (var (at, tool, ok) in _host.Activity().Take(12))
        {
            var line = new Grid { ColumnSpacing = 12 };
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            line.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            line.Children.Add(Caption(at.ToString("T")));
            var name = new TextBlock { Text = tool, FontFamily = Mono, FontSize = 12, TextTrimming = TextTrimming.CharacterEllipsis };
            Grid.SetColumn(name, 1);
            line.Children.Add(name);
            if (!ok)
            {
                var failed = Caption(T("McpFailed", "failed"));
                failed.Foreground = Themed("SystemFillColorCriticalBrush");
                Grid.SetColumn(failed, 2);
                line.Children.Add(failed);
            }
            _activity.Children.Add(line);
        }
        _refreshing = false;
    }

    // ── What gets copied: the same for every app but for its name and address ──

    private string Snippet(int client)
    {
        string name = _host.ServerName, url = _host.Url();
        return client switch
        {
            0 => $"claude mcp add --transport http --scope user {name} {url}",
            1 => $"codex mcp add {name} --url {url}",
            2 => $$"""
                {
                  "mcpServers": {
                    "{{name}}": { "command": "npx", "args": ["-y", "mcp-remote", "{{url}}"] }
                  }
                }
                """,
            _ => $$"""
                {
                  "mcpServers": {
                    "{{name}}": { "type": "http", "url": "{{url}}" }
                  }
                }
                """,
        };
    }

    /// <summary>The prompt to paste in an agent's chat: it connects itself. Written for the agent, in English whatever the app's language.</summary>
    private string ConnectPrompt()
    {
        string name = _host.ServerName, url = _host.Url();
        return string.Join("\n",
            $"Connect to my {_host.AppName} app over MCP. {_host.About}",
            "",
            $"Add an MCP server named \"{name}\" with the Streamable HTTP transport at {url}. It needs no token and no headers; it only answers programs on this PC, and only while {_host.AppName} is open.",
            "",
            $"- Claude Code: run `{Snippet(0)}`",
            $"- Codex: run `{Snippet(1)}`",
            $"- Any other client: add this to its MCP configuration: {{\"mcpServers\":{{\"{name}\":{{\"type\":\"http\",\"url\":\"{url}\"}}}}}}",
            "",
            $"If your client needs a reload to see new MCP servers, tell me. {_host.FirstCalls}");
    }

    private async void Copy(Button button, string label, string text)
    {
        var data = new DataPackage();
        data.SetText(text);
        Clipboard.SetContent(data);

        // The button says so for a moment; Tag marks it as mid-confirmation so Refresh leaves it alone.
        object stamp = new();
        button.Tag = stamp;
        button.Content = T("McpCopied", "Copied");
        await Task.Delay(1600);
        if (!ReferenceEquals(button.Tag, stamp)) return;
        button.Tag = null;
        button.Content = label;
    }

    // ── Pieces ──

    private static readonly FontFamily Mono = new("Cascadia Mono, Consolas");

    private string T(string key, string english) => _host.Text(key, english);

    private static Brush Themed(string key) => Forms.Brush(key);

    private static TextBlock Caption(string text) => new()
    {
        Text = text,
        Style = (Style)Application.Current.Resources["CaptionTextBlockStyle"],
        Foreground = Themed("TextFillColorSecondaryBrush"),
        TextWrapping = TextWrapping.Wrap,
    };

    private static Grid Row(string glyph, string title, UIElement[] under, FrameworkElement control)
    {
        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        grid.Children.Add(new FontIcon
        {
            Glyph = glyph,
            FontSize = 20,
            Width = 24,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = Themed("TextFillColorSecondaryBrush"),
        });

        var text = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        text.Children.Add(new TextBlock
        {
            Text = title,
            Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"],
            TextWrapping = TextWrapping.Wrap,
        });
        foreach (var element in under) text.Children.Add(element);
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);

        control.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(control, 2);
        grid.Children.Add(control);
        return grid;
    }

    private static StackPanel Section(string label, params UIElement[] content)
    {
        var panel = new StackPanel { Spacing = 6 };
        panel.Children.Add(new TextBlock { Text = label, Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"] });
        foreach (var element in content) panel.Children.Add(element);
        return panel;
    }

    private static Grid Line(FrameworkElement text, FrameworkElement button)
    {
        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.Children.Add(text);
        Grid.SetColumn(button, 1);
        grid.Children.Add(button);
        return grid;
    }
}
