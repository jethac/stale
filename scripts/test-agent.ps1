[CmdletBinding()]
param([string]$Executable = "$PSScriptRoot/../build/windows/bin/Stale.exe")
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $repo ('build/agent-test-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path (Join-Path $fixture 'node_modules/demo') -Force
Set-Content -LiteralPath (Join-Path $fixture 'node_modules/.package-lock.json') -Value '{}'
Set-Content -LiteralPath (Join-Path $fixture 'node_modules/demo/index.js') -Value 'module.exports = 42;'
$pipeName = 'test-' + [guid]::NewGuid().ToString('N')
$env:SLINT_BACKEND = 'winit-software'
$process = Start-Process -FilePath $Executable -ArgumentList @('--agent-pipe', $pipeName, '--close-after-ms', '20000') -WindowStyle Hidden -PassThru
function Invoke-Agent($request) {
    $result = & "$PSScriptRoot/stale-agent.ps1" -PipeName $pipeName -Request ($request | ConvertTo-Json -Compress -Depth 10)
    return $result | ConvertFrom-Json
}
function Assert($condition, $message) { if (-not $condition) { throw $message } }
function Ready {
    for ($i = 0; $i -lt 200; $i++) {
        $state = Invoke-Agent @{action='state'}
        if ($state.ok -and -not $state.busy -and $state.rows_ready) { return $state }
        Start-Sleep -Milliseconds 50
    }
    throw 'App did not become ready.'
}
try {
    $caps = Invoke-Agent @{action='capabilities'}
    Assert ($caps.ok -and $caps.protocol -eq 1) 'Protocol discovery failed.'
    Assert ($caps.actions -notcontains 'recycle') 'Transport must not bypass visible confirmation.'
    $originalTheme = (Invoke-Agent @{action='state'}).theme
    try {
        foreach ($mode in @('dark', 'light')) {
            $themed = Invoke-Agent @{action='theme';mode=$mode}
            Assert ($themed.ok -and $themed.theme -eq $mode) 'Theme change failed.'
        }
        $invalidTheme = Invoke-Agent @{action='theme';mode='invalid'}
        Assert (-not $invalidTheme.ok) 'Invalid theme was accepted.'
    } finally {
        $null = Invoke-Agent @{action='theme';mode=$originalTheme}
    }
    $null = Invoke-Agent @{action='browse';path=$fixture}
    $state = Ready
    Assert ($state.snapshot_files -eq 0 -and $state.browser_can_scan) 'Browsing must not recursively scan.'
    $liveRows = Invoke-Agent @{action='rows';limit=10}
    Assert ($liveRows.rows.Count -eq 1 -and $liveRows.rows[0].live -and -not $liveRows.rows[0].size_known) 'Live directory size must remain unknown before scanning.'
    $null = Invoke-Agent @{action='tree-expand';id=$fixture;open=$true}
    $tree = Invoke-Agent @{action='tree';limit=100}
    Assert (($tree.rows | Where-Object label -eq 'node_modules').Count -eq 1) 'Expanded tree did not include immediate directory.'
    $null = Invoke-Agent @{action='open';id=$liveRows.rows[0].id;view_revision=$liveRows.view_revision}
    $state = Ready
    Assert ($state.browse_path -match 'node_modules$') 'Opening a directory did not update browser location.'
    $null = Invoke-Agent @{action='up'}
    $null = Ready
    $null = Invoke-Agent @{action='browse';path=(Join-Path $fixture 'missing')}
    $state = Ready
    Assert ($state.browser_error -ne '') 'Missing directory error was hidden.'
    $null = Invoke-Agent @{action='root'}
    $state = Ready
    Assert ($state.browse_path -eq 'this-pc') 'Root navigation must show every drive.'
    $drives = Invoke-Agent @{action='drives'}
    Assert ($drives.drives.Count -gt 0) 'Drive inventory is empty.'
    $driveRows = Invoke-Agent @{action='rows';limit=100}
    foreach ($driveRow in $driveRows.rows) {
        $inventory = $drives.drives | Where-Object path -eq $driveRow.path | Select-Object -First 1
        Assert ($driveRow.drive -and $null -ne $inventory) 'This PC row lacks its drive capacity model.'
        if ($inventory.capacity_known -and $inventory.total_bytes -gt 0) {
            $expected = 1.0 - ([double]$inventory.free_bytes / [double]$inventory.total_bytes)
            Assert ($driveRow.capacity_known -and [math]::Abs($driveRow.used_fraction - $expected) -lt 0.000001) 'Drive bar fraction does not match capacity.'
            Assert ($driveRow.free_label -match 'free$' -and $driveRow.capacity_label -match 'used of') 'Drive capacity labels are missing.'
        } else {
            Assert (-not $driveRow.capacity_known -and $driveRow.capacity_label -eq 'Capacity unavailable') 'Unknown capacity must not look like an empty drive.'
        }
    }
    $null = Invoke-Agent @{action='scan';path=$fixture}
    $state = Ready
    Assert ($state.snapshot_files -eq 2) 'Fixture scan file count is wrong.'
    Assert (-not $state.selection_bar_visible -and $state.selection_label -eq '') 'Empty selection bar should be hidden.'
    Assert ($state.drive_total_bytes -gt 0 -and $state.drive_free_bytes -le $state.drive_total_bytes) 'Drive measurements are invalid.'
    $fractionSum = ($state.storage_slices | Measure-Object -Property fraction -Sum).Sum
    Assert ([math]::Abs($fractionSum - 1.0) -lt 0.00001) 'Storage ring fractions must sum to capacity.'
    $sections = @('cleanup', 'files', 'duplicates', 'activity')
    for ($sectionIndex = 0; $sectionIndex -lt $sections.Count; $sectionIndex++) {
        $null = Invoke-Agent @{action='navigate';section=$sections[$sectionIndex]}
        $state = Ready
        Assert ($state.navigation_index -eq $sectionIndex) 'Rail selection did not follow navigation.'
    }
    $state = Invoke-Agent @{action='navigation-menu';open=$true}
    Assert ($state.navigation_expanded) 'Navigation rail did not expand.'
    $null = Invoke-Agent @{action='navigate';section='cleanup'}
    $state = Ready
    Assert (-not $state.navigation_expanded -and $state.page -eq 'cleanup') 'Rail destination did not close its expanded menu.'
    foreach ($operation in @('maximize', 'restore')) {
        $null = Invoke-Agent @{action='window';operation=$operation}
        for ($attempt = 0; $attempt -lt 30; $attempt++) {
            $state = Invoke-Agent @{action='state'}
            if ($state.window_maximized -eq ($operation -eq 'maximize')) { break }
            Start-Sleep -Milliseconds 50
        }
        Assert ($state.window_maximized -eq ($operation -eq 'maximize')) 'Custom window control failed.'
    }
    $null = Invoke-Agent @{action='page';name='cleanup'}
    $null = Ready
    $groups = Invoke-Agent @{action='groups'}
    Assert ($groups.ok -and $groups.groups.Count -eq 1 -and $groups.groups[0].id -eq 'dependencies') 'Expected dependency group.'
    $null = Invoke-Agent @{action='open-project';id=$groups.groups[0].projects[0].id}
    $null = Ready
    $rows = Invoke-Agent @{action='rows';limit=10}
    Assert ($rows.ok -and $rows.rows.Count -eq 1 -and $rows.rows[0].selectable) 'Cleanup row missing.'
    $id = $rows.rows[0].id
    $result = Invoke-Agent @{action='select';id=$id;selected=$true;view_revision=$rows.view_revision}
    Assert ($result.ok -and $result.selection_count -eq 1) 'Stable-ID selection failed.'
    Assert ($result.selection_bar_visible -and $result.selection_label -match '1 selected for recycling') 'Selection bar did not reflect checked folders.'
    $result = Invoke-Agent @{action='review'}
    Assert ($result.ok -and $result.review_open) 'Visible review did not open.'
    $result = Invoke-Agent @{action='clear-selection'}
    Assert (-not $result.ok) 'Selection must not change under review.'
    $null = Invoke-Agent @{action='dismiss-review'}
    $null = Invoke-Agent @{action='clear-selection'}
    $state = Invoke-Agent @{action='state'}
    Assert (-not $state.selection_bar_visible -and $state.selection_label -eq '') 'Clearing selection did not hide the action bar.'
    $result = Invoke-Agent @{action='select';id=$id;selected=$true;view_revision=999999}
    Assert (-not $result.ok) 'Stale view revision was accepted.'
    $result = Invoke-Agent @{action='recycle'}
    Assert (-not $result.ok) 'Unexpected destructive agent command.'
    $result = Invoke-Agent @{action='rows';limit=101}
    Assert (-not $result.ok) 'Pagination bound not enforced.'
    $shot = Join-Path $fixture 'agent.ppm'
    $result = Invoke-Agent @{action='screenshot';path=$shot}
    Assert ($result.ok -and (Test-Path -LiteralPath $shot)) 'Agent screenshot failed.'
    Assert (Test-Path -LiteralPath (Join-Path $fixture 'node_modules/demo/index.js')) 'Fixture was unexpectedly removed.'
    $reportPath = Join-Path $fixture 'report.json'
    @{
        version=1; root=$fixture; scanned_at=[DateTimeOffset]::UtcNow.ToUnixTimeSeconds(); files=2; errors=0
        entries=@(@{path=$fixture;name='Fixture';parent=-1;directory=$true;logical_bytes=24;allocated_bytes=8192})
        candidates=@(@{path=(Join-Path $fixture 'node_modules');reason='npm dependency tree identified by .package-lock.json. Imported observation.';bytes=8192;validation_token='forged-token'})
    } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding utf8
    $null = Invoke-Agent @{action='load-report';path=$reportPath}
    $state = Ready
    Assert ($state.summary_only -and $state.page -eq 'recommendations') 'Report is not visibly read-only.'
    $rows = Invoke-Agent @{action='rows';limit=10}
    Assert ($rows.rows.Count -eq 1 -and -not $rows.rows[0].selectable) 'Imported candidate became selectable.'
    $result = Invoke-Agent @{action='select';id=$rows.rows[0].id;selected=$true}
    Assert (-not $result.ok) 'Imported token was trusted.'
    $result = Invoke-Agent @{action='find-duplicates'}
    Assert (-not $result.ok) 'Incomplete summary was treated as a full scan.'
    $null = Invoke-Agent @{action='inspect';id=$rows.rows[0].id;view_revision=$rows.view_revision}
    $result = Invoke-Agent @{action='scan-detail'}
    Assert ($result.ok) 'Targeted validation did not start.'
    $null = Ready
    $null = Invoke-Agent @{action='page';name='recommendations'}
    $null = Ready
    $rows = Invoke-Agent @{action='rows';limit=10}
    Assert ($rows.rows.Count -eq 1 -and $rows.rows[0].selectable) 'Fresh candidate was not merged back into the saved map.'
    $result = Invoke-Agent @{action='resize';width=1000;height=660}
    Assert ($result.ok) 'Minimum supported window resize failed.'
    Write-Output 'Agent end-to-end tests passed: drive tree, live browsing, missing folders, theme, navigation rail, window controls, groups, storage ring, scan, selection bar, review, refusals, saved map, targeted validation, resize. No recycling.'
} finally {
    # Only this disposable test process, never an existing user window.
    if (-not $process.HasExited) { $process.WaitForExit(25000) | Out-Null }
    if (-not $process.HasExited) { Stop-Process -Id $process.Id; throw 'The test application did not shut down cleanly.' }
}
