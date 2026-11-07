# Third-Party Notices

AppSandbox is licensed under the MIT License (see [LICENSE](LICENSE)). That MIT
grant covers **only AppSandbox's own source code**. The repository also bundles
the third-party components listed below, which are **not** licensed under the
MIT License and remain under their own licenses and copyrights. Each vendored
source file additionally carries its own `SPDX-License-Identifier` header, which
governs that file.

This file and [LICENSE](LICENSE) ship at the root of the Windows release zip
and of the MSIX package. Paths below are repository paths; where a component
ships in the Windows release, its location there is given too.

---

## tools/linux/dxgkrnl/ — Microsoft WSL2 `dxgkrnl` GPU driver

- **License:** GNU General Public License, version 2 (`SPDX: GPL-2.0`).
  A verbatim copy of the license text is provided at
  [`tools/linux/dxgkrnl/COPYING`](tools/linux/dxgkrnl/COPYING).
- **Copyright:** Copyright (c) Microsoft Corporation.
- A vendored copy of the WSL2 paravirtualized GPU kernel driver. Every file
  carries its own `SPDX-License-Identifier: GPL-2.0` header. This component is
  GPL-2.0 and is *not* relicensed under MIT; it is included as a separate work
  (mere aggregation in the same repository).

## tools/iso-patch/engine/xz/ — XZ Embedded decoder

- **License:** BSD Zero Clause License (`SPDX: 0BSD`).
- **Copyright:** Lasse Collin and the XZ Embedded contributors.
- An embedded XZ/LZMA decompressor. Every file carries its own
  `SPDX-License-Identifier: 0BSD` header. 0BSD is a public-domain-equivalent,
  MIT-compatible permissive license.

## tools/linux/wsl-mesa/prebuilt/ — Mesa 3D Graphics Library (prebuilt binary)

- **License:** MIT License (the Mesa project's overall license; some bundled
  components are under other compatible permissive licenses, e.g. the SGI Free
  Software License B). The headline MIT license text is shipped at
  [`tools/linux/wsl-mesa/prebuilt/MESA-LICENSE.txt`](tools/linux/wsl-mesa/prebuilt/MESA-LICENSE.txt);
  the full per-component listing is at <https://docs.mesa3d.org/license.html>.
- **Copyright:** Copyright (c) the Mesa contributors.
- `tools/linux/wsl-mesa/prebuilt/ubuntu-26.04-amd64/wsl-mesa.tar.zst` is an
  **unmodified upstream build** of Mesa 25.3.6 (gallium `llvmpipe`/`d3d12`,
  Vulkan `swrast`/`dzn`), produced by the in-repo `build/build-mesa.sh` with no
  source modifications. It contains Mesa's own binaries only; LLVM (used by
  `llvmpipe`) is an external `apt` dependency installed on the guest and is
  **not** bundled in this repository. The surrounding `tools/linux/wsl-mesa/`
  scripts and configs are AppSandbox's own code (MIT).

## tools/vdd/ThirdParty/wdf/ — Microsoft Windows Driver Framework (WDF)

- **License:** Microsoft Software License Terms for the Windows Driver Kit
  (redistributable SDK headers). See
  <https://learn.microsoft.com/en-us/legal/windows-sdk/>.
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/webview2/ — Microsoft Edge WebView2 SDK

- **License:** Microsoft Software License Terms — Microsoft Edge WebView2 SDK
  (redistributable). See
  <https://learn.microsoft.com/en-us/microsoft-edge/webview2/>.
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/devcon/devcon.exe — Microsoft DevCon utility

- **License:** Microsoft Software License Terms for the Windows Driver Kit
  (redistributable tool).
- **Copyright:** Copyright (c) Microsoft Corporation.

## vendor/qemu-ivshmem/ — QEMU + its runtime dependencies (bundled macOS VMM)

- **License:** QEMU — GNU General Public License, version 2 (`SPDX: GPL-2.0`);
  GLib and gettext/libintl — GNU Lesser General Public License, version 2.1
  (`SPDX: LGPL-2.1`); PCRE2 — BSD ("PCRE2 Licence"); EDK2/TianoCore guest UEFI
  firmware — BSD-2-Clause-Patent (`SPDX: BSD-2-Clause-Patent`).
- **Copyright:** the QEMU, GLib, GNU gettext, PCRE2 (University of Cambridge),
  and TianoCore/EDK2 contributors.
- The macOS host runs Windows guests on a vendored, patched build of **QEMU
  11.0.1** (ivshmem-plain on HVF) together with its GLib / gettext / PCRE2
  runtime and the EDK2 UEFI firmware, embedded under
  `AppSandbox.app/Contents/Resources/qemu/`. QEMU runs as a separate process
  (aggregated with, not linked into, the app); the LGPL dylibs are dynamically
  linked (LGPL §6 relink clause satisfied). Per-component notices, the GPL/LGPL
  written offer, and corresponding-source pointers are in
  [`vendor/qemu-ivshmem/LICENSES/NOTICE.md`](vendor/qemu-ivshmem/LICENSES/NOTICE.md)
  and [`vendor/qemu-ivshmem/WRITTEN_OFFER.txt`](vendor/qemu-ivshmem/WRITTEN_OFFER.txt)
  (both also shipped in the app bundle); `vendor/qemu-ivshmem/fetch-sources.sh`
  retrieves the corresponding source tarballs.

## vendor/virtio-win/ — NetKVM (Red Hat VirtIO Ethernet Adapter)

- **License:** BSD 3-Clause (`SPDX: BSD-3-Clause`). License text:
  [`vendor/virtio-win/virtio-win_license.txt`](vendor/virtio-win/virtio-win_license.txt).
