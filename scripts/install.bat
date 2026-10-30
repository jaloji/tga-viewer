@echo off
rem Installs the TGA WIC codec (Windows Explorer thumbnails for TGA images).
rem Run as administrator from the release folder (TgaWic.dll next to this file).
setlocal
cd /d "%~dp0"

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)
if not exist TgaWic.dll (
  echo TgaWic.dll not found next to install.bat. Build the project first, then run dist\install.bat.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\TgaViewer"

if not exist "%APPDIR%" mkdir "%APPDIR%"
if exist "%APPDIR%\TgaWic.dll" regsvr32 /s /u "%APPDIR%\TgaWic.dll"
copy /y TgaWic.dll "%APPDIR%\TgaWic.dll" >nul || (
  echo Could not replace "%APPDIR%\TgaWic.dll": it is probably in use by Explorer.
  echo Run uninstall.bat, sign out or restart, then run install.bat again.
  exit /b 1
)
regsvr32 /s "%APPDIR%\TgaWic.dll" || (
  echo Codec registration failed. If Microsoft Defender attack surface reduction rules are
  echo enforced on this PC, allow the DLL first ^(admin PowerShell^):
  echo   Add-MpPreference -AttackSurfaceReductionOnlyExclusions "%APPDIR%\TgaWic.dll"
  exit /b 1
)
echo TGA codec installed: %APPDIR%\TgaWic.dll
echo.
echo Done. If .tga files still show blank icons, run clear-thumbnails.bat.
