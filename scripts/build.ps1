#requires -Version 5.1
<#
.SYNOPSIS
  Configures and builds Perfect Dark for Windows x86_64 (ntsc-final) using
  the MSYS2 MinGW64 toolchain.

.PARAMETER Clean
  Remove the build directory before configuring.

.PARAMETER Configure
  Force re-run of cmake configure even if the build dir already exists.

.PARAMETER Run
  Launch the resulting pd.x86_64.exe after a successful build.

.PARAMETER BuildType
  CMake build type. Defaults to RelWithDebInfo, matching CMakeLists.txt.

.PARAMETER Jobs
  Parallel build jobs. Defaults to the number of logical processors.

.PARAMETER Msys2Root
  Path to MSYS2 install. Auto-detected if omitted.
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Configure,
    [switch]$Run,
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
    [string]$BuildType = 'RelWithDebInfo',
    [int]$Jobs = [Environment]::ProcessorCount,
    [string]$Msys2Root
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'build'

if (-not $Msys2Root) {
    $candidates = @('C:\tools\msys64', 'C:\msys64', "$env:ProgramData\msys64")
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'usr\bin\bash.exe')) {
            $Msys2Root = $c
            break
        }
    }
}

if (-not $Msys2Root) {
    throw "MSYS2 not found. Run scripts\setup-deps.ps1 first (or install via 'choco install msys2 -y')."
}

$mingwBin = Join-Path $Msys2Root 'mingw64\bin'
if (-not (Test-Path (Join-Path $mingwBin 'gcc.exe'))) {
    Write-Host "MinGW64 toolchain missing under $mingwBin; running setup-deps.ps1..."
    & (Join-Path $PSScriptRoot 'setup-deps.ps1') -Msys2Root $Msys2Root
}

# Drive the build directly from PowerShell by prepending the MSYS2 mingw64
# bin to PATH. Avoids the overhead of an MSYS2 bash subshell and keeps stdout
# behaving normally for VSCode's problem matcher.
$env:PATH = "$mingwBin;" + (Join-Path $Msys2Root 'usr\bin') + ";$env:PATH"

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

$cacheExists = Test-Path (Join-Path $buildDir 'CMakeCache.txt')
if ($Configure -or -not $cacheExists) {
    Write-Host "Configuring (build type: $BuildType)"
    $cmakeExe = Join-Path $mingwBin 'cmake.exe'
    & $cmakeExe -G 'MinGW Makefiles' "-B$buildDir" "-DCMAKE_BUILD_TYPE=$BuildType" '-DCMAKE_SH=CMAKE_SH-NOTFOUND' $repoRoot
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

Write-Host "Building (-j $Jobs)"
$cmakeExe = Join-Path $mingwBin 'cmake.exe'
& $cmakeExe --build $buildDir -j $Jobs
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $buildDir 'pd.x86_64.exe'
if (-not (Test-Path $exe)) {
    throw "Expected $exe to exist after build."
}

# Stage runtime DLLs next to the executable so the game can launch in place.
$runtimeDlls = @(
    'SDL2.dll',
    'zlib1.dll',
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll'
)
foreach ($name in $runtimeDlls) {
    $src = Join-Path $mingwBin $name
    if (Test-Path $src) {
        Copy-Item -Force $src $buildDir
    } else {
        Write-Warning "Runtime DLL not found, skipping: $src"
    }
}

# Ensure the data folder exists so the user knows where to drop the ROM.
$dataDir = Join-Path $buildDir 'data'
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
$marker = Join-Path $dataDir 'put_your_rom_here.txt'
if (-not (Test-Path $marker)) {
    Set-Content -Path $marker -Value 'Place pd.ntsc-final.z64 in this directory.' -Encoding ascii
}

Write-Host ""
Write-Host "Built: $exe"

if ($Run) {
    $romPath = Join-Path $dataDir 'pd.ntsc-final.z64'
    if (-not (Test-Path $romPath)) {
        Write-Warning "ROM not found at $romPath; the game will likely fail to start."
    }
    Write-Host "Launching $exe --log"
    Push-Location $buildDir
    try {
        # --log makes sysLogPrintf write to pd.log next to the exe so we can
        # diagnose startup failures without a console.
        & $exe --log
    } finally {
        Pop-Location
    }
}
