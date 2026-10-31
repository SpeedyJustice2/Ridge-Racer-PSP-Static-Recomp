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
  -GenerateOnly  stop after turning the game's code into C++ (used by build-web.ps1)
  -NoPrebuilt  ignore the ready-made parts in .\prebuilt (compile them here instead)
  -NoUnity     compile the game's generated files one by one instead of in batches (slower, same result)
  -PrepareRelease <folder>  for the person who makes Setup.exe: compiles the parts that do not depend on the game and
               writes them (and a warmed-up compiler cache) into <folder>\prebuilt and <folder>\toolchain\zig-cache-warm
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
  [switch]$Progress,
  [switch]$GenerateOnly,
  [switch]$NoPrebuilt,
  [switch]$NoUnity,
  [string]$PrepareRelease = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root
$build = if ($WorkDir) { $WorkDir } else { Join-Path $root "build" }
New-Item -ItemType Directory -Force $build | Out-Null

$script:stepName = ""
$script:stepStart = Get-Date
$script:allStart = Get-Date
function Fail($m) { if ($Progress) { Write-Host "@@ERROR $m" }; Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }
function Step($m, $key = "") {
  $now = Get-Date
  if ($script:stepName) { Write-Host ("   [{0}: {1:N0} s]" -f $script:stepName, ($now - $script:stepStart).TotalSeconds) -ForegroundColor DarkGray }
  $script:stepName = $m; $script:stepStart = $now
  if ($Progress -and $key) { Write-Host "@@STEP $key" }
  Write-Host "`n== $m" -ForegroundColor Cyan
}

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
# The compiler builds its own C++ runtime libraries the first time it is used (minutes on a slow PC).  The release package
# carries a cache with those already built; start from a copy of it.  If anything about it is off the compiler simply rebuilds.
$warmCache = Join-Path $root "toolchain\zig-cache-warm"
if ((Test-Path $warmCache) -and -not $PrepareRelease -and -not (Test-Path $env:ZIG_GLOBAL_CACHE_DIR)) {
  Write-Host "Copying the prepared compiler cache..."
  try { Copy-Item -LiteralPath $warmCache -Destination $env:ZIG_GLOBAL_CACHE_DIR -Recurse -Force }
  catch { Write-Host "(could not copy it; the compiler will build its libraries itself)"; Remove-Item $env:ZIG_GLOBAL_CACHE_DIR -Recurse -Force -ErrorAction SilentlyContinue }
}

# ---- helpers
function Quote($a) { if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a } }
# Runs the compiler for every item that is out of date, $Jobs at a time.  Item = @{src=..; obj=..; args=@(..)}
# With -Soft the sources that failed are returned (instead of stopping the build).
function Invoke-Compiles($items, $label, [switch]$Soft) {
  $todo = @($items | Where-Object { $Rebuild -or -not (Test-Path $_.obj) -or ((Get-Item $_.obj).LastWriteTime -lt (Get-Item $_.src).LastWriteTime) })
  Write-Host ("{0}: {1} to compile, {2} up to date" -f $label, $todo.Count, ($items.Count - $todo.Count))
  $running = New-Object System.Collections.ArrayList
  $done = 0; $failed = @()
  $every = [Math]::Max(1, [int][Math]::Floor($todo.Count / 40))
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
        if (($done % $every) -eq 0 -or $done -eq $todo.Count) { Write-Host ("  {0}/{1}" -f $done, $todo.Count); if ($Progress) { Write-Host ("@@PCT {0} {1}" -f $done, $todo.Count) } }
      }
    }
  }
  if ($Soft) { return ,$failed }
  if ($failed.Count) { Fail ("{0} file(s) failed to compile." -f $failed.Count) }
}
function New-ItemList($files, $objdir, $compileArgs) {
  New-Item -ItemType Directory -Force $objdir | Out-Null
  $files | ForEach-Object { @{ src = $_.FullName; obj = (Join-Path $objdir ($_.BaseName + ".o")); args = $compileArgs } }
}

