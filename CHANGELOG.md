# Changelog

## Unreleased — shared core, GUI and API

### MCP server for AI agents (Windows host)

- Settings → AI agents (MCP) turns on a Model Context Protocol server at
  `http://127.0.0.1:47841/mcp` (Streamable HTTP, no token, this PC only; off by
  default; `--headless --mcp` for the daemon). It offers the VM lifecycle,
  snapshots, the activity log and screenshots as tools, with a read-only mode.
- New guest command channel (Hyper-V socket service 8): the guest agent runs
  commands as SYSTEM (root on Linux) and reads and writes guest files for the
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

- The GUI hosts the local API automatically. GUI, API, and tray/menu-bar actions
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
- New Ubuntu disks on Windows hosts include an ext4 journal for filesystem
  recovery after crashes or forced stops. Existing disks retain their current
  filesystem configuration.
- Ubuntu disk creation reserves enough free inodes for first-boot installation
  on minimum-size disks.

### macOS

- The host minimum is macOS 27. New Mac VMs require a macOS 27+ restore image and
  provision the configured account and automatic desktop login through Apple's API.
  Existing older guests keep their normal boot path. AppSandbox retains its own
  SSH enablement, socket transport, and key deployment.
- Administrator authorization occurs at host startup and covers its lifetime.
  Subsequent VM creation and boot do not request authorization again.
- The menu-bar icon provides running-VM controls. Closing the management window
  leaves the core/API running, and the Dock can reopen the management window.
- Local desktop headless mode also offers the menu-bar icon, a "Headless API"
  indicator, and Quit. Menu creation is best-effort. Remote/non-graphical sessions
  remain independent of AppKit and the menu bar.
- Diagnostic messages use the regular app log rather than a separate log window.
- Windows guests use nested virtualization automatically when the host reports
  support. Nested virtualization is not available for macOS guests, and there is
  no user-facing toggle.

### Templates and networking

- Mac hosts can build templates for both Windows and macOS guests through the GUI
  or API, then create independent instances with the same or a larger usable disk.
  Windows templates use the shared Windows provisioning and Sysprep path. Mac bases
  defer account provisioning until each instance's first boot.
- Template selection and counts follow the selected guest OS. Incomplete Mac-host
  templates are not offered as ready bases.
- New template instances on both hosts continue working after their template is
  deleted. Windows reports copy progress and cancels unfinished copies on shutdown.
  Older Windows VMs remain supported; template deletion is refused while an older
  VM or an active copy requires it, or dependencies cannot be verified.
- Clone virtual hardware identities are distinct. Filesystem identifiers may be
  retained; templates do not promise unique fingerprints to every application.
- Windows and macOS guests on Mac hosts share Apple's NAT/DHCP network.
- New Windows NAT guests on both host platforms configure the relevant adapter as
  Private and enable the existing Windows IPv4 echo-request rule for the local
  subnet. No additional application firewall rules are created.
- New Ubuntu guests start their agent after the network manager, preventing a
  first-boot race that could remove the configured NAT address.
- On Mac hosts, quitting during disk creation stops owned workers and detaches mounted
  installer media. Unfinished disks are rejected for boot and can be deleted.

These are unreleased branch changes. No app release or deployment-system update
is part of this work.
