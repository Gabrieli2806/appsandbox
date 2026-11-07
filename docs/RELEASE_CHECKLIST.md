# Release checklist: stable zip and Microsoft Store

What has to be done before the first stable release of this fork, as a zip
and as a Microsoft Store (MSIX) package. Items marked **(owner)** need a
decision, an account or a certificate that only the owner has; **(code)**
items need a change in the repository.

---

## 1. Decisions to make first

1. **(owner) Product name and publisher.** "App Sandbox" and "App Sandbox LLC"
   belong to the upstream author (James Stringer). MIT allows the fork, not
   the name. Either get written permission or choose a new name, then:
   - reserve it in Partner Center (section 4);
   - pass it to `packaging/msix/make-msix.ps1` with `-DisplayName` and
     `-PublisherDisplayName` (the manifest no longer hard-codes it);
   - **(code)** change `ProductName` in `build/version.rc`, the window and
     tray titles (`src/app_win/ui.c`, `main.c`), `web/index.html`,
     `web/app.js` and README;
   - add an About line: "Based on App Sandbox by James Stringer (MIT)", and
     your own copyright line in `LICENSE` under James Stringer's (use the
     legal name you publish under).
2. **(owner) Signing identity.** Pick one: Azure Trusted Signing (cheapest,
   no hardware token, identity-validated) or an OV/EV certificate. Driver
   attestation (section 2.2) needs an **EV** certificate to register the
   Partner Center hardware account.
