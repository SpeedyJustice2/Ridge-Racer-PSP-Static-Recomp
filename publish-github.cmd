@echo off
rem Publishes the Ridge Racer port source to GitHub.
rem  - Collects only what building the port needs into a clean copy (no test output, no other projects, no game files).
rem  - Refuses to go on if anything that looks like game data or an unexpected large binary is in that copy.
rem  - First time (empty repository): one clean first commit.  Later: clones the repository and commits the changes on top.
rem Before the first run: create an empty repository on github.com (no README/licence) and install Git for Windows.
rem Usage: publish-github.cmd [repository URL]          Nothing is pushed until you confirm.
setlocal
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "PUB=%TEMP%\rr-publish"
set "GIT=%TEMP%\rr-publish-git"
where git >nul 2>&1 || (echo Git is not installed. Get it from https://git-scm.com/download/win & pause & exit /b 1)
set "URL=https://github.com/ngrshow/Ridge-Racer-PSP-Static-Recomp.git"
if not "%~1"=="" set "URL=%~1"
echo Target repository: %URL%
if exist "%PUB%" rmdir /s /q "%PUB%"
mkdir "%PUB%"

echo == Collecting source files
for %%D in (include src web tools) do robocopy "%SRC%\%%D" "%PUB%\%%D" /E /XD build generated node_modules .wrangler /NFL /NDL /NJH /NJS >nul
robocopy "%SRC%\online" "%PUB%\online" /E /XD node_modules .wrangler /NFL /NDL /NJH /NJS >nul
if exist "%SRC%\github-meta" robocopy "%SRC%\github-meta" "%PUB%\.github" /E /NFL /NDL /NJH /NJS >nul
robocopy "%SRC%\tests" "%PUB%\tests" *.cpp synthetic_* /NFL /NDL /NJH /NJS >nul
robocopy "%SRC%\profiles\ridgeracer" "%PUB%\profiles\ridgeracer" /E /XD build generated game analysis captures progress /NFL /NDL /NJH /NJS >nul
for %%F in (CMakeLists.txt build.ps1 build-web.ps1 build-web.cmd build-web.sh installer.ps1 "Install Ridge Racer.cmd" make-release.cmd make-source-zip.cmd publish-github.cmd sign-release.example.cmd test-in-sandbox.cmd LICENSE .gitignore README-BETA.md) do if exist "%SRC%\%%~F" copy /Y "%SRC%\%%~F" "%PUB%\" >nul
copy /Y "%SRC%\README-GITHUB.md" "%PUB%\README.md" >nul

echo == Safety check (game data, keys, generated code, large binaries)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$all = Get-ChildItem -LiteralPath '%PUB%' -Recurse -File; $bad = $all | Where-Object { $_.Name -match '\.(iso|cso|prx|pmf|at3|pbp|elf|bin|dat|sfo|dmp)$' -or $_.Name -match '^(BOOT|EBOOT)' -or ($_.Length -gt 2MB -and $_.Name -ne 'SDL2.dll') -or $_.FullName -match '\\(generated|game|web-dist|build-web)\\' }; if ($bad) { Write-Host 'NOT PUBLISHING - these files must not be in the public source:' -ForegroundColor Red; $bad | ForEach-Object { Write-Host ('  ' + $_.FullName + '  (' + $_.Length + ' bytes)') }; exit 1 }; Write-Host ('OK: ' + $all.Count + ' files, ' + [math]::Round(($all | Measure-Object Length -Sum).Sum / 1MB, 1) + ' MB')"
if errorlevel 1 (echo Fix the list above first. Nothing was pushed. & pause & exit /b 1)
echo.

set "GNAME="
set "GMAIL="
for /f "delims=" %%A in ('git config --global user.name 2^>nul') do set "GNAME=%%A"
for /f "delims=" %%A in ('git config --global user.email 2^>nul') do set "GMAIL=%%A"
if "%GNAME%"=="" set "GNAME=ngrshow"
if "%GMAIL%"=="" set "GMAIL=ngrshow@users.noreply.github.com"

rem Does the repository already have commits?
set "HASREMOTE="
for /f %%A in ('git ls-remote --heads "%URL%" 2^>nul') do set "HASREMOTE=1"
if defined HASREMOTE echo The repository already has commits: the changes will be committed on top of them.
if not defined HASREMOTE echo The repository is empty (or not reachable yet): this makes the first commit.
set "MSG="
set /p MSG=Short description of this update (Enter = Update): 
if "%MSG%"=="" set "MSG=Update"
set /p OK=Push to %URL% now? (y/n): 
if /i not "%OK%"=="y" (echo Cancelled. The files are in %PUB% & pause & exit /b 0)

if exist "%GIT%" rmdir /s /q "%GIT%"
if defined HASREMOTE goto :update
robocopy "%PUB%" "%GIT%" /E /NFL /NDL /NJH /NJS >nul
cd /d "%GIT%"
git init -q -b main || (pause & exit /b 1)
git remote add origin "%URL%"
goto :commit

:update
git clone -q "%URL%" "%GIT%" || (echo Could not clone %URL% & pause & exit /b 1)
cd /d "%GIT%"
rem replace everything but .git with the new copy, so files deleted here disappear from the repository too
for /d %%I in ("%GIT%\*") do if /i not "%%~nxI"==".git" rmdir /s /q "%%I"
for %%I in ("%GIT%\*") do del /f /q "%%I"
for %%I in ("%GIT%\.gitignore") do if exist "%%I" del /f /q "%%I"
robocopy "%PUB%" "%GIT%" /E /NFL /NDL /NJH /NJS >nul

:commit
git config user.name "%GNAME%"
git config user.email "%GMAIL%"
git add -A
git diff --cached --quiet && (echo Nothing changed since the last publish. & pause & exit /b 0)
git commit -q -m "%MSG%" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01W84c99NCCVmopwSXKK4LVs" || (echo Commit failed. & pause & exit /b 1)
git push -u origin main || (echo Push failed: check that you are signed in to GitHub and that the URL is right. & pause & exit /b 1)
echo.
echo == Done: %URL%
echo (If a sign-in window appeared, that was GitHub asking you to log in.)
pause
