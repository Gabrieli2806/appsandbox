<img width="1028" height="446" alt="App Sandbox" src="https://github.com/user-attachments/assets/f66dcaff-201e-4cf5-8e23-bc480338f353" />


# App Sandbox

App Sandbox is a virtual machine app for Windows and macOS that's focused on performance and ease of use.

Windows features:
- Works on Windows 11 Home or Pro, without Hyper-V
- Windows 11 or Ubuntu 26.04 LTS VM Support
- Zero touch install
- Copy and Paste
- 2 Channel Audio
- GPU Acceleration via Paravirtualization (GPU-PV) with support for DirectX 12 (Windows only), OpenGL, Vulkan, CUDA, OpenCL
- GPU Hardware Video Decoder/Encoder support
- SSH via Hyper-V socket proxy (no network required)
- Snapshots
- Display up to 4K at 60/120/144 Hz, fixed sizes or **adaptive** (follows the window, with a blurred backdrop while it catches up)
- **Performance mode** (default): the VM's GPU encodes the screen as H.264 or HEVC (NVENC on NVIDIA, Media Foundation otherwise) and the viewer decodes it on your GPU, like Moonlight — smooth at high resolutions and frame rates. **Quality mode** sends exact, uncompressed pixels. Codec and bit rate are per VM.
- **Game controllers**: Xbox (XInput) controllers on the host appear in the guest as Xbox 360 controllers (needs ViGEmBus in the guest)
- Floating toolbar, FPS/resolution in the title bar (both optional), borderless fullscreen
- Direct3D 11 or Direct3D 12 renderer for the VM window (Settings → VM windows)
- Custom icon per VM; **manual install** of any OS from its ISO ("Other")
- Settings → **Folders and logs** (open the VM, data, log and app folders; copy diagnostic info) and **About**
- Stability: the guest agent works around the Windows GPU-PV crash (bugcheck 0x3B with Edge/Discord/Steam) and reports guest crashes to the host log; the viewer recovers from GPU driver resets (see Troubleshooting)
- Desktop and Start-menu shortcuts that start a VM straight into its window
- Import/export of VMs; one-click (or automatic) update of the guest components
- Host to client hot-key support
- Provision and boot with / without internet
- Supports running Claude Cowork and Docker inside the VM thanks to Nested Virtualization Support
- Headless mode: a scriptable local HTTP/JSON API + dependency-free Python SDK (`asb.py`) for creating and driving GPU-accelerated VMs programmatically

Mac features:
- macOS or Windows 11 ARM VM support
- Zero touch install for Windows 11 and macOS
- Copy and Paste
- 2 Channel Audio
- macOS: GPU Acceleration via Paravirtualization with support for Metal, Windows: No GPU Acceleration
- SSH via virtio-vsock (no network required)
- Fixed 1080P60 display on Windows 11 guests, Dynamic display sizing on macOS guests
- Provision and boot with / without internet
- Headless mode: the same scriptable local HTTP/JSON API + Python SDK (`asb.py`) as on Windows, for driving macOS and Windows 11 VMs programmatically

Requirements:
Windows 11 with an x64 Processor or macOS 27 or newer (M Series)

## About App Sandbox

