function Write-JsonFile($Value, [string]$Path) {
    $json = ConvertTo-Json -InputObject $Value -Depth 12
    [IO.File]::WriteAllText($Path, $json, (New-Object Text.UTF8Encoding($false)))
}

function Get-ExecutableSha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}

function Read-Capture([string]$Path) {
    if (-not $Path) { throw 'ReportPath is required.' }
    $report = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($report.schemaVersion -ne 1) { throw 'Unsupported performance report schema.' }
    return $report
}

function Get-Summary($Report) {
    $seconds = [double]$Report.durationMs / 1000
    $scopes = @($Report.groups | Where-Object { $_.kind -eq 'scope' } | ForEach-Object {
        [pscustomobject]@{
            module = $_.module; phase = $_.phase; owner = $_.owner; calls = $_.count
            callsPerSecond = if ($seconds -gt 0) { $_.count / $seconds } else { $null }
            wallMs = $_.wallMs; selfWallMs = $_.selfWallMs
            selfCpuMs = if ($_.cpuSamples -gt 0) { $_.selfCpuMs } else { $null }; cpuSamples = $_.cpuSamples
            p95WallUpperMs = $_.p95WallUpperMs; maxWallMs = $_.maxWallMs
        }
    } | Sort-Object selfWallMs -Descending)
    $cpuSamples = @($Report.groups | Where-Object {
        $_.module -eq 'process' -and $_.phase -eq 'cpu_total_ms'
    })
    $cpuPercent = $null
    $cpuWindowMs = $null
    if ($cpuSamples.Count -eq 1 -and $cpuSamples[0].count -ge 2) {
        $cpuWindowMs = $cpuSamples[0].lastAtMs - $cpuSamples[0].firstAtMs
        if ($cpuWindowMs -gt 0 -and $Report.logicalProcessors -gt 0) {
            $cpuPercent = 100 * ($cpuSamples[0].max - $cpuSamples[0].min) /
                $cpuWindowMs / $Report.logicalProcessors
        }
    }
    [pscustomobject]@{
        schemaVersion = 1; session = $Report.session; hostVersion = $Report.hostVersion
        captureMode = if ($Report.PSObject.Properties['captureMode']) { $Report.captureMode } else { 'trace' }
        scopeCpuEnabled = if ($Report.PSObject.Properties['scopeCpuEnabled']) { $Report.scopeCpuEnabled } else { $true }
        processId = $Report.processId; durationMs = $Report.durationMs
        processCpuPercent = $cpuPercent; processCpuSampleWindowMs = $cpuWindowMs
        droppedEvents = $Report.droppedEvents; droppedGroups = $Report.droppedGroups
        droppedLinks = $Report.droppedLinks; probeErrors = $Report.probeErrors
        scopes = $scopes
        gauges = @($Report.groups | Where-Object { $_.kind -eq 'gauge' })
        limitations = $Report.limitations
    }
}

function Export-Summary([string]$Path) {
    $report = Read-Capture $Path
    $summary = Get-Summary $report
    $directory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $summaryPath = Join-Path $directory 'summary.json'
    Write-JsonFile $summary $summaryPath
    $summary.scopes | Export-Csv -LiteralPath (Join-Path $directory 'scopes.csv') -NoTypeInformation -Encoding UTF8
    $summary.gauges | Export-Csv -LiteralPath (Join-Path $directory 'gauges.csv') -NoTypeInformation -Encoding UTF8
    [pscustomobject]@{ report = [IO.Path]::GetFullPath($Path); summary = $summaryPath
        processCpuPercent = $summary.processCpuPercent; droppedEvents = $summary.droppedEvents }
}
