<#
  Ridge Racer PSP - PC port: setup window.
  Asks for your own copy of the game and an install folder, then builds the game in the background (it uses the compiler
  in .\toolchain\zig, see build.ps1) and installs it.  Nothing from the game is uploaded or shared.
#>
$ErrorActionPreference = "Stop"
# Any unexpected error is shown in a message box (the window runs without a console, so it would otherwise vanish silently).
trap {
  try { Add-Type -AssemblyName System.Windows.Forms } catch { }
  $msg = ($_ | Out-String)
  try { [System.IO.File]::WriteAllText((Join-Path ([System.IO.Path]::GetTempPath()) "rr-setup-error.txt"), $msg) } catch { }
  [void][System.Windows.Forms.MessageBox]::Show("Setup hit an unexpected error:`r`n`r`n" + $msg, "Ridge Racer setup", "OK", "Error")
  exit 1
}
$kit = $PSScriptRoot
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class RrDpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[void][RrDpi]::SetProcessDPIAware()
[System.Windows.Forms.Application]::EnableVisualStyles()

function Show-Message($text, $icon = "Information") {
  [void][System.Windows.Forms.MessageBox]::Show($text, "Ridge Racer setup", "OK", $icon)
}

# ---- is everything we need here?
$zigOk = (Test-Path (Join-Path $kit "toolchain\zig\zig.exe")) -or ($null -ne (Get-Command zig -ErrorAction SilentlyContinue))
if (-not $zigOk -or -not (Test-Path (Join-Path $kit "build.ps1"))) {
  Show-Message "Setup files are missing. Please extract the whole download (including the 'toolchain' folder) and run Install again." "Error"
  exit 1
}

# ---- state
$script:gamePath = ""
$script:isIso = $false
$script:isoGB = 0.0
$script:installDir = ""
$script:proc = $null
$script:logPath = ""
$script:logPos = 0L
$script:stage = ""
$script:stageFrac = 0.0
$script:fileDone = 0
$script:fileTotal = 0
$script:sawDone = $false
$script:lastError = ""
$script:phase = "idle"       # idle | build | prepare | finished | failed
$script:started = Get-Date
$script:page = 0

# ---- helpers
function Get-FreeGB($path) {
  try {
    $root = [System.IO.Path]::GetPathRoot($path)
    if (-not $root) { return $null }
    $di = New-Object System.IO.DriveInfo($root)
    return [math]::Round($di.AvailableFreeSpace / 1GB, 1)
  } catch { return $null }
}

# Returns @{ ok=$true/$false; text=...; iso=$true/$false; isoGB=... }
function Test-GameChoice($p) {
  $r = @{ ok = $false; text = ""; iso = $false; isoGB = 0.0 }
  if (-not $p) { $r.text = "Choose your Ridge Racer disc image (.iso) or the folder that contains PSP_GAME."; return $r }
  if (Test-Path -LiteralPath $p -PathType Leaf) {
    if ($p -match '\.iso$') {
      $len = (Get-Item -LiteralPath $p).Length
      $r.ok = $true; $r.iso = $true; $r.isoGB = [math]::Round($len / 1GB, 2)
      $r.text = "Disc image found ({0:N0} MB)." -f ($len / 1MB)
      return $r
    }
    $boot = $p
  } elseif (Test-Path -LiteralPath $p -PathType Container) {
    $boot = $null
    foreach ($rel in @("PSP_GAME\SYSDIR\BOOT.BIN", "SYSDIR\BOOT.BIN", "BOOT.BIN")) {
      $c = Join-Path $p $rel
      if (Test-Path -LiteralPath $c -PathType Leaf) { $boot = $c; break }
    }
    if (-not $boot) { $r.text = "That folder has no PSP_GAME\SYSDIR\BOOT.BIN. Pick the folder that contains PSP_GAME."; return $r }
  } else { $r.text = "That file or folder does not exist."; return $r }
  try {
    $fs = [System.IO.File]::OpenRead($boot); $b = New-Object byte[] 4; [void]$fs.Read($b, 0, 4); $fs.Close()
  } catch { $r.text = "BOOT.BIN could not be read."; return $r }
  if ($b[0] -eq 0x7E -and $b[1] -eq 0x50 -and $b[2] -eq 0x53 -and $b[3] -eq 0x50) {
    $r.text = "This game is still encrypted. A decrypted copy of your own game is needed (see the README)."; return $r
  }
  if (-not ($b[0] -eq 0x7F -and $b[1] -eq 0x45 -and $b[2] -eq 0x4C -and $b[3] -eq 0x46)) { $r.text = "BOOT.BIN is not a PSP program."; return $r }
  $r.ok = $true; $r.text = "Game files found."
  return $r
}

