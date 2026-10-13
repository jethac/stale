[CmdletBinding()]
param(
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repo 'build/windows' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'dist' }
$bin = Join-Path $BuildDirectory 'bin'
foreach ($name in @('Stale.exe', 'slint_cpp.dll', 'stale_windows_cli.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $name))) { throw "Missing $name. Build the desktop target first." }
}
$stage = Join-Path $BuildDirectory 'package/Stale-Windows-x64'
New-Item -ItemType Directory -Force -Path $stage, $OutputDirectory | Out-Null
foreach ($name in @('Stale.exe', 'slint_cpp.dll', 'stale_windows_cli.exe')) {
    Copy-Item -LiteralPath (Join-Path $bin $name) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'docs/WINDOWS.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $repo 'scripts/stale-agent.ps1') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'desktop/AGENT_API.md') -Destination $stage
$cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt')
$sourceLine = $cache | Where-Object { $_ -match '^STALE_SLINT_SOURCE_DIR:PATH=' } | Select-Object -First 1
if ($sourceLine) { $slintSource = $sourceLine.Substring($sourceLine.IndexOf('=') + 1) }
if (-not $slintSource) {
    $sourceLine = $cache | Where-Object { $_ -match '^stale_slint_SOURCE_DIR:STATIC=' } | Select-Object -First 1
    if ($sourceLine) { $slintSource = $sourceLine.Substring($sourceLine.IndexOf('=') + 1) }
}
if (-not $slintSource) { throw 'Cannot locate Slint source licenses from CMake cache.' }
$notices = Join-Path $stage 'third-party/Slint'
New-Item -ItemType Directory -Force -Path $notices | Out-Null
Copy-Item -LiteralPath (Join-Path $slintSource 'LICENSE.md') -Destination $notices
Copy-Item -LiteralPath (Join-Path $slintSource 'LICENSES') -Destination $notices -Recurse -Force
Copy-Item -LiteralPath (Join-Path $slintSource 'ui-libraries/material/src/fonts/roboto-flex/OFL.txt') -Destination (Join-Path $notices 'RobotoFlex-OFL.txt')
Copy-Item -LiteralPath (Join-Path $repo 'desktop/vendor/LICENSE.nlohmann-json') -Destination (Join-Path $stage 'third-party/LICENSE.nlohmann-json')
$archive = Join-Path $OutputDirectory 'Stale-Windows-x64.zip'
Compress-Archive -Path $stage -DestinationPath $archive -Force
Write-Output $archive
