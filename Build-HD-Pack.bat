@echo off
rem Rogue Galaxy Recompiled: builds the HD texture pack and the HD movies from your own
rem disc (run Install.bat first). Double-click it for a menu. This is a thin wrapper:
rem tools\hd\build-hd-pack.ps1 starts the setup program (python -m rgsetup,
rem docs\INSTALLER.md), which drives tools\hd\build_hd_pack.py.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\hd\build-hd-pack.ps1" %*
echo.
pause