function Test-InstallDir($d) {
  $r = @{ ok = $false; text = "" }
  if (-not $d) { $r.text = "Choose where to install the game."; return $r }
  if (-not [System.IO.Path]::IsPathRooted($d)) { $r.text = "Please enter a full path such as D:\Games\Ridge Racer."; return $r }
  if ($d.TrimEnd('\') -eq [System.IO.Path]::GetPathRoot($d).TrimEnd('\')) { $r.text = "Please use a folder, not the root of a drive."; return $r }
  if ($d.StartsWith($kit.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { $r.text = "Please choose a folder outside the setup folder."; return $r }
  if (Test-Path -LiteralPath $d -PathType Leaf) { $r.text = "That path is a file."; return $r }
  if (Test-Path -LiteralPath $d -PathType Container) {
    $items = @(Get-ChildItem -LiteralPath $d -Force -ErrorAction SilentlyContinue)
    $ours = Test-Path -LiteralPath (Join-Path $d "RidgeRacerNative.exe")
    if ($items.Count -gt 0 -and -not $ours) { $r.text = "That folder is not empty. Choose an empty or new folder."; return $r }
  }
  $need = 2.5
  if ($script:isIso) { $need += $script:isoGB }
  $free = Get-FreeGB $d
  if ($null -ne $free) {
    if ($free -lt $need) { $r.text = "Not enough free space on that drive: {0} GB free, about {1:N1} GB needed." -f $free, $need; return $r }
    $r.text = "{0} GB free on that drive (about {1:N1} GB needed while installing, less afterwards)." -f $free, $need
  } else { $r.text = "Folder looks fine." }
  $r.ok = $true
  return $r
}

# ---- window
$form = New-Object System.Windows.Forms.Form
$form.Text = "Ridge Racer PSP - PC port setup"
$form.ClientSize = New-Object System.Drawing.Size(680, 470)
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)

$header = New-Object System.Windows.Forms.Panel
$header.Location = New-Object System.Drawing.Point(0, 0); $header.Size = New-Object System.Drawing.Size(680, 74)
$header.BackColor = [System.Drawing.Color]::FromArgb(24, 32, 56)
$form.Controls.Add($header)
$lblTitle = New-Object System.Windows.Forms.Label
$lblTitle.Location = New-Object System.Drawing.Point(24, 12); $lblTitle.Size = New-Object System.Drawing.Size(630, 32)
$lblTitle.ForeColor = [System.Drawing.Color]::White
$lblTitle.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 16)
$header.Controls.Add($lblTitle)
$lblSub = New-Object System.Windows.Forms.Label
$lblSub.Location = New-Object System.Drawing.Point(26, 46); $lblSub.Size = New-Object System.Drawing.Size(630, 22)
$lblSub.ForeColor = [System.Drawing.Color]::FromArgb(190, 200, 225)
$header.Controls.Add($lblSub)

function New-Page() {
  $p = New-Object System.Windows.Forms.Panel
  $p.Location = New-Object System.Drawing.Point(0, 74); $p.Size = New-Object System.Drawing.Size(680, 332)
  $p.Visible = $false
  $form.Controls.Add($p)
  return $p
}
function New-Label($parent, $text, $x, $y, $w, $h) {
  $l = New-Object System.Windows.Forms.Label
  $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x, $y); $l.Size = New-Object System.Drawing.Size($w, $h)
  $parent.Controls.Add($l)
  return $l
}
function New-Button($parent, $text, $x, $y, $w, $h) {
  $b = New-Object System.Windows.Forms.Button
  $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x, $y); $b.Size = New-Object System.Drawing.Size($w, $h)
  $b.FlatStyle = "System"
  $parent.Controls.Add($b)
  return $b
}