App Sandbox creates and runs full desktop virtual machines — Windows 11, Ubuntu, and
macOS. It is free and open-source (MIT), distributed as prebuilt binaries (EV-signed on
Windows, Apple-Developer-signed on macOS) on its
[Releases](https://github.com/jamesstringer90/appsandbox/releases) page. It is driven two
ways: from a graphical UI, or programmatically through the local API with a
dependency-free Python SDK (`asb.py`). Either way you pick an OS, point it at an installer
image, and App Sandbox provisions the disk, runs an unattended install, and boots the
guest. You supply a Windows or Ubuntu ISO; macOS guests download their restore image
automatically. It runs on a Windows 11 (x64) PC or an Apple Silicon Mac, including laptops.

**Runs on Windows 11 Home, without Hyper-V.** On Windows, App Sandbox does not use Hyper-V
or Hyper-V Manager; it creates and runs VMs through the Windows Host Compute System (HCS)
and Host Compute Network (HCN) APIs, which require only the *Virtual Machine Platform*
feature. Hyper-V is limited to Windows 11 Pro and Enterprise, while Virtual Machine
Platform is available on Windows 11 Home, so a Windows 11 Pro license is not required.
Virtual Machine Platform is the same Windows feature WSL2 uses.

**What you can use it for:**
- A full Windows or Ubuntu **desktop** VM — a complete graphical OS with GPU acceleration
  (OpenGL, Vulkan, CUDA) — for building and testing your own software on a clean install.
- Running a program in a VM, kept separate from your main OS and files.
- Developing and testing Windows drivers: a built-in **Test Mode** enables test-signing
  inside the guest, so the host's Secure Boot and boot configuration are left unchanged.
- Running a computer-use AI agent (such as Claude's computer use model) in its own VM. App
  Sandbox provides the desktop and the VM; the agent's own model loop runs inside the guest.
  Several agents can run at once, each in its own VM, on a single laptop or desktop.
- Creating GPU-accelerated (**GPU-PV**) VMs, from the GUI or the headless API.
- Scripting the VM lifecycle from code — create, snapshot, branch, SSH into, and delete
  GPU-accelerated VMs — with the Python SDK.

**Headless API.** The GUI automatically exposes the local HTTP/JSON API on `127.0.0.1`.
`appsandbox.exe --headless` (or
`sudo /Applications/AppSandbox.app/Contents/MacOS/AppSandbox --headless` on macOS)
runs the same core without the management UI or automatic viewers. `-headless` is
also supported; only one core process can run at a time. The stdlib-only Python SDK
(`asb.py`) wraps it: create GPU-accelerated VMs (GPU-PV is a `gpuMode` create option), SSH
in over an auto-deployed key (Windows, Ubuntu, or macOS guests), snapshot and branch them,
open the live display, and run several at once — how many run concurrently is bounded by host
CPU, RAM, and GPU memory, not by a fixed limit. The daemon is single-host — it drives VMs on
the machine it runs on and does not provision cloud VMs; for CI, run it on a self-hosted
runner. Snapshots and branches are Windows-host-only. Templates support Windows guests
on both hosts and macOS guests on Mac hosts, avoiding a fresh OS installation. A VM persists
until deleted. A snapshot is a disk-state checkpoint taken with the VM stopped, and a branch
forks a writable disk from it to run divergent actions. The full API reference and runnable
examples are in [`tools/headless-api/`](tools/headless-api/).

**How it works:** this repo also aims to be a working example of creating full desktop VMs
programmatically with the Windows HCS/HCN APIs and Apple's Virtualization.framework. On
Windows, App Sandbox submits a hand-built HCS machine document to `computecore.dll` /
`computenetwork.dll` — the HCS/HCN layer that also underlies WSL2 and Windows Sandbox —
rather than going through Hyper-V Manager. GPU acceleration uses GPU paravirtualization
(GPU-PV) to share the host's installed GPU with the guest — the same WSL2 path, not a
dedicated passthrough (no VFIO/IOMMU, no second GPU). Windows guests get DirectX 12, OpenGL,
Vulkan, CUDA, and OpenCL; Ubuntu guests the same minus DirectX; macOS guests Metal. A custom
**IddCx** indirect display driver and a virtual audio device carry the screen and sound, and
guest↔host clipboard, audio, input, and SSH run over **Hyper-V sockets**. Guest disks are
built by an in-repo tool with support for ext4, squashfs, qcow2, and VHDX. On macOS, App
Sandbox uses Apple's **Virtualization.framework** (`VZVirtualMachine`, `VZMacOSInstaller`)
over **virtio-vsock**. On Windows, Linux (Ubuntu) guests reach the GPU through a custom
DRM/KMS kernel module (`asb_drm`), Microsoft's WSL2 `dxgkrnl`, and a custom Mesa build. The
apps are native C / Objective-C with an HTML/JS UI (WebView2 on Windows, WKWebView on macOS).

Windows guests on macOS use **QEMU 11.1.3** with HVF acceleration, built from our
public [asb-qemu fork](https://github.com/jamesstringer90/asb-qemu/tree/asb-ivshmem-11.1.3).
It preserves our four ivshmem changes, including support on macOS and the optional
`x-device-id` override. QEMU, its libraries, and firmware ship inside the macOS app.
See the [QEMU build notes](vendor/qemu-ivshmem/README.md) for the pinned source and
rebuild instructions.

# Tips:
Windows VMs: you choose the username and password when you create the VM (the username defaults to `user`; there is no default password).

[Windows] Display options: the VM's **Display** tab, or right-click the VM window's title bar → Display (resolution, refresh rate, Quality/Performance mode, codec). In Performance mode the title shows `GPU H.264` or `GPU HEVC`; HEVC needs "HEVC Video Extensions" from the Microsoft Store on the host.

[Windows] Game controllers: install [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases) once inside the Windows VM; controllers are sent while the VM window has focus.

[Windows] Logs: the Activity panel / `%ProgramData%\AppSandbox\appsandbox.log` on the host (Settings → Folders and logs opens it); inside a Windows guest `C:\Windows\AppSandbox\agent.log` and `C:\ProgramData\AppSandboxVDD.log` (display driver, encoder).

[Windows] Enable hotkeys or mute the VM audio: connect to the VM and right-click the connection title bar  

[Windows] Need a high performance remote desktop to remotely access your VM? [Phaze](https://phaze.app) works well  

[Windows] You can check if the GPU-PV driver setup is working by running gpu-test.exe inside your App Sandbox Windows VM, gpu-test.exe will show a box with 6 rotating cubes, each using a different rendering engine (D3D9, D3D10, D3D11, D3D12, OpenGL and Vulkan).  If one or more fail, they will not correctly show a rotating cube for that rendering API.  [gpu-test.zip](https://github.com/jamesstringer90/appsandbox/releases/download/v0.1.2/gpu-test.zip). Note: The rendering API succeeding means that the GPU-PV worked, but sometimes games or apps are coded in such a way that they will not detect the GPU-PV system correctly and still show an error.

## Troubleshooting

**The Windows VM crashes and restarts when Edge, Discord, Steam or some games start (GPU-PV).**
This is a bug in Windows itself (cumulative updates from September/October 2026, builds 26100/26200/26300 with UBR 9550 or later). An app that looks for a hardware video encoder (Chromium/Edge/Discord/Steam, Game DVR, Task Manager, dxdiag) makes the guest's `dxgkrnl` crash with bugcheck **0x3B** (SYSTEM_SERVICE_EXCEPTION).
- Fix: install **KB5127936** or the **November 10 2026** (or later) cumulative update **inside the VM** (and on the host). Windows 11 24H2 (26100) Home/Pro no longer gets the fix: upgrade the VM to 25H2.
- Until then the App Sandbox guest agent applies Microsoft's feature-flag workaround on affected builds automatically; it takes effect after the VM's **next restart**, and the Activity log says when it was applied. It removes itself once the VM has the fixed update. To turn it off: `HKLM\SOFTWARE\AppSandbox\Agent` → `GpuStabilityTweaks` (DWORD) = `0` in the VM.
- How to confirm: host Event Viewer → Hyper-V-Worker-Admin, event **18590** with error code `0x3B`; in the VM, System log event **1001** (BugCheck) with `0x0000003b (0x00000000c0000005, …)`. After a crash the agent also writes the bugcheck code to the Activity log.
- NVIDIA hosts with driver **621.01** and an RTX 50 card: update to **621.14** or later (621.01 can freeze video playback and reset the GPU).

**The VM display freezes or the GPU resets under heavy load.** The display driver now runs the video encoder at HIGH GPU priority (Sunshine's recommendation for NVIDIA). `HKLM\SOFTWARE\AppSandbox\VDD` → `RealtimeGpuPriority` = `1` in the VM restores the old REALTIME priority.

## Building from source

Windows: Visual Studio 2022 with the Windows SDK and the **WDK** (driver projects). From a
*Developer Command Prompt* (cmd, not PowerShell 7):

```
msbuild AppSandbox.sln /p:Configuration=Release /p:Platform=x64
```

The app, `resources\` (guest helpers) and `drivers\` (test-signed guest drivers) land in
`bin\Release\`. Run `AppSandbox.exe` as administrator. Version numbers come from
`Directory.Build.props`. The GitHub workflow in `.github/workflows/windows-test-build.yml`
builds the same way and publishes test pre-releases from `release/**` branches.

macOS: open `AppSandbox.xcodeproj` (see `tools/sign/SIGNING_MAC.md` for signing).

Changes to guest-side code (helpers in `tools/agent`, the display driver in `tools/vdd`)
reach existing Windows VMs through **Update guest components** (VM stopped) or the automatic
update before start; new VMs get them at creation.

## Repository layout and contributor/AI guide

[`AGENTS.md`](AGENTS.md) maps the repository, the host↔guest protocols, the
compatibility rules (old guests and hosts must keep working), how guest components are
delivered, and the roadmap (including an MCP server built on the headless API). Read it
before changing code, whether you are a person or an AI agent.

## Packaging

The release is a zip with `AppSandbox.exe`, `appsandbox_core.dll`, `web\`, `resources\` and
`drivers\`. An MSIX package is possible for the app itself, but App Sandbox needs
administrator rights (HCS, http.sys) and installs guest drivers inside VMs, so an MSIX build
must run as a full-trust desktop app that elevates; see `AGENTS.md`.

## Sharing fixes and features

Thank you for sharing fixes and ideas! Pull requests, including AI-assisted changes
from your own fork, are welcome as suggestions. They are unlikely to be formally
reviewed or merged, but contributions that help shape a change will be credited.
See the [contribution guidelines](CONTRIBUTING.md) for details.

## License

AppSandbox is licensed under the [MIT License](LICENSE) — Copyright (c) 2026 James Stringer.

It bundles third-party components that are **not** covered by the MIT license and
retain their own terms — most notably the Microsoft WSL2 `dxgkrnl` GPU driver
(GPL-2.0) and the embedded XZ decoder (0BSD). See
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for the full list. AppSandbox's
own Linux DRM kernel module (`tools/linux/asb_drm/`) is dual-licensed MIT OR
GPL-2.0 so it can resolve the kernel's GPL-only symbols.

## Acknowledgements

Beyond my own experience building [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV), I found [NanaBox](https://github.com/M2Team/NanaBox) to be a really helpful resource for understanding HCS.

## Author

[James Stringer](https://www.linkedin.com/in/jamesstringerphotography/) — author of [Easy-GPU-PV](https://github.com/jamesstringerparsec/Easy-GPU-PV).
