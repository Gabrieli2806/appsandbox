# AGENTS.md — guide for AI coding agents

This file tells an AI agent (Claude Code, Codex, Cursor, …) how App Sandbox is
built, where things live, and the rules that keep changes from breaking real
VMs. Read it before editing. Humans: README.md is the user-facing overview.

## 1. What App Sandbox is

A VM manager for Windows 11 hosts (HCS/HCN, no Hyper-V Manager) and macOS
hosts (Virtualization.framework, plus QEMU+HVF for Windows-on-Mac). Windows
guests get GPU-PV, an IddCx virtual display driver, audio, clipboard, input
and SSH over Hyper-V sockets. Ubuntu guests get the same through Linux
daemons and a DRM driver. The UI is HTML/JS in WebView2 (WKWebView on Mac);
everything else is C (C++ only where an API needs it).

## 2. Repository map

| Path | What |
|---|---|
| `AppSandbox.sln` | Windows solution (Debug/Release × x64/ARM64) |
| `AppSandbox.vcxproj` | `AppSandbox.exe`: GUI + `--headless` daemon. C (`CompileAsC`); `vm_video_decode.cpp` is per-file C++ |
| `AppSandboxCore.vcxproj` | `appsandbox_core.dll`: VM orchestration (C, `ASB_BUILDING_DLL`) |
| `src/app_win/` | `main.c` (entry, single instance, `--vm` shortcuts), `ui.c` (window, tray, WebView2 commands), `webview2_bridge.c`, `headless.c` (HTTP API) |
| `src/backend_win/` | `asb_core.c` (orchestrator, `vms.cfg`), `hcs_vm.c` (HCS JSON, GPU-PV, Plan9 shares, hvsocket service table), `hcn_network.c`, `gpu_enum.c`, `disk_util.c` (new-VM staging), `guest_tools.c` (offline guest update), `snapshot.c`, `vm_agent.c` (agent channel), `vm_display_idd.c` (viewer window), `vm_video_decode.cpp` (H.264/HEVC decode), `vm_clipboard.c`, `vm_ssh_proxy.c`, `prereq.c` |
| `src/core/` | Platform-neutral: `protocol.h` (input packets), `asb_events.*`, `asb_types.h`, `asb_vm_id.h` |
| `src/app_mac/`, `src/backend_mac/` | macOS app (Objective-C), `AppSandbox.xcodeproj` |
| `web/` | `index.html`, `app.js`, `style.css` (one UI for both hosts) |
| `tools/agent/` | Windows guest: `agent.c` (service `AppSandboxAgent`), `appsandbox-input.c` (SendInput + ViGEm gamepads), `appsandbox-displays.c`, clipboard, audio, `p9copy.c` |
| `tools/vdd/` | IddCx display driver (UMDF2, C++17): `vdd.cpp`, `vdd.h` (wire protocol), `vdd_encoder.cpp` (NVENC / Media Foundation), `ThirdParty/` (UMDF headers, `nvEncodeAPI.h` MIT) |
| `tools/vad/`, `tools/ivshmem/` | Virtual audio driver (KMDF), ivshmem driver for Mac hosts |
| `tools/transport/` | `asb_transport.*`: one API over AF_HYPERV (PC) or ivshmem rings (Mac) |
| `tools/linux/` | Ubuntu guest: daemons (`agent/`), `asb_drm/` display driver, `dxgkrnl/`, `wsl-mesa/` |
| `tools/iso-patch/` | ISO/VHDX builder, repo prefetch for Linux guests |
| `tools/headless-api/` | Python SDK `asb.py`, API reference README, examples, integration tests |
| `tools/sign/` | Release/sign scripts (EV signing needs the owner's hardware key) |
| `vendor/` | devcon, WebView2, QEMU (Mac), virtio |
| `.github/workflows/` | `windows-test-build.yml` (Release x64 build, zip + MSIX, pre-release `test-build-N` on `release/**` pushes), `ci.yml` (build check x64 + ARM64 on PRs and other branches, web UI syntax and `tools/check-sources.py`), `codeql.yml` (CodeQL for the host C/C++) |

Stale, ignore: `tools/agent/build.cmd`, `tools/agent/deploy.bat`, the
`src\ui.h` entry in `AppSandboxCore.vcxproj`, links to
`docs/linux-idd-implementation-plan.md` and `tools/shm-test` (do not exist).

## 3. Building and testing

- Windows: Visual Studio 2022 + Windows SDK + **WDK**. From a *cmd* prompt
  (not PowerShell 7 — the driver post-build steps need Windows PowerShell 5):
  `msbuild AppSandbox.sln /p:Configuration=Release /p:Platform=x64`.
  Output: `bin\Release\` (`AppSandbox.exe`, `resources\` guest exes, `drivers\`).
- Drivers are **test-signed** per build; the `.cer` is extracted and must ship
  next to them (App Sandbox enables test signing in the guest).
- Version: `Directory.Build.props` (`AsbVersionMajor/Minor/Patch/Revision`).
  CI passes `/p:AsbVersionRevision=<run number>` so every build's driver
  package is newer than the last (Windows will not "update" to the same
  DriverVer). `AsbSourceRepo`/`AsbSourceRef` decide where Ubuntu guests fetch
  their sources from.
- No unit tests. Verification = CI build + manual run on a real Windows host
  with a GPU. `tools/headless-api/tests` is a slow integration suite (real
  ISOs, elevated daemon). Without a Windows box you can still syntax-check C
  with `x86_64-w64-mingw32-gcc -fsyntax-only` (include `-Isrc -Isrc/core
  -Isrc/app_win -Isrc/backend_win -Ivendor/webview2 -Itools/transport`);
  driver code needs the WDK and can only be compiled in CI.
- JS: `node -e "new Function(require('fs').readFileSync('web/app.js','utf8'))"`
  catches syntax errors.
- CI: `ci.yml` builds Release x64 and ARM64 for every pull request and every
  push outside `release/**` (an ARM64 failure does not fail the run yet), and
  runs the JS check and `tools/check-sources.py` on Linux. `codeql.yml` runs
  CodeQL on the user-mode host projects for pull requests, `main`,
  `release/**` and once a week. Set `ASB_SKIP_PACKAGE_PROJECT=1` (environment
  variable or `/p:`) to stop `AppSandboxPackage` from running
  `make-release.ps1` after a Release build.

## 4. Host ↔ guest channels

Hyper-V socket services `a5b0cafe-000N-4000-8000-000000000001` (Windows
guests) / vsock port N (Linux guests), registered in `hcs_vm.c` (ports 1–6):

| N | Channel | Format |
|---|---|---|
| 1 | Agent | newline text, optional `tag:` prefix. Guest→host: `hello`, `heartbeat`, `gpu_query`, `gpu_copy_*`, `idd_status:`, `hyperv_video:`, `displays:`, `log:<text>` (shown in the host log), `ssh_*`. Host→guest: `ping`, `shutdown`, `restart`, `gpu_query_response:`, `set_ip:`, `ssh_enable`, `idd_connect`, … |
| 2 | Display (VDD) | binary, `tools/vdd/vdd.h` (mirrored in `vm_display_idd.c`) |
| 3 | Input | 20-byte `InputPacket` (`src/core/protocol.h`) after an `IRDY` greeting |
| 4 | Audio | `ASA1` header then length-prefixed PCM |
| 5/6 | Clipboard host→guest / guest→host | `vm_clipboard.c` |
| 7 | SSH | proxied TCP |

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
2. **Mirrored definitions are kept in sync by hand** (except the display
   protocol, which is one shared header): audio header (`appsandbox-audio.c` ↔ viewer);
   clipboard (Windows host, Mac host, guests); `protocol.h` (shared).
3. **Guest-side changes need delivery.** New VMs get files from
   `disk_util.c`. Existing Windows VMs get the 6 helper exes and the VDD
   package from `guest_tools.c` (offline, VM stopped, signature in
   `guest_tools.sig`); the agent installs a staged VDD at the next boot
   (`apply_pending_vdd_update`, result reported with `log:`). If you add a
   guest file, add it to **both** lists. VAD/ivshmem changes need a new VM.
   Linux guests compile from GitHub at `ASB_SOURCE_REPO@ASB_SOURCE_REF`.
4. **Driver (UMDF/IddCx) constraints** (`tools/vdd`): built with /WX; some
   Win32 macros are missing (use `0` instead of `WAIT_OBJECT_0` in new files);
   IddCx APIs must run on WDF threads (use WDF timers, not raw threads); the
   cached D3D device must survive IddCx teardown; load optional DLLs at run
   time (`mfplat.dll`, `nvEncodeAPI64.dll`) so a missing one only disables a
   feature. The VDD log is `C:\ProgramData\AppSandboxVDD.log` in the guest.
5. **Mac host (ivshmem)**: one writer per shared word, no atomic RMW on the
   BAR, VDD stays at 1920x1080@60 (`legacySingleMode`).
6. **C by default.** New host/guest code is C (`CompileAsC`). Use C++ only
   for COM-heavy APIs (Media Foundation, D3D video) in its own file with an
   `extern "C"` header and a per-file `<CompileAs>CompileAsCpp</CompileAs>`.
7. **Elevation**: the app requires admin (HCS, http.sys). Do not add features
   that read or write host files on behalf of the guest without an explicit,
   narrow user choice — the VM is untrusted.
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

## 6b. Manual-install VMs (os type "Other")

For OSes App Sandbox cannot provision: an empty disk plus the ISO, no
account, no guest agent, GPU, SSH or display driver (`asb_vm_create` forces
them off; `vm_agent_start` / `idd_probe_start` return early). The screen is
the Hyper-V basic console: `hcs_vm.c` adds `VideoMonitor.ConnectionOptions`
(`\\.\pipe\<name>.BasicSession`, ACL = current user) and `ui.c`
`do_connect_vm` routes these VMs to the RDP viewer (`vm_display.c`). Secure
Boot is skipped. `install_complete` is TRUE from creation. The ISO stays
attached until "Remove installer ISO" (`asb_vm_eject_iso`). Shutdown falls
back to `HcsShutDownComputeSystem`. Not yet: headless API create for
"Other", import of "Other" exports, Secure Boot option.

## 7. UI ↔ C

`web/app.js` → `sendCmd(action, data)` → `on_webview2_message` →
`handle_webview2_message` in `src/app_win/ui.c` (one `else if` per action).
State returns as JSON via `webview2_post`: `vmListChanged {vms, hostInfo}`,
`hostInfo`, `fullState`, `log`, `alert`, …; per-VM fields are built in
`build_vm_json`, host fields in `send_host_info`. Add the field in C, render
it in `app.js`, style in `style.css`. Keep `.win-only` for Windows-host UI.

## 8. Headless API (agent-facing)

`src/app_win/headless.c` (Mac mirror: `src/app_mac/headless.m`): http.sys on
`127.0.0.1:8787` (+20 ports), bearer token and port in
`%ProgramData%\AppSandbox\host.json`. Routes under `/v1`: `version`,
`events` (SSE), `host`, `templates`, `vms` (list/create), `vms/{name}`
(status/edit), `start`, `shutdown`, `stop`, `delete`, `sshInfo`, `display`
(status/open/close), `snapshots` (list/take/rename/delete/branches),
`shutdown` (daemon). Every `vms/{name}` route also exists as
`vm-ids/{id}`. SDK: `tools/headless-api/asb.py`; full reference:
`tools/headless-api/README.md`.

## 9. Roadmap: MCP server (how to do it right)

Goal: let AI agents use App Sandbox as a disposable, GPU-capable sandbox.

- **Build it on the headless API**, not inside the GUI. Either a small
  stdio MCP server (Python, reusing `asb.py`, shipped under
  `tools/mcp/`) or a `--mcp` mode of `AppSandbox.exe` that speaks MCP over
  stdio and calls the same core functions as `headless.c`. Prefer the Python
  wrapper first: no new attack surface in the elevated process.
- **Tools** (thin over existing routes): `list_vms`, `vm_status`,
  `start_vm`, `shutdown_vm`, `stop_vm`, `take_snapshot`, `restore_snapshot`
  (start from snapshot), `list_snapshots`, `ssh_info`, `wait_online`.
- **New capabilities that need guest work** (add routes to `headless.c` and
  agent commands first, then expose them):
  - `run_command`: agent command on ch1 that runs a process in the guest and
    returns exit code + output (size-capped, time-limited).
  - `screenshot`: one frame from the viewer path (`asb_display.c` already
    receives frames headless) encoded as PNG.
  - `send_input` (keys, text, click at x,y) on ch3 via `asb_display.c`.
  - `copy_file_in` / `copy_file_out`: restricted to one per-VM exchange
    folder; never arbitrary host paths.
- **Safety**: off by default (setting + explicit opt-in), loopback only,
  token from `host.json`, per-tool permissions (read-only vs. control vs.
  destructive), automatic snapshot before destructive actions, no host
  file access outside the exchange folder, audit lines in the host log.
- Document every tool in `tools/headless-api/README.md` and add an example.

## 10. Other roadmap items

### Drag and drop files into the VM window ("Inbox")

Drop files from the host onto the VM window; they are copied into an
**Inbox** folder in the guest (e.g. `C:\Users\<user>\Desktop\Inbox`, or
`~/Desktop/Inbox` on Ubuntu), and the folder opens when the copy finishes.

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
  guests (rule 5.1), and update the Linux guest daemon too.
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
- Localisation (Spanish UI) with a language selector.
- Packaging: MSIX is possible for the app, but HCS, drivers and elevation
  need care (see README "Packaging").

## 11. Working conventions

- Small, focused commits; message explains why. Keep comments describing
  behaviour, not history.
- Test builds: push to `release/test` (fork) → CI publishes `test-build-N`.
- When unsure whether a change reaches existing VMs, assume it does not and
  check rule 3.