$pages = @((New-Page), (New-Page), (New-Page), (New-Page), (New-Page))

# page 0: welcome
$null = New-Label $pages[0] ("This sets up the Ridge Racer PSP PC port on your computer.`r`n`r`n" +
  "You need your own copy of the game: a decrypted disc image (.iso) or the game's files as a folder. " +
  "Nothing from the game is uploaded or shared. The game is built on this PC from your copy, which takes " +
  "about 5 to 15 minutes depending on your processor.`r`n`r`n" +
  "No other programs or downloads are needed. You can keep using your computer while it works.") 28 20 624 190

# page 1: the game
$null = New-Label $pages[1] "Select your Ridge Racer disc image (.iso), or the folder that contains PSP_GAME:" 28 24 624 22
$txtGame = New-Object System.Windows.Forms.TextBox
$txtGame.Location = New-Object System.Drawing.Point(28, 54); $txtGame.Size = New-Object System.Drawing.Size(624, 26)
$pages[1].Controls.Add($txtGame)
$btnIso = New-Button $pages[1] "Choose ISO file..." 28 92 150 30
$btnFolder = New-Button $pages[1] "Choose folder..." 190 92 150 30
$lblGame = New-Label $pages[1] "" 28 140 624 60

# page 2: where
$null = New-Label $pages[2] "Where should Ridge Racer be installed?" 28 24 624 22
$txtDir = New-Object System.Windows.Forms.TextBox
$txtDir.Location = New-Object System.Drawing.Point(28, 54); $txtDir.Size = New-Object System.Drawing.Size(530, 26)
$pages[2].Controls.Add($txtDir)
$btnDir = New-Button $pages[2] "Browse..." 568 52 84 30
$lblDir = New-Label $pages[2] "" 28 96 624 60
$null = New-Label $pages[2] "The game, its settings and your saved games are kept in this folder. You can choose any drive." 28 170 624 44

# page 3: installing
$lblStage = New-Label $pages[3] "Starting..." 28 28 624 24
$lblStage.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 11)
$bar = New-Object System.Windows.Forms.ProgressBar
$bar.Location = New-Object System.Drawing.Point(28, 62); $bar.Size = New-Object System.Drawing.Size(624, 24)
$bar.Minimum = 0; $bar.Maximum = 1000
$pages[3].Controls.Add($bar)
$lblDetail = New-Label $pages[3] "" 28 94 624 22
$btnLog = New-Button $pages[3] "Show details" 28 124 120 28
$txtLog = New-Object System.Windows.Forms.TextBox
$txtLog.Location = New-Object System.Drawing.Point(28, 160); $txtLog.Size = New-Object System.Drawing.Size(624, 160)
$txtLog.Multiline = $true; $txtLog.ReadOnly = $true; $txtLog.ScrollBars = "Vertical"; $txtLog.Visible = $false
$txtLog.Font = New-Object System.Drawing.Font("Consolas", 8.5)
$pages[3].Controls.Add($txtLog)

# page 4: done
$lblDone = New-Label $pages[4] "" 28 24 624 104
$chkShortcut = New-Object System.Windows.Forms.CheckBox
$chkShortcut.Text = "Create a desktop shortcut"; $chkShortcut.Checked = $true
$chkShortcut.Location = New-Object System.Drawing.Point(28, 130); $chkShortcut.Size = New-Object System.Drawing.Size(400, 26)
$pages[4].Controls.Add($chkShortcut)
$chkPlay = New-Object System.Windows.Forms.CheckBox
$chkPlay.Text = "Start Ridge Racer now"; $chkPlay.Checked = $true
$chkPlay.Location = New-Object System.Drawing.Point(28, 162); $chkPlay.Size = New-Object System.Drawing.Size(400, 26)
$pages[4].Controls.Add($chkPlay)
$btnOpen = New-Button $pages[4] "Open install folder" 28 204 170 30
$btnReadme = New-Button $pages[4] "Read me (controls, bug reports)" 210 204 240 30

