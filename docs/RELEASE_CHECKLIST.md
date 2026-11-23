# Release checklist: stable MSIX release and Microsoft Store

What has to be done before the first stable release of this fork, as an MSIX
on GitHub and as a Microsoft Store package. (MSIX is the only form the app
ships in; there is no zip to unpack and run.) Items marked **(owner)** need a
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
   - **(code)** change `ProductName` in `build/version.rc`, the name in the
     app (`app/AppSandbox`: `Package.appxmanifest`, the title in
     `MainWindow.xaml`, `Views/TrayMenuWindow`, `Views/SettingsPage`, and
     `Product` in `AppSandbox.csproj`), the native dialogs and messages
     (`src/app_win/ui.c`, `host.c`) and README;
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

## 2. Before any stable release (GitHub and Store)

### 2.1 Code signing (owner + code)
- Sign every PE file the native solution builds: `appsandbox_host.dll`,
  `appsandbox_core.dll`, `iso-patch.exe`, everything under `resources\`
  (guest helpers, NVIDIA shims) and the drivers' binaries before attestation.
  `devcon.exe` is already Microsoft-signed.
- The app's own files (`AppSandbox.exe`, `AppSandbox.dll`) and the .NET and
  Windows App SDK files beside them are made by the app build inside
  `make-msix.ps1`; they are covered by the signature of the MSIX
  (`-Thumbprint` or `-PfxPath`), or by the Store's for a Store package.
- **(code)** `tools/sign/make-release.ps1` only signs when the upstream
  "App Sandbox LLC" YubiKey is present. Change it (or add a CI release job)
  to use your certificate, for example `signtool sign /dlib
  Azure.CodeSigning.Dlib.dll /dmdf metadata.json` for Trusted Signing, with
  an RFC 3161 timestamp (`/tr ... /td SHA256`).

### 2.2 Driver attestation: no more test signing in guests (owner + code)
- Register a Partner Center **hardware** account (needs the EV certificate),
  fill `tools/sign/partner-center.local.json` and your own
  `submission-config.json`, and run `tools/sign/sign-drivers.ps1` for
  **AppSandboxVDD** and **AppSandboxVAD** (x64, and ARM64
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
  display driver and audio working and `bcdedit` shows
  `testsigning No`.

### 2.3, 2.4 (no longer apply)
- These covered the Ubuntu guest sources and the Ubuntu package downloads.
  Ubuntu guests were removed, and with them every download from GitHub source
  archives, `archive.ubuntu.com` and `pkgs.dev.azure.com`.

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
  - Delete `d3dlayers`, `d3dlayers.verified` and `d3dlayers.retry` in
    `%ProgramData%\AppSandbox`, start a
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
  Pack from the Store itself.
- Disclose both downloads in the privacy policy (section 5).

### 2.6 Licences (done in code, keep it that way)
- `LICENSE` and `THIRD-PARTY-NOTICES.md` are always in the MSIX (the app
  project `app/AppSandbox/AppSandbox.csproj` takes them from the repository
  root) and in the native stage of `tools/sign/make-release.ps1` (checked by
  its `$required` list).
- When you add a third-party file, add it to `THIRD-PARTY-NOTICES.md`.
- Settings → About shows the version, the license and the notices.
  **(code)** Add a link to the privacy policy there.

### 2.7 Test pass on a clean Windows 11 x64 PC
- Install the MSIX, start (UAC), create a Windows VM with SSH and GPU;
  the host log shows `OpenSSH MSI downloaded and verified`.
- Update guest components on a VM made with the previous release.
- Uninstall: note what stays behind (section 4.6).

---

## 3. GitHub release (MSIX)

1. Build the native solution, Release x64, with `/p:AsbSourceRepo=<owner>/<repo>`
   (the repository Settings → About links to).
2. Sign binaries, attest drivers, stage without `*.cer` (2.1, 2.2).
   `make-release.ps1` writes that native stage as
   `bin\Release\AppSandbox-<version>-win-x64.zip`; it is the input of the
   next step, not something to publish.
3. Unzip the stage and build the package from it, signed with your
   certificate (its subject must be the `-Publisher` you pass):
   ```powershell
   .\packaging\msix\make-msix.ps1 -Stage <unzipped stage> `
       -Version <Major.Minor.Patch.Build> -OutDir out `
       -Name '<package name>' -Publisher '<CN=...>' `
       -DisplayName '<name>' -PublisherDisplayName '<publisher>' `
       -Thumbprint <certificate thumbprint> -TimestampUrl <RFC 3161 URL>
   ```
   (`-PfxPath` / `-PfxPassword` instead of `-Thumbprint` for a .pfx file.)
   No `Install-AppSandbox.ps1` is written: that is only for packages signed
   with the test certificate.
4. Publish the `.msix` as a GitHub release (not pre-release) with its SHA-256
   in the notes.
5. Remove the "test signing" and "SmartScreen" text from the release notes.

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
  (not possible yet: the native projects build for ARM64, but the app
  project and `make-msix.ps1` are x64 only), or **(code)** add a start-up
  check in `src/app_win/host.c` (`asb_host_begin`):
  `IsWow64Process2(GetCurrentProcess(), &proc, &native)` and, when an x64
  build sees `native == IMAGE_FILE_MACHINE_ARM64`, show "This is the x64
  version. Install the ARM64 version of <name>." and exit.

### 4.4 Restricted capabilities: justification text
> The app is a virtual machine manager for Windows 11. It is a full-trust
> Win32 desktop application (`runFullTrust`): its UI is WinUI 3 (Windows App
> SDK, shipped inside the package) and it calls Win32 APIs that are not
> available to packaged apps in an app container: the Host Compute Service and Host Compute Network APIs (create
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
>   microsoft.com/software-download/windows11. Creating a VM downloads
>   components from GitHub and Microsoft (see the privacy policy). A GPU is
>   optional.
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
| `127.0.0.1` (first free port from 8787 to 8806) | While the headless service (`AppSandbox.exe --headless`) runs | Local automation API (bearer token in `host.json`); not reachable from other computers |

Also state that:
- The UI is a native WinUI 3 window (the Windows App SDK ships inside the
  package). It loads nothing from the network and shows no web content.
- Network traffic of the virtual machines (Windows Update, browsing in
  the guest) is the guest operating system's, not the app's.
- Stored locally only: `%ProgramData%\AppSandbox\vms.cfg` (VM list and
  settings), `host.json` (local API token), `ssh\id_appsandbox` (private key
  for SSH into guests), download caches (also in `%ProgramData%\AppSandbox`),
  `%LOCALAPPDATA%\AppSandbox\shell.json` (how the app's window looks), and
  in each VM's folder its disks,
  `display_settings.json` and the setup answer file, which contains the VM
  account password you entered (base64, not encrypted).

Find new endpoints before each release:
`git grep -nE "https?://" -- src tools/iso-patch tools/agent app`.