3. **(owner) Prebuilt binaries signed by upstream.** The committed
   `tools/win-mesa-gl/prebuilt/*/*.dll` carry the "App Sandbox LLC" EV
   signature. Redistributing them unmodified is allowed, but if the product
   is renamed, rebuild them with `build-mesa.cmd` and sign them with your
   certificate (the README's `sign-mesa.ps1` step).
4. **(owner) Version.** The Store needs a major version of at least 1
   (`AsbVersionMajor` in `Directory.Build.props`).

---

## 2. Before any stable release (zip and Store)

### 2.1 Code signing (owner + code)
- Sign every PE file the project builds: `AppSandbox.exe`,
  `appsandbox_core.dll`, `iso-patch.exe`, everything under `resources\`
  (guest helpers, NVIDIA shims) and the drivers' binaries before attestation.
  `WebView2Loader.dll` and `devcon.exe` are already Microsoft-signed.
- **(code)** `tools/sign/make-release.ps1` only signs when the upstream
  "App Sandbox LLC" YubiKey is present. Change it (or add a CI release job)
  to use your certificate, for example `signtool sign /dlib
  Azure.CodeSigning.Dlib.dll /dmdf metadata.json` for Trusted Signing, with
  an RFC 3161 timestamp (`/tr ... /td SHA256`).

### 2.2 Driver attestation: no more test signing in guests (owner + code)
- Register a Partner Center **hardware** account (needs the EV certificate),
  fill `tools/sign/partner-center.local.json` and your own
  `submission-config.json`, and run `tools/sign/sign-drivers.ps1` for
  **AppSandboxVDD**, **AppSandboxVAD** and **AppSandboxSHM** (x64, and ARM64
  if you ship it).
- Build the release stage from the attested drivers **without** `*.cer`
  (`make-release.ps1` already excludes them). Guest component updates work
  without a `.cer` (`guest_tools.c` treats it as optional).
- **(code)** `tools/provision/win_provision.c` runs `bcdedit /set
  testsigning on` whenever the VDD INF is present, and imports any `.cer`
  into the guest's Trusted Root. Limit testsigning to "Test Mode" VMs and to
  stages that ship a `.cer`. Release notes must not tell users that test
  signing is enabled.
- Check: a new Windows VM from the signed stage, with Secure Boot on, has the
  display driver, audio and SHM driver working and `bcdedit` shows
  `testsigning No`.

### 2.3 Linux guest sources (code)
- Ubuntu VMs download and compile the guest sources from
  `AsbSourceRepo@AsbSourceRef` (default: upstream `jamesstringer90/appsandbox`
  at `main`). Only `windows-test-build.yml` sets them. Every release build
  must pass `/p:AsbSourceRepo=<your repo> /p:AsbSourceRef=<release tag or
  commit>`; make `make-release.ps1` fail when they are empty. Keep the tag
  forever (a deleted tag or a private repository breaks Ubuntu VM creation
  for that release). Better: pack the Linux sources into `resources\` at
  build time.

### 2.4 Ubuntu packages over HTTP with `[trusted=yes]` (code, must fix)
- `tools/iso-patch/prefetch_build_deps.c` downloads `Packages.xz` and the
  `.deb` files from `http://archive.ubuntu.com` and checks the `.deb` hashes
  only against that unauthenticated index; `ubuntu_vhdx.c` then installs them
  with `deb [trusted=yes]`. Anyone on the network path while a VM is created
  can run code as root in the new guest.
- Fix: download `dists/<codename>{,-updates,-security}/InRelease`, verify it
  against the Ubuntu archive key (embedded, or `gpgv --keyring
  /usr/share/keyrings/ubuntu-archive-keyring.gpg` in the guest), check
  `Packages.xz` against the SHA256 listed in `InRelease`, and drop
  `[trusted=yes]` (ship the verified `InRelease` in the local repository).
- ARM64: `archive.ubuntu.com` has no arm64 packages (`ports.ubuntu.com`
  does). Fix the mirror before shipping an ARM64 build.

### 2.5 D3DMappingLayers from Windows Update (owner + code)
- `src/backend_win/d3dlayers.c` downloads Microsoft's "OpenCL, OpenGL and
  Vulkan Compatibility Pack" (Store product 9NQPSL29BFFF) through the
  undocumented Windows Update FE3 service, presenting a generic Windows
  Insider VM (`fe3_templates.h`), and copies its DLLs into guests. It is not
  a documented redistribution channel; Microsoft can change it at any time,
  and a Store reviewer may ask about it.
- A download is accepted when the `.appx` matches the digest FE3 returns
  over HTTPS (SHA-256 `AdditionalDigest`, else the SHA-1 `Digest`) **or**
  every DLL has a valid Microsoft Authenticode signature. `dzn_icd.json` is
  always written from a fixed template (only `api_version` is copied). The
  SHA-256 of each accepted file goes to `d3dlayers.verified` beside the
  `d3dlayers` folder; later VM starts compare against it and need no network.
  A set that fails both checks is discarded and `d3dlayers.retry` blocks new
  downloads for 24 hours. Neither check has been confirmed against a real
  download yet. **Before release, on a real host:**
  - Delete `d3dlayers`, `d3dlayers.verified` and `d3dlayers.retry` beside
    `AppSandbox.exe` (zip) or in `%ProgramData%\AppSandbox` (MSIX), start a
    GPU VM and check the log: `package matches the SHA-256 digest` (or
    `SHA-1`) and `staged 6 mapping-layer files`. If it says the package does
    not match or has no digest, the FE3 parsing in `parse_leaf` needs a look.
  - Run `Get-AuthenticodeSignature` on `d3dlayers\*.dll`. If any DLL is not
    Valid with a Microsoft signer, the digest is the only check that works:
    note it here.
  - In the guest, `C:\Windows\AppSandbox\d3dlayers\dzn_icd.json` has
    `library_path` `C:\\Windows\\AppSandbox\\d3dlayers\\vulkan_dzn.dll` and
    `vulkaninfo --summary` lists the Dozen device.
- Longer term, prefer redistributable sources: Mesa d3d12/dzn (already built
  in `tools/win-mesa-gl`), microsoft/OpenCLOn12 (MIT) and `dxil.dll` from
  DirectXShaderCompiler releases; or let the guest install the Compatibility
  Pack from the Store itself. Likewise the Linux `libd3d12`/`libdxcore`
  NuGet packages from `pkgs.dev.azure.com/shine-oss`
  (`prefetch_wsl_deps.c`): prefer the host's `System32\lxss\lib`.
- Disclose both downloads in the privacy policy (section 5).

### 2.6 Licences (done in code, keep it that way)
- `LICENSE` and `THIRD-PARTY-NOTICES.md` are in the CI zip (checked by the
  `$required` list) and always in the MSIX (`make-msix.ps1` adds them).
  **(code)** Add both to `tools/sign/make-release.ps1`'s stage and its
  `$required` list too.
- When you add a third-party file, add it to `THIRD-PARTY-NOTICES.md`.
- **(code)** An About panel in the UI with version, a link to the notices
  and to the privacy policy.

### 2.7 Test pass on a clean Windows 11 x64 PC
- Zip and MSIX: install, start (UAC), create a Windows VM with SSH and GPU
  and an Ubuntu VM; the host log shows `OpenSSH MSI downloaded and verified`.
- Update guest components on a VM made with the previous release.
- Uninstall: note what stays behind (section 4.6).

---

## 3. Zip release

1. Build Release x64 with `AsbSourceRepo`/`AsbSourceRef` set (2.3).
2. Sign binaries, attest drivers, stage without `*.cer` (2.1, 2.2).
   Until `make-release.ps1` copies them (2.6), copy `LICENSE` and
   `THIRD-PARTY-NOTICES.md` to the root of the stage by hand.
3. Zip; publish as a GitHub release (not pre-release) with SHA-256 of the zip
   in the notes.
4. Remove the "test signing" and "SmartScreen" text from the release notes.

---

## 4. Microsoft Store submission

### 4.1 Partner Center (owner)
- Developer account (individual or company), then **Apps and games → New
  product → MSIX or PWA app**, reserve the name from section 1.
- **Product identity** page: copy `Package/Identity/Name`,
  `Package/Identity/Publisher` and `Package/Properties/PublisherDisplayName`.

### 4.2 Build the package
```powershell
.\packaging\msix\make-msix.ps1 -Store -Stage <signed release stage> `
    -Version <Major.Minor.Patch.Build> -OutDir out `
    -Name '<Identity Name>' -Publisher '<CN=...>' `
    -DisplayName '<reserved name>' -PublisherDisplayName '<publisher display name>'
```
- `-Store` turns `Major.Minor.Patch.Build` into `Major.Minor.Build.0` (the
  Store owns the fourth part; the build counter keeps each upload newer). It
  refuses the test identity, a missing display name, `Major = 0` and a stage
  with `drivers\*.cer`. The package is left unsigned; Partner Center signs it.
- The build counter must only grow. In CI it is `github.run_number` of
  `windows-test-build.yml`; renaming that workflow restarts the counter.
- Test-install it first: run the same command without `-Store`; the
  package is then signed with a test certificate whose subject is the
  `-Publisher` you pass, and `Install-AppSandbox.ps1` installs it.
- Run the **Windows App Certification Kit** on the `.msix`.
- `tools/headless-api` (Python SDK) is not in the MSIX; mention it in the
  description with a link to the GitHub release.

### 4.3 ARM64 (owner + code)
- The Store offers an x64-only package to Windows on ARM PCs, where it would
  build x64 guests that cannot run. Either upload an arm64 package too
  (`make-msix.ps1 -Arch arm64` from an ARM64 stage; CI does not build one
  yet), or **(code)** add a start-up check in `src/app_win/main.c`:
  `IsWow64Process2(GetCurrentProcess(), &proc, &native)` and, when an x64
  build sees `native == IMAGE_FILE_MACHINE_ARM64`, show "This is the x64
  version. Install the ARM64 version of <name>." and exit.

### 4.4 Restricted capabilities: justification text
> The app is a virtual machine manager for Windows 11. It is a full-trust
> Win32 desktop application (`runFullTrust`): its UI runs in WebView2 and it
> calls Win32 APIs that are not available to packaged apps in an app
> container: the Host Compute Service and Host Compute Network APIs (create
> and run VMs, virtual switches and NAT), the Virtual Disk API (create and
> mount VHDX files), Hyper-V sockets to talk to its guest agents, and HTTP
> Server API (http.sys) for its local automation API on 127.0.0.1.
> `allowElevation` is needed because the Host Compute Service, attaching
> virtual disks, registering the local http.sys URL and creating the VM
> management certificate in the machine store all require administrator
> rights. The app does not install drivers or services on the host. Besides
> its own VM files, it creates its own virtual NAT network (Host Compute
> Network) and, at the user's request, enables the Windows optional feature
> "Virtual Machine Platform" (DISM), which needs a restart.

### 4.5 Certification notes for reviewers
> - Requires Windows 11 (build 22000 or later) x64 with hardware
>   virtualization enabled in the firmware. If the test machine is itself a
>   VM, nested virtualization must be on.
> - The app runs elevated, so every start shows a UAC prompt.
> - On first start the app checks for the Windows feature "Virtual Machine
>   Platform". If it is off, the app offers to enable it; a restart is
>   needed.
> - To create a VM you need an installation ISO: a Windows 11 ISO from
>   microsoft.com/software-download/windows11 or an Ubuntu Desktop ISO from
>   ubuntu.com. Creating a VM downloads components from GitHub, Microsoft and
>   archive.ubuntu.com (see the privacy policy). A GPU is optional.
> - Without an ISO you can still review the main window, the Create VM
>   dialog and Settings.
> - The guest drivers in the package are installed only inside the virtual
>   machines, never on the host.

### 4.6 Store listing (owner)
- Privacy policy URL (section 5).
- System requirements: Windows 11 x64, virtualization, 16 GB RAM
  recommended, optional GPU.
- Say in the description that uninstalling leaves VM disks and
  `%ProgramData%\AppSandbox` (VM list, settings, the local API token, the SSH
  key for guests, download caches) and the VM management certificate in the
  machine store; tell users how to remove them. **(code)** Better: a
  "Remove App Sandbox data" action in Settings.
- Updates come only from the Store (the app has no self-updater).

---

## 5. Privacy policy: what the app sends and stores

The app has no telemetry and no account. It contacts these hosts:

| Host | When | What is sent / received |
|---|---|---|
| `github.com` (redirects to `objects.githubusercontent.com` / `release-assets.githubusercontent.com`) | Creating a Windows VM with SSH, if the OpenSSH installer is not cached | Plain download request (IP address, user agent); receives the Win32-OpenSSH MSI (SHA-256 pinned) |
| `displaycatalog.mp.microsoft.com` | Starting or creating a Windows VM with GPU, if the D3D mapping layers are not cached | Product lookup for 9NQPSL29BFFF (market US, en-us) |
| `fe3.delivery.mp.microsoft.com` | Same | Windows Update protocol requests with a fixed, generic device description (no data about the user's PC) and random message IDs; receives a download link |
| `*.delivery.mp.microsoft.com` (Microsoft CDN, often plain HTTP) | Same | Downloads the Compatibility Pack package |
| `codeload.github.com` | Creating an Ubuntu VM | Downloads this project's Linux guest sources |
| `archive.ubuntu.com` (plain HTTP) | Creating an Ubuntu VM | Downloads package indexes and `.deb` packages |
| `pkgs.dev.azure.com` | Creating an Ubuntu VM | Downloads Microsoft's Direct3D/DXCore NuGet packages |
| `127.0.0.1` (first free port from 8787 to 8806) | While the headless service (`AppSandbox.exe --headless`) runs | Local automation API (bearer token in `host.json`); not reachable from other computers |

Also state that:
- The UI is local files shown in Microsoft Edge WebView2; WebView2 itself
  follows Microsoft's privacy statement.
- Network traffic of the virtual machines (Windows Update, apt, browsing in
  the guest) is the guest operating system's, not the app's.
- Stored locally only: `%ProgramData%\AppSandbox\vms.cfg` (VM list and
  settings), `host.json` (local API token), `ssh\id_appsandbox` (private key
  for SSH into guests), download caches (beside `AppSandbox.exe` for the zip,
  in `%ProgramData%\AppSandbox` for the MSIX), and in each VM's folder its disks,
  `display_settings.json` and the setup answer file, which contains the VM
  account password you entered (base64, not encrypted).
- The macOS host (not part of this release) downloads from the upstream
  repository (`src/backend_mac/iso_patch_mac.m`); change those URLs before
  a Mac release.

Find new endpoints before each release:
`git grep -nE "https?://" -- src tools/iso-patch tools/agent web`.