- **Copyright:** Copyright (c) Red Hat, Inc. and the virtio-win contributors.
- `vendor/virtio-win/netkvm-arm64.zip` is the unmodified, WHQL-signed ARM64
  NetKVM virtio-net guest driver, JIT-staged into a Mac-hosted Windows guest for
  NAT networking. Redistributed unmodified; the license text is staged alongside
  the driver in the guest.

## tools/win-mesa-gl/prebuilt/ — Mesa OpenGL (gallium d3d12) and zlib (prebuilt binaries)

Ships in the Windows release as `resources\win-mesa-gl\` (`opengl32.dll`,
`gallium_wgl.dll`, `z-1.dll`); the guest agent copies the three files into a
Windows guest's `System32`.

- `opengl32.dll`, `gallium_wgl.dll`: **Mesa 26.1.2**, built unmodified from the
  upstream `26.1` branch by `tools/win-mesa-gl/build/build-mesa.cmd`.
  - **License:** MIT License (Mesa's overall license). Mesa also contains
    components under other permissive licenses, among them the Microsoft
    DirectX-Headers (MIT) compiled into the d3d12 driver; the full list is at
    <https://docs.mesa3d.org/license.html>.
  - **Copyright:** Copyright (C) 1999-2007 Brian Paul and the Mesa contributors.
- `z-1.dll`: **zlib 1.3.1** (a Mesa build subproject).
  - **License:** zlib License (`SPDX: Zlib`).
  - **Copyright:** Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler.

Mesa license text:

```
Copyright (C) 1999-2007 Brian Paul All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

zlib license text:

```
  (C) 1995-2024 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

## tools/vdd/ThirdParty/nvenc/nvEncodeAPI.h — NVIDIA Video Codec SDK (NVENC API header)

- **License:** MIT License (`SPDX: MIT`); the notice is at the top of the file.
- **Copyright:** Copyright (c) 2010-2023 NVIDIA Corporation.
- The NVENC API 12.1 header, as distributed in FFmpeg's `nv-codec-headers`
  (<https://github.com/FFmpeg/nv-codec-headers>). It is compiled into the
  guest display driver (`drivers\AppSandboxVDD.dll`). The NVENC runtime
  (`nvEncodeAPI64.dll`) is part of the NVIDIA driver and is not shipped.

```
Copyright (c) 2010-2023 NVIDIA Corporation

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the software, and to permit persons to whom the
software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

## tools/agent/appsandbox-input.c — ViGEmClient interface definitions

- **License:** MIT License (`SPDX: MIT`).
- **Copyright:** Copyright (c) 2018 Benjamin Höglinger-Stelzer.
- The guest input helper (`resources\appsandbox-input.exe`) talks to the
  ViGEmBus driver with the bus interface GUID, IOCTL codes and request
  structures of ViGEmClient (<https://github.com/nefarius/ViGEmClient>). No
  ViGEmClient source file is included. ViGEmBus itself (BSD-3-Clause) is not
  shipped; the agent installs it only when `drivers\ViGEmBus_setup.exe` is
  added to the guest components.

```
MIT License

Copyright (c) 2018 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Downloaded at run time (not included in the zip or the MSIX)

The Windows app downloads these when a VM is created, from their publishers'
servers, and copies them into that VM. They are not redistributed by this
project and stay under their own terms.

| Component | Downloaded from | License | Used for |
|---|---|---|---|
| Win32-OpenSSH 10.0.0.0p2 (`OpenSSH-Win64/ARM64-v10.0.0.0.msi`, Microsoft-signed; SHA-256 pinned in `disk_util.c`) | `github.com/PowerShell/Win32-OpenSSH` releases | OpenSSH license (BSD-style) and those of its bundled components; see the project's `LICENSE` | Installed in Windows guests when SSH is enabled |
| Microsoft OpenCL, OpenGL and Vulkan Compatibility Pack (`Microsoft.D3DMappingLayers`, Store ID 9NQPSL29BFFF): `OpenGLOn12.dll`, `OpenCLOn12.dll`, `clon12compiler.dll`, `dxil.dll`, `vulkan_dzn.dll` | Microsoft Store delivery service (`displaycatalog.mp.microsoft.com`, `fe3.delivery.mp.microsoft.com`, Microsoft CDN) | Microsoft software license terms of the Store package | Copied into Windows guests with GPU (the DLLs must carry Microsoft's Authenticode signature) |
| `Microsoft.Direct3D.Linux` and DXCore NuGet packages (`libd3d12`, `libdxcore`) | `pkgs.dev.azure.com/shine-oss` (the public feed the WSL build uses) | Microsoft license terms included in the packages | Copied into Ubuntu guests (GPU acceleration) |
| Ubuntu packages (build dependencies of the guest components) | `archive.ubuntu.com` | Each package's own license (`/usr/share/doc/<package>/copyright` in the guest) | Installed in Ubuntu guests |
| This project's Linux guest sources (incl. `tools/linux/dxgkrnl`, GPL-2.0) | `codeload.github.com` (the repository and commit set at build time) | As listed in this file | Compiled inside Ubuntu guests |

---

## AppSandbox's own Linux kernel module (not third-party)

`tools/linux/asb_drm/` is **AppSandbox's own** code, intentionally dual-licensed
under **MIT OR GPL-2.0** (`SPDX-License-Identifier: (GPL-2.0 OR MIT)`) and
declaring `MODULE_LICENSE("Dual MIT/GPL")`. The dual license lets the module
resolve GPL-only kernel symbols (`EXPORT_SYMBOL_GPL` DRM/KMS helpers) when it is
built into or loaded by the Linux kernel, while keeping the source available
under MIT for everyone else. It is listed here only for clarity — it is covered
by AppSandbox's own copyright, not a third party's.
