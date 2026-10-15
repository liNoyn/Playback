# Installs the live-stream build of Playback into every LeviLauncher game version that has Playback.
# Only playback.dll and manifest.json are replaced (recordings and settings in mods\playback\data are kept);
# the originals are saved once to %USERPROFILE%\.playback2flashback\backup\ so they can be put back.
#
# Usage:  powershell -ExecutionPolicy Bypass -File install-live.ps1 [-Restore]
param(
    [switch] $Restore
)
$ErrorActionPreference = 'Stop'

if (Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue) {
    throw 'Close Minecraft first (the mod DLL is in use while the game runs).'
}

$versions = Join-Path $env:APPDATA 'levilauncher.exe\versions'
$mods = @()
if (Test-Path $versions) {
    $mods = Get-ChildItem $versions -Directory | ForEach-Object { Join-Path $_.FullName 'mods\playback' } |
        Where-Object { Test-Path (Join-Path $_ 'manifest.json') }
}
if (-not $mods) { throw "No Playback installation found under $versions (install Playback with LeviLauncher first)." }

$backupRoot = Join-Path $env:USERPROFILE '.playback2flashback\backup'
foreach ($mod in $mods) {
    $installed = Get-Content (Join-Path $mod 'manifest.json') -Raw | ConvertFrom-Json
    $gameVersion = Split-Path (Split-Path (Split-Path $mod)) -Leaf
    $backup = Join-Path $backupRoot "$gameVersion-playback-$($installed.version)"

    if ($Restore) {
        $latest = Get-ChildItem $backupRoot -Directory -Filter "$gameVersion-playback-*" | Sort-Object LastWriteTime | Select-Object -Last 1
        if (-not $latest) { Write-Host "No backup for $gameVersion"; continue }
        Copy-Item (Join-Path $latest.FullName '*') $mod -Force
        Write-Host "Restored $($latest.Name) -> $mod"
        continue
    }

    $newDll = Join-Path $PSScriptRoot 'playback.dll'
    $newManifest = Join-Path $PSScriptRoot 'manifest.json'
    if (-not (Test-Path $newDll) -or -not (Test-Path $newManifest)) {
        throw 'playback.dll / manifest.json not found next to install-live.ps1 - extract the whole release zip first.'
    }

    # Back up the first time only, so reinstalling never overwrites the original with the fork build.
    if ($installed.version -notmatch '-live\.' -and -not (Test-Path $backup)) {
        New-Item -ItemType Directory -Force $backup | Out-Null
        foreach ($file in 'playback.dll', 'playback.pdb', 'manifest.json') {
            $path = Join-Path $mod $file
            if (Test-Path $path) { Copy-Item $path $backup }
        }
        Write-Host "Backed up the original to $backup"
    }

    Copy-Item $newDll (Join-Path $mod 'playback.dll') -Force
    Copy-Item $newManifest (Join-Path $mod 'manifest.json') -Force
    $pdb = Join-Path $PSScriptRoot 'playback.pdb'
    if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $mod 'playback.pdb') -Force }
    Write-Host "Installed the live-stream build into $mod"
}

if (-not $Restore) {
    Write-Host ''
    Write-Host 'Next: install playback2flashback (its install.ps1) so recordings are converted while you record.'
    Write-Host 'To go back to the original Playback:  install-live.ps1 -Restore'
}
