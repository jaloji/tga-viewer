@echo off
rem Removes the TGA WIC codec.
rem Run as administrator.
setlocal

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\TgaViewer"

if exist "%APPDIR%\TgaWic.dll" (
  regsvr32 /s /u "%APPDIR%\TgaWic.dll"
  rem Explorer may still hold the DLL; it can then be deleted after a restart.
  del /f "%APPDIR%\TgaWic.dll" 2>nul
  if exist "%APPDIR%\TgaWic.dll" echo TgaWic.dll is in use, delete "%APPDIR%" after a restart.
  rmdir "%APPDIR%" 2>nul
  echo TGA codec removed.
)
