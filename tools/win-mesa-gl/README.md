# win-mesa-gl — Mesa standalone OpenGL (d3d12 gallium), prebuilt

Custom Mesa build that gives AppSandbox **Windows** GPU-PV guests hardware
OpenGL by shipping Mesa's own `opengl32.dll` (gallium **d3d12** driver + WGL
frontend) system-wide in the guest's System32.

Built from unmodified upstream Mesa. Mesa is MIT, so
only the binaries + license notice are vendored here; the source is **not**
copied into this repo. See `THIRD-PARTY-NOTICES.md`.

Mesa's `opengl32` drives OpenGL on top of the guest's projected D3D12 driver —
hardware GL 4.6, or WARP software GL 4.6 when there is no GPU.

Meson configuration (see `build/build-mesa.cmd`):

```
-Dgallium-drivers=d3d12          # d3d12 only; WARP is the software fallback
-Dvulkan-drivers=                # none (Vulkan stays on the dzn registry ICD)
-Dgallium-wgl-dll-name=gallium_wgl   # else the drop-in imports itself
-Dllvm=disabled
-Dvideo-codecs=all               # video/d3d12-encoder kept ON (matches MS)
-Dcpp_args=/wd4189 -Dc_args=/wd4189  # dodge the MSVC warning-as-error from the video encoder
-Db_vscrt=mt                     # static CRT -> no VCRUNTIME/MSVCP to ship
-Dopengl=true -Dshared-glapi=enabled
-Degl=disabled -Dgbm=disabled -Dglx=disabled -Dgles1=disabled -Dgles2=disabled
```

## Layout

```
tools/win-mesa-gl/
├── README.md
├── build/
│   ├── build-mesa.cmd          ← builds the trio for x64 or arm64
│   └── aarch64-cross.txt       ← meson cross file (x64 host -> arm64 target)
└── prebuilt/
    ├── x64/                    ← x64 trio + BUILDINFO
    │   ├── opengl32.dll  gallium_wgl.dll  z-1.dll
    │   └── BUILDINFO
    └── arm64/                  ← arm64 trio + BUILDINFO
        ├── opengl32.dll  gallium_wgl.dll  z-1.dll
        └── BUILDINFO
```

`opengl32.dll` imports `gallium_wgl.dll`, which imports `z-1.dll` — **the three
are a unit and must live in the same directory.**

## Install in a Windows VM

The agent deploys these system-wide (`gl_provision`); manually, the build's own
arch goes into `System32`:

| Guest | Trio → | Covers |
|---|---|---|
| x64 Windows | `C:\Windows\System32` (x64 trio) | all x64 apps |
| ARM64 Windows | `C:\Windows\System32` (arm64 trio) | native ARM64 apps |

(`System32` is the native system dir on both; x64 apps emulated on ARM64 load
`opengl32` from `System32` via ARM64X, so the pure-ARM64 trio does not serve
them — that needs an ARM64X build.)

`opengl32.dll` is TrustedInstaller-owned: take ownership, rename the in-use DLL
aside, then copy the trio in (back up the MS copy as `opengl32.dll.msbak`). Keep
`dxil.dll` in System32 (Mesa's d3d12 driver signs DXIL with it). The agent
re-applies this every boot, so it self-heals if Windows servicing/SFC restores
Microsoft's `opengl32`.

## Versioning

Built from a **stable Mesa release branch**, with the
resulting version recorded in each `BUILDINFO`. Current branch: **`26.1`** — the
latest upstream stable and the closest to the `26.2.0-devel` Mesa that
Microsoft's `OpenGLOn12` ships. Bump `MESA_BRANCH` in `build-mesa.cmd`
deliberately and re-verify (below) before re-vendoring.

## Rebuilding

Build **once per arch, in a fresh shell** (the x64 and ARM64 `vcvarsall`
environments must not be mixed):

```bat
build\build-mesa.cmd x64
build\build-mesa.cmd arm64
```

Prereqs: VS 2022 with the **x64 + ARM64** C++ toolsets, Python + meson + mako,
Ninja, win_flex_bison, and internet (meson fetches the DirectX-Headers + zlib
wraps). The script clones Mesa to `%USERPROFILE%\source\repos\mesa` if `MESA_SRC`
isn't set, checks out `MESA_BRANCH`, configures, builds, and drops the trio +
`BUILDINFO` into `prebuilt\<arch>\`.

## Signing

These DLLs are prebuilt (not built at release time), so they are EV
Authenticode-signed **once, here**, after a (re)build — not by
`make-release.ps1`. With the EV token inserted:

```powershell
build\sign-mesa.ps1
```

It signs all six DLLs (both arches) in one `signtool` call (one PIN),
SHA-256 + RFC-3161 timestamped, auto-detecting the same EV cert that
`make-release.ps1` uses. It is **standalone** — pure Authenticode, with no
driver-attestation config or Partner Center dependency. Re-run after every
rebuild, before committing the binaries. (Override with `-Thumbprint` /
`-TimestampUrl` if needed.)

## Verify before vendoring

```
dumpbin /dependents prebuilt\x64\opengl32.dll
  # expect gallium_wgl.dll; NOT opengl32.dll (forgot -Dgallium-wgl-dll-name),
  # NOT VCRUNTIME140/MSVCP140 (forgot -Db_vscrt=mt)
```

Functional check: run any OpenGL app in the guest — PASS =
`GL_RENDERER: D3D12 (<gpu>)`, `GL_VERSION: 4.6`; FAIL = `GDI Generic` / `1.1`.
With no GPU it should fall back to `D3D12 (Microsoft Basic Render Driver)` (WARP),
still GL 4.6.