# bottom bar
$bottom = New-Object System.Windows.Forms.Panel
$bottom.Location = New-Object System.Drawing.Point(0, 406); $bottom.Size = New-Object System.Drawing.Size(680, 64)
$form.Controls.Add($bottom)
$sep = New-Object System.Windows.Forms.Label
$sep.Location = New-Object System.Drawing.Point(0, 0); $sep.Size = New-Object System.Drawing.Size(680, 1); $sep.BorderStyle = "Fixed3D"
$bottom.Controls.Add($sep)
$btnBack = New-Button $bottom "< Back" 350 16 96 32
$btnNext = New-Button $bottom "Next >" 454 16 110 32
$btnCancel = New-Button $bottom "Cancel" 572 16 88 32
$btnNext.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 9.5)
$form.AcceptButton = $btnNext

$titles = @(
  @("Welcome", "Ridge Racer PSP - PC port (beta)"),
  @("Your game", "Step 1 of 2"),
  @("Install location", "Step 2 of 2"),
  @("Installing", "Please wait - this window can stay in the background"),
  @("All done", "")
)

function Show-Page($i) {
  $script:page = $i
  for ($k = 0; $k -lt $pages.Count; $k++) { $pages[$k].Visible = ($k -eq $i) }
  $lblTitle.Text = $titles[$i][0]; $lblSub.Text = $titles[$i][1]
  $btnBack.Enabled = ($i -eq 1 -or $i -eq 2)
  $btnBack.Visible = ($i -ne 4)
  $btnCancel.Visible = ($i -ne 4)
  $btnNext.Enabled = $true
  switch ($i) {
    0 { $btnNext.Text = "Next >" }
    1 { $btnNext.Text = "Next >"; Update-GamePage }
    2 { $btnNext.Text = "Install"; Update-DirPage }
    3 { $btnNext.Text = "Next >"; $btnNext.Enabled = $false }
    4 { $btnNext.Text = "Finish" }
  }
}

function Update-GamePage() {
  $r = Test-GameChoice $txtGame.Text.Trim().Trim('"')
  $lblGame.Text = $r.text
  if ($r.ok) { $lblGame.ForeColor = [System.Drawing.Color]::FromArgb(0, 110, 40) } else { $lblGame.ForeColor = [System.Drawing.Color]::FromArgb(180, 30, 30) }
  if ($txtGame.Text.Trim() -eq "") { $lblGame.ForeColor = [System.Drawing.Color]::Gray }
  $btnNext.Enabled = $r.ok
  $script:isIso = $r.iso; $script:isoGB = $r.isoGB
}
function Update-DirPage() {
  $r = Test-InstallDir $txtDir.Text.Trim().Trim('"')
  $lblDir.Text = $r.text
  if ($r.ok) { $lblDir.ForeColor = [System.Drawing.Color]::FromArgb(0, 110, 40) } else { $lblDir.ForeColor = [System.Drawing.Color]::FromArgb(180, 30, 30) }
  $btnNext.Enabled = $r.ok
}

$txtGame.Add_TextChanged({ if ($script:page -eq 1) { Update-GamePage } })
$txtDir.Add_TextChanged({ if ($script:page -eq 2) { Update-DirPage } })

