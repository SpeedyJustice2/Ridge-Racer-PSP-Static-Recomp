@echo off
rem Starts the setup window. (A console window may flash for a moment; setup itself runs in its own window.)
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0installer.ps1"
