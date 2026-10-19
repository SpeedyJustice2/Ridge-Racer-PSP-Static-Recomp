@echo off
rem Builds dist\Setup.exe: a single installer exe = small stub + zip of (source, setup window, zig compiler).
setlocal
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "ZIP=C:\Users\Ian\Downloads\zig-x86_64-windows-0.18.0-dev.349+d924f83bf.zip"
set "ZIG=%SRC%\toolchain\zig\zig.exe"
set "DIST=%SRC%\dist"
set "STAGE=%DIST%\_stage"
if not exist "%ZIG%" (
  echo == Unpacking zig into toolchain\zig
  mkdir "%SRC%\toolchain\_x"
  tar -xf "%ZIP%" -C "%SRC%\toolchain\_x"
  for /d %%D in ("%SRC%\toolchain\_x\*") do move "%%D" "%SRC%\toolchain\zig" >nul
  rmdir "%SRC%\toolchain\_x"
)
if not exist "%ZIG%" (echo ZIG NOT FOUND & pause & exit /b 1)
echo == Closing any running Setup.exe from a previous test
taskkill /f /im Setup.exe >nul 2>&1
timeout /t 2 /nobreak >nul
if exist "%DIST%" rmdir /s /q "%DIST%"
if exist "%DIST%\Setup.exe" (echo COULD NOT DELETE OLD Setup.exe - close it in Task Manager and run again & pause & exit /b 1)
mkdir "%STAGE%"
echo == Compiling the setup stub
"%ZIG%" c++ -target x86_64-windows-gnu -O2 -std=c++17 -municode -static -Wl,--subsystem,windows -o "%DIST%\stub.exe" "%SRC%\tools\setup_stub.cpp"
if errorlevel 1 (echo STUB BUILD FAILED & pause & exit /b 1)
echo == Collecting files
for %%D in (include profiles src tools) do robocopy "%SRC%\%%D" "%STAGE%\%%D" /E /XD build generated /NFL /NDL /NJH /NJS >nul
for %%F in (CMakeLists.txt build.ps1 installer.ps1 LICENSE README.md README-BETA.md) do copy /Y "%SRC%\%%F" "%STAGE%\" >nul
robocopy "%SRC%\toolchain\zig" "%STAGE%\toolchain\zig" /E /NFL /NDL /NJH /NJS >nul
echo == Compressing (this takes a minute)
tar -a -cf "%DIST%\payload.zip" -C "%STAGE%" .
if errorlevel 1 (echo ZIP FAILED & pause & exit /b 1)
copy /b "%DIST%\stub.exe" + "%DIST%\payload.zip" "%DIST%\Setup.exe" >nul
if errorlevel 1 (echo COULD NOT WRITE Setup.exe - is it still running? & pause & exit /b 1)
rmdir /s /q "%STAGE%"
del "%DIST%\stub.exe" "%DIST%\payload.zip"
for %%A in ("%DIST%\Setup.exe") do echo == Done: %%~fA  (%%~zA bytes)
pause
