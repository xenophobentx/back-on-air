@echo off
rem uninstall.bat - removes Back on Air, the Split/Second PC patch (same as "install.bat /uninstall").
setlocal
title Back on Air, a PC patch for Split/Second
if not exist "%~dp0install.ps1" (
  echo Unpack the whole zip first, then run uninstall.bat from the unpacked folder.
  pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" /uninstall
set RC=%ERRORLEVEL%
echo.
pause
exit /b %RC%
