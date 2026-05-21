#requires -Version 5.1
<#
.SYNOPSIS
  Ensures MSYS2 + the MinGW-w64 packages needed to build Perfect Dark for
  Windows x86_64 are installed.

.DESCRIPTION
  The chocolatey "mingw" package ships a broken cc1.exe on some Windows 11
  builds (entry-point-not-found in UCRT API sets), and the README recommends
  MSYS2 anyway. This script locates an MSYS2 install, then uses pacman
  inside it to install the toolchain, SDL2, zlib, and cmake for MINGW64.

  Re-running is idempotent: pacman -S --needed only installs missing packages.

.PARAMETER Msys2Root
  Path to MSYS2 install. Auto-detected if omitted.

.PARAMETER Update
  Run `pacman -Syuu` first to refresh the package database and upgrade
  installed packages. Off by default to keep CI-like runs fast.
#>
[CmdletBinding()]
param(
    [string]$Msys2Root,
    [switch]$Update
)

$ErrorActionPreference = 'Stop'

if (-not $Msys2Root) {
    $candidates = @('C:\tools\msys64', 'C:\msys64', "$env:ProgramData\msys64")
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'usr\bin\pacman.exe')) {
            $Msys2Root = $c
            break
        }
    }
}

if (-not $Msys2Root -or -not (Test-Path (Join-Path $Msys2Root 'usr\bin\pacman.exe'))) {
    throw @"
MSYS2 not found. Install it (admin shell) with:
    choco install msys2 -y
or download it from https://www.msys2.org and re-run this script. If installed
to a non-default path, pass -Msys2Root <path>.
"@
}

Write-Host "Using MSYS2 at: $Msys2Root"

$bash = Join-Path $Msys2Root 'usr\bin\bash.exe'

# MSYSTEM=MINGW64 selects the mingw-w64 x86_64 environment so /mingw64 is
# the active prefix that pacman packages target.
$env:MSYSTEM = 'MINGW64'
$env:CHERE_INVOKING = '1'
# MSYS2 prepends carefully when launched via the right entry points; calling
# bash.exe directly works as long as MSYSTEM is set before launch.

$packages = @(
    'mingw-w64-x86_64-toolchain',
    'mingw-w64-x86_64-SDL2',
    'mingw-w64-x86_64-zlib',
    'mingw-w64-x86_64-cmake',
    'mingw-w64-x86_64-python',
    'make',
    'git'
)

if ($Update) {
    Write-Host "Updating package database (pacman -Syuu)..."
    & $bash -lc "pacman -Syuu --noconfirm"
    if ($LASTEXITCODE -ne 0) { throw "pacman -Syuu failed" }
}

$pkgList = $packages -join ' '
Write-Host "Installing/verifying packages: $pkgList"
& $bash -lc "pacman -S --noconfirm --needed $pkgList"
if ($LASTEXITCODE -ne 0) { throw "pacman -S failed" }

Write-Host ""
Write-Host "MSYS2 toolchain ready under: $Msys2Root\mingw64"
