# Changelog

## Unreleased (0.2.0) — shared core, GUI and API

### New app (WinUI 3)

- The app has a new look: a native WinUI 3 window (title bar, navigation on
  the left, cards, Mica backdrop, light and dark theme) with the pages
  **Virtual machines**, **Activity** and **Settings**.
- Each VM has its own page with **Overview**, **Hardware**, **Display** and
  **Snapshots**.
- **Activity** is a page of its own for the log that used to be a panel.
- The notification-area icon opens a menu that lists the running VMs, each
  with Connect, Shut down and Force stop, plus Open, Settings and Quit. The
  app keeps running there when its window is closed with VMs running.
- Under the hood `AppSandbox.exe` is now the WinUI shell (C#, .NET 10, Windows
  App SDK) and the former program is the native host it loads
  (`appsandbox_host.dll`, with `appsandbox_core.dll`). The local API, the MCP
  server, `--headless` and `--vm` shortcuts work as before.

### Distribution

- MSIX is the only way App Sandbox is distributed, installed and updated. The
  zip that could be unpacked and run is gone; a test build is a `*-msix.zip`
  with the package, its test certificate and `Install-AppSandbox.ps1` (run it
  elevated, with App Sandbox exited). The .NET runtime and the Windows App
  SDK are inside the package: nothing else has to be installed.

### Removed

- Removed: the WebView2 dependency. The UI is no longer HTML/JS shown in
  Microsoft Edge WebView2, so the app does not need the WebView2 Runtime
  and loads nothing in a browser engine.

- Removed: Ubuntu guests and the macOS host. App Sandbox now runs on Windows 11
  hosts and provisions Windows 11 guests; any other OS can still be installed by
  hand from its ISO ("Other"). An existing Ubuntu VM stays in the list and opens
  with the basic console, like a manual-install VM (no agent, GPU, SSH or
  display driver).

### MCP server for AI agents

- Settings → AI agents (MCP) turns on a Model Context Protocol server at
  `http://127.0.0.1:47841/mcp` (Streamable HTTP, no token, this PC only; off by
  default; `--headless --mcp` for the daemon). It offers the VM lifecycle,
  snapshots, the activity log and screenshots as tools, with a read-only mode.
- New guest command channel (Hyper-V socket service 8): the guest agent runs
  commands as SYSTEM and reads and writes guest files for the
  `run_command`, `copy_to_guest`, `copy_from_guest` and `get_guest_crash`
  tools. Existing Windows VMs get it with the next guest-components update;
  a VM with an older agent answers those tools with a message to update.
- Driver testing: `install_driver`, `uninstall_driver`, `get_devices`,
  `get_debug_output` (DbgPrint text through a kernel trace, no debugger),
  `set_verifier`, `restart_vm`, `analyze_dump` (`!analyze -v` with the host's
  cdb.exe, run without elevation), `send_input`, and the prompts `test_driver`
  and `new_test_vm`.
- `screenshot` works with the VM's window open in performance mode and on a
  still desktop: it falls back to a copy made inside the guest (the agent can
  now start a program in the signed-in user's desktop session).

### Shared control and lifecycle

- The GUI hosts the local API automatically. GUI, API, and tray actions
  use the same core state and viewer ownership. A second core process is rejected.
- Standalone `--headless` and `-headless` launches keep the management UI closed
  and open VM viewers only on explicit requests. Windows headless startup failures
  report through stderr/logging instead of a GUI error dialog.
- VM records have persistent host IDs separate from their names. Existing records
  are migrated automatically; name-based clients and older guest agents remain
  supported. Deleted IDs cannot target a replacement VM with the same name.
- Core events update the shared frontend. Windows records remain stable while
  asynchronous work is active, and viewer-close notifications identify the viewer
  instance so a stale close cannot affect its replacement.

### Templates and networking

- New template instances continue working after their template is
  deleted. Windows reports copy progress and cancels unfinished copies on shutdown.
  Older Windows VMs remain supported; template deletion is refused while an older
  VM or an active copy requires it, or dependencies cannot be verified.
- Clone virtual hardware identities are distinct. Filesystem identifiers may be
  retained; templates do not promise unique fingerprints to every application.
- New Windows NAT guests configure the relevant adapter as
  Private and enable the existing Windows IPv4 echo-request rule for the local
  subnet. No additional application firewall rules are created.

These are unreleased branch changes. No app release or deployment-system update
is part of this work.
