[CmdletBinding()]
param([Parameter(Mandatory)][string]$ManifestPath,[string]$OutputDirectory,[switch]$Offline)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/performance/ensure_trace_processor.ps1')
function Quote-ProcessArgument([string]$Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1') + '"'
}
function Number-OrNull($Value) {
    if ($null -eq $Value -or [string]$Value -in @('','[NULL]')) { return $null }
    return [double]::Parse([string]$Value,[Globalization.CultureInfo]::InvariantCulture)
}
try {
    $manifestFile = [IO.Path]::GetFullPath($ManifestPath)
    if ([IO.Path]::GetExtension($manifestFile) -eq '.pftrace') { $manifestFile = Join-Path ([IO.Path]::GetDirectoryName($manifestFile)) 'session.json' }
    if ((Get-Item -LiteralPath $manifestFile).Length -gt 65536) { throw 'manifestLimit' }
    $manifest = Get-Content -LiteralPath $manifestFile -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($manifest.schemaVersion -ne 2 -or $manifest.format -ne 'perfetto' -or -not $manifest.completion -or $manifest.state -ne 'completed') { throw 'captureIncompleteOrUnsupported' }
    if ($manifest.traceFile -ne [IO.Path]::GetFileName($manifest.traceFile) -or [IO.Path]::GetExtension($manifest.traceFile) -ne '.pftrace') { throw 'manifestTracePathRejected' }
    $trace = Join-Path ([IO.Path]::GetDirectoryName($manifestFile)) $manifest.traceFile
    if ((Get-Item -LiteralPath $trace).Length -gt 268435456 -or
        (Get-FileHash -LiteralPath $trace -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.traceSha256) { throw 'traceFingerprintMismatch' }
    $processor = Get-SnowDesktopTraceProcessor -Offline:$Offline
    if (-not $OutputDirectory) { $OutputDirectory = Join-Path ([IO.Path]::GetDirectoryName($manifestFile)) ('analysis-' + [Guid]::NewGuid().ToString('N')) }
    $directory = [IO.Path]::GetFullPath($OutputDirectory)
    if (Test-Path -LiteralPath $directory) { throw 'analysisOutputAlreadyExists' }
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $sqlRoot = Join-Path $PSScriptRoot '../tools/performance/sql'
    $common = Get-Content -LiteralPath (Join-Path $sqlRoot 'common.sql') -Raw -Encoding UTF8
    $tables = @{}
    $queryHashes = [ordered]@{}
    foreach ($name in @('quality','scopes','modules','counters','diagnostics')) {
        $sqlPath = Join-Path $sqlRoot ($name + '.sql')
        $queryHashes[$name] = (Get-FileHash -LiteralPath $sqlPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $query = Join-Path $directory ($name + '.sql')
        [IO.File]::WriteAllText($query,($common + "`n" + (Get-Content -LiteralPath $sqlPath -Raw -Encoding UTF8)),(New-Object Text.UTF8Encoding($false)))
        $start = New-Object Diagnostics.ProcessStartInfo
        $start.FileName = $processor
        $start.Arguments = 'query -f ' + (Quote-ProcessArgument $query) + ' ' + (Quote-ProcessArgument $trace)
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
        $process = [Diagnostics.Process]::Start($start)
        try {
            $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(60000)) { $process.Kill(); $process.WaitForExit(); throw 'analysisTimeout' }
            $csv = $stdout.GetAwaiter().GetResult(); $errors = $stderr.GetAwaiter().GetResult()
            [IO.File]::WriteAllText((Join-Path $directory ($name + '.log')),$errors)
            if ($process.ExitCode -ne 0 -or $csv.Length -gt 16777216) { throw ('analysisQueryFailed: ' + $name) }
            [IO.File]::WriteAllText((Join-Path $directory ($name + '.csv')),$csv,(New-Object Text.UTF8Encoding($false)))
            $tables[$name] = @($csv | ConvertFrom-Csv)
        }
        finally { $process.Dispose() }
    }
    $queryHashes.common = (Get-FileHash -LiteralPath (Join-Path $sqlRoot 'common.sql') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($tables.quality.Count -ne 1) { throw 'analysisQualityRowMissing' }
    $q = $tables.quality[0]
    $duration = Number-OrNull $q.durationMs
    $probeErrors = [double]$manifest.probeQuality.droppedOwners + [double]$manifest.probeQuality.droppedCounters +
        [double]$manifest.probeQuality.droppedLinks + [double]$manifest.probeQuality.probeErrors
    $valid = $null -ne $duration -and $duration -gt 0 -and [int]$q.starts -eq 1 -and [int]$q.stops -eq 1 -and [double]$q.lossOrErrors -eq 0 -and $probeErrors -eq 0
    $quality = [ordered]@{valid=$valid;lossOrErrors=[double]$q.lossOrErrors;probeErrors=$probeErrors
        scopeCoverage=if([int]$q.incompleteSlices -gt 0 -or [int]$q.boundarySlices -gt 0){'partial'}else{'complete_recorded'}
        unattributedServiceDiscardedChunks=[double]$q.serviceChunksDiscarded
        unattributedServiceDiscardedPatches=[double]$q.servicePatchesDiscarded
        incompleteSlices=[int]$q.incompleteSlices;boundarySlices=[int]$q.boundarySlices;missingCpuSlices=[int]$q.missingCpuSlices
        window='SDK capture markers; only complete contained slices; resource rates use their actual first/last sample window'}
    $scopes = @($tables.scopes | ForEach-Object {
        [pscustomobject]@{module=$_.module;phase=$_.phase;owner=$_.owner;calls=[int]$_.calls
            wallMs=(Number-OrNull $_.wallMs);selfWallMs=(Number-OrNull $_.selfWallMs)
            p50WallMs=(Number-OrNull $_.p50WallMs);p95WallMs=(Number-OrNull $_.p95WallMs);p99WallMs=(Number-OrNull $_.p99WallMs)
            maxWallMs=(Number-OrNull $_.maxWallMs);inclusiveCpuMs=(Number-OrNull $_.inclusiveCpuMs);selfCpuMs=$null;cpuSamples=[int]$_.cpuSamples}
    })
    $modules = @($tables.modules | ForEach-Object { [pscustomobject]@{module=$_.module;calls=[int]$_.calls;selfWallMs=(Number-OrNull $_.selfWallMs);selfCpuMs=$null} })
    $cumulative = @('cpu_total_ms','page_fault_count','io_read_bytes','io_write_bytes','io_other_bytes','io_read_operations','io_write_operations',
        'lua_allocated_bytes_total','lua_freed_bytes_total','lua_allocations_total','lua_frees_total','lua_reallocations_total')
    $counters = @($tables.counters | ForEach-Object {
        $parts = $_.name.Split('|'); $window = Number-OrNull $_.sampleWindowMs
        $first = Number-OrNull $_.firstValue; $last = Number-OrNull $_.lastValue
        $isCumulative = $parts[1] -in $cumulative
        $delta = if ($valid -and $isCumulative -and [int]$_.samples -ge 2 -and $window -gt 0 -and [int]$_.decreases -eq 0) {$last-$first} else {$null}
        [pscustomobject]@{module=$parts[0];metric=$parts[1];owner=$parts[2];semantics=if($isCumulative){'cumulative'}else{'gauge'}
            samples=[int]$_.samples;sampleWindowMs=$window;first=$first;last=$last;peak=(Number-OrNull $_.peakValue)
            resetOrDecreaseCount=[int]$_.decreases;delta=$delta;ratePerSecond=if($null -ne $delta){$delta*1000/$window}else{$null}}
    })
    $cpuRows = @($counters | Where-Object {$_.module -eq 'process' -and $_.metric -eq 'cpu_total_ms'})
    $cpu = if ($cpuRows.Count -eq 1) {$cpuRows[0].ratePerSecond} else {$null}
    $summary = [ordered]@{schemaVersion=2;queryVersion=1;sdkVersion=$manifest.sdkVersion;sessionId=$manifest.sessionId
        hostVersion=$manifest.hostVersion;executableSha256=$manifest.executableSha256;sourceCommit=$manifest.sourceCommit
        processId=$manifest.processId;processRole=$manifest.processRole;preset=$manifest.preset;scopeCpuEnabled=$manifest.scopeCpuEnabled
        configHash=$manifest.configHash;traceSha256=$manifest.traceSha256;manifestSha256=(Get-FileHash -LiteralPath $manifestFile -Algorithm SHA256).Hash.ToLowerInvariant()
        queryHashes=$queryHashes;effectiveDurationMs=$duration;quality=$quality;participants=$manifest.participants
        processCpuMsPerSecond=$cpu;processCpuSingleCorePercent=if($null -ne $cpu){$cpu/10}else{$null}
        processCpuNormalizedPercent=if($null -ne $cpu -and $manifest.logicalProcessors -gt 0){$cpu/10/$manifest.logicalProcessors}else{$null}
        scopes=$scopes;modules=$modules;counters=$counters
        limitations=@('Host process only; excluded processes are listed in the manifest.',
            'Inclusive thread CPU is sampled by the SDK; missing CPU remains null. Self CPU is unavailable when full child coverage is not proved.',
            'Wall time includes waiting and nested work. App callbacks, updates and commits do not measure OS wakeups or presented frames.',
            'GPU counters describe the process and cannot be divided into widget GPU time.',
            'Opaque instance IDs are local to this session. Small or empty observation windows cannot prove idle overhead or a leak.')}
    [IO.File]::WriteAllText((Join-Path $directory 'summary.json'),(ConvertTo-Json $summary -Depth 12),(New-Object Text.UTF8Encoding($false)))
    $report = @('# Perfetto analysis', '', ('Trace: `' + $trace + '`'), ('Session: `' + $manifest.sessionId + '`'),
        ('Effective observation window: ' + $duration + ' ms. Evidence valid: ' + $valid + '.'),
        ('Process CPU: ' + $(if($null -eq $cpu){'unavailable'}else{$cpu.ToString('F3',[Globalization.CultureInfo]::InvariantCulture)+' ms/s'}) + '.'),
        '', 'Queries and CSV results are saved beside summary.json. Query hashes bind this report to its fixed SQL package.', '',
        'Open questions: workload, display configuration, actual desktop behavior, and unrecorded participants require separate validation.', '')
    $report += $summary.limitations | ForEach-Object {'- ' + $_}
    [IO.File]::WriteAllLines((Join-Path $directory 'perfetto_analysis_report.md'),[string[]]$report,(New-Object Text.UTF8Encoding($false)))
    [pscustomobject]@{schemaVersion=2;sessionId=$manifest.sessionId;summary=(Join-Path $directory 'summary.json');report=(Join-Path $directory 'perfetto_analysis_report.md');quality=$quality} | ConvertTo-Json -Depth 6 -Compress
}
catch { throw }
