param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests,
    [switch]$SkipPackage
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'

# A running Windows executable is locked and cannot be replaced by the linker.
# Always close every launched jukebox copy before configuring or compiling so
# the canonical output remains build\Release\neon_jukebox.exe.
$runningJukebox = Get-Process -Name 'neon_jukebox' -ErrorAction SilentlyContinue
if ($runningJukebox) {
    Write-Host 'Closing the running Neon Jukebox before build...' -ForegroundColor Yellow
    $runningJukebox | Stop-Process -Force
    $runningJukebox | Wait-Process -ErrorAction SilentlyContinue
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw 'CMake was not found. Install it with: winget install --id Kitware.CMake --exact'
}
$cmakeBin = Split-Path -Parent $cmake.Source
$ctest = Join-Path $cmakeBin 'ctest.exe'
$cpack = Join-Path $cmakeBin 'cpack.exe'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'Git was not found. Install it with: winget install --id Git.Git --exact'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools was not found. Install the VCTools workload documented in README.md.'
}

$installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installationPath) {
    throw 'The Visual Studio C++ x64 toolchain was not found.'
}

& $cmake.Source -S $projectRoot -B $buildRoot -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake.Source --build $buildRoot --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }

if (-not $SkipTests) {
    & $ctest --test-dir $buildRoot -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}

if (-not $SkipPackage) {
    & $cmake.Source --install $buildRoot --config $Configuration --component Application --prefix (Join-Path $buildRoot 'package\stage')
    if ($LASTEXITCODE -ne 0) { throw 'Application staging failed.' }
    & $cpack --config (Join-Path $buildRoot 'CPackConfig.cmake') -C $Configuration -B (Join-Path $buildRoot 'package')
    if ($LASTEXITCODE -ne 0) { throw 'Package creation failed.' }
}

Write-Host "Neon Jukebox build completed: $buildRoot" -ForegroundColor Cyan
