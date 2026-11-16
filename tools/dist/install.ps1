# install.ps1 - installs, updates or removes Back on Air, the Split/Second PC patch, on Windows. Started by install.bat and
# uninstall.bat (powershell -NoProfile -ExecutionPolicy Bypass -File install.ps1 ...).
#
#   install.ps1                      install or update
#   install.ps1 /uninstall           remove the patch, put back what the install had moved away
#   install.ps1 /gamedir "D:\...\SplitSecond"   use this game folder instead of searching the Steam libraries
#
# Copies dinput8.dll, splitsecond.conf, the dxvk folder and pipeline-warmup next to SplitSecond.exe. Anything it replaces goes into
# <game folder>\patch-backup\YYYY-MM-DD_HHMMSS first. It never touches the game's own files or the saves.
# Windows PowerShell 5.1 or later. ASCII only (5.1 reads a file without BOM as the ANSI code page).

$ErrorActionPreference = 'Stop'
$AppId = 297860
$Exe = 'SplitSecond.exe'
$Mark = 'splitsecond.conf'                     # a string every build of the patch's dinput8.dll holds
$DxvkMark = 'Split/Second PC patch'           # in the first line of dxvk\README.txt (every release; keep it)
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Stamp = Get-Date -Format 'yyyy-MM-dd_HHmmss'
$script:Bk = $null

function Say([string]$t) { Write-Host $t }
function Stop-Here([string]$t) { Write-Host ("STOPPED: " + $t); exit 1 }
function P { [IO.Path]::Combine([string[]]$args) }

$Mode = 'install'; $GameDir = ''
for ($i = 0; $i -lt $args.Count; $i++) {
  $a = [string]$args[$i]
  if ($a -match '^(/|-{1,2})?uninstall$') { $Mode = 'uninstall' }
  elseif ($a -match '^(/|-{1,2})game-?dir$') { $i++; if ($i -lt $args.Count) { $GameDir = [string]$args[$i] } }
  else { Stop-Here "unknown option '$a' (use /uninstall or /gamedir FOLDER)" }
}

