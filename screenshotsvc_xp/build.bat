@echo off
setlocal
cd /d "%~dp0"
set "MSBUILD_EXE="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    if exist "%%I\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD_EXE=%%I\MSBuild\Current\Bin\MSBuild.exe"
  )
)
if not defined MSBUILD_EXE (
  for /f "delims=" %%I in ('where msbuild 2^>nul') do if not defined MSBUILD_EXE set "MSBUILD_EXE=%%I"
)
if not defined MSBUILD_EXE (
  echo ERROR: MSBuild was not found. Install Visual Studio 2022 with Desktop development with C++.
  goto :fail
)
if not exist "ScreenshotServiceClassic.vcxproj" (
  echo ERROR: ScreenshotServiceClassic.vcxproj was not found.
  goto :fail
)
"%MSBUILD_EXE%" ScreenshotServiceClassic.vcxproj /nologo /m /t:Build /p:Configuration=Release /p:Platform=Win32 /v:minimal
if errorlevel 1 goto :fail
echo.
echo BUILD SUCCESSFUL: build\Release\ScreenshotServiceClassic.exe
echo This build uses the v141_xp toolset. Validate on Windows XP SP3 before distributing.
goto :eof
:fail
echo.
echo BUILD FAILED.
echo Confirm that C++ Windows XP Support for VS 2017 (v141) tools is installed in Visual Studio Installer.
exit /b 1
