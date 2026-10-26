@echo off
rem Starts the local web server for the browser build and opens it in your browser.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1" %*
