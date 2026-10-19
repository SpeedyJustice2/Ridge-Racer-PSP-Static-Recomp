<#
.SYNOPSIS
  Builds the Ridge Racer PSP PC port from your own copy of the game, using the bundled compiler (no Visual Studio, no CMake).

.EXAMPLE
  .\build.ps1                     (asks for your game folder the first time)
  .\build.ps1 -Game "D:\RidgeRacer\Ridge Racer.iso"

.NOTES
  Needs the compiler in .\toolchain\zig (included in the release package) or a "zig" on your PATH.
  -Game     your disc image (.iso), your decrypted BOOT.BIN (an ELF file), or the folder that contains PSP_GAME
  -Jobs     parallel compiler processes (default: number of CPU cores)
  -Rebuild  recompile everything
  -NoLaunch do not start the game afterwards
  -InstallDir  where the finished game goes (default: .\RidgeRacer-Beta)
  -WorkDir     where intermediate files go while building (default: .\build)
  -CleanBuild  delete the work folder when the build succeeded
  -Progress    print machine-readable progress lines (used by the installer window)
  The first build takes a while (the game's code is turned into 428 C++ files and compiled); later runs only redo what changed.
  Nothing from the game is copied into this folder except the code generated from it, which stays on your PC.
#>
param(
  [string]$Game = "",
  [int]$Jobs = 0,
  [switch]$Rebuild,
  [switch]$NoLaunch,
  [string]$InstallDir = "",
  [string]$WorkDir = "",
  [switch]$CleanBuild,
  [switch]$Progress
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root
$build = if ($WorkDir) { $WorkDir } else { Join-Path $root "build" }
New-Item -ItemType Directory -Force $build | Out-Null

function Fail($m) { if ($Progress) { Write-Host "@@ERROR $m" }; Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }
function Step($m, $key = "") { if ($Progress -and $key) { Write-Host "@@STEP $key" }; Write-Host "`n== $m" -ForegroundColor Cyan }

# ---- compiler
Step "Looking for the compiler" "compiler"
$zig = Join-Path $root "toolchain\zig\zig.exe"
if (-not (Test-Path $zig)) {
  $cmd = Get-Command zig -ErrorAction SilentlyContinue
  if ($cmd) { $zig = $cmd.Source } else { Fail "Compiler not found. Expected toolchain\zig\zig.exe (it is part of the release package)." }
}
Write-Host "Compiler: $zig"
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build "zig-cache"
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build "zig-cache-local"
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

# ---- game file
Step "Finding your game" "game"
if (-not $Game) {
  $saved = Join-Path $build "game_path.txt"
  if (Test-Path $saved) { $Game = (Get-Content $saved -Raw).Trim() }
}
function Resolve-Boot($p) {
  if (-not $p) { return $null }
  if (Test-Path $p -PathType Leaf) { return (Resolve-Path $p).Path }
  foreach ($rel in @("PSP_GAME\SYSDIR\BOOT.BIN", "SYSDIR\BOOT.BIN", "BOOT.BIN")) {
    $c = Join-Path $p $rel
    if (Test-Path $c -PathType Leaf) { return (Resolve-Path $c).Path }
  }
  return $null
}
# A disc image (.iso): take BOOT.BIN out of it with a small helper built by the bundled compiler (a few seconds).
function Get-BootFromIso($iso) {
  $hostDir = Join-Path $root "profiles\ridgeracer\host"
  $toolExe = Join-Path $build "rr_iso_tool.exe"
  if (-not (Test-Path $toolExe)) {
    Write-Host "Building the disc image helper..."
    & $zig c++ -target x86_64-windows-gnu -std=c++20 -O1 -g0 -Wno-everything -I $hostDir (Join-Path $root "tools\rr_iso_tool.cpp") (Join-Path $hostDir "rr_iso.cpp") -o $toolExe
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $toolExe)) { Fail "Could not build the disc image helper." }
  }
  $dest = Join-Path $build "iso_boot\BOOT.BIN"
  New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
  Remove-Item $dest -ErrorAction SilentlyContinue
  $out = & $toolExe $iso --file "PSP_GAME/SYSDIR/BOOT.BIN" $dest 2>&1
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dest)) { Fail ("Could not read BOOT.BIN from the disc image: " + ($out -join " ")) }
  return $dest
}
$picked = $null
$bin = Resolve-Boot $Game
if (-not $bin -and $Progress) { Fail "The game was not found at: $Game" }
if (-not $bin) {
  Add-Type -AssemblyName System.Windows.Forms
  $ofd = New-Object System.Windows.Forms.OpenFileDialog
  $ofd.Title = "Select your Ridge Racer disc image (.iso) - or press Cancel to pick a folder instead"
  $ofd.Filter = "PSP disc image (*.iso)|*.iso|All files (*.*)|*.*"
  if ($ofd.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $bin = Resolve-Boot $ofd.FileName }
}
if (-not $bin) {
  $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
  $dlg.Description = "Select the folder that contains PSP_GAME (your own, decrypted Ridge Racer files)"
  while (-not $bin) {
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail "No game chosen." }
    $bin = Resolve-Boot $dlg.SelectedPath
    if (-not $bin) { [System.Windows.Forms.MessageBox]::Show("PSP_GAME\SYSDIR\BOOT.BIN was not found in that folder. Please choose the folder that contains PSP_GAME.") | Out-Null }
  }
}
$picked = $bin
if ($bin -match '\.iso$') { $bin = Get-BootFromIso $bin }
$head = [System.IO.File]::ReadAllBytes($bin)[0..3]
if ($head[0] -eq 0x7E -and $head[1] -eq 0x50 -and $head[2] -eq 0x53 -and $head[3] -eq 0x50) {
  Fail "That BOOT.BIN (or the disc image) is still encrypted ('~PSP'). The build needs a decrypted copy of your own game's executable (an ELF file starting with 'ELF')."
}
if (-not ($head[0] -eq 0x7F -and $head[1] -eq 0x45 -and $head[2] -eq 0x4C -and $head[3] -eq 0x46)) { Fail "That file is not an ELF executable. Expected a decrypted BOOT.BIN." }
$hash = (Get-FileHash $bin -Algorithm SHA256).Hash
Set-Content (Join-Path $build "game_path.txt") $picked
Write-Host "BOOT.BIN ok: $bin"

