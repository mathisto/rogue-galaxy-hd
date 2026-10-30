@echo off
rem Rogue Galaxy Recompiled: builds the HD texture pack and the HD movies from your own
rem disc (run Install.bat first). Double-click it for a menu. The work is done by
rem tools\hd\build-hd-pack.ps1 and tools\hd\build_hd_pack.py, which say more.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\hd\build-hd-pack.ps1" %*
echo.
pause
