# AppSandbox — Code Signing & Release Packaging

How to produce a fully-signed AppSandbox release: every native binary we build is
**EV Authenticode-signed**, the two drivers are **Microsoft attestation-signed**,
and the result is zipped as the **native stage** that `packaging\msix\make-msix.ps1`
turns into the MSIX (§4). The MSIX is the only thing that is distributed; the zip is
not a runnable release.

This is written so anyone with **(a)** an EV code-signing certificate on a hardware
token and **(b)** a Microsoft Partner Center *Hardware* account can reproduce it.

> **Golden rule:** signing + driver attestation + ZIP only happen when the EV token
> is plugged in. On any machine without it, the build still works and produces a
> normal **test-signed** dev build — nothing is signed or zipped.

---

## 1. What gets signed, and how

| Artifact | Signing | Tool |
| --- | --- | --- |
| `appsandbox_host.dll`, `appsandbox_core.dll`, `iso-patch.exe`, the 6 `appsandbox-*.exe` (the guest helpers under `resources\`) | **EV Authenticode** (your cert) | `signtool` |
| `AppSandboxVDD.{dll,inf,cat}`, `AppSandboxVAD.{sys,inf,cat}` | **EV-signed** (binary + catalog) **then Microsoft attestation** — you EV-sign the `.sys`/`.dll`, regenerate + EV-sign the `.cat`, EV-sign the CAB; Microsoft returns the package with the **catalog MS-signed** (the binary keeps your EV signature) | `signtool` + `inf2cat` + `makecab` + Hardware API |
| `devcon.exe` (`drivers\`) | already Microsoft-signed — **not** re-signed | — |
| The MSIX (`AppSandbox.exe`, the WinUI app, with the .NET runtime, the Windows App SDK and the signed stage inside) | **package signature** — your cert (`-Thumbprint` / `-PfxPath`), a test certificate, or left unsigned for the Store (`-Store`) | `make-msix.ps1` (`signtool`) |
| Mesa GL trio — `opengl32.dll`, `gallium_wgl.dll`, `z-1.dll` (both arches, vendored in `tools/win-mesa-gl/prebuilt`) | **EV Authenticode**, **pre-signed once** via `tools/win-mesa-gl/build/sign-mesa.ps1` (they are prebuilt, not built at release time; `make-release.ps1` skips files already signed by our cert, so it does not re-sign them) | `signtool` |

Two different signatures because kernel-mode / UMDF drivers will not load on retail
Windows from a self/EV signature alone — they need Microsoft's signature, obtained
via attestation through Partner Center.

---

## 2. Prerequisites (one-time)

1. **EV code-signing certificate on a hardware token** (e.g. a YubiKey). When the
   token is inserted, the cert appears in `Cert:\CurrentUser\My` with a private key.
   Signing prompts for the token PIN.
2. **Microsoft Partner Center — Hardware program** account, with your **EV cert
   associated** to it (Partner Center adds the cert under the hardware account).
3. **Azure AD (Entra) app** for the Hardware submission API (below).
4. Windows SDK + WDK installed (`signtool.exe`, `infverif.exe`, `makecab.exe`).
5. For **ARM64** releases: the **ARM64 MSVC + WDK build tools** (cross-building from an x64
   host is fully supported — no ARM device is needed to build, sign, or submit).

### 2a. Create the Entra app (for the submission API)

1. Azure portal → **Microsoft Entra ID → App registrations → New registration**
   (single tenant). Copy the **Application (client) ID** and **Directory (tenant) ID**.
2. **Certificates & secrets → New client secret** → copy the **Value** (shown once).
3. *Do not* add API permissions or admin consent — the Hardware API authorizes via
   Partner Center roles, not Entra permissions.

### 2b. Authorize the app in Partner Center

1. Partner Center → ⚙ **Account settings → User management → Microsoft Entra ID
   applications → Add** → your app → assign the **Manager** role.
2. ⚙ **Developer settings → Users → Microsoft Entra ID applications →** your app →
   **Roles → Hardware** → check **Driver Submitter** + **Shipping Label owner**
   (+ **Shipping Label promoter** if shown).

> Associating the app requires a **Global Administrator** of the Entra tenant.

### 2c. Create the credentials file (git-ignored)

The driver-signing scripts read the Entra app secrets from
`tools/sign/partner-center.local.json`. **This file is not in the repo — you must
create it** (driver attestation fails without it), holding the three values from §2a:
```json
{ "tenantId": "...", "clientId": "...", "clientSecret": "..." }
```
This file matches `tools/sign/*.local.*` in `.gitignore` — **never commit it**.
Validate it (run in your own terminal, paste nothing secret):
```powershell
tools\sign\test-hardware-api.ps1
# expect:  AUTH: PASS ...   PRODUCTS GET: PASS (HTTP 200) ...
```

---

## 3. Files in `tools/sign/`

| File | Committed? | Purpose |
| --- | --- | --- |
| `partner-center.local.json` | **no** (git-ignored) | Entra API creds (tenant/client/secret) |
| `submission-config.json` | yes | Product names, OS signature targets, driver→cab mapping, timestamp URL, optional `evCertThumbprint` (x64 values; `sign-drivers.ps1 -Platform ARM64` derives the ARM64 equivalents at sign time) |
| `test-hardware-api.ps1` | yes | Validate creds + Hardware roles |
| `sign-drivers.ps1` | yes | EV-sign drivers (binary + catalog + cab) → attestation submit (both in parallel) → download MS-signed |
| `make-release.ps1` | yes | Build → (if YubiKey) sign all native binaries + driver-attest + ZIP (the native stage); else skip |
| `AppSandboxPackage.vcxproj` | yes | Utility project; runs `make-release.ps1 -NoBuild -Platform $(Platform)` after a successful `Release|x64` **or** `Release|ARM64` VS build |
| `SIGNING.md` | yes | This document |

Versioning lives at the **repo root**: `Directory.Build.props` (the version numbers + the
non-VS `InfVerif` skip), `Directory.Build.targets` (auto-injects VERSIONINFO), `build\version.rc`
(the shared resource). See §8.

Secrets live only in `*.local.json` (git-ignored) and on the token (the PIN). The EV
cert's identity (org name, thumbprint) is **public** on any shipped signed binary —
that is inherent to EV signing — so the goal is "keep it out of source," not secret.

---

## 4. Running it

### From Visual Studio (the usual path)
**Build Solution** in **Release|x64** or **Release|ARM64**. The `AppSandboxPackage` project
depends on every other project, so Visual Studio builds it **last** and — via its
`AfterTargets="Build"` step — runs `make-release.ps1 -NoBuild -Platform $(Platform)`, **but
only if every project built successfully** (a failed build skips it, so no signing/packaging
on a broken build). With no EV YubiKey it no-ops (build only); with the token in it signs +
attestation-signs + zips (ARM64 outputs to `bin\Release-ARM64`, with its ARM64 attestation
profile and `-win-arm64.zip`). Debug builds never trigger it.

### Normal release (the one command)
```powershell
tools\sign\make-release.ps1                  # x64
tools\sign\make-release.ps1 -Platform ARM64  # Windows on ARM
```
- Builds `Release|x64` (or `Release|ARM64` with `-Platform ARM64`; output under `bin\Release-ARM64\`).
- **If the EV YubiKey is in:** EV-signs all binaries — app `.exe`/`.dll` **and** driver
  `.sys`/`.dll` — in one call (**PIN 1**); then `sign-drivers.ps1` regenerates + EV-signs the
  catalogs (**PIN 2**) and EV-signs the cabs (**PIN 3**), submits both drivers to Microsoft,
  waits, and downloads the MS-signed drivers; then writes `bin\Release\AppSandbox-<ver>-win-x64.zip`
  (no `.pdb`/intermediates, MS-signed drivers swapped in, test `.cer` dropped). **Three PINs
  total.** (If `bin\Release\drivers-signed\` already exists it's reused — no resubmit.)
  The zip is the native stage: `appsandbox_host.dll`, `appsandbox_core.dll`, `iso-patch.exe`,
  `resources\`, `drivers\`, `headless-api\`, `LICENSE` and `THIRD-PARTY-NOTICES.md`. It has
  no `AppSandbox.exe` and cannot be run as it is.
- **If the YubiKey is out:** stops after the build — no signing, no ZIP.

Switches: `-NoBuild`, `-SkipDrivers`, `-ForceDriverSign`, `-Version <v>`. The version comes
from `Directory.Build.props` unless `-Version` overrides it.

### From the signed stage to the MSIX
Unzip the stage and give it to `packaging\msix\make-msix.ps1` (needs the .NET 10 SDK). It
builds the WinUI app (`app\AppSandbox`) around those files, which makes the package, and
signs the package:
```powershell
# release: signed with a certificate already in Cert:\CurrentUser\My (e.g. the EV token)
packaging\msix\make-msix.ps1 -Stage <unzipped stage> -Version <Major.Minor.Patch.Build> -OutDir out `
    -Name '<package name>' -Publisher '<CN=...>' -DisplayName '<name>' -PublisherDisplayName '<publisher>' `
    -Thumbprint <thumbprint> -TimestampUrl <RFC 3161 URL>

# Microsoft Store
packaging\msix\make-msix.ps1 -Store -Stage <unzipped stage> -Version <Major.Minor.Patch.Build> -OutDir out `
    -Name '<Identity Name>' -Publisher '<CN=...>' -DisplayName '<reserved name>' -PublisherDisplayName '<publisher display name>'
```
- `-Version` always has four parts and the package gets exactly that; with `-Store` it
  becomes `Major.Minor.Build.0` (the Store reserves the fourth part) and `Major` must be at
  least 1.
- Signing: `-Thumbprint` (a code-signing certificate already in `Cert:\CurrentUser\My`, for
  example on a hardware token), or `-PfxPath` with `-PfxPassword` (imported for the signing
  and removed afterwards). With neither, a self-signed **test** certificate is made (subject =
  `-Publisher`) and its `.cer` and `Install-AppSandbox.ps1` are written next to the package —
  that is what CI test builds are. `-TimestampUrl` adds an RFC 3161 timestamp.
- `-Store` leaves the package unsigned (Partner Center signs it) unless `-Thumbprint` or
  `-PfxPath` is given, and refuses the test identity defaults (`-Name`, `-Publisher`), a
  missing `-DisplayName` / `-PublisherDisplayName`, and a stage with test-signed drivers
  (`drivers\*.cer`).
- **x64 only** for now: the app project is x64 (`-Arch` accepts only `x64`), although the
  native projects, and `make-release.ps1 -Platform ARM64`, also build ARM64.
- The files the app build adds (`AppSandbox.exe`, `AppSandbox.dll`, the .NET runtime, the
  Windows App SDK) are not EV-signed one by one; the package signature covers them.

See `docs\RELEASE_CHECKLIST.md` (§3, §4.2) for the release and Store steps around it.

### Driver attestation only (when a driver changes)
```powershell
tools\sign\sign-drivers.ps1
```
Per driver: EV-signs the `.sys`/`.dll` (skipped if `make-release.ps1` already signed them),
regenerates + EV-signs the catalog, builds the CAB, then EV-signs all CABs — up to **three
PINs** (binaries, catalogs, cabs). Submits **both drivers in parallel** to the Hardware API,
waits (no timeout; `-Resume` re-attaches to in-flight submissions), and extracts each MS-signed
package into `bin\Release\drivers-signed\<DriverName>\`. Re-run only when a driver binary
changes; `make-release.ps1` reuses the cached output otherwise.

---

## 5. The EV signing routine (reference)

```
signtool sign /sha1 <THUMBPRINT> /fd SHA256 /tr <timestamp-url> /td SHA256 /v <files...>
signtool verify /pa /v <file>
```
- `/sha1 <thumbprint>` selects the cert; the scripts **auto-detect** it (the one
  CA-issued code-signing cert with a private key in `CurrentUser\My`) or use
  `evCertThumbprint` from `submission-config.json`.
- Always SHA-256 (`/fd`,`/td`) and RFC-3161 timestamped (`/tr`) so signatures outlive
  cert expiry. Pass many files to one `sign` call → **one PIN** for the batch.

---

## 6. Driver attestation details

**The CAB is only the submission envelope — it is never shipped.** Everything inside it is
**EV-signed first**, so the package we submit already carries our identity: the `.sys`/`.dll`
is EV-signed, the catalog is regenerated with `inf2cat` (so it hashes the EV-signed binary)
and then EV-signed, and the CAB itself is EV-signed (that signature authenticates the
submission). Microsoft returns the same shape with the **catalog re-signed by Microsoft**; the
binary keeps our EV embedded signature. `make-release.ps1` swaps that package into the stage zip.

CAB rules (`sign-drivers.ps1` handles these): each driver in its **own subfolder**, no files
at the cab root, include the **`.pdb`** (Microsoft's crash analysis wants it).

API flow (`https://manage.devcenter.microsoft.com/v2.0/my/hardware`): token →
`POST /products` (`testHarness:"Attestation"`) → `POST …/submissions` (returns a SAS upload
URL) → PUT the signed CAB → `POST …/commit` → poll `workflowStatus.state` until `completed` →
download `downloads.items[type=="signedPackage"]`. Both drivers run **in parallel**.

**One product per version per architecture.** A product accepts exactly **one** submission (a
second returns `"Initial submission already exists"`), so each release is its own product — the
version **and architecture** are baked into the product name (e.g. `App Sandbox Virtual Display
Driver x64 0.1.0` / `… ARM64 0.1.0`) so the dashboard reads as a per-arch version history.
Products **cannot be deleted** via the API (only renamed, via
`PATCH /products/{id}`), so retire old ones by renaming, not removal.

**OS targets** are set in `submission-config.json` → `requestedSignatures`. Current codes
are listed in Microsoft's *Get product data* doc; Windows 11 x64: `WINDOWS_v100_X64_CO_FULL`
(21H2), `_NI_FULL` (22H2), `_GE_FULL` (24H2), `_25H2_FULL` (25H2). For an ARM64 build,
`sign-drivers.ps1 -Platform ARM64` rewrites these to the `ARM64` codes (and uses `inf2cat
/os:10_CO_ARM64`) at sign time, so the committed config stays x64.

**x64 and ARM64 are submitted separately.** `sign-drivers.ps1 -Platform <arch>` (driven by
`make-release.ps1 -Platform <arch>`) builds the cab from that arch's driver output,
regenerates the catalog with `inf2cat /os:10_CO_<ARCH>`, and creates its **own** product.
The **architecture is in the product/submission name** (`… x64 <ver>` / `… ARM64 <ver>`)
so the two are easy to tell apart in the Partner Center dashboard. (A single combined
multi-arch submission is technically possible but would need both arches built before
signing, which breaks the per-platform "build one config → auto sign+zip" automation —
hence separate per-arch submissions.)

**ARM64 ApiValidator** (the Universal-DDI check the WDK forces on ARM64 drivers) is **disabled
for ARM64 cross-builds from a non-ARM64 host** (`ApiValidator_Enable=false` in
`Directory.Build.props`, gated on `Platform==ARM64` + host≠ARM64). The WDK defaults it to the x86
ApiExtractor, which can't read an ARM64 PE and fails with exit 193; native ARM64 hosts and all x64
builds still run it, and Microsoft re-runs API validation on submission. Same rationale as the
InfVerif skip below.

**INF verification** (`infverif /h`, required by Partner Center since Apr 2025) runs at
**build time inside Visual Studio**. Command-line / CI builds skip the build-time task
(`SkipPackageVerification` in `Directory.Build.props` — the WDK task can't load `InfVerif.dll`
outside the VS environment), and Microsoft re-runs full INF validation on submission. To check
an INF by hand:
```
"C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64\infverif.exe" /h <built-inf>
```

---

## 7. Install implication (TODO when drivers are MS-signed)

The guest install scripts in `src\backend_win\disk_util.c` currently do
`bcdedit /set testsigning on` + `certutil -addstore Root/TrustedPublisher <cer>` +
`devcon install <inf>` — required for **test-signed** drivers. Once the shipped
drivers are **Microsoft-signed**, the `testsigning` and `certutil`/`.cer` steps are
no longer needed (the MS signature is trusted by retail Windows); only
`devcon install <inf>` remains. That change spans three near-identical script
generators (`iso_create_resources`, `iso_create_instance_resources`,
`generate_vhdx_setupcomplete`) plus the `.cer` staging in `stage_agent_and_setup`
and `generate_vhdx_manifest` — best gated on a "test-signed vs MS-signed" build flag.

---

## 8. Versioning (reference)

**One source of truth: `Directory.Build.props` at the repo root.** Edit the four numbers
(`AsbVersionMajor/Minor/Patch/Revision`) and the next build flows them everywhere:

- **App binaries** (every `.exe`/`.dll`) get a `VERSIONINFO` resource — FileVersion +
  ProductVersion + `ProductName` "App Sandbox" + per-binary `OriginalFilename` — from
  `build\version.rc`, which `Directory.Build.targets` auto-injects into each project (no
  per-project edits; it derives each binary's identity from MSBuild properties). It carries
  **no CompanyName / LegalCopyright** on purpose, so a third-party build makes no false claim —
  the publisher identity comes only from the EV signature on official builds.
- **Drivers** stamp `DriverVer` from `$(AsbVersion)` via the `<TimeStamp>` metadata on the
  `<Inf>` item in each driver `.vcxproj` (WDK StampInf). They opt out of the binary VERSIONINFO
  via `<AsbEmbedVersion>false</AsbEmbedVersion>`.
- **Stage zip name** (`make-release.ps1`) and **Partner Center product/submission names**
  (`sign-drivers.ps1`) read `Directory.Build.props` too.
- **The MSIX and the app** (`AppSandbox.exe`) get the version passed to
  `make-msix.ps1 -Version` (CI passes the same four parts).

Current value: **`0.2.0.0`**. The drivers are **Windows 11+ only** (the VDD references the
inbox `WudfRd.inf`, which is Win11+).

`Directory.Build.props` also sets `SkipPackageVerification=true` for **non-VS** builds (see §6).
