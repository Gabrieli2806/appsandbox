# AGENTS.md — guide for AI coding agents

This file tells an AI agent (Claude Code, Codex, Cursor, …) how App Sandbox is
built, where things live, and the rules that keep changes from breaking real
VMs. Read it before editing. Humans: README.md is the user-facing overview.

## 1. What App Sandbox is

A VM manager for Windows 11 hosts (HCS/HCN, no Hyper-V Manager). Windows
guests get GPU-PV, an IddCx virtual display driver, audio, clipboard, input
and SSH over Hyper-V sockets. Any other system can be installed by hand in a
manual-install ("Other") VM with the basic console. The app is a WinUI 3
shell (C#) over a native host DLL; everything that touches VMs is C (C++
only where an API needs it). It is installed and updated as an MSIX.

Linux (Ubuntu) guests and the macOS host were removed: do not add code
paths for them back.

## 2. Repository map

| Path | What |
|---|---|
| `AppSandbox.sln` | Native solution (Debug/Release × x64/ARM64): host DLLs, guest tools, drivers |
| `AppSandbox.vcxproj` | `appsandbox_host.dll`: the UI-thread logic, VM windows, local API, MCP server. C (`CompileAsC`); `vm_video_decode.cpp` is per-file C++ |
| `AppSandboxCore.vcxproj` | `appsandbox_core.dll`: VM orchestration (C, `ASB_BUILDING_DLL`) |
| `app/AppSandbox/` | `AppSandbox.exe`: the WinUI 3 shell (C#, .NET 10, Windows App SDK, self-contained) and its MSIX. Not in the solution: built with `dotnet build` (section 3). `Program.cs` (entry, UI thread), `MainWindow` (navigation, tray icon, notices, closing), `Services/` (`HostBridge` = P/Invoke to the host, `AppState` = what the host said, `VmActions`, `Dialogs`, `UiSettings`, `TrayIcon`), `Models/`, `Views/` (pages, dialogs, `TrayMenuWindow`, `McpSettingsCard`) |
| `src/app_win/` | `host.c` / `host.h` (the DLL's entry points: instance lock, command line, `--headless`, start), `ui.c` (JSON commands from the shell, display windows, VM operations that wait, exit sequence), `ui_bridge.c` (messages to the shell, JSON helpers), `headless.c` (HTTP API), `mcp.c` (MCP server for AI agents, section 9) |
| `src/backend_win/` | `asb_core.c` (orchestrator, `vms.cfg`), `hcs_vm.c` (HCS JSON, GPU-PV, Plan9 shares, hvsocket service table), `hcn_network.c`, `gpu_enum.c`, `disk_util.c` (new-VM staging), `guest_tools.c` (offline guest update), `snapshot.c`, `vm_agent.c` (agent channel), `vm_exec.c` (guest command channel: run a command, copy files), `asb_display.c` (frame reader without a window, used by the MCP screenshot), `vm_display_idd.c` (viewer window), `vm_video_decode.cpp` (H.264/HEVC decode), `vm_clipboard.c`, `vm_ssh_proxy.c`, `prereq.c` |
| `src/core/` | Shared by host and guest: `protocol.h` (input packets), `asb_display_protocol.h`, `asb_exec_protocol.h` (guest command channel), `asb_events.*`, `asb_types.h`, `asb_vm_id.h` |
| `tools/agent/` | Windows guest: `agent.c` (service `AppSandboxAgent`), `appsandbox-input.c` (SendInput + ViGEm gamepads), `appsandbox-displays.c`, clipboard, audio, `p9copy.c` |
| `tools/vdd/` | IddCx display driver (UMDF2, C++17): `vdd.cpp`, `vdd.h` (wire protocol), `vdd_encoder.cpp` (NVENC / Media Foundation), `ThirdParty/` (UMDF headers, `nvEncodeAPI.h` MIT) |
| `tools/vad/` | Virtual audio driver (KMDF) |
| `tools/transport/` | `asb_transport.*`: the guest side's Hyper-V socket helpers |
| `tools/provision/` | What a new Windows VM runs at first boot (SetupComplete) |
| `tools/iso-patch/` | `iso-patch.exe`: Windows installer ISO to VHDX (DISM, bcdboot) |
| `tools/headless-api/` | Python SDK `asb.py`, API reference README, examples, integration tests |
| `tools/sign/` | Release/sign scripts (EV signing needs the owner's hardware key) |
| `packaging/msix/` | `make-msix.ps1`: builds the app around a native build, signs the MSIX, writes `Install-AppSandbox.ps1` |
| `vendor/` | devcon |
| `.github/workflows/` | `windows-test-build.yml` (native Release x64 build, the app, signed MSIX, pre-release `test-build-N` on `release/**` pushes), `ci.yml` (native x64 + ARM64 and the app on PRs and other branches, `tools/check-sources.py`, generated MCP scripts), `codeql.yml` (CodeQL for the host C/C++) |

Stale, ignore: `tools/agent/build.cmd`, `tools/agent/deploy.bat`, the
`src\ui.h` entry in `AppSandboxCore.vcxproj`.

## 3. Building and testing

- Native: Visual Studio 2022 + Windows SDK + **WDK**. From a *cmd* prompt
  (not PowerShell 7 — the driver post-build steps need Windows PowerShell 5):
  `msbuild AppSandbox.sln /p:Configuration=Release /p:Platform=x64`.
  Output: `bin\Release\` (`appsandbox_host.dll`, `appsandbox_core.dll`,
  `iso-patch.exe`, `resources\` guest exes, `drivers\`).
- App: .NET 10 SDK. `dotnet build app\AppSandbox\AppSandbox.csproj -c Release
  -p:Platform=x64` (build the csproj, never the solution). It takes the
  native files from `bin\Release` (`-p:AsbStage=<folder>` for another one)
  and makes an unsigned MSIX under `app\AppSandbox\AppPackages`; add
  `-p:GenerateAppxPackageOnBuild=false` for a quick compile check.
- MSIX to install: `packaging\msix\make-msix.ps1 -Stage bin\Release -Version
  a.b.c.d -OutDir out` builds the app, signs the package with a test
  certificate and writes `Install-AppSandbox.ps1` (run elevated; it trusts
  the certificate and installs or updates). The app must not be running.
- Without the WDK (drivers cannot be built): build the user-mode projects one
  by one with `/p:PlatformToolset=v145` (Build Tools 2026) into a scratch
  folder, put them over the unzipped `drivers\` + `resources\` of the last
  CI package, and use that folder as the stage.
- Looking at the shell without administrator rights or VMs: a **Debug** build
  with `ASB_UI_DEMO=<file with one host message per line>` plays those
  messages instead of starting the host (`HostBridge.Start`), and
  `ASB_UI_PAGE` (`settings`, `activity`, `create`, `tray`, `vm:<id>|<section>`,
  `edit:<id>`) opens a page; start it with `__COMPAT_LAYER=RunAsInvoker`.
- Drivers are **test-signed** per build; the `.cer` is extracted and must ship
  next to them (App Sandbox enables test signing in the guest).
- Version: `Directory.Build.props` (`AsbVersionMajor/Minor/Patch/Revision`).
  CI passes `/p:AsbVersionRevision=<run number>` so every build's driver
  package is newer than the last (Windows will not "update" to the same
  DriverVer), and the same four parts to `make-msix.ps1` (an MSIX with the
  same version and other contents does not install). `AsbSourceRepo` is the
  GitHub repository Settings > About links to.
- No unit tests. Verification = CI build + manual run on a real Windows host
  with a GPU. `tools/headless-api/tests` is a slow integration suite (real
  ISOs, elevated daemon). Driver code needs the WDK and can only be compiled
  in CI.
- CI: `ci.yml` builds the native solution for Release x64 and ARM64 and, on
  x64, the app, for every pull request and every push outside `release/**`;
  a Linux job runs `tools/check-sources.py` and `tools/gen-mcp-scripts.py
  --check`. `codeql.yml` runs CodeQL on the user-mode host projects for pull
  requests, `main`, `release/**` and once a week. Set
  `ASB_SKIP_PACKAGE_PROJECT=1` (environment variable or `/p:`) to stop
  `AppSandboxPackage` from running `make-release.ps1` after a Release build.

## 4. Host ↔ guest channels

Hyper-V socket services `a5b0cafe-000N-4000-8000-000000000001`, registered
in `hcs_vm.c` (ports 1–6; 7 and 8 are not in the service table and work
with HCS's defaults):

| N | Channel | Format |
|---|---|---|
| 1 | Agent | newline text, optional `tag:` prefix. Guest→host: `hello`, `caps:<words>` (what the agent can do; `exec` = channel 8), `heartbeat`, `gpu_query`, `gpu_copy_*`, `idd_status:`, `hyperv_video:`, `displays:`, `log:<text>` (shown in the host log), `ssh_*`. Host→guest: `ping`, `shutdown`, `restart`, `gpu_query_response:`, `set_ip:`, `ssh_enable`, `idd_connect`, … |
| 2 | Display (VDD) | binary, `tools/vdd/vdd.h` (mirrored in `vm_display_idd.c`) |
| 3 | Input | 20-byte `InputPacket` (`src/core/protocol.h`) after an `IRDY` greeting |
| 4 | Audio | `ASA1` header then length-prefixed PCM |
| 5/6 | Clipboard host→guest / guest→host | `vm_clipboard.c` |
| 7 | SSH | proxied TCP |
| 8 | Command (exec) | `src/core/asb_exec_protocol.h`: one `ASXQ` request and one `ASXR` response per connection (ping, run a command, write a file, read a file). The guest agent listens (`tools/agent/agent.c`); host side is `vm_exec.c` |

Display protocol (`VDD_PROTOCOL_VERSION`, currently 4), defined once in
`src/core/asb_display_protocol.h` for the driver and the viewer:
- Guest→host: `ASFR` raw frame (+dirty rects), `ASCR` cursor, `ASVH` hello
  `{version}`, `ASPS` performance status `{mode, codec, hresult}`, `ASVE`
  encoded access unit `{w, h, codec, flags(key), size, seq}` + Annex-B bytes.
- Host→guest (16 bytes each): `ASHQ` hello query, `ASMR` mode request
  `{w, h, hz}`, `ASPF` performance request `{mode 0 raw/1 H.264/2 HEVC, kbps,
  flags bit0 = key frame}`.
- History: 2 = mode requests, 3 = performance mode, 4 = HEVC.

Input types 0–14: mouse, keys, keyboard/mouse version queries, `SET_DISPLAY`
(12), `GAMEPAD` (13, XINPUT_GAMEPAD layout, pad index in bits 8–9 of type),
`GAMEPAD_REMOVE` (14).

## 5. Rules that keep VMs working

1. **Old guests must keep working with a new host, and old hosts with new
   guests.** New display messages need a `VDD_PROTOCOL_VERSION` bump and the
   host must gate on the version from `ASVH` (old drivers never answer
   `ASHQ`; after `HELLO_FRAME_GRACE` frames the host treats them as old).
   Unknown input types are ignored by older helpers — keep it that way.
   The command channel (8) has a version in every message; a guest agent
   older than the channel simply does not listen, and the host tells the
   user to update the guest components.
2. **Mirrored definitions are kept in sync by hand** (except the display,
   input and command protocols, which are shared headers in `src/core`):
   audio header (`appsandbox-audio.c` ↔ viewer); clipboard (host ↔ guest
   helpers).
3. **Guest-side changes need delivery.** New VMs get files from
   `disk_util.c`. Existing Windows VMs get the 6 helper exes and the VDD
   package from `guest_tools.c` (offline, VM stopped, signature in
   `guest_tools.sig`); the agent installs a staged VDD at the next boot
   (`apply_pending_vdd_update`, result reported with `log:`). If you add a
   guest file, add it to **both** lists. VAD changes need a new VM.
4. **Driver (UMDF/IddCx) constraints** (`tools/vdd`): built with /WX; some
   Win32 macros are missing (use `0` instead of `WAIT_OBJECT_0` in new files);
   IddCx APIs must run on WDF threads (use WDF timers, not raw threads); the
   cached D3D device must survive IddCx teardown; load optional DLLs at run
   time (`mfplat.dll`, `nvEncodeAPI64.dll`) so a missing one only disables a
   feature. The VDD log is `C:\ProgramData\AppSandboxVDD.log` in the guest.
5. **The shell never touches VMs.** `app/AppSandbox` only sends commands to
   the host and shows what it answers (section 7). VM state, settings in
   `vms.cfg`, files of a VM and anything elevated belong in the host. The
   shell keeps only how its own window looks (`%LOCALAPPDATA%\AppSandbox\
   shell.json`).
6. **C by default.** New host/guest code is C (`CompileAsC`); the shell is C#. Use C++ only
   for COM-heavy APIs (Media Foundation, D3D video) in its own file with an
   `extern "C"` header and a per-file `<CompileAs>CompileAsCpp</CompileAs>`.
7. **Elevation**: the app requires admin (HCS, http.sys). Do not add features
   that read or write host files on behalf of the guest without an explicit,
   narrow user choice — the VM is untrusted. The MCP file tools act for the
   MCP client (a program of the user), never for the guest, and open host
   files with the user's non-elevated rights (`impersonate_plain_user` in
   `mcp.c`).
8. **Fail soft.** Features like GPU video, HEVC, controllers fall back
   (raw frames, H.264, ignored packets) and say why in the log.

## 6. Where the display/performance features live

- Viewer: `src/backend_win/vm_display_idd.c` — receive thread (frames, video
  queue), window thread (D3D11 flip-model swap chain, decode via
  `vm_video_decode.cpp`, adaptive backdrop shader, floating toolbar, Display
  menu), gamepad thread (XInput → `INPUT_GAMEPAD`).
- Per-VM settings: `display_settings.json` beside the VHDX (`resolution`,
  `refreshHz`, `streamMode`, `codec`, `bitrateMbps`, clipboard/hotkey flags).
- Global settings: `[Settings]` in `%ProgramData%\AppSandbox\vms.cfg`
  (`AutoUpdateGuestTools`, `ViewerTitleStats`, `ViewerToolbar`, …).
- Guest encoder: `tools/vdd/vdd_encoder.cpp` — BGRA → NV12 with the D3D11
  video processor, then NVENC (preferred; GPU-PV guests do not register the
  NVIDIA Media Foundation encoder) or a hardware MF encoder.
- Controllers in the guest need **ViGEmBus**; the agent installs it if
  `drivers\ViGEmBus_setup.exe` ships with the guest components (not bundled
  by CI yet — third-party binary, needs an explicit decision).
- Renderers: D3D11 swap chain, or `vm_display_d3d12.cpp` (Settings → VM
  windows → Renderer). D3D12 reads the decoder's output textures in place
  (they are created shareable) and copies only raw frames and the backdrop;
  it waits for the previous frame at the start of the next one
  (`dx12_frame_begin` before decoding), not after `Present`.
- Adaptive backdrop: `g_ps_blur_hlsl` (dimmed, desaturated blurred cover +
  rounded picture with shadow and outline); the 16-float constant layout is
  documented above the shader and filled by `d3d_blend_consts` for both
  renderers. A lost device (TDR, driver update) rebuilds the renderer.
- Guest stability switches (in the guest, DWORD):
  `HKLM\SOFTWARE\AppSandbox\Agent\GpuStabilityTweaks=0` turns off and
  reverts the agent's feature-flag workaround for the Windows GPU-PV 0x3B
  crash (`gpu_crash_workaround` in `tools/agent/agent.c`; only on builds
  26100/26200/26300 with UBR ≥ 9550, removed again once UBR ≥ 9818);
  `HKLM\SOFTWARE\AppSandbox\VDD\RealtimeGpuPriority=1` restores the VDD's
  REALTIME process GPU priority (default HIGH). The agent reports the last
  bugcheck (System log 1001 / Kernel-Power 41) to the host after a crash.
- Logs: host `%ProgramData%\AppSandbox\appsandbox.log` (absolute path,
  rotates at 10 MB); guest `C:\Windows\AppSandbox\agent.log` (rotates at
  8 MB to `agent.old.log`).

## 6b. Manual-install VMs (os type "Other")

For OSes App Sandbox cannot provision: an empty disk plus the ISO, no
account, no guest agent, GPU, SSH or display driver (`asb_vm_create` forces
them off; `vm_agent_start` / `idd_probe_start` return early). The screen is
the Hyper-V basic console: `hcs_vm.c` adds `VideoMonitor.ConnectionOptions`
(`\\.\pipe\<name>.BasicSession`, ACL = current user) and `ui.c`
`do_connect_vm` routes these VMs to the RDP viewer (`vm_display.c`). Secure
Boot is skipped. `install_complete` is TRUE from creation. The ISO stays
attached until "Remove installer ISO" (`asb_vm_eject_iso`). (Windows VMs
detach their installer ISO automatically once the agent reports the
installation complete.) A VM record from an older version with a Linux or
macOS guest is loaded as "Other" (`retire_unsupported_guest`). Shutdown falls
back to `HcsShutDownComputeSystem`. Not yet: headless API create for
"Other", import of "Other" exports, Secure Boot option.

## 7. Shell ↔ host

`AppSandbox.exe` (WinUI, `app/AppSandbox`) loads `appsandbox_host.dll` and
calls four functions (`src/app_win/host.h`): `asb_host_begin` (first thing
in the process: data-folder access list, command line, single-instance
lock; says whether to run, hand over to the running instance or be the
`--headless` daemon), `asb_host_run_headless`, `asb_host_start(shell window,
callback)` and `asb_host_command(json)`.

- **One thread.** `Program.cs` starts WinUI on a thread with an 8 MB stack
  (and the exe reserves 8 MB for the threads the host starts: `VmConfig` /
  `VmInstance` are about 600 KB). The host creates its own never-shown
  window on that thread (`ui_create_main_window`): it receives the messages
  of the backend, the local API, a second launch (`UI_SHOW_MAIN_MESSAGE`,
  `WM_COPYDATA` for `--vm`) and Windows ending the session.
- **Shell → host**: `HostBridge.Send(action, fields)` → `asb_host_command` →
  posted to the host's window (`WM_SHELL_COMMAND`) → `on_shell_message` →
  `handle_shell_message` in `ui.c` (one `else if` per action). Never called
  synchronously from a XAML handler: a command can wait on HCS or open a
  dialog, and both dispatch messages. Actions: `uiReady`, `createVm`,
  `startVm`, `shutdownVm`, `stopVm`, `confirmForceStop`, `connectIddVm`,
  `sshConnect`, `deleteVm`, `deleteTemplate`, `editVm`, `setDisplayMode`,
  `setVideoCodec`, `setVideoBitrate`, `setPerformanceMode`, `ejectIso`,
  `getVmIcon`, `setVmIcon`, `clearVmIcon`, `createShortcut`,
  `updateGuestTools`, `exportVm`, `importVm`, `snapTake`, `snapDelete`,
  `snapDeleteBranch`, `snapRename`, `setViewerPrefs`,
  `setAutoUpdateGuestTools`, `setCloseBehavior`, `getMcpStatus`,
  `setMcpEnabled`, `setMcpReadOnly`, `getDiskSpace`, `browseDiskDirectory`,
  `browseImage`, `enableFeature`, `enableFeatureReboot`, `openFolder`,
  `openVmFolder`, `copyVmPath`, `copyDiagnostics`, `openLink`,
  `showDocument`, `dismissVmError`, `cancelExit`, and `requestExit
  {fromClose}` (the window's close button or Quit in the tray: the host
  decides with the user what happens to running VMs).
- **Host → shell**: `ui_bridge_post(json)` → the callback → the shell copies
  the text and handles it from its dispatcher queue (`AppState.OnMessage`).
  Types: `vmListChanged {vms, hostInfo}`, `fullState`, `hostInfo`,
  `templates`, `coreLoading`, `coreReady`, `prereqRequired`, `prereqReboot`,
  `prereqProgress`, `prereqResult`, `log`, `alert`, `notice`, `createResult`,
  `document`, `vmIcon`, `diskSpace`, `browseResult`,
  `diskDirectoryBrowseResult`, `exitProgress`, `mcpStatus`, `window {show}`
  (show the window, or hide it to the tray) and `exit` (the host has shut
  down: the process ends). Per-VM fields are built in `build_vm_json`, host
  fields in `build_host_info_json`; `VmInfo.Parse` / `HostInfo.Parse` read
  them.
- **Adding something**: add the field or action in C, read it in `Models/`,
  show it in a page. Pages listen to `AppState` events in `Loaded` and stop
  in `Unloaded`; they keep no copy of the state.
- **Dialogs**: file and folder pickers and the exit questions are native
  (`ui.c`, owned by the shell's window: WinRT pickers do not work elevated).
  Everything else is a `ContentDialog` through `Dialogs.ShowAsync`, which
  queues them (WinUI shows one at a time), so never open one from inside
  another: use a flyout.
- **Design**: the shell follows the owner's other WinUI apps (GCapture,
  InputMapper, GMic, GDeck): the shared styles in `App.xaml`
  (`PageIconTileStyle`, `CardStyle`, `SectionLabelStyle`, `RowGlyphStyle`,
  `TrayMenuItemStyle`…), `TitleBar` + `NavigationView`, pages in a
  `ScrollViewer Padding="36,24,36,24"`, one card per setting row, Segoe
  Fluent glyphs, Mica by default, and the notification-area menu as a small
  acrylic WinUI window (`TrayMenuWindow`; one click opens it, a double click
  the app). `McpSettingsCard.cs` is the same file in every app.
- Long VM operations (force stop, delete, shut down) run on a worker thread
  in the host and answer with `notice`/`createResult`/`exitProgress`
  messages. The core starts on a worker thread: `ui.c` names its thread with
  `asb_set_ui_thread` first so HCS waits made on it keep pumping messages.

## 8. Headless API (agent-facing)

`src/app_win/headless.c`: http.sys on
`127.0.0.1:8787` (+20 ports), bearer token and port in
`%ProgramData%\AppSandbox\host.json`. Routes under `/v1`: `version`,
`events` (SSE), `host`, `templates`, `vms` (list/create), `vms/{name}`
(status/edit), `start`, `shutdown`, `stop`, `delete`, `sshInfo`, `display`
(status/open/close), `snapshots` (list/take/rename/delete/branches),
`shutdown` (daemon). Every `vms/{name}` route also exists as
`vm-ids/{id}`. SDK: `tools/headless-api/asb.py`; full reference:
`tools/headless-api/README.md`.

## 9. MCP server for AI agents

Lets an AI agent (Claude Code, Codex, Claude Desktop…) use App Sandbox as a
disposable, GPU-capable sandbox — in particular to test drivers and other
low-level things in a VM: copy a driver in, load it, see whether the guest
bugchecked, read the result, go back to a snapshot, try again. It follows
the contract shared by the owner's apps (`..\CLAUDE.md`, section "MCP");
`python ..\tools\mcp-check.py appsandbox` checks it against the running app.

- **Endpoint**: `POST http://127.0.0.1:47841/mcp`, server name `appsandbox`.
  Streamable HTTP, one JSON-RPC message per request, `application/json`
  answers, no SSE, no sessions, no stdio. Legacy protocol versions
  (`initialize`) and the stateless `2026-07-28` (`server/discover`,
  `resultType`, `ttlMs`, `cacheScope`) on the same address.
- **Off by default.** Settings → AI agents (MCP) turns it on (saved as
  `McpEnabled` / `McpReadOnly` under `[Settings]` in `vms.cfg`); the daemon
  takes `--headless --mcp` or `--headless --mcp-read-only`. The switch takes
  effect at once: `mcp_set_enabled` adds or removes the address
  (`http://127.0.0.1:47841/mcp/` and the `localhost` form) on the local
  API's http.sys URL group. The port is fixed; if another program has it,
  Settings says so.
- **No token** (anything running as the user could read `host.json`
  anyway). Web pages are kept out instead: `Host` must be `127.0.0.1`,
  `localhost` or `[::1]` (403; http.sys itself does not check it), an
  `Origin` that is not loopback is 403, a `Content-Type` other than
  `application/json` is 415, `GET`/`DELETE` are 405. Never 401.
- **Code**: `src/app_win/mcp.c` (HTTP checks, a small JSON reader — the flat
  `json_get_*` scanners cannot read nested `params.arguments` —, JSON-RPC,
  the tool table `g_tools`, `g_instructions`, recent activity for Settings).
  `headless.c` only routes: `api_thread` hands requests that arrived on port
  47841 to `mcp_handle_http`, which reads the body and answers on a worker
  thread per request, so `wait_for_vm` or a ten-minute `run_command` never
  blocks the window thread or other requests.
- **One implementation per operation**: lifecycle tools call the REST routes
  in-process with `asb_api_call` (same `handle_request`, same validation; the
  answer is captured like the app's own API calls, on the window thread in
  the app and under `g_direct_cs` in the daemon). `asb_api_run` runs a
  function in that same context for the few things without a route.
- **Tools** (`vm` is always the VM's name or id): `list_vms`, `get_vm`,
  `get_host`, `create_vm`, `start_vm`, `shutdown_vm`, `stop_vm`, `delete_vm`,
  `wait_for_vm` (until `online` / `stopped` / `restart`, at most 100 s per
  call), `restart_vm`, `run_command`, `copy_to_guest`, `copy_from_guest`,
  `install_driver`, `uninstall_driver`, `get_devices`, `get_debug_output`,
  `set_verifier`, `get_guest_crash`, `analyze_dump`, `screenshot`,
  `send_input`, `list_snapshots`, `take_snapshot`, `restore_snapshot` (start
  from a snapshot on a new branch), `delete_snapshot`, `ssh_info`,
  `open_display`, `close_display`, `get_log`. Read-only mode hides and
  refuses every tool whose `changes` flag is set: it leaves `list_vms`,
  `get_vm`, `get_host`, `wait_for_vm`, `get_devices`, `get_guest_crash`,
  `screenshot`, `list_snapshots`, `ssh_info` and `get_log`.
- **Prompts** (`prompts/list`, `prompts/get`; slash commands in Claude Code):
  `test_driver` (vm, driver) and `new_test_vm` (vm), in `g_prompts`. They are
  text for the agent to follow with the tools and run nothing themselves.
- **Driver tools** run PowerShell scripts in the guest: sources in
  `src/app_win/mcp_scripts/*.ps1`, turned into C strings in `mcp_scripts.h`
  by `python tools/gen-mcp-scripts.py` (run it after editing a script;
  `--check` tells whether the header is current). `guest_script` writes the
  script and a JSON parameter file into `C:\Windows\Temp\AppSandbox` and
  runs `powershell -File`; arguments never reach a command line. Each script
  prints one JSON object.
  - `install_driver`: copies the .inf's folder (or one .sys) to
    `C:\Drivers\<name>`, reports test signing / Secure Boot / signatures,
    trusts the signer in the guest (Root + TrustedPublisher), runs `pnputil
    /add-driver /install` or `sc create` + `sc start`, returns the published
    name and the devices with their problem codes. It warns when the VM has
    no snapshot; it cannot take one, because snapshots need a stopped VM.
  - `get_debug_output`: DbgPrint text through the NT Kernel Logger session
    with `EVENT_TRACE_FLAG_DBGPRINT` (0x40000), written to an .etl; `read`
    stops the session, decodes the file with `tracerpt` (XML) and starts it
    again, so lines printed in that second are lost. Only output that passes
    the Debug Print Filter is traced; `enable_filter` sets the registry
    values (reboot needed). `providers` adds a second session for ETW / WPP
    providers whose .etl `stop` returns.
  - `set_verifier`: `verifier.exe /standard | /flags | /reset`, optional
    restart. `restart_vm`: `shutdown /r` and a wait
    for the agent to go offline and come back.
  - `analyze_dump`: finds `cdb.exe` / `kd.exe` on the host (Windows Kits
    Debuggers, PATH, the WinDbg app's folder), copies the dump out with the
    `copy_from_guest` code and runs `-z <dump> -c "!analyze -v; q"` as the
    non-elevated user (`run_plain_process`, symbol path in
    `_NT_SYMBOL_PATH`, cache in `%LOCALAPPDATA%\AppSandbox\symbols`), then
    picks the fields out of the text. Without a host debugger it uses one in
    the guest; with none anywhere it says how to install one.
- **send_input**: `InputPacket`s on channel 3. With the VM's window open
  they go through the window's own input connection
  (`vm_display_idd_send_packets`, a few at a time from the request context);
  otherwise `asb_guest_send_input` (`vm_exec.c`) connects, waits for `IRDY`
  and sends. Text becomes key presses with the host's keyboard layout
  (`VkKeyScanW`): the guest helper has no Unicode packet. The result names
  the window that had the focus (`foreground.ps1`, best effort).
- **Guest tools** use the command channel (section 4, channel 8), not SSH and
  not the control channel: `vm_exec.c` opens one Hyper-V socket connection
  per call to the guest agent, which runs the command as SYSTEM (Windows:
  `cmd.exe /c`, PowerShell through `-EncodedCommand`, or directly; in a job
  object so the time limit ends the whole process tree), with stdout and stderr captured
  separately and capped. File copies stream in 256 KiB chunks; the guest
  writes to `<name>.asbpart` and renames. `copy_from_guest` saves only into
  the user's `Downloads\App Sandbox\<vm>`; `copy_to_guest` reads a host path
  with the user's non-elevated rights (rule 5.7). `get_guest_crash` runs a
  fixed read-only script in the guest (bugcheck events 1001, Kernel-Power 41,
  the dump files) and adds the agent's crash lines from the host log.
- **Readiness of the command channel**: the agent's listener retries every
  3 s for as long as the service runs (early in a warm restart the bind can
  fail) and listens again when accept keeps failing. The agent announces
  `caps:exec` on channel 1 after `hello` (`VmInstance.agent_caps`), so the
  host can tell "not listening yet" (it retries for 12 s, then says so) from
  "agent too old". `wait_for_vm`, `restart_vm` and `set_verifier`'s restart
  report online only once a PING on the channel answers
  (`command_channel_ready`).
- **run_command with `desktop`**: `ASX_EXEC_AS_USER` runs the command with
  the signed-in user's token (`WTSQueryUserToken`; `elevated` takes its
  linked administrator token) and environment, outside the kill-on-close
  job, so a program it starts stays open. The agent announces `user` in its
  PING answer. `ASX_EXEC_DESKTOP` (SYSTEM in the user's session) is what the
  screenshot and focused-window scripts use.
- **Delivery of the guest side** (rule 5.3): the agent with the channel
  reaches an existing Windows VM through the normal guest-components update
  (before its next start when automatic updates are on); a VM whose agent is
  older answers the tools with "update the guest components".
- **screenshot**: with the VM's window open, a copy of the window's picture
  (`vm_display_idd_copy_frame`; not available in performance mode, where the
  window holds no CPU copy); otherwise `asb_display.c` connects to the frame
  channel for a few seconds (a new frame connection starts in raw mode and
  replaces any other reader, which is why the open window is never
  bypassed). The driver only sends when the screen changes or when it still
  holds a raw frame, so after a performance-mode window on a still desktop
  nothing arrives. In both cases the fallback copies the screen inside the
  guest: `screenshot.ps1` runs in the signed-in user's desktop session
  (`ASX_EXEC_DESKTOP`; the agent announces it as `desktop` in its PING
  answer) and the PNG is read back over the command channel. It cannot see
  the sign-in or lock screen.
- **Audit**: every call is kept in memory for Settings (last 50: time, tool,
  VM, failed or not, and the last client's name); every call of a tool that
  changes something also writes an `MCP: tool (vm) -> ok/failed` line to the
  host log.
- **Adding a tool**: write `tool_xxx(ToolCall *, const JVal *args)` in
  `mcp.c` (answer with `result_json`, `result_text` or `result_error`; an
  error text says what to do next), add its row to `g_tools` (title,
  description for the model, flat input schema, the four annotations,
  `changes`), mention it in `g_instructions` if it is part of a flow, list
  it in `tools/headless-api/README.md`, and run `mcp-check.py`. If it needs
  the guest, add an operation to `asb_exec_protocol.h` (old agents answer
  `ASX_STATUS_UNSUPPORTED`) and to the agent.
- **Security** (the app is elevated, the endpoint has no token, the guest
  is not trusted):
  - Host files are opened, and the debugger is started, with a de-elevated
    token (`plain_user_token`: Safer "normal user", medium integrity). There
    is no fallback: when it cannot be made the operation is refused.
  - Files from a guest land only in `Downloads\App Sandbox\<vm>`: the name
    is reduced to a plain file name (no separators, `:`, device names), the
    file is created with the user's rights and a reparse point in the way is
    refused.
  - Every length the guest sends on the command channel is checked before
    use (`vm_exec.c`); the frame reader caps frame and access-unit sizes.
  - Requests: Host / Origin / Content-Type are checked before the body is
    read; body at most 8 MB; JSON depth 64 and 200 000 values; at most 32
    requests in flight. The guest calls work on a copy of the VM's record
    (`AsbGuestTarget`, filled in the request context), never on the core's
    table.
  - Text from the caller that reaches the host log (the VM name) has its
    control characters removed.
  - Accepted: any program of any user signed in on this PC can reach
    127.0.0.1:47841 (the contract has no token), so with the server on they
    can drive VMs and, through `copy_to_guest` + `run_command`, read files
    the app's user can read. Turn the server off on a shared PC.
- **Not done**: automatic snapshots (they need a stopped VM); Unicode text
  in `send_input`; Ctrl+Alt+Del; export/import and VM edits.

## 10. Other roadmap items

### Drag and drop files into the VM window ("Inbox")

Drop files from the host onto the VM window; they are copied into an
**Inbox** folder in the guest (e.g. `C:\Users\<user>\Desktop\Inbox`), and
the folder opens when the copy finishes.

- **Host, receiving the drop**: `vm_display_idd.c` calls `DragAcceptFiles`
  on the VM window and handles `WM_DROPFILES` (`DragQueryFileW`). App
  Sandbox runs elevated, and UIPI blocks drops from the non-elevated
  Explorer: allow them with `ChangeWindowMessageFilterEx` for
  `WM_DROPFILES`, `WM_COPYDATA` and `WM_COPYGLOBALDATA (0x0049)`. OLE
  `IDropTarget` does not work across integrity levels; use `WM_DROPFILES`.
  Folders are walked recursively, and the relative paths are kept.
- **Transport**: reuse the clipboard file transfer (`vm_clipboard.c`,
  FILE_DATA messages, 1 MiB chunks, host→guest channel 5) with a new
  "deliver to Inbox" message type. Alternatively add an agent command on
  channel 1. Either way, bump the protocol and ignore the message on old
  guests (rule 5.1).
- **Guest**: the receiver writes to a temporary name and renames it when
  done, resolves name clashes (`name (2).ext`), refuses `..` and absolute
  paths, and creates the Inbox in the interactive user's profile (the
  clipboard SYSTEM writer must resolve that user's desktop).
- **UX**:
  - Show a drop overlay on the window while dragging over it.
  - Show progress in the floating toolbar, with a cancel option.
  - Write a log line per batch.
  - Set limits: warn above a few GB, and stop if the guest disk is low.
- **Security**: one direction only (host→guest), and only for files the
  user dropped. The guest can never ask the host for files. No change to
  what the guest can read on the host.


- Controllers: bundle ViGEmBus (license OK, BSD-3; needs owner approval),
  rumble back channel (needs a host reader on ch3), PlayStation controllers
  (host HID/GameInput), stats overlay (FPS, encode/decode time, bit rate).
- Secure shared folders (read-only or allow-listed).
- Localisation (Spanish UI): the shell's strings are in code and XAML; the
  owner's other apps use `Strings\<language>\Resources.resw` with `x:Uid`.
- Start with Windows: the other apps use a `windows.startupTask`; an
  elevated app is not started that way (Windows shows no UAC prompt at
  sign-in), so it needs a scheduled task with highest privileges.

## 11. Working conventions

- Small, focused commits; message explains why. Keep comments describing
  behaviour, not history.
- Test builds: push to `release/test` (fork) → CI publishes `test-build-N`
  (`*-msix.zip`: the package, its test certificate, `Install-AppSandbox.ps1`).
- When unsure whether a change reaches existing VMs, assume it does not and
  check rule 3.

## 12. Current state (handoff)

Read this first when continuing the work on another machine or with another
agent. Keep it short and current.

- **Branches** (fork `Gabrieli2806/appsandbox`): the WinUI app is on
  `feature/winui`; `release/test` is fast-forwarded to the branch being
  tested and CI ("Windows test build") publishes `test-build-N`
  (`*-msix.zip`). Also on push: `ci.yml` and CodeQL. Upstream is
  `jamesstringer90/appsandbox` (`main`).
- **WinUI app** (0.2.0): Linux guests and the macOS host removed; the
  WebView2 page replaced by `app/AppSandbox` over `appsandbox_host.dll`
  (sections 2, 3, 7). Checked on the build PC: native and app builds, every
  page and dialog with recorded host messages (`ASB_UI_DEMO`), the real
  host started without elevation (it reports Virtual Machine Platform as
  off), a second launch showing the hidden window, closing with no VM
  running, the MSIX built and signed by `make-msix.ps1`. Not yet checked
  elevated on real VMs: create, start, the VM window, shutdown with VMs
  running (exit question, tray), `--vm` shortcuts, the MCP server from the
  packaged app, Explorer restart bringing the tray icon back.
- **Local checks**: the native user-mode projects build with Build Tools 2026
  (`/p:PlatformToolset=v145`); the drivers need the WDK (CI only). The owner
  tests on real hardware.
- **Done recently** (test-build-27 to 29): adaptive-resize look (rounded
  picture, shadow, dimmed blur); viewer teardown/use-after-free and
  GPU-reset recovery; Settings → Folders and logs, About; exit confirmation
  with running VMs; worker-thread VM ops; WebView2 lockdown and
  ProcessFailed handling; core fixes (export/import, ISO auto-detach,
  snapshots, data/VM-folder ACLs, private-namespace instance lock);
  clipboard hardening; Store packaging + `docs/RELEASE_CHECKLIST.md`;
  guest agent: Windows GPU-PV 0x3B crash workaround (feature overrides),
  bugcheck reports, log rotation, VDD HIGH GPU priority; D3D12 reads decoded
  frames in place.
- **Stability pass** (after test-build-30): guest state (`vm.vmgs`) stays in
  the VM folder across snapshots; unreadable `vms.cfg` no longer overwritten;
  stop/delete vs. HCS exit callback; re-entrant `reconcile_displays`; headless
  API loop survives a dropped request; viewer (swap chain sized at open, one
  queued frame message, cross-thread log posted); clipboard and agent socket
  teardown; `Install-AppSandbox.ps1` installs its exact package. The host
  projects build locally with Build Tools 2026: add
  `/p:PlatformToolset=v145` (drivers still need the WDK, CI only).
- **MCP server** (section 9): the lifecycle tools, `run_command`, the file
  copies, `get_guest_crash` and the de-elevated host file access were
  verified in the real app on a Windows 11 VM. Added after that and only
  checked against a test harness (real `mcp.c` on http.sys, app and guest
  stubbed; `vm_exec.c` against the agent's channel code over TCP): the
  driver tools (`install_driver`, `uninstall_driver`, `get_devices`,
  `set_verifier`, `get_debug_output`), `restart_vm`, `analyze_dump` (no
  debugger was installed on the build PC), `send_input`, the prompts, and
  the screenshot fallback inside the guest (needs the updated agent). The
  PowerShell scripts ran on the host where they need no administrator.
  A second round on the VM confirmed the in-guest screenshot, `send_input`
  through the window, `get_devices`, `restart_vm` and the capture mechanics
  of `get_debug_output`. Still unproven: decoding of real DbgPrint output
  (no test-signed driver could be loaded; the result lists
  `otherTraceEvents` when no DbgPrint line was found and `undecodedSample`
  when one was found but not read), `analyze_dump` with a real debugger,
  `run_command` with `desktop` as the signed-in user, and the command
  channel coming back after a guest restart (the listener's retry).
  The host log line `Monitor: props query failed 0x8037010D` every 5 s is
  the HCS monitor thread in `hcs_vm.c`, not the MCP server.
- **Open / to verify**:
  - From the stability pass, not changed: `gpu_crash_build_status` never
    returns "fixed" for build 26100 (section 6 says UBR ≥ 9818 for all three
    builds; confirm against the KB); the VDD teardown waits 5–10 s for its
    threads and then frees their state (`VddDestroySwapProc`); the guest
    clipboard writer does not match `FORMAT_DATA_RESP` to its request and
    shares `g_clip_fmts` between threads without a lock; guest helpers have no
    invalid-parameter handler, so a `swprintf_s` path overflow ends them.
  - D3DMappingLayers signature/digest check (`d3dlayers.c`) is unverified
    against a real download; if guests lose GL/Vulkan, relax it.
  - Not done: shortcuts by VM id (`--vm-id`), Start-menu shortcuts under
    MSIX (`FOLDERID_CommonPrograms` when packaged), theme following Windows
    by default, x64-on-ARM64 start-up check, per-OS RAM minimum, cancel for
    long operations, the shell's strings in `.resw` (Spanish), start with
    Windows (scheduled task), trimming the WebView2 / ONNX files the Windows
    App SDK metapackage brings into the package.
- **Owner decisions** (see `docs/RELEASE_CHECKLIST.md`): product name and
  branding (upstream's "App Sandbox"), code-signing certificate, driver
  attestation signing (guests still need test signing), privacy policy,
  bundling ViGEmBus.