# ------------------------------------------------------------------ Steam libraries and the game folder
function Get-SteamLibraries {
  $roots = @()
  if ($env:SPLITSECOND_STEAM_PATH) { $roots += $env:SPLITSECOND_STEAM_PATH }   # unusual installs and tests
  foreach ($k in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
    try {
      $v = Get-ItemProperty -Path $k -ErrorAction Stop
      if ($v.SteamPath) { $roots += ($v.SteamPath -replace '/', '\') }
      if ($v.InstallPath) { $roots += $v.InstallPath }
    } catch { }
  }
  $libs = New-Object System.Collections.Generic.List[string]
  foreach ($r in $roots) {
    if (-not (Test-Path -LiteralPath (P $r 'steamapps'))) { continue }
    $libs.Add($r)
    $vdf = P $r 'steamapps' 'libraryfolders.vdf'
    if (-not (Test-Path -LiteralPath $vdf)) { continue }
    $text = [IO.File]::ReadAllText($vdf)
    foreach ($m in [regex]::Matches($text, '"path"\s+"([^"]+)"')) { $libs.Add($m.Groups[1].Value.Replace('\\', '\')) }
  }
  $seen = @{}; $out = @()
  foreach ($l in $libs) {
    $key = $l.TrimEnd('\', '/').ToLowerInvariant()
    if (-not $seen.ContainsKey($key)) { $seen[$key] = 1; $out += $l }
  }
  return $out
}

function Find-Game {
  foreach ($lib in (Get-SteamLibraries)) {
    $acf = P $lib 'steamapps' "appmanifest_$AppId.acf"
    if (-not (Test-Path -LiteralPath $acf)) { continue }
    $m = [regex]::Match([IO.File]::ReadAllText($acf), '"installdir"\s+"([^"]+)"')
    if (-not $m.Success) { continue }
    $dir = P $lib 'steamapps' 'common' $m.Groups[1].Value
    if (Test-Path -LiteralPath (P $dir $Exe)) { return $dir }
  }
  return $null
}

function Ask-GameDir {
  Say "Split/Second was not found in the Steam libraries. Choose the game folder (the folder with $Exe)."
  try {
    Add-Type -AssemblyName System.Windows.Forms
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = "Choose the Split/Second folder (the folder with $Exe)"
    $d.ShowNewFolderButton = $false
    if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $d.SelectedPath }
    return ''
  } catch {
    return (Read-Host "Type the game folder and press Enter")
  }
}

if (-not $GameDir) { $GameDir = Find-Game }
if (-not $GameDir) { $GameDir = Ask-GameDir }
if (-not $GameDir) { Stop-Here "no game folder chosen." }
$GameDir = $GameDir.Trim().Trim('"').TrimEnd('\', '/')
if (-not (Test-Path -LiteralPath (P $GameDir $Exe))) { Stop-Here "$Exe is not in $GameDir" }
Say "Back on Air, a PC patch for Split/Second"
Say "Game folder: $GameDir"

if (Get-Process -Name 'SplitSecond' -ErrorAction SilentlyContinue) {
  Stop-Here "Split/Second is running. Quit the game, then run this again."
}

# The game folder must be writable (Steam's folders usually are; otherwise run as administrator).
try {
  $probe = P $GameDir 'splitsecond-install.tmp'
  [IO.File]::WriteAllText($probe, 'x'); Remove-Item -LiteralPath $probe -Force
} catch {
  Stop-Here "cannot write into $GameDir. Right-click install.bat and choose 'Run as administrator'."
}

# ------------------------------------------------------------------ helpers
function Test-OursDll([string]$f) {
  try { return [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($f)).Contains($Mark) } catch { return $false }
}
function Test-OursDxvk([string]$d) {
  $r = P $d 'README.txt'
  if (-not (Test-Path -LiteralPath $r)) { return $false }
  return ([IO.File]::ReadAllText($r)).Contains($DxvkMark)
}
function Test-Same([string]$a, [string]$b) {
  if (-not ((Test-Path -LiteralPath $a -PathType Leaf) -and (Test-Path -LiteralPath $b -PathType Leaf))) { return $false }
  return (Get-FileHash -LiteralPath $a -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
}
function Get-BackupDir {
  if ($script:Bk) { return $script:Bk }
  $root = P $GameDir 'patch-backup'
  if (-not (Test-Path -LiteralPath $root)) { New-Item -ItemType Directory -Path $root | Out-Null }
  $b = P $root $Stamp; $n = 2
  while (Test-Path -LiteralPath $b) { $b = P $root "$Stamp-$n"; $n++ }
  New-Item -ItemType Directory -Path $b | Out-Null
  $script:Bk = $b
  return $b
}
# Moves <game>\Name into the backup folder; Kind is 'patch' (ours) or 'foreign' (put back on uninstall).
function Move-Away([string]$Name, [string]$Kind) {
  $b = Get-BackupDir
  Move-Item -LiteralPath (P $GameDir $Name) -Destination (P $b $Name)
  Add-Content -LiteralPath (P $b 'backup.txt') -Value "$Kind $Name" -Encoding ASCII
  Say ("  $Name moved to patch-backup\" + (Split-Path -Leaf $b) + "\")
}
function Copy-In([string]$Src, [string]$Dst) {
  $to = P $GameDir $Dst
  if (Test-Path -LiteralPath (P $Here $Src) -PathType Container) {
    Copy-Item -LiteralPath (P $Here $Src) -Destination $to -Recurse -Force
  } else {
    Copy-Item -LiteralPath (P $Here $Src) -Destination "$to.ss-new" -Force
    Move-Item -LiteralPath "$to.ss-new" -Destination $to -Force
  }
  if (Get-Command Unblock-File -ErrorAction SilentlyContinue) {
    try { Get-ChildItem -LiteralPath $to -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue } catch { }
    try { Unblock-File -LiteralPath $to -ErrorAction SilentlyContinue } catch { }
  }
}

try {
# ------------------------------------------------------------------ uninstall
if ($Mode -eq 'uninstall') {
  $Stamp = "$Stamp-uninstall"
  $dll = P $GameDir 'dinput8.dll'
  if (Test-Path -LiteralPath $dll) {
    if (Test-OursDll $dll) { Move-Away 'dinput8.dll' 'patch' } else { Say "  dinput8.dll is not the patch's; left alone" }
  }
  $dx = P $GameDir 'dxvk'
  if (Test-Path -LiteralPath $dx) {
    if (Test-OursDxvk $dx) { Move-Away 'dxvk' 'patch' } else { Say "  the dxvk folder is not the patch's; left alone" }
  }
  foreach ($f in 'splitsecond.conf', 'splitsecond.conf.new', 'splitsecond-patch.log', 'splitsecond-patch.prev.log', 'splitsecond-patch.warned') {
    if (Test-Path -LiteralPath (P $GameDir $f)) { Move-Away $f 'patch' }
  }
  if (Test-Path -LiteralPath (P $GameDir 'pipeline-warmup') -PathType Container) { Move-Away 'pipeline-warmup' 'patch' }
  # put back what an install moved away (the newest copy of each), unless something is there again
  $root = P $GameDir 'patch-backup'
  $dirs = @()
  if (Test-Path -LiteralPath $root) { $dirs = @(Get-ChildItem -LiteralPath $root -Directory | Sort-Object Name -Descending) }
  foreach ($f in 'dinput8.dll', 'd3d9.dll', 'dxvk') {
    if (Test-Path -LiteralPath (P $GameDir $f)) { continue }
    foreach ($d in $dirs) {
      $info = P $d.FullName 'backup.txt'
      if (-not (Test-Path -LiteralPath $info)) { continue }
      if (-not (@(Get-Content -LiteralPath $info) -contains "foreign $f")) { continue }
      if (-not (Test-Path -LiteralPath (P $d.FullName $f))) { continue }
      Copy-Item -LiteralPath (P $d.FullName $f) -Destination (P $GameDir $f) -Recurse
      Say ("  $f put back from patch-backup\" + $d.Name + "\")
      break
    }
  }
  Say "Removed. Left alone: your saves, save-backups (copies of your saves), shader-cache (can be deleted),"
  Say "patch-backup (what the patch replaced; your settings file is in the newest folder there)."
  if (Test-Path -LiteralPath (P $GameDir 'splitsecond-online.key')) {
    Say "splitsecond-online.key is kept: it is your online account key. Keep it if you want the same account and name later."
  }
  exit 0
}

# ------------------------------------------------------------------ install / update
foreach ($f in 'dinput8.dll', 'splitsecond.conf', (P 'dxvk' 'd3d9.dll')) {
  if (-not (Test-Path -LiteralPath (P $Here $f))) { Stop-Here "$f is missing next to install.ps1. Unpack the whole zip, then run install.bat from the unpacked folder." }
}

if (Test-Path -LiteralPath (P $GameDir 'DXFreezerServer.dll')) {
  Stop-Here ("the old Split/Second HFR mod is installed (DXFreezerServer.dll). It conflicts with this patch " +
             "(the game freezes). Move DXFreezerServer.dll and SplitSecondHFR.ini out of the game folder, then run this again.")
}
if (Test-Path -LiteralPath (P $GameDir 'SplitSecondHFR.ini')) {
  Say "  note: SplitSecondHFR.ini (old HFR mod) is in the game folder; it does nothing without DXFreezerServer.dll"
}

# dinput8.dll
$dll = P $GameDir 'dinput8.dll'
if (Test-Same (P $Here 'dinput8.dll') $dll) {
  Say "  dinput8.dll is already this version"
} else {
  if (Test-Path -LiteralPath $dll) { if (Test-OursDll $dll) { Move-Away 'dinput8.dll' 'patch' } else { Move-Away 'dinput8.dll' 'foreign' } }
  Copy-In 'dinput8.dll' 'dinput8.dll'; Say "  dinput8.dll installed"
}

# A d3d9.dll beside the exe (the older layout: DXVK copied there) is always used and overrides the renderer switch.
if (Test-Path -LiteralPath (P $GameDir 'd3d9.dll')) {
  Say "  d3d9.dll beside the game (older layout) would override the renderer setting:"
  Move-Away 'd3d9.dll' 'foreign'
}

# dxvk folder (DXVK 2.7.1, used when renderer = dxvk)
$dx = P $GameDir 'dxvk'
if ((Test-Path -LiteralPath $dx) -and (Test-OursDxvk $dx) -and (Test-Same (P $Here 'dxvk' 'd3d9.dll') (P $dx 'd3d9.dll'))) {
  Say "  dxvk folder is already this version"
} else {
  if (Test-Path -LiteralPath $dx) { if (Test-OursDxvk $dx) { Move-Away 'dxvk' 'patch' } else { Move-Away 'dxvk' 'foreign' } }
  Copy-In 'dxvk' 'dxvk'; Say "  dxvk folder installed (DXVK 2.7.1)"
}

# pre-compile records (pipeline-warmup\<level>.bin); the player's own records in pipeline-warmup\local stay
$pwSrc = P $Here 'pipeline-warmup'
if (Test-Path -LiteralPath $pwSrc -PathType Container) {
  $pw = P $GameDir 'pipeline-warmup'
  if (-not (Test-Path -LiteralPath $pw -PathType Container)) { New-Item -ItemType Directory -Path $pw | Out-Null }
  $n = 0
  foreach ($f in @(Get-ChildItem -LiteralPath $pwSrc -File -Filter '*.bin')) {
    if (Test-Same $f.FullName (P $pw $f.Name)) { continue }
    Copy-In (P 'pipeline-warmup' $f.Name) (P 'pipeline-warmup' $f.Name); $n++
  }
  if ($n) { Say "  pre-compile records installed (pipeline-warmup, $n files)" } else { Say "  pre-compile records are already this version" }
}

# the settings file: an existing one is the player's and stays
$conf = P $GameDir 'splitsecond.conf'
if (-not (Test-Path -LiteralPath $conf)) {
  Copy-In 'splitsecond.conf' 'splitsecond.conf'; Say "  splitsecond.conf installed (default settings)"
} elseif (Test-Same (P $Here 'splitsecond.conf') $conf) {
  Say "  splitsecond.conf is already the default one"
} else {
  Copy-In 'splitsecond.conf' 'splitsecond.conf.new'
  Say "  your splitsecond.conf is kept; this version's defaults are in splitsecond.conf.new"
}

if ($script:Bk) { Say "Backup of what was replaced: $script:Bk" }
Say ""
Say "Done. Start Split/Second from Steam."
Say "PC SETTINGS in the game: F1, hold Back (View), or Y on the Options screen."
exit 0
} catch {
  Stop-Here ("$($_.Exception.Message)")
}