$btnIso.Add_Click({
  $d = New-Object System.Windows.Forms.OpenFileDialog
  $d.Title = "Select your Ridge Racer disc image"
  $d.Filter = "PSP disc image (*.iso)|*.iso|All files (*.*)|*.*"
  if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $txtGame.Text = $d.FileName }
})
$btnFolder.Add_Click({
  $d = New-Object System.Windows.Forms.FolderBrowserDialog
  $d.Description = "Select the folder that contains PSP_GAME (your own, decrypted Ridge Racer files)"
  if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $txtGame.Text = $d.SelectedPath }
})
$btnDir.Add_Click({
  $d = New-Object System.Windows.Forms.FolderBrowserDialog
  $d.Description = "Choose where to install Ridge Racer"
  $d.ShowNewFolderButton = $true
  if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
    $sel = $d.SelectedPath
    $items = @(Get-ChildItem -LiteralPath $sel -Force -ErrorAction SilentlyContinue)
    if ($items.Count -eq 0 -or (Test-Path -LiteralPath (Join-Path $sel "RidgeRacerNative.exe"))) { $txtDir.Text = $sel }
    else { $txtDir.Text = Join-Path $sel "Ridge Racer PSP PC" }
  }
})
$btnLog.Add_Click({
  $txtLog.Visible = -not $txtLog.Visible
  if ($txtLog.Visible) { $btnLog.Text = "Hide details" } else { $btnLog.Text = "Show details" }
})
$btnOpen.Add_Click({ if (Test-Path -LiteralPath $script:installDir) { Start-Process explorer.exe -ArgumentList ('"' + $script:installDir + '"') } })
$btnReadme.Add_Click({
  $readme = Join-Path $script:installDir "README.txt"
  if (Test-Path -LiteralPath $readme) { Start-Process notepad.exe -ArgumentList ('"' + $readme + '"') }
})

# ---- the install
$stageInfo = @{
  compiler = @("Preparing...", 0.00, 0.02)
  game     = @("Checking your game...", 0.02, 0.05)
  codegen  = @("Building the tools...", 0.05, 0.12)
  generate = @("Reading your game's code...", 0.12, 0.15)
  compile  = @("Building the game - this is the long part...", 0.15, 0.85)
  link     = @("Putting it together...", 0.85, 0.89)
  finish   = @("Finishing up...", 0.89, 0.90)
  prepare  = @("Copying the game files from your disc image...", 0.90, 1.00)
}

function Quote-Arg($a) { return '"' + $a + '"' }

function Start-Install() {
  $script:installDir = $txtDir.Text.Trim().Trim('"').TrimEnd('\')
  $script:gamePath = $txtGame.Text.Trim().Trim('"')
  try { New-Item -ItemType Directory -Force -Path $script:installDir | Out-Null } catch {
    Show-Message ("The install folder could not be created:`r`n" + $_.Exception.Message) "Error"; Show-Page 2; return
  }
  $script:logPath = Join-Path $script:installDir "install.log"
  $script:startedAt = Get-Date
  Set-Content -LiteralPath $script:logPath -Value "" -Encoding ASCII
  $script:logPos = 0L; $script:stage = "compiler"; $script:stageFrac = 0.0; $script:fileDone = 0; $script:fileTotal = 0
  $script:sawDone = $false; $script:lastError = ""; $script:phase = "build"; $script:started = Get-Date
  $txtLog.Text = ""; $bar.Value = 0; $lblStage.Text = "Starting..."; $lblDetail.Text = ""
  Show-Page 3
  $ps = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
  $inner = (Quote-Arg $ps) + ' -NoProfile -ExecutionPolicy Bypass -File ' + (Quote-Arg (Join-Path $kit "build.ps1")) +
    ' -Game ' + (Quote-Arg $script:gamePath) + ' -InstallDir ' + (Quote-Arg $script:installDir) +
    ' -WorkDir ' + (Quote-Arg (Join-Path $script:installDir "_build")) + ' -Progress -NoLaunch -CleanBuild' +
    ' > ' + (Quote-Arg $script:logPath) + ' 2>&1'
  Start-Child $inner
  $timer.Start()
}

function Start-Child($inner) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = "cmd.exe"
  $psi.Arguments = '/d /s /c "' + $inner + '"'
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.WorkingDirectory = $kit
  $script:proc = [System.Diagnostics.Process]::Start($psi)
}

