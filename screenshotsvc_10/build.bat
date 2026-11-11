@echo off
setlocal
cd /d "%~dp0"

set "SRC=main.c hotkey.c monitorpicker.c colorpicker.c annotate.c livepen.c settings.c theme.c capture.c save.c log.c overlay.c toolbar.c poly.c hdr.c settings_ui.c record.c audio.c recordbar.c encoder.c hdrvideo.c mp4tag.c wavsink.c remux.c rawpcm.c rawmix.c flac_core.c flacenc.c mp4multi.c gif.c recovery.c lang.c tabs.c nvenc.c amfenc.c qsvenc.c vpconv.c crash.c"
set "LIBS=user32.lib gdi32.lib comdlg32.lib shell32.lib advapi32.lib ole32.lib oleaut32.lib uxtheme.lib shlwapi.lib comctl32.lib d3d11.lib dxgi.lib dxguid.lib windowscodecs.lib uuid.lib dwmapi.lib mfplat.lib mfreadwrite.lib mfuuid.lib setupapi.lib"

rem Locate MSVC in Build Tools, Community, Professional or Enterprise installs.
rem Also support running from an already-initialized x64 Developer Command Prompt.
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS for %%P in (
  "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist "%%~P" set "VCVARS=%%~P"

if defined VCVARS (
  rem vcvars64 can print a harmless diagnostic on some installs; its environment is what matters.
  call "%VCVARS%" >nul 2>&1
  if errorlevel 1 echo Warning: vcvars64.bat returned a non-zero code; trying the configured compiler.
)
where cl >nul 2>&1 && goto :msvc
where gcc >nul 2>&1 && goto :mingw

echo ERROR: Could not find the MSVC compiler (cl.exe) or MinGW compiler (gcc.exe).
echo Install the Visual Studio C++ workload and Windows SDK, or run this script from

echo "x64 Native Tools Command Prompt for VS 2022".
goto :fail

:msvc
rc /nologo /fo screenshotservice.res ScreenshotService.rc || goto :fail
cl /nologo /TC /std:c17 /O2 /MT /W4 /WX /external:W0 /permissive- /guard:cf /Qspectre /sdl /utf-8 ^
   /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /external:I"third_party\amf\include" /external:I"third_party\nvenc" /external:I"third_party\vpl\include" %SRC% ^
   /link screenshotservice.res /SUBSYSTEM:WINDOWS /OUT:ScreenshotService.exe ^
   %LIBS% /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT /DEPENDENTLOADFLAG:0x800 || goto :fail
del /q *.obj screenshotservice.res >nul 2>&1
echo Built ScreenshotService.exe with MSVC.
goto :eof

:mingw
where gcc >nul 2>&1 || goto :fail
rem --codepage=65001: the .rc is UTF-8 and carries a copyright sign
windres --codepage=65001 ScreenshotService.rc -O coff -o screenshotservice.res.o || goto :fail
rem mingw-w64 always links its own default-manifest.o, which collides with
rem ours ("multiple non-default manifests"); shadow it with an empty object
rem found first via -B.
break > empty.c
gcc -c empty.c -o default-manifest.o || goto :fail
gcc -std=c17 -O2 -municode -mwindows -B. -Wall -Wextra ^
    -isystem third_party/amf/include -isystem third_party/nvenc -isystem third_party/vpl/include %SRC% screenshotservice.res.o -o ScreenshotService.exe ^
    -luser32 -lgdi32 -lcomdlg32 -lshell32 -ladvapi32 -lole32 -loleaut32 -luxtheme -lshlwapi ^
    -lcomctl32 -ld3d11 -ldxgi -ldxguid -lwindowscodecs -luuid -ldwmapi -lmfplat -lmfreadwrite -lmfuuid -lsetupapi -lm || goto :fail
del /q screenshotservice.res.o empty.c default-manifest.o >nul 2>&1
echo Built ScreenshotService.exe with MinGW.
goto :eof

:fail
echo BUILD FAILED
exit /b 1
