<#
.SYNOPSIS
  Builds the browser version of the Ridge Racer PSP port from your own copy of the game (for use on your own computer).

.EXAMPLE
  .\build-web.cmd -Game "D:\RidgeRacer\Ridge Racer.iso"
  .\web-dist\serve.cmd        (then open the page it shows, pick your disc image, press Start)

.NOTES
  - Needs git (to fetch the Emscripten compiler into .\toolchain\emsdk the first time) and the normal build tools
    (.\toolchain\zig, used to turn your game's code into C++).  Nothing from the game is uploaded anywhere.
  - The result in .\web-dist contains your game's code translated to WebAssembly.  It is for YOUR use only:
    do not publish or share the web-dist folder (that would distribute the game).
  -Game     your disc image (.iso), your decrypted BOOT.BIN, or the folder that contains PSP_GAME
  -Jobs     parallel compiler processes (default: number of CPU cores)
  -Rebuild  recompile everything
#>
param(
  [string]$Game = "",
  [int]$Jobs = 0,
  [switch]$Rebuild
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root
$build = Join-Path $root "build-web"
$dist = Join-Path $root "web-dist"
New-Item -ItemType Directory -Force $build | Out-Null
function Fail($m) { Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }
function Step($m) { Write-Host "`n== $m" -ForegroundColor Cyan }
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

# ---- Emscripten compiler
Step "Looking for the Emscripten compiler"
$emsdk = Join-Path $root "toolchain\emsdk"
$emVersion = "3.1.74"
if (-not (Test-Path (Join-Path $emsdk "emsdk.bat"))) {
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed once to download the Emscripten compiler (https://git-scm.com). Install it and run this again." }
  New-Item -ItemType Directory -Force (Split-Path $emsdk) | Out-Null
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git $emsdk
  if ($LASTEXITCODE) { Fail "Could not download the Emscripten SDK." }
}
$marker = Join-Path $emsdk ".rr-installed-$emVersion"
if (-not (Test-Path $marker)) {
  Write-Host "Installing Emscripten $emVersion (a few hundred MB, once)..."
  Push-Location $emsdk
  # The Emscripten installer itself needs Python on the PATH (the Microsoft Store shortcut does not count).
  $pyOk = $false
  try { $v = & python --version 2>&1; $pyOk = ($LASTEXITCODE -eq 0 -and "$v" -match "^Python 3") } catch {}
  if (-not $pyOk) {
    Pop-Location
    Fail "Python 3 is needed once to install Emscripten. Run:  winget install -e --id Python.Python.3.12   then open a NEW terminal window and run this again."
  }
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $emsdk "emsdk.ps1") install $emVersion
  if ($LASTEXITCODE) { Pop-Location; Fail "Installing Emscripten failed." }
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $emsdk "emsdk.ps1") activate $emVersion
  if ($LASTEXITCODE) { Pop-Location; Fail "Activating Emscripten failed." }
  Pop-Location
  Set-Content $marker "ok"
}
. (Join-Path $emsdk "emsdk_env.ps1") | Out-Null
$emcc = (Get-Command emcc.bat -ErrorAction SilentlyContinue).Source
if (-not $emcc) { Fail "emcc.bat not found after setting up Emscripten." }
Write-Host "Compiler: $emcc"

# ---- generate the game's C++ (same as the Windows build)
Step "Generating code from your game"
$gen = Join-Path $build "generated"
$genWork = Join-Path $build "gen-work"
& (Join-Path $root "build.ps1") -Game $Game -WorkDir $genWork -GenerateOnly -NoLaunch -Jobs $Jobs
if ($LASTEXITCODE) { Fail "Generating the game's code failed." }
$gen = Join-Path $genWork "generated"
if (-not (Test-Path (Join-Path $gen "generated_registry.cpp"))) { Fail "Generated code not found in $gen." }

# ---- compile
function Quote($a) { if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a } }
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
      $psi.FileName = $emcc
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
        if (($done % 10) -eq 0 -or $done -eq $todo.Count) { Write-Host ("  {0}/{1}" -f $done, $todo.Count) }
      }
    }
  }
  if ($failed.Count) { Fail ("{0} file(s) failed to compile." -f $failed.Count) }
}
function New-ItemList($files, $objdir, $compileArgs) {
  New-Item -ItemType Directory -Force $objdir | Out-Null
  $files | ForEach-Object { @{ src = $_.FullName; obj = (Join-Path $objdir ($_.BaseName + ".o")); args = $compileArgs } }
}

Step "Compiling (first time: 20-40 minutes)"
$prof = Join-Path $root "profiles\ridgeracer"
$hostDir = Join-Path $prof "host"
$a3p = Join-Path $prof "third_party\atrac3plus"
$common = @("-O2", "-pthread", "-fwasm-exceptions", "-msimd128", "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-DNDEBUG", "-w")
$cxx = @("-std=c++20") + $common + @("-I", (Join-Path $root "include"))

# Warm-up: lets Emscripten build its system headers/cache once, before the parallel compiles start.
$warm = Join-Path $build "warm.cpp"
Set-Content $warm "#include <thread>`n#include <mutex>`nint main(){return 0;}"
& $emcc @($cxx) -c $warm -o (Join-Path $build "warm.o")
if ($LASTEXITCODE) { Fail "Emscripten test compile failed." }

