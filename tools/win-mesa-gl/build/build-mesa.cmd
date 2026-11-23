@echo off
REM ======================================================================
REM build-mesa.cmd -- build Mesa's standalone OpenGL stack (the "opengl32
REM trio") for AppSandbox Windows GPU-PV guests.
REM
REM Produces three self-contained DLLs (static CRT, no VCRUNTIME/MSVCP):
REM     opengl32.dll      Mesa drop-in OpenGL (full gl*/wgl*)
REM     gallium_wgl.dll   gallium d3d12 WGL driver it imports
REM     z-1.dll           zlib (Mesa subproject)
REM These replace Microsoft's opengl32 system-wide so GL apps get hardware
REM OpenGL 4.6 via D3D12 (WARP software 4.6 when no GPU).
REM
REM Tracks a STABLE Mesa release branch (like build-mesa.sh), recording the
REM resulting version in BUILDINFO -- not a -devel snapshot or commit pin.
REM
REM Usage:    build-mesa.cmd [x64^|arm64]
REM     x64    native build on an x64 host (default)
REM     arm64  cross build from an x64 host (x64_arm64 toolset + cross file)
REM Run ONCE PER ARCH in a FRESH shell (vcvarsall x64 and x64_arm64 must not
REM share an environment).
REM
REM Prereqs: VS 2022 Build Tools with the x64 AND ARM64
REM C++ toolsets + Win11 SDK; Python 3 with meson, mako, packaging, pyyaml,
REM setuptools; Ninja; win_flex/win_bison; internet (meson fetches the
REM DirectX-Headers + zlib wraps). Output lands in ..\prebuilt\<arch>\.
REM
REM Overridable via environment:
REM     MESA_SRC     Mesa checkout    (default %USERPROFILE%\source\repos\mesa)
REM     MESA_BRANCH  stable branch    (default below)
REM     MESON        meson launcher   (auto-detected)
REM ======================================================================
setlocal EnableExtensions EnableDelayedExpansion

REM ---- pinned to a STABLE release branch.
REM      26.1 is the latest stable and the closest to the 26.2.0-devel Mesa
REM      that Microsoft's OpenGLOn12 ships. Bump deliberately + re-verify
REM      before re-vendoring. ----
if not defined MESA_BRANCH set "MESA_BRANCH=26.1"

REM ---- target arch ----
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
if /I "%ARCH%"=="x64"   ( set "VCARG=x64"       & set "CROSS=" )            else ^
if /I "%ARCH%"=="arm64" ( set "VCARG=x64_arm64" & set "CROSS=--cross-file ""%~dp0aarch64-cross.txt""" ) else (
    echo [build-mesa] ERROR: arch must be x64 or arm64 ^(got "%ARCH%"^)
    exit /b 2
)

set "SCRIPT_DIR=%~dp0"
set "OUT_DIR=%SCRIPT_DIR%..\prebuilt\%ARCH%"
if not defined MESA_SRC set "MESA_SRC=%USERPROFILE%\source\repos\mesa"

echo [build-mesa] arch=%ARCH%  mesa=%MESA_SRC%  branch=%MESA_BRANCH%

REM ---- get the Mesa source on the stable branch ----
if not exist "%MESA_SRC%\meson.build" (
    echo [build-mesa] cloning Mesa %MESA_BRANCH% to "%MESA_SRC%" ...
    git clone --depth 1 -b "%MESA_BRANCH%" https://gitlab.freedesktop.org/mesa/mesa.git "%MESA_SRC%" || exit /b 1
) else (
    echo [build-mesa] updating existing checkout to %MESA_BRANCH% ...
    git -C "%MESA_SRC%" fetch --depth 1 origin "%MESA_BRANCH%" || exit /b 1
    git -C "%MESA_SRC%" checkout -B "%MESA_BRANCH%" FETCH_HEAD || exit /b 1
)
for /f "usebackq delims=" %%v in ("%MESA_SRC%\VERSION") do set "MVER=%%v"
echo [build-mesa] Mesa version: !MVER!

REM ---- MSVC environment for the target arch (vcvarsall located via vswhere) ----
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" ( echo [build-mesa] ERROR: vcvarsall.bat not found ^(install VS 2022 Build Tools^) & exit /b 1 )
echo [build-mesa] using %VCVARS% %VCARG%
call "%VCVARS%" %VCARG% || exit /b 1

