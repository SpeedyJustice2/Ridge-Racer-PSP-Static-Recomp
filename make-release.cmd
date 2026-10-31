@echo off
rem Builds dist\Setup.exe: a single installer exe = small stub + zip of (source, setup window, zig compiler, prebuilt parts).
rem Only the Ridge Racer profile goes in (not the other projects under profiles\), and the result is checked for game files.
setlocal
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "ZIG=%SRC%\toolchain\zig\zig.exe"
set "DIST=%SRC%\dist"
set "STAGE=%DIST%\_stage"
set "T0=%TIME%"
rem The compiler version releases are tested with (a stable release, not a development build).
set "ZIGVER=0.17.0"
if not exist "%ZIG%" (
  rem zig for Windows from Downloads: the tested stable version first (%ZIGVER%), otherwise the newest zig zip there
  set "ZIP="
  if exist "%USERPROFILE%\Downloads\zig-x86_64-windows-%ZIGVER%.zip" set "ZIP=%USERPROFILE%\Downloads\zig-x86_64-windows-%ZIGVER%.zip"
  for /f "delims=" %%Z in ('dir /b /o-d "%USERPROFILE%\Downloads\zig-x86_64-windows-*.zip" 2^>nul') do if not defined ZIP set "ZIP=%USERPROFILE%\Downloads\%%Z"
  if not defined ZIP (echo ZIG NOT FOUND: download zig-x86_64-windows-%ZIGVER%.zip from https://ziglang.org/download/ into Downloads & pause & exit /b 1)
  echo == Unpacking zig into toolchain\zig
  mkdir "%SRC%\toolchain\_x"
  call tar -xf "%%ZIP%%" -C "%SRC%\toolchain\_x"
  for /d %%D in ("%SRC%\toolchain\_x\*") do move "%%D" "%SRC%\toolchain\zig" >nul
  rmdir "%SRC%\toolchain\_x"
)
if not exist "%ZIG%" (echo ZIG NOT FOUND & pause & exit /b 1)
"%ZIG%" version > "%TEMP%\rr-zigver.txt" 2>nul
set /p ZIGHAVE=<"%TEMP%\rr-zigver.txt"
echo == Compiler: zig %ZIGHAVE%
if not "%ZIGHAVE%"=="%ZIGVER%" (
  echo    Note: releases are tested with zig %ZIGVER%.  To switch: delete the toolchain\zig folder, put
  echo    zig-x86_64-windows-%ZIGVER%.zip from https://ziglang.org/download/ in Downloads and run this again.
)
echo == Closing any running Setup.exe from a previous test
taskkill /f /im Setup.exe >nul 2>&1
timeout /t 2 /nobreak >nul
if exist "%DIST%" rmdir /s /q "%DIST%"
if exist "%DIST%\Setup.exe" (echo COULD NOT DELETE OLD Setup.exe - close it in Task Manager and run again & pause & exit /b 1)
mkdir "%STAGE%"
echo == Compiling the setup stub
"%ZIG%" c++ -target x86_64-windows-gnu -O2 -std=c++17 -municode -static -Wl,--subsystem,windows -lgdi32 -luser32 -o "%DIST%\stub.exe" "%SRC%\tools\setup_stub.cpp"
if errorlevel 1 (echo STUB BUILD FAILED & pause & exit /b 1)
echo == Collecting files
for %%D in (include src tools) do robocopy "%SRC%\%%D" "%STAGE%\%%D" /E /XD build generated node_modules /NFL /NDL /NJH /NJS >nul
robocopy "%SRC%\profiles\ridgeracer" "%STAGE%\profiles\ridgeracer" /E /XD build generated game analysis captures progress /NFL /NDL /NJH /NJS >nul
for %%F in (CMakeLists.txt build.ps1 installer.ps1 LICENSE README.md README-BETA.md) do if exist "%SRC%\%%F" copy /Y "%SRC%\%%F" "%STAGE%\" >nul
robocopy "%SRC%\toolchain\zig" "%STAGE%\toolchain\zig" /E /NFL /NDL /NJH /NJS >nul
echo == Safety check (no game files in the installer)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$bad = Get-ChildItem -LiteralPath '%STAGE%' -Recurse -File | Where-Object { $_.FullName -notmatch '\\toolchain\\' -and ($_.Name -match '\.(iso|cso|prx|pmf|at3|pbp|elf|bin|dat|sfo|dmp)$' -or $_.Name -match '^(BOOT|EBOOT)' -or $_.FullName -match '\\(generated|game)\\') }; if ($bad) { Write-Host 'STOPPED - these files must not be in the installer:' -ForegroundColor Red; $bad | ForEach-Object { Write-Host ('  ' + $_.FullName) }; exit 1 }; Write-Host 'OK'"
if errorlevel 1 (pause & exit /b 1)
echo == Building the parts that do not depend on the game (once here, so every install is much faster)
echo    This is the slow step of making a release (about as long as the old installer took); it only runs on your PC.
powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%\build.ps1" -PrepareRelease "%STAGE%" -WorkDir "%DIST%\_prep"
if errorlevel 1 (echo PREPARING THE PREBUILT PARTS FAILED & pause & exit /b 1)
if not exist "%STAGE%\prebuilt\psp_recomp.exe" (echo PREBUILT PARTS MISSING & pause & exit /b 1)
if not exist "%STAGE%\prebuilt\RidgeRacerNative.exe" (echo PROGRAM MISSING FROM THE PREBUILT PARTS & pause & exit /b 1)
echo == Update package ^(the program only, for the in-game updater^)
mkdir "%DIST%\_upd"
move /Y "%STAGE%\prebuilt\RidgeRacerNative.exe" "%DIST%\_upd\RidgeRacerNative.exe" >nul
if exist "%SRC%\profiles\ridgeracer\third_party\sdl2\SDL2.dll" copy /Y "%SRC%\profiles\ridgeracer\third_party\sdl2\SDL2.dll" "%DIST%\_upd\" >nul
if exist "%SRC%\README-BETA.md" copy /Y "%SRC%\README-BETA.md" "%DIST%\_upd\README.txt" >nul
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t = Get-Content -Raw -LiteralPath '%SRC%\profiles\ridgeracer\gen\rr_gen_abi_version.hpp'; if ($t -match 'kAbiVersion\s*=\s*(\d+)') { Set-Content -LiteralPath '%DIST%\_upd\abi.txt' -Value $Matches[1] -Encoding ASCII } else { exit 1 }"
if errorlevel 1 (echo COULD NOT READ kAbiVersion & pause & exit /b 1)
echo == Safety check ^(the update package must hold only our own program, no game data or converted game code^)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d = '%DIST%\_upd'; $ok = @('RidgeRacerNative.exe','SDL2.dll','README.txt','abi.txt'); $files = Get-ChildItem -LiteralPath $d -File; $bad = $files | Where-Object { $ok -notcontains $_.Name }; if ($bad -or (Get-ChildItem -LiteralPath $d -Directory)) { Write-Host 'STOPPED - unexpected files in the update package:' -ForegroundColor Red; $bad | ForEach-Object { Write-Host ('  ' + $_.Name) }; exit 1 }; $exe = Get-Item (Join-Path $d 'RidgeRacerNative.exe'); if ($exe.Length -gt 30MB) { Write-Host ('STOPPED - the program is ' + [math]::Round($exe.Length / 1MB) + ' MB; it should not contain game code.') -ForegroundColor Red; exit 1 }; $txt = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe.FullName)); if ($txt.Contains('recomp_unit_0') -or $txt.Contains('generated_unit')) { Write-Host 'STOPPED - converted game code is inside the program.' -ForegroundColor Red; exit 1 }; Write-Host 'OK'"
if errorlevel 1 (pause & exit /b 1)
tar -a -cf "%DIST%\RidgeRacer-Update.zip" -C "%DIST%\_upd" .
if errorlevel 1 (echo UPDATE PACKAGE FAILED & pause & exit /b 1)
rmdir /s /q "%DIST%\_upd"
echo == Compressing (this takes a minute)
tar -a -cf "%DIST%\payload.zip" -C "%STAGE%" .
if errorlevel 1 (echo ZIP FAILED & pause & exit /b 1)
copy /b "%DIST%\stub.exe" + "%DIST%\payload.zip" "%DIST%\Setup.exe" >nul
if errorlevel 1 (echo COULD NOT WRITE Setup.exe - is it still running? & pause & exit /b 1)
rem Code signing (optional): sign-release.cmd next to this file signs Setup.exe (see sign-release.example.cmd).  The setup
rem stub knows that a signature follows its payload.
if exist "%SRC%\sign-release.cmd" (
  echo == Signing Setup.exe
  call "%SRC%\sign-release.cmd" "%DIST%\Setup.exe"
  if errorlevel 1 (echo SIGNING FAILED - Setup.exe is there but unsigned & pause & exit /b 1)
) else (
  echo == Not signed ^(no sign-release.cmd^): testers will see the "unknown publisher" warning
)
rmdir /s /q "%STAGE%"
if exist "%DIST%\_prep" rmdir /s /q "%DIST%\_prep"
del "%DIST%\stub.exe" "%DIST%\payload.zip"
if exist "%DIST%\stub.pdb" del "%DIST%\stub.pdb"
for %%A in ("%DIST%\Setup.exe") do echo == Done: %%~fA  (%%~zA bytes)
for %%A in ("%DIST%\RidgeRacer-Update.zip") do echo ==       %%~fA  (%%~zA bytes)  - upload this too: it is what the in-game updater downloads

echo    Started %T0%, finished %TIME%.
pause
