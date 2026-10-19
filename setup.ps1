<#
.SYNOPSIS
  Builds the Ridge Racer PSP PC port from your own copy of the game.

.EXAMPLE
  .\setup.ps1 -Game "D:\RidgeRacer\PSP_GAME\SYSDIR\BOOT.BIN"

.NOTES
  Needs: Visual Studio 2022 (Desktop development with C++) and CMake 3.20+.
  -Game      your decrypted BOOT.BIN (an ELF file), or the folder that contains PSP_GAME
  The first build takes a long time (the game is turned into several hundred C++ files and compiled).
  Nothing from the game is ever copied into this repository; the generated code stays on your PC.
#>
param(
  [Parameter(Mandatory = $true)][string]$Game,
  [int]$Jobs = 0,
  [switch]$Regenerate
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
Set-Location $root

function Fail($m) { Write-Host "`nERROR: $m" -ForegroundColor Red; exit 1 }
function Step($m) { Write-Host "`n== $m" -ForegroundColor Cyan }

# ---- tools
Step "Checking tools"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail "CMake not found. Install it from https://cmake.org/download/ (tick 'add to PATH')." }
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "Visual Studio 2022 not found. Install the 'Desktop development with C++' workload (the free Community edition works)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail "Visual Studio is installed but has no C++ tools. Add the 'Desktop development with C++' workload." }
Write-Host "Visual Studio: $vs"

# ---- game file
Step "Checking the game file"
$bin = $Game
if (Test-Path $bin -PathType Container) { $bin = Join-Path $bin "PSP_GAME\SYSDIR\BOOT.BIN" }
if (-not (Test-Path $bin -PathType Leaf)) { Fail "BOOT.BIN not found at: $bin" }
$bin = (Resolve-Path $bin).Path
$magic = [System.IO.File]::ReadAllBytes($bin)[0..3]
if ($magic[0] -eq 0x7E -and $magic[1] -eq 0x50 -and $magic[2] -eq 0x53 -and $magic[3] -eq 0x50) {
  Fail "That BOOT.BIN is still encrypted ('~PSP'). Use a decrypted copy (an ELF file starting with 'ELF'), e.g. from a decrypted dump of your own disc."
}
if (-not ($magic[0] -eq 0x7F -and $magic[1] -eq 0x45 -and $magic[2] -eq 0x4C -and $magic[3] -eq 0x46)) {
  Fail "That file is not an ELF executable. Expected a decrypted BOOT.BIN."
}
$hash = (Get-FileHash $bin -Algorithm SHA256).Hash
Write-Host "BOOT.BIN ok ($hash)"

# ---- psp_recomp (the code generator)
Step "Building the code generator"
cmake -S . -B out/tools -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF | Out-Host
if ($LASTEXITCODE) { Fail "CMake configure failed." }
cmake --build out/tools --config Release --target psp_recomp | Out-Host
if ($LASTEXITCODE) { Fail "Building psp_recomp failed." }
$recomp = Get-ChildItem out/tools -Recurse -Filter psp_recomp.exe | Select-Object -First 1
if (-not $recomp) { Fail "psp_recomp.exe not found after the build." }

# ---- generate
$gen = Join-Path $root "generated"
$stamp = Join-Path $gen ".source-sha256"
$have = (Test-Path (Join-Path $gen "generated_registry.cpp")) -and (Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $hash)
if ($have -and -not $Regenerate) {
  Step "Generated code already matches this BOOT.BIN (use -Regenerate to redo it)"
} else {
  Step "Generating code from your game (a few minutes)"
  if (Test-Path $gen) { Remove-Item $gen -Recurse -Force }
  & $recomp.FullName $bin --auto $gen --jobs $Jobs --builtin-accessors --function-gates 0x08804000 4096 | Out-Host
  if ($LASTEXITCODE) { Fail "Code generation failed." }
  if (-not (Test-Path (Join-Path $gen "generated_registry.cpp"))) { Fail "Generation finished but generated_registry.cpp is missing." }
  Set-Content $stamp $hash
}

# ---- build the port
Step "Building the game (first time: 20+ minutes, uses all CPU cores)"
$cfg = @("-S", ".", "-B", "out/ridgeracer", "-DPSPRECOMP_PROFILE=ridgeracer",
         "-DPSPRECOMP_RIDGERACER_GENERATED_DIR=$gen",
         "-DPSPRECOMP_BUILD_TESTS=OFF", "-DPSPRECOMP_BUILD_PROFILE_TESTS=OFF")
cmake @cfg | Out-Host
if ($LASTEXITCODE) { Fail "CMake configure for the game failed." }
cmake --build out/ridgeracer --config Release --target RidgeRacerNative | Out-Host
if ($LASTEXITCODE) { Fail "Building the game failed." }

# ---- package
Step "Collecting the finished game"
$bindir = Join-Path $root "out/ridgeracer/bin/Release"
if (-not (Test-Path (Join-Path $bindir "RidgeRacerNative.exe"))) { Fail "RidgeRacerNative.exe not found in $bindir" }
$dest = Join-Path $root "RidgeRacer-Beta"
New-Item -ItemType Directory -Force $dest | Out-Null
Get-ChildItem $bindir -File | Where-Object { $_.Extension -in ".exe", ".dll" } | Copy-Item -Destination $dest -Force
Write-Host "`nDone. Start the game with:" -ForegroundColor Green
Write-Host "  $dest\RidgeRacerNative.exe"
Write-Host "The settings window opens on the first start; pick your game folder there."