$inc = @("-I", (Join-Path $root "include"))
$cxx = @("c++", "-target", "x86_64-windows-gnu", "-std=c++20", "-O2", "-g0", "-DNDEBUG", "-D_CRT_SECURE_NO_WARNINGS", "-mavx", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-Wno-everything")
$prof = Join-Path $root "profiles\ridgeracer"
$hostDir = Join-Path $prof "host"
$a3p = Join-Path $prof "third_party\atrac3plus"
$cFlags = @("cc", "-target", "x86_64-windows-gnu", "-std=c11", "-O2", "-g0", "-DNDEBUG", "-mavx", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-Wno-everything")
$libNames = "user32","gdi32","d3d11","dxgi","d3dcompiler_47","opengl32","mfplat","mfuuid","mf","mfreadwrite","ole32","winmm","comctl32","shell32","uuid","comdlg32","advapi32","shlwapi","ws2_32","winhttp","bcrypt"
$hostNames = "main","rr_hle","rr_log","rr_ge","rr_raster","rr_gl","rr_png","rr_atrac","rr_mpeg","rr_pmf","rr_audio","rr_atrac_decoder",
             "rr_video_win32","rr_osk_ui","rr_lobby_ui","rr_pad","rr_glyph_patch","rr_report","rr_update","rr_menusound","rr_utility","rr_settings","rr_iso","rr_net","rr_http","rr_relay","rr_sysinfo","rr_launcher_win32","rr_present_win32","rr_gen_loader"
$atracNames = "atrac","atrac3plus","atrac3plusdsp","atrac3plusdec","rr_compat"

# ---- the parts that do not depend on the game: compiler runtime library, code generator, the game's host code
$coreObjDir = Join-Path $build "obj-core"
$toolObjDir = Join-Path $build "obj-tools"
$hostObjDir = Join-Path $build "obj-host"
$atracObjDir = Join-Path $build "obj-atrac"
$genExe = Join-Path $build "psp_recomp.exe"
$core = Get-ChildItem (Join-Path $root "src") -Filter *.cpp
$coreObj = @(New-ItemList $core $coreObjDir ($cxx + $inc))
$toolObj = @(New-ItemList @(Get-Item (Join-Path $root "tools\codegen_main.cpp")) $toolObjDir ($cxx + $inc + @("-I", (Join-Path $root "tools"))))
# -I of the generated folder comes last; it does not exist yet (and the host files do not need it) while preparing a release
$gen = Join-Path $build "generated"
$gameArgs = $cxx + $inc + @("-I", $hostDir, "-I", $a3p, "-I", $gen)
$hostFiles = $hostNames | ForEach-Object { Get-Item (Join-Path $hostDir ($_ + ".cpp")) }
# The program is built apart from the converted game code (RidgeRacerGame.dll): see profiles\ridgeracer\gen\rr_gen_abi.hpp.
$hostArgs = $gameArgs + @("-DRR_GAME_LIBRARY=1")
$hostObj = @(New-ItemList $hostFiles $hostObjDir $hostArgs)
$cfiles = $atracNames | ForEach-Object { Get-Item (Join-Path $a3p ($_ + ".c")) }
$cArgs = $cFlags + @("-I", (Join-Path $a3p "compat"), "-I", $a3p)
$atracObj = @(New-ItemList $cfiles $atracObjDir $cArgs)

# A fingerprint of everything those parts are made from (sources, headers, compiler, flags), so ready-made objects are only
# used when they match exactly.
function Get-TreeHash {
  $rels = New-Object System.Collections.Generic.List[string]
  foreach ($d in @("include", "src", "tools", "profiles\ridgeracer\host", "profiles\ridgeracer\gen", "profiles\ridgeracer\third_party\atrac3plus")) {
    $p = Join-Path $root $d
    if (-not (Test-Path $p)) { continue }
    foreach ($f in (Get-ChildItem $p -Recurse -File)) {
      $e = $f.Extension.ToLowerInvariant()
      if ($e -eq ".cpp" -or $e -eq ".hpp" -or $e -eq ".h" -or $e -eq ".c" -or $e -eq ".inc" -or $e -eq ".def") { $rels.Add($f.FullName.Substring($root.Length).Replace("/", "\")) }
    }
  }
  $arr = $rels.ToArray()
  [Array]::Sort($arr, [StringComparer]::Ordinal)
  $sb = New-Object System.Text.StringBuilder
  foreach ($r in $arr) { [void]$sb.Append($r).Append(" ").Append((Get-FileHash -LiteralPath ($root + $r) -Algorithm SHA256).Hash).Append("`n") }
  $zv = (& $zig version 2>&1 | Out-String).Trim()
  [void]$sb.Append($zv).Append("`n").Append(($cxx -join " ")).Append("`n").Append(($cFlags -join " ")).Append("`n").Append(($libNames -join " ")).Append("`n")
  $sha = [System.Security.Cryptography.SHA256]::Create()
  $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($sb.ToString()))
  return (($bytes | ForEach-Object { $_.ToString("x2") }) -join "")
}

function Build-Generator {
  Invoke-Compiles $coreObj "core library"
  Invoke-Compiles $toolObj "code generator"
  $linkGen = @("c++", "-target", "x86_64-windows-gnu") + (($coreObj + $toolObj) | ForEach-Object { $_.obj }) + @("-o", $genExe)
  if ($Rebuild -or -not (Test-Path $genExe)) { & $zig @linkGen; if ($LASTEXITCODE) { Fail "Linking the code generator failed." } }
}

# ---- release preparation (run by make-release.cmd): everything that does not need the game, plus a warm compiler cache
if ($PrepareRelease) {
  Step "Preparing a release: building the parts that do not depend on the game" "compile"
  $outRoot = $PrepareRelease
  $pre = Join-Path $outRoot "prebuilt"
  if (Test-Path $pre) { Remove-Item $pre -Recurse -Force }
  New-Item -ItemType Directory -Force $pre | Out-Null
  Build-Generator
  Invoke-Compiles $hostObj "host code"
  Invoke-Compiles $atracObj "audio decoder"
  foreach ($pair in @(@($coreObjDir, "obj-core"), @($toolObjDir, "obj-tools"), @($hostObjDir, "obj-host"), @($atracObjDir, "obj-atrac"))) {
    Copy-Item -LiteralPath $pair[0] -Destination (Join-Path $pre $pair[1]) -Recurse -Force
  }
  Copy-Item -LiteralPath $genExe -Destination (Join-Path $pre "psp_recomp.exe") -Force
  # The program itself (everything except the converted game code, which becomes RidgeRacerGame.dll on the player's PC).  It is
  # shipped inside Setup.exe and, with SDL2.dll, as the small update package.
  $resP = Join-Path $build "rr.res"
  Push-Location $hostDir; & $zig rc "rr.rc" $resP; $rcP = $LASTEXITCODE; Pop-Location
  if ($rcP) { Fail "Building resources failed." }
  $relP = { param($p) $p.Substring($build.Length + 1) }
  $objsP = @(($hostObj + $atracObj + $coreObj) | ForEach-Object { & $relP $_.obj })
  $libsP = $libNames | ForEach-Object { "-l$_" }
  Push-Location $build
  & $zig @(@("c++", "-target", "x86_64-windows-gnu", "-O2") + $objsP + @("rr.res") + $libsP + @("-Wl,--stack,67108864", "-o", (Join-Path $pre "RidgeRacerNative.exe")))
  $lcP = $LASTEXITCODE
  Pop-Location
  if ($lcP) { Fail "Linking the program failed." }
  Set-Content -LiteralPath (Join-Path $pre "manifest.txt") (Get-TreeHash) -Encoding ASCII
  # Warm cache: use the compiler the way the installer will (same target and CPU flags, same libraries), on tiny programs.
  Step "Preparing the compiler cache" "compile"
  $warm = Join-Path $outRoot "toolchain\zig-cache-warm"
  if (Test-Path $warm) { Remove-Item $warm -Recurse -Force }
  New-Item -ItemType Directory -Force (Split-Path $warm) | Out-Null
  $env:ZIG_GLOBAL_CACHE_DIR = $warm
  $env:ZIG_LOCAL_CACHE_DIR = Join-Path $build "zig-cache-local-warm"
  $wdir = Join-Path $build "warm"
  New-Item -ItemType Directory -Force $wdir | Out-Null
  $wcpp = Join-Path $wdir "warm.cpp"
  Set-Content -LiteralPath $wcpp -Encoding ASCII -Value @(
    "#include <string>", "#include <vector>", "#include <map>", "#include <thread>", "#include <mutex>", "#include <atomic>", "#include <filesystem>",
    "#include <fstream>", "#include <sstream>", "#include <iostream>", "#include <algorithm>", "#include <chrono>", "#include <functional>",
    "#include <windows.h>",
    "int main(int argc, char **argv) { std::vector<std::string> v; std::map<int, std::string> m; std::atomic<int> a{0}; std::mutex mu;",
    "  std::thread t([&] { std::lock_guard<std::mutex> g(mu); a++; }); t.join(); std::ostringstream o; o << argc << std::filesystem::current_path().string();",
    "  v.push_back(o.str()); m[1] = v[0]; std::ifstream in(argv[0]); std::sort(v.begin(), v.end());",
    "  return static_cast<int>(m.size() + v.size() + a.load()) + (in ? 0 : 1) + static_cast<int>(GetTickCount() & 1); }")
  $wc = Join-Path $wdir "warm.c"
  Set-Content -LiteralPath $wc -Encoding ASCII -Value @("#include <math.h>", "#include <string.h>", "int main(int c, char **v) { return (int)sqrt((double)c) + (int)strlen(v[0]); }")
  $wexe = Join-Path $wdir "warm.exe"
  $libs = $libNames | ForEach-Object { "-l$_" }
  & $zig @cxx $wcpp @libs "-Wl,--stack,67108864" -o $wexe
  if ($LASTEXITCODE) { Fail "Preparing the compiler cache failed (C++)." }
  & $zig @cFlags -I (Join-Path $a3p "compat") -c $wc -o (Join-Path $wdir "warm_c.o")
  if ($LASTEXITCODE) { Fail "Preparing the compiler cache failed (C)." }
  & $zig c++ -target x86_64-windows-gnu -O2 (Join-Path $wdir "warm_c.o") -o (Join-Path $wdir "warm_c.exe")
  Write-Host "Prepared: $pre and $warm"
  Write-Host "`nDone." -ForegroundColor Green
  exit 0
}

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
  $toolExe = Join-Path $build "rr_iso_tool.exe"
  if (-not (Test-Path $toolExe)) {
    Write-Host "Building the disc image helper..."
    & $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -g0 -mavx -Wno-everything -I $hostDir (Join-Path $root "tools\rr_iso_tool.cpp") (Join-Path $hostDir "rr_iso.cpp") -o $toolExe
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

# ---- code generator (ready-made when the release package carries it and it matches this source exactly)
Step "Building the code generator" "codegen"
$usedPrebuilt = $false
$preDir = Join-Path $root "prebuilt"
if (-not $NoPrebuilt -and -not $Rebuild -and (Test-Path (Join-Path $preDir "manifest.txt")) -and (Test-Path (Join-Path $preDir "psp_recomp.exe"))) {
  try {
    $want = (Get-Content (Join-Path $preDir "manifest.txt") -Raw).Trim()
    $have = Get-TreeHash
    if ($want -eq $have) {
      Write-Host "Using the ready-made tools and game-independent code from the package."
      $now = Get-Date
      foreach ($pair in @(@("obj-core", $coreObjDir), @("obj-tools", $toolObjDir), @("obj-host", $hostObjDir), @("obj-atrac", $atracObjDir))) {
        New-Item -ItemType Directory -Force $pair[1] | Out-Null
        foreach ($f in (Get-ChildItem (Join-Path $preDir $pair[0]) -Filter *.o)) {
          $dest = Join-Path $pair[1] $f.Name
          Copy-Item -LiteralPath $f.FullName -Destination $dest -Force
          (Get-Item -LiteralPath $dest).LastWriteTime = $now.AddSeconds(2)   # newer than its source: counts as up to date
        }
      }
      Copy-Item -LiteralPath (Join-Path $preDir "psp_recomp.exe") -Destination $genExe -Force
      $usedPrebuilt = $true
    } else { Write-Host "The ready-made parts do not match this source; building them here." }
  } catch { Write-Host ("(could not use the ready-made parts: " + $_.Exception.Message + "); building them here.") }
}
Build-Generator

# ---- generate
Step "Generating code from your game" "generate"
$stamp = Join-Path $gen ".source-sha256"
$have = (Test-Path (Join-Path $gen "generated_registry.cpp")) -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $hash)
if ($have) { Write-Host "Already generated for this BOOT.BIN." }
else {
  if (Test-Path $gen) { Remove-Item $gen -Recurse -Force }
  Remove-Item (Join-Path $build "obj-gen") -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $build "obj-unity") -Recurse -Force -ErrorAction SilentlyContinue
  & $genExe $bin --auto $gen --jobs 0 --builtin-accessors --function-gates 0x08804000 4096 | Out-Host
  if ($LASTEXITCODE) { Fail "Code generation failed." }
  if (-not (Test-Path (Join-Path $gen "generated_registry.cpp"))) { Fail "Generation finished but generated_registry.cpp is missing." }
  Set-Content $stamp $hash
}

if ($GenerateOnly) { Write-Host "Generated: $gen"; exit 0 }

# ---- the game
Step "Compiling the game (this is the long step the first time)" "compile"
$objs = @()
$objs += $hostObj
$objs += $atracObj
# The generated files are compiled several at a time as one unit ("batches"): every compiler start otherwise re-reads the same
# large headers, which was a third of the time.  If a batch ever fails, its files are compiled one by one instead.
$genFiles = @(Get-ChildItem $gen -Filter *.cpp | Sort-Object Name)
$objGen = Join-Path $build "obj-gen"
$unityDir = Join-Path $build "unity"
$unityObjDir = Join-Path $build "obj-unity"
$genItems = @()
if ($NoUnity) {
  $genItems = @(New-ItemList $genFiles $objGen $gameArgs)
  $genItems += @(New-ItemList @(Get-Item (Join-Path $prof "gen\rr_gen_shim.cpp")) $objGen $gameArgs)
  $objs += $genItems
  Invoke-Compiles ($hostObj + $atracObj + $genItems) "game"
} else {
  New-Item -ItemType Directory -Force $unityDir | Out-Null
  New-Item -ItemType Directory -Force $unityObjDir | Out-Null
  $units = @($genFiles | Where-Object { $_.Name -like "generated_unit_*.cpp" })
  $others = @($genFiles | Where-Object { $_.Name -notlike "generated_unit_*.cpp" })
  $batches = New-Object System.Collections.ArrayList
  $cur = New-Object System.Collections.ArrayList
  $curBytes = 0
  foreach ($u in $units) {
    if ($cur.Count -ge 6 -or ($cur.Count -gt 0 -and ($curBytes + $u.Length) -gt 700000)) { [void]$batches.Add($cur.ToArray()); $cur = New-Object System.Collections.ArrayList; $curBytes = 0 }
    [void]$cur.Add($u); $curBytes += $u.Length
  }
  if ($cur.Count -gt 0) { [void]$batches.Add($cur.ToArray()) }
  $bi = 0
  $batchItems = @()
  $batchMembers = @{}
  foreach ($b in $batches) {
    $name = "unity_{0:D3}" -f $bi; $bi++
    $wrapper = Join-Path $unityDir ($name + ".cpp")
    $text = (($b | ForEach-Object { '#include "' + $_.Name + '"' }) -join "`n") + "`n"
    if (-not (Test-Path $wrapper) -or ((Get-Content $wrapper -Raw) -ne $text)) { [System.IO.File]::WriteAllText($wrapper, $text) }
    # a batch is rebuilt when any of its files is newer than the batch object
    $newest = ($b | ForEach-Object { $_.LastWriteTime } | Measure-Object -Maximum).Maximum
    $objPath = Join-Path $unityObjDir ($name + ".o")
    if ((Test-Path $objPath) -and ((Get-Item $objPath).LastWriteTime -lt $newest)) { Remove-Item $objPath -Force }
    $batchItems += @{ src = $wrapper; obj = $objPath; args = $gameArgs }
    $batchMembers[$wrapper] = $b
  }
  $otherItems = @(New-ItemList $others $objGen $gameArgs)
  $otherItems += @(New-ItemList @(Get-Item (Join-Path $prof "gen\rr_gen_shim.cpp")) $objGen $gameArgs)
  # big files first so the long ones do not end up last
  $all = @($hostObj + $atracObj + $otherItems + $batchItems)
  $failedSrc = Invoke-Compiles $all "game" -Soft
  if ($failedSrc.Count -gt 0) {
    Write-Host ("{0} batch(es) did not compile as a batch; compiling their files one by one..." -f $failedSrc.Count) -ForegroundColor Yellow
    $retry = @()
    foreach ($fs in $failedSrc) {
      if ($batchMembers.ContainsKey($fs)) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($fs)
        Remove-Item (Join-Path $unityObjDir ($name + ".o")) -Force -ErrorAction SilentlyContinue
        $retry += @(New-ItemList $batchMembers[$fs] $objGen $gameArgs)
        $batchItems = @($batchItems | Where-Object { $_.src -ne $fs })
      } else { Fail ("{0} failed to compile." -f $fs) }
    }
    Invoke-Compiles $retry "game (single files)"
    $batchItems += $retry
  }
  $objs += $otherItems
  $objs += $batchItems
}

# ---- resources (visual styles for the settings window)
$res = Join-Path $build "rr.res"
if ($Rebuild -or -not (Test-Path $res)) { Push-Location $hostDir; & $zig rc "rr.rc" $res; $rc = $LASTEXITCODE; Pop-Location; if ($rc) { Fail "Building resources failed." } }

# ---- link
Step "Linking" "link"
$out = if ($InstallDir) { $InstallDir } else { Join-Path $root "RidgeRacer-Beta" }
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out "RidgeRacerNative.exe"
$libs = $libNames | ForEach-Object { "-l$_" }
# Object paths are made relative to the build folder: with 430+ objects the full paths would exceed the command-line length limit.
$rel = { param($p) $p.Substring($build.Length + 1) }
# 1) the program: this project's own code only (no converted game code)
$progObjs = @(($hostObj + $atracObj) | ForEach-Object { & $rel $_.obj }) + @($coreObj | ForEach-Object { & $rel $_.obj })
$linkGame = @("c++", "-target", "x86_64-windows-gnu", "-O2") + $progObjs + @("rr.res") + $libs + @("-Wl,--stack,67108864", "-o", $exe)
Push-Location $build
& $zig @linkGame
$lc = $LASTEXITCODE
Pop-Location
if ($lc) { Fail "Linking the game failed." }
# 2) the game library: the code converted from your game, built here and kept here
Step "Linking the game library" "link"
$dll = Join-Path $out "RidgeRacerGame.dll"
$libObjs = @($objs | Where-Object { $hostObj.obj -notcontains $_.obj -and $atracObj.obj -notcontains $_.obj } | ForEach-Object { & $rel $_.obj })
$linkLib = @("c++", "-target", "x86_64-windows-gnu", "-O2", "-shared") + $libObjs + @("-o", $dll)
Push-Location $build
& $zig @linkLib
$lc2 = $LASTEXITCODE
Pop-Location
if ($lc2) { Fail "Linking the game library failed." }
Remove-Item (Join-Path $out "RidgeRacerGame.lib") -Force -ErrorAction SilentlyContinue
# Controller support for PlayStation / Switch / other pads (loaded at run time; without it only XInput pads work).
$sdlDll = Join-Path $root "profiles\ridgeracer\third_party\sdl2\SDL2.dll"
if (Test-Path $sdlDll) {
  try { Copy-Item -LiteralPath $sdlDll -Destination (Join-Path $out "SDL2.dll") -Force } catch { Write-Host "Note: could not copy SDL2.dll ($($_.Exception.Message)); only Xbox-style controllers will work." }
}

Step "Finishing" "finish"
Write-Host ("Total build time: {0:N0} s{1}" -f ((Get-Date) - $script:allStart).TotalSeconds, $(if ($usedPrebuilt) { " (with the ready-made parts)" } else { "" }))
Write-Host "`nDone: $exe" -ForegroundColor Green
# First start: remember the game the player picked so the settings window opens with it filled in.
$ini = Join-Path $out "ridgeracer.ini"
if (-not (Test-Path $ini)) { Set-Content $ini ("game_path=" + $picked) }
if ($CleanBuild) { try { Remove-Item $build -Recurse -Force -ErrorAction Stop } catch { Write-Host "Note: could not delete the work folder $build ($($_.Exception.Message))" } }
if ($Progress) { Write-Host "@@DONE $exe" }
if (-not $NoLaunch) { Start-Process $exe -WorkingDirectory $out }
