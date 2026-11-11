@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-registry-settings.ps1"
if errorlevel 1 (
  echo Registry setup failed. Right-click this file and choose Run as administrator.
  pause
  exit /b 1
)
echo Registry settings setup complete.
pause
