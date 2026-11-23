# AppSandbox headless SDK (`asb.py`)

A small, dependency-free Python client for driving AppSandbox programmatically —
create, configure, snapshot, and run GPU-enabled VMs with or without the GUI. This README
is the usage/API reference.

```python
import asb

c = asb.connect()                      # auto-discovers the GUI or headless host
c.create(name="dev", osType="Windows", imagePath=r"C:\ISOs\Win11_24H2.iso",
         ramMb=8192, cpuCores=4, adminUser="user", adminPass="test123")
c.wait_online("dev")                   # blocks through build + install + boot
print(c.ssh_info("dev"))               # {'host': '127.0.0.1', 'port': ..., ...}
c.shutdown("dev")                      # graceful ACPI power-off
c.delete_vm("dev")
```

---

## How it works

The GUI automatically hosts the local API; `asb.connect()` uses that same core.
`AppSandbox.exe --headless` runs it without
the management UI or automatic viewers. `-headless` is also supported. GUI and API
controls share VM state and viewers. Exiting the host terminates its VMs.

- **Loopback only.** The listener binds `http://127.0.0.1:<port>` (`http.sys`).
  Nothing is exposed off-box.
- **Discovery file.** On startup the GUI or headless host writes `host.json`
  (in `%ProgramData%\AppSandbox\`) with the endpoint,
  port, and a random per-run bearer token. `asb.connect()` reads it, so you
  never hard-code a port or token.
- **Single instance.** A global mutex guarantees
  one AppSandbox at a time — GUI or daemon (a second `--headless` exits with
  status 2). One owner, one source of truth.
- **Serialized core.** API commands run on the GUI thread in GUI mode, or the
  API command thread in standalone headless mode.
- **Capabilities.** `version()["capabilities"]` advertises what the host
  supports (snapshots, templates).

### Security model — read this before "hardening" it

The API is **admin-gated, and that gate is functional, not a security boundary.**
Creating, starting, and snapshotting VMs go through the Host Compute Service
(HCS), which requires elevation; binding the `http.sys` URL likewise needs
elevation. So the daemon must run elevated, and only an administrator can launch
it. The bearer token exists to stop *other processes* from stumbling onto the
port, not to defend against an authenticated administrator on the same machine —
that is explicitly **out of scope**. The threat model is single-user. Do not add
auth layers, DACL tightening, or token ceremony that the local single-user model
doesn't justify.

---

## Requirements

The host is Windows; the guests the API creates are Windows 11 (`osType`
`"Windows"`). Ubuntu guests and the macOS host are no longer supported.

| Requirement | Why |
|---|---|
| **Windows 11, build 22000+** | The bundled virtual display (VDD) and audio (VAD) drivers are Windows-11-only (their INFs target `10.0…22000`; the VDD uses the Win11 UMDF reflector). |
| **Administrator** | HCS VM operations and the `http.sys` bind both require elevation. |
| **`VirtualMachinePlatform` feature** | The HCS backend the core uses. Enable with `dism /online /Enable-Feature /FeatureName:VirtualMachinePlatform /All` then reboot. |

A missing prerequisite makes `--headless` **fail fast with a clear message**
and a non-zero exit — it never half-starts:

- not elevated → "must be run as Administrator…"; unsupported OS →
  "requires Windows 11, build 22000 or newer"; feature off → "requires the
  'VirtualMachinePlatform' Windows feature…"
- a second instance (GUI or daemon already running) → exit 2

---

## Getting started

1. **Open the GUI** (App Sandbox in the Start menu), or start the headless host:
   ```
   # elevated shell
   AppSandbox.exe --headless
   ```
   App Sandbox is installed as an MSIX, so there is no program folder to
   start it from: `AppSandbox.exe` is the package's execution alias
   (`%LOCALAPPDATA%\Microsoft\WindowsApps\AppSandbox.exe`, on `PATH` for the
   user who installed it). It runs in the foreground-less GUI subsystem. Diagnostics go to
   `headless.log` next to the discovery file.

2. **Connect** from Python (stdlib only — no `pip install`):
   ```python
   import asb
   c = asb.connect()
   print(c.version())   # {'product': 'AppSandbox', 'version': '0.1.9', ...}
   ```

`asb.py` has no third-party dependencies; copy it next to your script, or add
`tools/headless-api` to `sys.path` (the examples do the latter).

---

## VM lifecycle

A VM's `state` is **derived** from the core's flags, so it's always live —
there's no cached status to go stale:

```
building → stopped → installing → booting → online → stopping → stopped
   │           ▲          (running = true ─────────────┘)
   └─ vhdx build         agent connects at "online"
```

| state | meaning |
|---|---|
| `building` | the VHDX is being created/staged (the build worker holds the disk) |
| `stopped` | powered off |
| `installing` | powered on, guest OS still installing (`installComplete` = false) |
| `booting` | installed and powered on, guest agent not yet connected |
| `online` | guest agent connected — fully up |
| `stopping` | graceful shutdown requested, not yet powered off |

`create()` is **async and auto-starts** the VM, so a fresh create walks
`building → installing → booting → online` on its own. Watch it with
`wait()`/`status()` (poll-based and miss-proof) or `events()` (push, for UIs).

> **Create VMs sequentially, not all at once.** A create runs a VHDX build that
> stages the guest agent and drivers. Firing multiple creates simultaneously can
> make those concurrent staging steps race, and a build may fail (`iso-patch.exe exited with code
> 1`) and get removed. It's generally advised to let one create clear its build phase
> (or wait a short interval, e.g. ~30 s) before starting the next.

---

## API surface

`connect()` returns a `Client`. Query methods return parsed JSON; lifecycle
methods return `(http_status, body)` so you can branch on the status code.
VM arguments accept name strings or persistent `asb.VmId` values. Use `c.vm_id(name)`
to retain an identity across requests; plain strings remain names.

### Queries
| Method | Returns |
|---|---|
| `version()` | `{product, version, apiVersion, hostOs, capabilities}` |
| `host()` | `{hostCores, hostRamMb, freeGb, vmCores, vmRamMb, vmHddGb}` (capacity + what running/declared VMs use) |
| `list()` | list of VM status objects |
| `status(name)` | one VM's status (raises `KeyError` on 404) |
| `ssh_info(name)` | `{host, port, user, sshState, enabled, keyDeployed}` (loopback-forwarded SSH; `sshState 4` = ready + key deployed) |
| `templates()` | `[{name, osType}, …]` |
| `snapshots(name)` | list of `{index, name, parent, branchCount, branches:[…]}` (`parent`: index of the snapshot it was taken on, `-2` = the base) |
| `snapshots_full(name)` | the above **plus** `current: {snapIndex, branchIndex}` |

A **status object** has: `id, name, osType, state, running, agentOnline,
installComplete, building, progress, sshState, sshPort, ramMb, hddGb, cpuCores,
gpuMode, networkMode, displayOpen`.

### Lifecycle  *(return `(status, body)`)*
| Method | Effect |
|---|---|
| `create(**cfg)` | create + auto-start (validated; see below) |
| `edit(name, **fields)` | change config — **VM must be stopped** (else 409) |
| `start(name, snap_index=-1, branch_index=-1, branch_name=None)` | power on; optionally boot a snapshot/branch |
| `shutdown(name)` | graceful ACPI power-off |
| `stop(name)` | force power-off (pull the plug) |
| `delete_vm(name)` | remove the VM and its disk (refused 409 while building) |
| `delete_template(name)` | remove a template; refused while an older VM or active copy depends on it |
| `shutdown_daemon(force=False)` | stop the GUI or headless host (refused 409 if VMs are active unless `force=True`) |

### Snapshots & branches *(return `(status, body)`)*
| Method | Effect |
|---|---|
| `snap_take(name, snapname=None)` | snapshot — **VM must be stopped** (else 409) |
| `snap_rename(name, index, new_name, branch_index=-1)` | rename a snapshot (or a branch with `branch_index`) |
| `snap_delete(name, index)` | delete a snapshot |
| `snap_delete_branch(name, index, branch_index)` | delete one branch |

A **branch** is created by *starting from* a snapshot with a new name:
`start(name, snap_index=2, branch_name="experiment")`. This mirrors the GUI,
which has no other way to fork a snapshot.

**Base branches** are branches forked directly off the base disk rather than a
named snapshot — address them with `snap_index = -2`. `snapshots_full(name)`
returns them under a `baseBranches` key (alongside `snapshots`), and you create /
rename / delete them the same way: `start(name, snap_index=-2, branch_name=...)`,
`snap_rename(name, -2, new_name, branch_index=b)`, `snap_delete_branch(name, -2, b)`.
The base itself is **not** a deletable snapshot — `snap_delete(name, -2)` is
refused (deleting the base disk is "delete the VM"); only its branches delete.

### Events (SSE)
`events()` is a generator yielding parsed dicts as the daemon pushes them. It
**blocks** — run it in a thread. Event shapes:

```json
{"event": "vmStateChanged", "id": "vm-0123456789abcdef", "name": "dev", "running": true}
{"event": "vmProgress",     "id": "vm-0123456789abcdef", "name": "dev", "progress": 42, "staging": false}
{"event": "alert",          "message": "..."}
```

Events are a convenience for live UIs. **For control flow, prefer
`wait()`/`status()`** — they read the current state on each call, so a dropped
connection can never make you miss a transition.

### Waiting
- `wait(name, target_states, timeout=300, interval=2)` — poll until `state` is in
  `target_states` (a string or set); raises `TimeoutError`.
- `wait_online(name, timeout=900)` — shorthand for `wait(name, {"online"})`.

`asb.RUNNING_STATES == {"booting", "online"}` — the powered-on states.

### Display

Open the VM's **display window** — the same IDD/Connect view the GUI shows — on
the **host's local desktop**. GUI and API controls share the same viewer;
standalone headless mode opens it only on explicit request.

| Method | Returns |
|---|---|
| `display_status(name)` | `{open, ready}` — poll this; **no window is opened by polling** |
| `display_ready(name)` | `bool` — shorthand for `display_status()["ready"]` |
| `open_display(name)` | `(status, body)` — open (or focus) the window |
| `close_display(name)` | `(status, body)` — close it |

The pattern is **poll-then-open**:

```python
c.start("dev")
while not c.display_ready("dev"):    # running + agentOnline + agent says display driver up
    time.sleep(1)
c.open_display("dev")                # a window appears on the daemon's desktop
...
c.close_display("dev")              # or the user just closes it with the [X]
```

- **`ready` gates the open.** It is `running && agentOnline &&
  idd_ready`, where `idd_ready` is the **guest agent's own report** that the display
  driver is up (`idd_status:ok`) — a latched flag, **not** a probe: reading it touches no
  window and no frame channel, so polling can't disturb the display. (Connecting to the
  frame channel to "check" would itself become the display's single consumer.)
  Opening before `ready` would show a black window, so
  `open_display` returns `409 display_not_ready`.
- **`displayOpen`** (in every status object) reflects the *live* window, however
  it closed — `close_display`, the window's `[X]`, VM delete, and daemon exit all
  drive it false. `open_display` on an already-open VM just focuses the window
  (foregrounding it).
- **Local desktop only.** The window shows on the session the daemon runs in. A
  non-interactive session (a service/SSH daemon with no visible desktop) can't
  show one, so `open_display` returns `409 no_display` rather than spawning an
  invisible window. Multiple VMs' displays can be open at once.

---

## Create config & validation

The daemon validates create/edit input **exactly like the GUI** (it mirrors the
checks of the Create VM dialog, `app/AppSandbox/Views/CreateVmDialog.cs`, and
both go through the host's own checks), so the API rejects precisely what the
UI would rather than forwarding bad input to the core.

| Key | Notes |
|---|---|
| `name` | guest hostname; **set at create, not editable later**. Letters/digits/hyphens; not all-digits; no leading/trailing hyphen; ≤15 chars (NetBIOS). Must be unique across VMs *and* templates. |
| `osType` | `"Windows"`. (`"Linux"` is refused with `400`.) |
| `imagePath` | Windows installer ISO, required unless using a template |
| `templateName` | create from a ready template |
| `ramMb` | ≥512, **2 MB-aligned** (even). The SDK rounds down to even for you. |
| `hddGb` | ≥1; template instances cannot be smaller than their base |
| `cpuCores` | ≥1 |
| `gpuMode` | `0` None · `1` Default (paravirtual) |
| `networkMode` | `0` None · `1` NAT · `2` External · `3` Internal |
| `netAdapter` | host adapter name (for External mode) |
| `adminUser` | required for a normal create. ≤20, none of `"\/[]:;|=,+*?<>`, no trailing `.`, not a reserved name (CON, PRN, …). |
| `adminPass` | required; ≤127 UTF-16 code units |
| `testMode` | skip interactive setup where supported |
| `sshEnabled` | provision SSH + forward a loopback port |
| `sshDeployKey` | deploy the AppSandbox public key for password-less login (**requires `sshEnabled`**; rejected `400` otherwise) |
| `isTemplate` | build a Windows template; cannot use another template |

`edit()` accepts `ramMb`, `cpuCores`, `gpuMode`, `networkMode` with the same
range rules, and **only while the VM is stopped**. `name` cannot be changed.

### SSH key deploy (password-less login)

With `sshEnabled=True, sshDeployKey=True`, the daemon generates an **AppSandbox
ed25519 keypair** once (under `%ProgramData%\AppSandbox\ssh\`) and the guest agent
writes the public key into the guest's `administrators_authorized_keys`
(with the strict ACL sshd needs). The deployed key is saved in the VM's config. Once it
lands, `sshInfo` reports `keyDeployed: true` and `sshState: 4` (ready + key
deployed), and you can connect with no password:

```python
import asb, subprocess
c = asb.connect()
i = c.ssh_info("dev")                       # {'sshState': 4, 'keyDeployed': True, 'port': ..., 'user': ...}
subprocess.run(["ssh", "-i", asb.key_path(), "-p", str(i["port"]),
                "%s@127.0.0.1" % i["user"], "hostname"])
```

`asb.key_path()` returns the private key path. (`sshState` values: 1 installing,
2 ready, 3 failed, 4 ready + key deployed.)

---

## HTTP status / error model

| Status | When |
|---|---|
| `200 OK` | a query, or a successful `edit` |
| `202 Accepted` | an async action accepted (create/start/stop/snapshot/…) — watch `status`/`events` for the outcome |
| `400 Bad Request` | input failed validation — body has `{"error":{"code":"invalid_arg","message":…}}` |
| `401 Unauthorized` | missing/bad bearer token (only `version` is open) |
| `404 Not Found` | unknown VM/route |
| `405 Method Not Allowed` | wrong verb for the route |
| `409 Conflict` | state conflict: edit/snapshot while running (`vm_running`), delete while building (`vm_building`), daemon shutdown with active VMs (`vms_active`, lists them), or open-display refused (`no_display` no interactive desktop · `not_running` · `display_not_ready` agent hasn't reported the display driver up yet) |
| `500 Internal Server Error` | the core returned a failure (`error.hr`) |

---

## MCP server for AI agents

The host also serves a [Model Context Protocol](https://modelcontextprotocol.io)
endpoint, so an AI agent can use App Sandbox directly — no SDK, no token:

| | |
|---|---|
| Server name | `appsandbox` |
| Address | `http://127.0.0.1:47841/mcp` (Streamable HTTP; only reachable from this PC) |
| Turn it on | App: **Settings → AI agents (MCP)**. Daemon: `AppSandbox.exe --headless --mcp` (or `--mcp-read-only`). Off by default. |
| Claude Code | `claude mcp add --transport http --scope user appsandbox http://127.0.0.1:47841/mcp` |
| Codex | `codex mcp add appsandbox --url http://127.0.0.1:47841/mcp` |
| Claude Desktop | `{"mcpServers":{"appsandbox":{"command":"npx","args":["-y","mcp-remote","http://127.0.0.1:47841/mcp"]}}}` |
| Other clients | `{"mcpServers":{"appsandbox":{"type":"http","url":"http://127.0.0.1:47841/mcp"}}}` |

The lifecycle tools are the REST routes above, called in-process. Every VM
argument is `vm`: the VM's name or id.

| Tool | What it does |
|---|---|
| `list_vms` | Every VM with its state. Call it first. |
| `get_vm` | One VM: state, install progress, agent online, IP, GPU, test mode, outdated guest components, last guest crash. |
| `get_host` | Cores, RAM, free disk, GPUs, templates, app version. |
| `create_vm` | New VM from an ISO or a template (`test_mode` for test-signed drivers). |
| `start_vm`, `shutdown_vm`, `stop_vm`, `delete_vm` | Boot, clean shutdown, power off, delete. |
| `wait_for_vm` | Blocks up to 100 s until `online`, `stopped` or `restart` (offline and back online). |
| `restart_vm` | Reboots the guest OS and waits until its agent is back. |
| `run_command` | Runs a command in the guest as SYSTEM (cmd, PowerShell or direct); exit code, stdout, stderr. Through the guest agent: no network or SSH needed. `desktop` runs it as the signed-in user in their desktop session (so it can show windows; `elevated` for their administrator rights). |
| `copy_to_guest` | Writes a guest file from a host file (up to 4 GB) or inline content. |
| `copy_from_guest` | Copies a guest file to `Downloads\App Sandbox\<vm>` on the host and returns the path. |
| `install_driver` | One call: copies a driver package (.inf folder, or a legacy .sys) from the host, checks test signing / Secure Boot / signatures, trusts the test certificate, runs `pnputil /add-driver /install` (or `sc create`), returns the published name and the devices with their problem codes. Warns when the VM has no snapshot. |
| `uninstall_driver` | `pnputil /delete-driver oemNN.inf /uninstall /force`, or stops and deletes a legacy driver service. |
| `get_devices` | Devices with their problem codes (by default only the failing ones) and the third-party driver packages. |
| `get_debug_output` | A driver's DbgPrint / KdPrint text without a kernel debugger: `start`, `read` (lines since the last read), `stop`. Captures what passes the debug print filter (`DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, …)` always does; plain DbgPrint needs `enable_filter` and a restart). `providers` traces ETW / WPP providers into an .etl. |
| `set_verifier` | Driver Verifier for named drivers (`standard`, `custom` flags, `reset`, `query`), optionally restarting the guest. |
| `get_guest_crash` | Recorded bugchecks (code and parameters), unexpected shutdowns, dump files, last boot. |
| `analyze_dump` | `!analyze -v` on a guest dump (default: the newest minidump): bugcheck code and arguments, faulting module / symbol, stack. Runs cdb.exe / kd.exe on the host, non-elevated; needs the Debugging Tools for Windows (or uses a debugger in the guest). `symbol_path` adds your .pdb folder. |
| `screenshot` | The guest's screen as a PNG, with the VM's window open or closed (falls back to a copy made inside the guest). |
| `send_input` | Types text, presses keys or chords, clicks or moves the mouse at x,y, scrolls. |
| `list_snapshots`, `take_snapshot`, `restore_snapshot`, `delete_snapshot` | Snapshot tree; take (VM stopped); start from a snapshot on a new branch; delete. |
| `ssh_info` | Host, port and user of the SSH proxy. |
| `open_display`, `close_display` | The VM's window on the host desktop. |
| `get_log` | Recent lines of App Sandbox's activity log, optionally for one VM. |

**Read-only** (Settings, or `--mcp-read-only`) leaves only `list_vms`, `get_vm`,
`get_host`, `wait_for_vm`, `get_devices`, `get_guest_crash`, `screenshot`,
`list_snapshots`, `ssh_info` and `get_log`.

Two prompts (slash commands in Claude Code): `test_driver` (vm, driver) and
`new_test_vm` (vm).

Testing a driver with an agent: create the VM with `test_mode`, shut it down,
`take_snapshot`, start it; then loop `get_debug_output` start →
`install_driver` → `run_command` (exercise it) and `get_debug_output` read →
`set_verifier` / `restart_vm` → `get_guest_crash` and `analyze_dump` when the
guest crashed → `stop_vm`, `restore_snapshot`. No tool snapshots a running VM:
snapshots need it stopped.

Security: the server has no token and listens on 127.0.0.1 only, so every
program of every user signed in on this PC can use it while it is on. Host
files are read and written, and the debugger is run, with the app user's
non-elevated rights; files from a guest go only to `Downloads\App Sandbox`.

The guest tools need a guest agent with the command channel. Existing VMs get
it with the guest-components update before their next start. Tool calls that change something are written
to the activity log (`MCP: …`).

## Examples

Runnable scripts in [`examples/`](examples):

| Script | Shows |
|---|---|
| `list_vms.py` | connect, `version`, `host`, `list`, `templates` (read-only overview) |
| `start_vm.py` | `start` + poll to `online` |
| `stop_vm.py` | graceful `shutdown` vs `--force` `stop` |
| `full_lifecycle.py` | the whole surface: create → wait → `ssh_info` → snapshot → edit → branch → delete-branch → delete (self-contained) |
| `stream_events.py` | live SSE event stream |
| `admin.py` | operator/maintenance: `version`, `host`, `templates`, `delete_template`, `shutdown_daemon` (destructive actions gated behind flags) |

---

## Tests

The test suite lives in [`tests/`](tests/) and is **self-contained**: it assumes
no existing configuration, creates the throwaway `brk-*` VMs it needs, and
deletes them at the end (so it never leaves disk behind or touches your VMs).

Point it at your install ISO first (env var; or drop the ISO in `tests/`,
which is the default), then run the harness:

```
set ASB_ISO_WINDOWS=C:\path\to\Win11_x64.iso
python tests\run_all.py
```

`run_all.py` runs the no-VM static checks once (`test_host_and_validation.py`:
host reporting vs the real machine + create-validation rejections), then spawns
the full per-VM lifecycle (`vm_lifecycle.py`) for each spec.
The per-VM lifecycle also covers SSH key
auto-deploy: each VM is built with `sshDeployKey` and, once online, the suite
asserts `keyDeployed`/`sshState 4` and performs a key-only SSH login (no
password). `test_windows_template.py` covers the Windows template build →
create-from-template → delete cycle. Default guest credentials are
`user` / `test123`. See [`tests/README.md`](tests/README.md) for the file-by-file
layout.
