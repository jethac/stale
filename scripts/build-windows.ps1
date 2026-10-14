[CmdletBinding()]
param(
    [switch]$BackendOnly,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$SlintSource = '',
    [string]$SlintPackage = '',
    [string]$BuildDirectory = '',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $repo $(if ($BackendOnly) { 'build/windows-core' } else { 'build/windows' })
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with Desktop development with C++ and CMake tools.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio C++ toolchain found.' }
& (Join-Path $vs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'Install the Visual Studio C++ CMake tools component.' }
$ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$configure = @('-S', $repo, '-B', $BuildDirectory, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_BUILD_TYPE=$Configuration", "-DSTALE_BUILD_DESKTOP=$(-not $BackendOnly)")
if ($SlintSource) { $configure += "-DSTALE_SLINT_SOURCE_DIR=$SlintSource" }
if ($SlintPackage) { $configure += "-DSTALE_SLINT_PACKAGE_DIR=$SlintPackage" }
& $cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmake --build $BuildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if (-not $SkipTests) {
    & $ctest --test-dir $BuildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
