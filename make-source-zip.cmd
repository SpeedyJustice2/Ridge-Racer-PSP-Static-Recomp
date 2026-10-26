@echo off
rem Packs just the source needed to build on Linux or macOS into dist\RidgeRacer-source.zip (a few MB, no game data, only the Ridge Racer profile).
setlocal
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "DIST=%SRC%\dist"
set "STAGE=%DIST%\_srcstage"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
for %%D in (include src tools) do robocopy "%SRC%\%%D" "%STAGE%\%%D" /E /XD build generated node_modules /NFL /NDL /NJH /NJS >nul
robocopy "%SRC%\profiles\ridgeracer" "%STAGE%\profiles\ridgeracer" /E /XD build generated game analysis captures progress /NFL /NDL /NJH /NJS >nul
robocopy "%SRC%\tests" "%STAGE%\tests" *.cpp synthetic_* /NFL /NDL /NJH /NJS >nul
for %%F in (CMakeLists.txt build-unix.sh build-linux.sh LICENSE README.md) do if exist "%SRC%\%%F" copy /Y "%SRC%\%%F" "%STAGE%\" >nul
tar -a -cf "%DIST%\RidgeRacer-source.zip" -C "%STAGE%" .
rmdir /s /q "%STAGE%"
for %%A in ("%DIST%\RidgeRacer-source.zip") do echo == Done: %%~fA  (%%~zA bytes)
pause