$objs = @()
$core = Get-ChildItem (Join-Path $root "src") -Filter *.cpp
$objs += New-ItemList $core (Join-Path $build "obj-core") $cxx
$hostNames = "main","rr_hle","rr_log","rr_ge","rr_raster","rr_gl","rr_png","rr_atrac","rr_mpeg","rr_pmf","rr_audio","rr_atrac_decoder",
             "rr_osk_ui","rr_lobby_ui","rr_pad","rr_glyph_patch","rr_report","rr_menusound","rr_utility","rr_settings","rr_iso","rr_net","rr_http","rr_relay","rr_sysinfo","rr_update","rr_gen_loader","rr_launcher_null","rr_present_web"
$hostFiles = $hostNames | ForEach-Object { Get-Item (Join-Path $hostDir ($_ + ".cpp")) }
$gameArgs = $cxx + @("-I", $hostDir, "-I", $a3p, "-I", $gen)
$objs += New-ItemList $hostFiles (Join-Path $build "obj-host") $gameArgs
# Movies: FFmpeg's H.264 decoder (third_party\ffmpeg-h264, LGPL) behind host\rr_video_ffmpeg.cpp
$ff = Join-Path $prof "third_party\ffmpeg-h264"
$objs += New-ItemList @(Get-Item (Join-Path $hostDir "rr_video_ffmpeg.cpp")) (Join-Path $build "obj-host") ($gameArgs + @("-I", $ff))
$ffSources = Get-Content (Join-Path $ff "sources.txt") | Where-Object { $_.Trim() -ne "" }
$ffCommon = @("-std=c17", "-O2", "-pthread", "-msimd128", "-w", "-fno-math-errno", "-fno-signed-zeros", "-D_ISOC11_SOURCE", "-D_FILE_OFFSET_BITS=64",
              "-D_LARGEFILE_SOURCE", "-D_POSIX_C_SOURCE=200112", "-D_XOPEN_SOURCE=600", "-DHAVE_AV_CONFIG_H", "-Dff_vlc_init_from_lengths=ffh264_vlc_init_from_lengths", "-I", $ff)
foreach ($lib in "libavcodec", "libavutil") {
  $files = $ffSources | Where-Object { $_.StartsWith($lib + "/") } | ForEach-Object { Get-Item (Join-Path $ff ($_ -replace "/", "\")) }
  $objs += New-ItemList $files (Join-Path $build ("obj-ff-" + $lib)) ($ffCommon + @("-DBUILDING_" + $lib.Substring(3)))
}
$cfiles = "atrac","atrac3plus","atrac3plusdsp","atrac3plusdec","rr_compat" | ForEach-Object { Get-Item (Join-Path $a3p ($_ + ".c")) }
$cArgs = @("-std=c11") + $common + @("-I", (Join-Path $a3p "compat"), "-I", $a3p)
$objs += New-ItemList $cfiles (Join-Path $build "obj-atrac") $cArgs
$genFiles = Get-ChildItem $gen -Filter *.cpp
$objs += New-ItemList $genFiles (Join-Path $build "obj-gen") $gameArgs
Invoke-Compiles $objs "game"

# ---- link
Step "Linking (a few minutes)"
New-Item -ItemType Directory -Force $dist | Out-Null
$rsp = Join-Path $build "objects.rsp"
Set-Content $rsp (($objs | ForEach-Object { '"' + ($_.obj -replace '\\', '/') + '"' }) -join "`n")
$js = Join-Path $dist "RidgeRacer.js"
# Link-time -O1 on purpose: at -O2 Binaryen inlines the 6500-entry function registry into main() (one 9.9 MB function, over the browser limit).
$linkArgs = @("@$rsp", "-O1", "-pthread", "-fwasm-exceptions", "-o", $js,
  "-sWASM_BIGINT=1", "-sMODULARIZE=1", "-sEXPORT_NAME=RRModule", "-sENVIRONMENT=worker",
  "-sINITIAL_MEMORY=536870912", "-sALLOW_MEMORY_GROWTH=1", "-sMAXIMUM_MEMORY=2147483648", "-sSTACK_SIZE=67108864",
  "-sMIN_WEBGL_VERSION=2", "-sMAX_WEBGL_VERSION=2", "-sFULL_ES3=1", "-sGL_ENABLE_GET_PROC_ADDRESS=1",
  "--profiling-funcs", "-sFILESYSTEM=1", "-lworkerfs.js", "-sEXPORTED_RUNTIME_METHODS=FS,GL,UTF8ToString,ENV", "-sEXIT_RUNTIME=1", "-sJSPI=1", "-sASSERTIONS=0")
& $emcc @linkArgs
if ($LASTEXITCODE) { Fail "Linking failed." }

Copy-Item (Join-Path $root "web\*") $dist -Force
Write-Host "`nDone. Start it with (paste into PowerShell):  & `"$dist\serve.cmd`"" -ForegroundColor Green
Write-Host "Keep web-dist on your own computer: it contains your game's code and must not be shared."