# ---- helpers
function Quote($a) { if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a } }
# Runs the compiler for every item that is out of date, $Jobs at a time.  Item = @{src=..; obj=..; args=@(..)}
function Invoke-Compiles($items, $label) {
  $todo = @($items | Where-Object { $Rebuild -or -not (Test-Path $_.obj) -or ((Get-Item $_.obj).LastWriteTime -lt (Get-Item $_.src).LastWriteTime) })
  Write-Host ("{0}: {1} to compile, {2} up to date" -f $label, $todo.Count, ($items.Count - $todo.Count))
  $running = New-Object System.Collections.ArrayList
  $done = 0; $failed = @()
  $queue = New-Object System.Collections.Queue
  foreach ($t in $todo) { $queue.Enqueue($t) }
  while ($queue.Count -gt 0 -or $running.Count -gt 0) {
    while ($queue.Count -gt 0 -and $running.Count -lt $Jobs) {
      $t = $queue.Dequeue()
      $psi = New-Object System.Diagnostics.ProcessStartInfo
      $psi.FileName = $zig
      $psi.Arguments = (($t.args + @("-c", $t.src, "-o", $t.obj)) | ForEach-Object { Quote $_ }) -join " "
      $psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
      $psi.RedirectStandardError = $true; $psi.RedirectStandardOutput = $true
      $p = [System.Diagnostics.Process]::Start($psi)
      [void]$running.Add(@{ proc = $p; item = $t; err = $p.StandardError.ReadToEndAsync(); out = $p.StandardOutput.ReadToEndAsync() })
    }
    Start-Sleep -Milliseconds 100
    for ($i = $running.Count - 1; $i -ge 0; $i--) {
      $r = $running[$i]
      if ($r.proc.HasExited) {
        $done++
        if ($r.proc.ExitCode -ne 0) { $failed += $r.item.src; Write-Host ("`nFAILED: " + $r.item.src) -ForegroundColor Red; Write-Host $r.err.Result }
        $running.RemoveAt($i)
        if (($done % 10) -eq 0 -or $done -eq $todo.Count) { Write-Host ("  {0}/{1}" -f $done, $todo.Count); if ($Progress) { Write-Host ("@@PCT {0} {1}" -f $done, $todo.Count) } }
      }
    }
  }
  if ($failed.Count) { Fail ("{0} file(s) failed to compile." -f $failed.Count) }
}
function New-ItemList($files, $objdir, $compileArgs) {
  New-Item -ItemType Directory -Force $objdir | Out-Null
  $files | ForEach-Object { @{ src = $_.FullName; obj = (Join-Path $objdir ($_.BaseName + ".o")); args = $compileArgs } }
}