function Read-LogLines() {
  $lines = @()
  try {
    $fs = New-Object System.IO.FileStream($script:logPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    if ($fs.Length -gt $script:logPos) {
      [void]$fs.Seek($script:logPos, [System.IO.SeekOrigin]::Begin)
      $n = [int]($fs.Length - $script:logPos)
      $buf = New-Object byte[] $n
      $got = $fs.Read($buf, 0, $n)
      $script:logPos += $got
      $text = [System.Text.Encoding]::UTF8.GetString($buf, 0, $got).Replace("`0", "")
      $lines = $text -split "`r?`n"
    }
    $fs.Close()
  } catch { }
  return $lines
}

function Update-Progress() {
  $info = $stageInfo[$script:stage]
  if ($null -eq $info) { $info = $stageInfo["compiler"] }
  $frac = $info[1] + ($info[2] - $info[1]) * $script:stageFrac
  $v = [int][math]::Round($frac * 1000)
  if ($v -gt 1000) { $v = 1000 }
  if ($v -lt $bar.Value -and $script:stage -ne "prepare") { $v = $bar.Value }   # never move backwards
  $bar.Value = $v
  $lblStage.Text = $info[0]
  $el = (Get-Date) - $script:started
  $detail = ""
  if ($script:stage -eq "compile" -and $script:fileTotal -gt 0) { $detail = "{0} of {1} files.  " -f $script:fileDone, $script:fileTotal }
  $lblDetail.Text = $detail + ("Time so far: {0:mm\:ss}" -f $el)
}

function Handle-Line($line) {
  if ($line -eq "") { return }
  if ($line.StartsWith("@@STEP ")) { $script:stage = $line.Substring(7).Trim(); $script:stageFrac = 0.0; $script:fileDone = 0; $script:fileTotal = 0 }
  elseif ($line.StartsWith("@@PCT ")) {
    $parts = $line.Substring(6).Trim().Split(" ")
    if ($parts.Count -ge 2) {
      $d = 0; $t = 0
      if ([int]::TryParse($parts[0], [ref]$d) -and [int]::TryParse($parts[1], [ref]$t) -and $t -gt 0) {
        $script:fileDone = $d; $script:fileTotal = $t; $script:stageFrac = [math]::Min(1.0, $d / $t)
      }
    }
  }
  elseif ($line.StartsWith("@@ERROR ")) { $script:lastError = $line.Substring(8).Trim() }
  elseif ($line.StartsWith("@@DONE")) { $script:sawDone = $true }
  else { $txtLog.AppendText($line + "`r`n") }
}

function Finish-Failed($why) {
  $timer.Stop()
  $script:phase = "failed"
  $lblStage.Text = "Setup could not finish"
  $txtLog.Visible = $true; $btnLog.Text = "Hide details"
  $msg = $why
  if (-not $msg) { $msg = "Something went wrong while building the game." }
  $lblDetail.Text = "A full log is in: " + $script:logPath
  $btnBack.Enabled = $true; $btnBack.Visible = $true
  $btnNext.Text = "Try again"; $btnNext.Enabled = $true
  Show-Message ($msg + "`r`n`r`nThe details are in the box on the setup page and in:`r`n" + $script:logPath) "Error"
}

function Finish-Success() {
  $timer.Stop()
  $script:phase = "finished"
  $bar.Value = 1000
  # the beta README next to the game (controls, online play, how to send a bug report), as plain text for Notepad
  try { Copy-Item -LiteralPath (Join-Path $kit "README-BETA.md") -Destination (Join-Path $script:installDir "README.txt") -Force } catch { }
  $took = ""
  if ($script:startedAt) { $mins = ((Get-Date) - $script:startedAt).TotalMinutes; $took = " (took {0:N1} minutes)" -f $mins; Add-Content -LiteralPath $script:logPath -Value ("Install time: {0:N1} minutes" -f $mins) -ErrorAction SilentlyContinue }
  $lblDone.Text = "Ridge Racer is installed in:`r`n$($script:installDir)$took`r`n`r`nThe first time you press Play, the game opens its settings window. Your saved games are kept in the 'savedata' folder there. Something wrong? Press Ctrl+F12 in the game to make a bug report."
  Show-Page 4
}

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 400
$timer.Add_Tick({
  foreach ($l in (Read-LogLines)) { Handle-Line $l }
  Update-Progress
  if ($null -ne $script:proc -and $script:proc.HasExited) {
    foreach ($l in (Read-LogLines)) { Handle-Line $l }
    $code = $script:proc.ExitCode
    $script:proc = $null
    if ($script:phase -eq "build") {
      if ($code -eq 0 -and $script:sawDone) {
        if ($script:isIso) {
          # copy the game files out of the disc image now, so the first start is instant
          $script:phase = "prepare"; $script:stage = "prepare"; $script:stageFrac = 0.0; $script:sawDone = $false
          $exe = Join-Path $script:installDir "RidgeRacerNative.exe"
          $inner = (Quote-Arg $exe) + ' --prepare-game ' + (Quote-Arg $script:gamePath) + ' >> ' + (Quote-Arg $script:logPath) + ' 2>&1'
          Start-Child $inner
        } else { Finish-Success }
      } else {
        $err = $script:lastError
        if (-not $err) { $err = "The build stopped unexpectedly (exit code $code)." }
        Finish-Failed $err
      }
    } elseif ($script:phase -eq "prepare") {
      if ($code -eq 0) { Finish-Success }
      else {
        $err = $script:lastError
        if (-not $err) { $err = "Copying the game files from the disc image failed (exit code $code)." }
        Finish-Failed $err
      }
    }
  }
})

function Stop-Install() {
  $timer.Stop()
  if ($null -ne $script:proc -and -not $script:proc.HasExited) {
    try { Start-Process -FilePath "taskkill.exe" -ArgumentList ("/PID " + $script:proc.Id + " /T /F") -WindowStyle Hidden -Wait } catch { }
  }
  $script:proc = $null
}

$btnNext.Add_Click({
  switch ($script:page) {
    0 { Show-Page 1 }
    1 { Show-Page 2 }
    2 { Start-Install }
    3 { if ($script:phase -eq "failed") { Start-Install } }
    4 {
      if ($chkShortcut.Checked) {
        try {
          $sh = New-Object -ComObject WScript.Shell
          $lnk = $sh.CreateShortcut((Join-Path ([Environment]::GetFolderPath("Desktop")) "Ridge Racer PSP.lnk"))
          $lnk.TargetPath = Join-Path $script:installDir "RidgeRacerNative.exe"
          $lnk.WorkingDirectory = $script:installDir
          $lnk.Save()
        } catch { }
      }
      if ($chkPlay.Checked) { Start-Process -FilePath (Join-Path $script:installDir "RidgeRacerNative.exe") -WorkingDirectory $script:installDir }
      $script:closing = $true
      $form.Close()
    }
  }
})
$btnBack.Add_Click({
  if ($script:page -eq 1) { Show-Page 0 }
  elseif ($script:page -eq 2) { Show-Page 1 }
  elseif ($script:page -eq 3 -and $script:phase -eq "failed") { Show-Page 2 }
})
$btnCancel.Add_Click({ $form.Close() })
$form.Add_FormClosing({
  param($sender, $e)
  if ($script:closing) { return }
  if ($script:phase -eq "build" -or $script:phase -eq "prepare") {
    $a = [System.Windows.Forms.MessageBox]::Show("Setup is still running. Stop it and close this window?", "Ridge Racer setup", "YesNo", "Question")
    if ($a -ne [System.Windows.Forms.DialogResult]::Yes) { $e.Cancel = $true; return }
    Stop-Install
  }
})

# a game path given on the command line pre-fills the window (handy for testing)
$txtDir.Text = Join-Path ([Environment]::GetFolderPath("UserProfile")) "Games\Ridge Racer PSP PC"
$script:closing = $false
Show-Page 0
[void]$form.ShowDialog()
