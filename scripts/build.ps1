param(
    [ValidateSet('debug', 'release')]
    [string]$Configuration = 'release',
    [string]$VcpkgRoot = $env:VCPKG_ROOT
)

$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot

if (-not $VcpkgRoot) {
    $vcpkgCommand = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($vcpkgCommand) {
        $VcpkgRoot = Split-Path -Parent $vcpkgCommand.Source
    }
}

if (-not $VcpkgRoot -or -not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts/buildsystems/vcpkg.cmake'))) {
    throw 'Set VCPKG_ROOT to a bootstrapped vcpkg checkout.'
}

$resolvedVcpkgRoot = (Resolve-Path -LiteralPath $VcpkgRoot).Path

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Install Visual Studio with the MSVC x64 tools and Windows SDK.'
}

$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) {
    throw 'MSVC C++ tools were not found by vswhere.'
}

& (Join-Path $visualStudio 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

$env:VCPKG_ROOT = $resolvedVcpkgRoot
$buildPreset = 'msvc-x64-' + $Configuration.ToLowerInvariant()

Push-Location -LiteralPath $projectDirectory
try {
    cmake --preset msvc-x64
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed ($LASTEXITCODE)."
    }

    cmake --build --preset $buildPreset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed ($LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}