$inc = @("-I", (Join-Path $root "include"))
$cxx = @("c++", "-target", "x86_64-windows-gnu", "-std=c++20", "-O2", "-g0", "-DNDEBUG", "-D_CRT_SECURE_NO_WARNINGS", "-mavx", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-Wno-everything")

# ---- code generator
Step "Building the code generator" "codegen"
$core = Get-ChildItem (Join-Path $root "src") -Filter *.cpp
$genExe = Join-Path $build "psp_recomp.exe"
$coreObj = @(New-ItemList $core (Join-Path $build "obj-core") ($cxx + $inc))
Invoke-Compiles $coreObj "core library"
$toolObj = @(New-ItemList @(Get-Item (Join-Path $root "tools\codegen_main.cpp")) (Join-Path $build "obj-tools") ($cxx + $inc + @("-I", (Join-Path $root "tools"))))
Invoke-Compiles $toolObj "code generator"
$linkGen = @("c++", "-target", "x86_64-windows-gnu") + ($coreObj + $toolObj | ForEach-Object { $_.obj }) + @("-o", $genExe)
if ($Rebuild -or -not (Test-Path $genExe)) { & $zig @linkGen; if ($LASTEXITCODE) { Fail "Linking the code generator failed." } }

# ---- generate
Step "Generating code from your game" "generate"
$gen = Join-Path $build "generated"
$stamp = Join-Path $gen ".source-sha256"
$have = (Test-Path (Join-Path $gen "generated_registry.cpp")) -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $hash)
if ($have) { Write-Host "Already generated for this BOOT.BIN." }
else {
  if (Test-Path $gen) { Remove-Item $gen -Recurse -Force }
  & $genExe $bin --auto $gen --jobs 0 --builtin-accessors --function-gates 0x08804000 4096 | Out-Host
  if ($LASTEXITCODE) { Fail "Code generation failed." }
  if (-not (Test-Path (Join-Path $gen "generated_registry.cpp"))) { Fail "Generation finished but generated_registry.cpp is missing." }
  Set-Content $stamp $hash
}

# ---- the game
Step "Compiling the game (this is the long step the first time)" "compile"
$prof = Join-Path $root "profiles\ridgeracer"
$hostDir = Join-Path $prof "host"
$a3p = Join-Path $prof "third_party\atrac3plus"
$hostNames = "main","rr_hle","rr_log","rr_ge","rr_raster","rr_gl","rr_png","rr_atrac","rr_mpeg","rr_pmf","rr_audio","rr_atrac_decoder",
             "rr_video_win32","rr_osk_ui","rr_menusound","rr_utility","rr_settings","rr_iso","rr_launcher_win32","rr_present_win32"
$hostFiles = $hostNames | ForEach-Object { Get-Item (Join-Path $hostDir ($_ + ".cpp")) }
$gameArgs = $cxx + $inc + @("-I", $hostDir, "-I", $a3p, "-I", $gen)
$objs = @()
$objs += New-ItemList $hostFiles (Join-Path $build "obj-host") $gameArgs
$cfiles = "atrac","atrac3plus","atrac3plusdsp","atrac3plusdec","rr_compat" | ForEach-Object { Get-Item (Join-Path $a3p ($_ + ".c")) }
$cArgs = @("cc", "-target", "x86_64-windows-gnu", "-std=c11", "-O2", "-g0", "-DNDEBUG", "-mavx", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-Wno-everything", "-I", (Join-Path $a3p "compat"), "-I", $a3p)
$objs += New-ItemList $cfiles (Join-Path $build "obj-atrac") $cArgs
$genFiles = Get-ChildItem $gen -Filter *.cpp
$objs += New-ItemList $genFiles (Join-Path $build "obj-gen") $gameArgs
Invoke-Compiles $objs "game"

# ---- resources (visual styles for the settings window)
$res = Join-Path $build "rr.res"
if ($Rebuild -or -not (Test-Path $res)) { Push-Location $hostDir; & $zig rc "rr.rc" $res; $rc = $LASTEXITCODE; Pop-Location; if ($rc) { Fail "Building resources failed." } }

# ---- link
Step "Linking" "link"
$out = if ($InstallDir) { $InstallDir } else { Join-Path $root "RidgeRacer-Beta" }
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out "RidgeRacerNative.exe"
$libs = "user32","gdi32","d3d11","dxgi","d3dcompiler_47","opengl32","mfplat","mfuuid","mf","mfreadwrite","ole32","winmm","comctl32","shell32","uuid","comdlg32","advapi32","shlwapi" | ForEach-Object { "-l$_" }
# Object paths are made relative to the build folder: with 430+ objects the full paths would exceed the command-line length limit.
$rel = { param($p) $p.Substring($build.Length + 1) }
$relObjs = @(($objs | ForEach-Object { & $rel $_.obj }) + ($coreObj | ForEach-Object { & $rel $_.obj }))
$linkGame = @("c++", "-target", "x86_64-windows-gnu", "-O2") + $relObjs + @("rr.res") + $libs + @("-Wl,--stack,67108864", "-o", $exe)
Push-Location $build
& $zig @linkGame
$lc = $LASTEXITCODE
Pop-Location
if ($lc) { Fail "Linking the game failed." }

if ($Progress) { Write-Host "@@STEP finish" }
Write-Host "`nDone: $exe" -ForegroundColor Green
# First start: remember the game the player picked so the settings window opens with it filled in.
$ini = Join-Path $out "ridgeracer.ini"
if (-not (Test-Path $ini)) { Set-Content $ini ("game_path=" + $picked) }
if ($CleanBuild) { try { Remove-Item $build -Recurse -Force -ErrorAction Stop } catch { Write-Host "Note: could not delete the work folder $build ($($_.Exception.Message))" } }
if ($Progress) { Write-Host "@@DONE $exe" }
if (-not $NoLaunch) { Start-Process $exe -WorkingDirectory $out }
