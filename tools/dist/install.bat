@echo off
rem install.bat - installs or updates Back on Air, the Split/Second PC patch. "install.bat /uninstall" removes it.
setlocal
title Back on Air, a PC patch for Split/Second
if not exist "%~dp0install.ps1" (
  echo Unpack the whole zip first, then run install.bat from the unpacked folder.
  pause
  exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set RC=%ERRORLEVEL%
echo.
pause
exit /b %RC%
