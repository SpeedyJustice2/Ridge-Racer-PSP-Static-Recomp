@echo off
rem Tests dist\Setup.exe on a clean Windows: starts Windows Sandbox (a throwaway copy of Windows, Pro/Enterprise/Education)
rem with Setup.exe and your disc image's folder mapped in, and runs Setup there.  Closing the sandbox deletes everything in it.
rem Usage: test-in-sandbox.cmd ["D:\path\to\your game.iso"]
setlocal
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
if not exist "%SRC%\dist\Setup.exe" (echo dist\Setup.exe not found: run make-release.cmd first. & pause & exit /b 1)
if not exist "%windir%\System32\WindowsSandbox.exe" (
  echo Windows Sandbox is not turned on ^(it needs Windows 10/11 Pro, Enterprise or Education^).
  echo To turn it on: open "Turn Windows features on or off", tick "Windows Sandbox", restart the PC.
  echo Or in an administrator PowerShell:  Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM
  pause & exit /b 1
)
set "ISO=%~1"
if "%ISO%"=="" set /p ISO=Full path of your decrypted disc image (.iso): 
set "ISO=%ISO:"=%"
if not exist "%ISO%" (echo Not found: %ISO% & pause & exit /b 1)
for %%I in ("%ISO%") do (set "ISODIR=%%~dpI" & set "ISONAME=%%~nxI")
if "%ISODIR:~-1%"=="\" set "ISODIR=%ISODIR:~0,-1%"
set "WSB=%TEMP%\rr-sandbox-test.wsb"
set "BOX=C:\Users\WDAGUtilityAccount\Desktop"
(
  echo ^<Configuration^>
  echo   ^<MemoryInMB^>8192^</MemoryInMB^>
  echo   ^<MappedFolders^>
  echo     ^<MappedFolder^>^<HostFolder^>%SRC%\dist^</HostFolder^>^<SandboxFolder^>%BOX%\RidgeRacerSetup^</SandboxFolder^>^<ReadOnly^>false^</ReadOnly^>^</MappedFolder^>
  echo     ^<MappedFolder^>^<HostFolder^>%ISODIR%^</HostFolder^>^<SandboxFolder^>%BOX%\Game^</SandboxFolder^>^<ReadOnly^>true^</ReadOnly^>^</MappedFolder^>
  echo   ^</MappedFolders^>
  echo   ^<LogonCommand^>^<Command^>%BOX%\RidgeRacerSetup\Setup.exe^</Command^>^</LogonCommand^>
  echo ^</Configuration^>
) > "%WSB%"
echo == Starting Windows Sandbox (a clean Windows; give it a minute to start)
echo    In the sandbox Setup starts by itself.  Pick this disc image:  Desktop\Game\%ISONAME%
echo    and keep the suggested install folder.  The last page shows how long the install took.
echo    Setup-log.txt (the full build log) appears in your dist folder: %SRC%\dist
echo    Closing the sandbox window deletes everything inside it.
start "" "%WSB%"