REM ---- win_flex/win_bison: the WinGet "Links" entries are symlinks, and
REM      win_bison resolves its data\m4sugar\ relative to argv[0] -- so when
REM      invoked through the alias it fails ("cannot open data/m4sugar/
REM      m4sugar.m4"). ALWAYS put the real WinFlexBison package dir FIRST on
REM      PATH (ahead of the Links alias) so the real exe + its adjacent data\
REM      win. ----
set "FOUND_FB="
for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\WinFlexBison.win_flex_bison*") do (
    if exist "%%d\win_bison.exe" ( set "PATH=%%d;!PATH!" & set "FOUND_FB=1" )
)
if not defined FOUND_FB (
    where win_flex >nul 2>nul || ( echo [build-mesa] ERROR: WinFlexBison not found ^(winget install WinFlexBison.win_flex_bison^) & exit /b 1 )
)

REM ---- meson launcher (PATH meson, else python -m mesonbuild) ----
if not defined MESON (
    where meson >nul 2>nul && ( set "MESON=meson" ) || ( set "MESON=python -m mesonbuild.mesonmain" )
)
where python >nul 2>nul || ( echo [build-mesa] ERROR: python not on PATH & exit /b 1 )

REM ---- configure. Video/d3d12-encoder are KEPT ON (-Dvideo-codecs=all),
REM      matching Microsoft's shipping build. /wd4189 neutralises
REM      the MSVC "unreferenced local" warning the video encoder promotes to
REM      an error; if a warning-as-error still breaks the build, add a (void)
REM      cast on the variable the failure names and re-run. ----
set "BUILD=%MESA_SRC%\build-%ARCH%"
%MESON% setup "%BUILD%" "%MESA_SRC%" --reconfigure ^
  -Dbuildtype=release ^
  -Dgallium-drivers=d3d12 ^
  -Dvulkan-drivers= ^
  -Dgallium-wgl-dll-name=gallium_wgl ^
  -Dllvm=disabled ^
  -Dshared-glapi=enabled ^
  -Dopengl=true ^
  -Dgles1=disabled -Dgles2=disabled ^
  -Degl=disabled -Dgbm=disabled -Dglx=disabled ^
  -Dmicrosoft-clc=disabled ^
  -Dvideo-codecs=all ^
  -Db_vscrt=mt ^
  -Dcpp_args=/wd4189 -Dc_args=/wd4189 ^
  --wrap-mode=default ^
  %CROSS%
if errorlevel 1 ( echo [build-mesa] meson setup failed. & exit /b 1 )

REM ---- build (win_flex can intermittently collide on a temp file; one retry) ----
%MESON% compile -C "%BUILD%"
if errorlevel 1 (
    echo [build-mesa] first build pass failed; clearing win_flex temp files and retrying once...
    del "%TEMP%\~flex_out*" 2>nul
    %MESON% compile -C "%BUILD%"
    if errorlevel 1 (
        echo [build-mesa] build failed. If it stopped on a C4189 warning-as-error in
        echo [build-mesa] d3d12_video_enc.cpp, add "(void) bUsedAsReference;" after its
        echo [build-mesa] declaration and re-run.
        exit /b 1
    )
)

REM ---- collect the trio ----
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
copy /Y "%BUILD%\src\gallium\targets\libgl-gdi\opengl32.dll" "%OUT_DIR%\"     || exit /b 1
copy /Y "%BUILD%\src\gallium\targets\wgl\gallium_wgl.dll"    "%OUT_DIR%\"     || exit /b 1
set "ZFOUND="
for /r "%BUILD%\subprojects" %%f in (z-1.dll) do ( copy /Y "%%f" "%OUT_DIR%\" >nul && set "ZFOUND=1" )
if not defined ZFOUND ( echo [build-mesa] ERROR: z-1.dll not found under %BUILD%\subprojects & exit /b 1 )

REM ---- record provenance (BUILDINFO) ----
> "%OUT_DIR%\BUILDINFO" echo mesa-version : !MVER!
>>"%OUT_DIR%\BUILDINFO" echo mesa-branch  : %MESA_BRANCH%
>>"%OUT_DIR%\BUILDINFO" echo arch         : %ARCH%
>>"%OUT_DIR%\BUILDINFO" echo built-on     : %DATE% %TIME%
>>"%OUT_DIR%\BUILDINFO" echo toolchain    : %VCVARS% %VCARG%
>>"%OUT_DIR%\BUILDINFO" echo gallium      : d3d12  (video-codecs=all, llvm=disabled, b_vscrt=mt)

echo.
echo [build-mesa] DONE (Mesa !MVER!, %ARCH%). Trio in: %OUT_DIR%
dir /b "%OUT_DIR%"
echo [build-mesa] sanity-check:  dumpbin /dependents "%OUT_DIR%\opengl32.dll"
echo [build-mesa]   ^(expect gallium_wgl.dll; NOT opengl32.dll, NOT VCRUNTIME140/MSVCP140^)
endlocal
