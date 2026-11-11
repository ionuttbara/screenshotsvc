# Build ScreenshotService Classic on Windows 11 for Windows XP

## Required toolset

Use Visual Studio 2022 on Windows 11, but do **not** use its default v143 compiler for the XP build. Install the legacy XP-targeting toolset:

1. Open **Visual Studio Installer**.
2. Choose **Modify** on the VS 2022 installation.
3. Keep **Desktop development with C++** selected.
4. Open **Individual components**.
5. Under **Compilers, build tools, and runtimes**, install **C++ Windows XP Support for VS 2017 (v141) tools [Deprecated]**. Ensure the component supplies the Windows 7 SDK needed by this project.
6. Finish installation and close/reopen terminal windows.

The project explicitly uses `PlatformToolset=v141_xp`, Win32/x86, static CRT (`/MT`), and Windows subsystem version 5.00. The `build.bat` locates MSBuild and builds that project instead of accidentally using the default VS 2022 toolset.

## Compile

Extract the archive, then run from a regular Command Prompt or Developer Command Prompt:

```bat
cd /d C:\path\to\ScreenshotServiceClassic
build.bat
```

Expected output path:

`build\Release\ScreenshotServiceClassic.exe`

## Minimum OS note

Microsoft's v141_xp toolset targets **Windows XP SP3 (x86)** or later for 32-bit binaries. It is not the correct toolset if Windows 2000 support is still required. For Windows 2000, use a separate build with Visual Studio 2008 SP1 / VC9 and test on a Windows 2000 VM. Vista/7/8 should also run the XP-targeted binary, subject to runtime testing.

## Validation

A successful build on Windows 11 does not guarantee every legacy API/codec path works on XP. Test on a Windows XP SP3 x86 VM, including screenshots, AVI start/stop, codec availability, and audio device availability. A machine with no compatible Video for Windows codec falls back to uncompressed AVI, which consumes disk space quickly.

**XP subsystem target:** the x86 project uses Windows subsystem version `5.01` (Windows XP). Do not set it to `5.00`: MSVC treats that as invalid for a Windows GUI executable and falls back to the newer default, which can cause Windows XP to reject the EXE.
