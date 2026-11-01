@echo off
rem Rogue Galaxy Recompiled: builds the game on this PC from your own disc
rem image. Double-click it, or drop the .iso onto it. This is a thin wrapper:
rem tools\install\install.ps1 starts the setup program (python -m rgsetup,
rem docs\INSTALLER.md), which does the work.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install.ps1" %*
echo.
pause
