[CmdletBinding()]
param([Parameter(Mandatory)][string]$ManifestPath,[string]$OutputDirectory,[switch]$Offline)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
$ProgressPreference = 'SilentlyContinue'
. (Join-Path $PSScriptRoot '../tools/performance/ensure_trace_processor.ps1')
. (Join-Path $PSScriptRoot '../tools/performance/bounded_query.ps1')
function Quote-ProcessArgument([string]$Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1') + '"'
}
function Number-OrNull($Value) {
    if ($null -eq $Value -or [string]$Value -in @('','[NULL]')) { return $null }
    return [double]::Parse([string]$Value,[Globalization.CultureInfo]::InvariantCulture)
}
function Sql-Records($Rows) {
    foreach ($row in $Rows) {
        $record = [ordered]@{}
        foreach ($property in $row.PSObject.Properties) {
            $value = [string]$property.Value
            $record[$property.Name] = if ($value -in @('[NULL]','')) { $null }
                elseif ($property.Name -notin @('owner','operation','animation','target') -and $value -match '^-?\d+(\.\d+)?([eE][+-]?\d+)?$') { Number-OrNull $value }
                else { $value }
        }
        [pscustomobject]$record
    }
}
$storageLease = $null
try {
    $manifestFile = [IO.Path]::GetFullPath($ManifestPath)
    if ([IO.Path]::GetExtension($manifestFile) -eq '.pftrace') { $manifestFile = Join-Path ([IO.Path]::GetDirectoryName($manifestFile)) 'session.json' }
    if ((Get-Item -LiteralPath $manifestFile).Length -gt 65536) { throw 'manifestLimit' }
    $manifest = Get-Content -LiteralPath $manifestFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $moduleFilter = $null; $enabledCategories = $null; $inputSampling = $null; $probeFiltering = $null
    if ($manifest.PSObject.Properties['moduleFilter']) {
        $moduleFilter = @($manifest.moduleFilter)
        $enabledCategories = @($manifest.enabledCategories)
        if ($moduleFilter.Count -gt 11 -or $enabledCategories.Count -gt 14 -or
            @($moduleFilter | Where-Object {$_ -notin @('resource','lua','task','frame','input','dock','popup','layout','render','icon','cache')}).Count -or
            $manifest.inputSamplingAlgorithm -ne 'operation-hash-v1' -or
            $manifest.inputSampleEvery -lt 1 -or $manifest.inputSampleEvery -gt 1000 -or
            $manifest.inputSampleEvery -ne [math]::Floor([double]$manifest.inputSampleEvery)) { throw 'captureFilterMetadataInvalid' }
        $probeFiltering = $manifest.probeFiltering
        foreach ($name in @('categoryFilteredCalls','ownerFilteredCalls','inputOperationsObserved','inputOperationsSelected')) {
            $number = [double]$probeFiltering.$name
            if ([double]::IsNaN($number) -or [double]::IsInfinity($number) -or $number -lt 0 -or $number -ne [math]::Floor($number)) { throw 'captureFilterMetadataInvalid' }
        }
        if ($probeFiltering.inputOperationsSelected -gt $probeFiltering.inputOperationsObserved) { throw 'captureFilterMetadataInvalid' }
        $inputSampling = [ordered]@{oneIn=[int]$manifest.inputSampleEvery;algorithm=$manifest.inputSamplingAlgorithm
            operationsObserved=$probeFiltering.inputOperationsObserved;operationsSelected=$probeFiltering.inputOperationsSelected
            interpretation='Capture-active per-producer synchronous operation-group observations, not OS input counts or the SQL slice window. Nested input scopes agree. Selected groups are probe decisions; consult trace loss/boundary quality. No scaled estimates.'}
    }
    if ($manifest.schemaVersion -ne 2 -or $manifest.format -ne 'perfetto' -or -not $manifest.completion -or $manifest.state -ne 'completed') { throw 'captureIncompleteOrUnsupported' }
    if (-not $manifest.PSObject.Properties['probeSchemaVersion'] -or $manifest.probeSchemaVersion -notin @(2,3)) { throw 'unsupportedProbeSchema: record a new native capture; historical JSON remains supported through report.' }
    if ($manifest.traceFile -ne [IO.Path]::GetFileName($manifest.traceFile) -or [IO.Path]::GetExtension($manifest.traceFile) -ne '.pftrace') { throw 'manifestTracePathRejected' }
    $trace = Join-Path ([IO.Path]::GetDirectoryName($manifestFile)) $manifest.traceFile
    if ((Get-Item -LiteralPath $trace).Length -gt 268435456 -or
        (Get-FileHash -LiteralPath $trace -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.traceSha256) { throw 'traceFingerprintMismatch' }
    $processor = Get-SnowDesktopTraceProcessor -Offline:$Offline
    if (-not $OutputDirectory) { $OutputDirectory = Join-Path ([IO.Path]::GetDirectoryName($manifestFile)) ('analysis-' + [Guid]::NewGuid().ToString('N')) }
    $directory = [IO.Path]::GetFullPath($OutputDirectory)
    if (Test-Path -LiteralPath $directory) { throw 'analysisOutputAlreadyExists' }
    if ($manifest.PSObject.Properties['managedStorage'] -and $manifest.managedStorage -and
        ([IO.DirectoryInfo][IO.Path]::GetDirectoryName($manifestFile)).Name -eq $manifest.sessionId) {
        $storage = [IO.Directory]::GetParent([IO.Path]::GetDirectoryName($manifestFile)).FullName
        $lockFile = Join-Path $storage '.capture-storage.lock'
        if ((Test-Path -LiteralPath $lockFile) -and ((Get-Item -LiteralPath $lockFile).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'captureStoragePathRejected' }
        $storageLease = [IO.File]::Open($lockFile,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        if ($storageLease.Length -ne 0) { throw 'captureStorageLockInvalid' }
        [long]$bytes = 0; $entries = 0
        $pending = [Collections.Generic.Stack[string]]::new(); $pending.Push($storage)
        while ($pending.Count) {
            foreach ($item in [IO.Directory]::EnumerateFileSystemEntries($pending.Pop())) {
                if (++$entries -gt 10000) { throw 'captureStorageEntryLimit' }
                $attributes = [IO.File]::GetAttributes($item)
                if ($attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'captureStoragePathRejected' }
                if ($attributes -band [IO.FileAttributes]::Directory) { $pending.Push($item) }
                else { $bytes += ([IO.FileInfo]$item).Length; if ($bytes -gt 1073741824) { throw 'captureStorageQuota' } }
            }
        }
        if ($bytes + 67108864 -gt 1073741824) { throw 'analysisStorageQuota' }
    }
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $sqlRoot = Join-Path $PSScriptRoot '../tools/performance/sql'
    $common = Get-Content -LiteralPath (Join-Path $sqlRoot 'common.sql') -Raw -Encoding UTF8
    $tables = @{}
    $queryHashes = [ordered]@{}
    foreach ($name in @('quality','scopes','modules','counters','diagnostics','events','widgets','tasks','animations','compositor')) {
        $sqlPath = Join-Path $sqlRoot ($name + '.sql')
        $queryHashes[$name] = (Get-FileHash -LiteralPath $sqlPath -Algorithm SHA256).Hash.ToLowerInvariant()
        $query = Join-Path $directory ($name + '.sql')
        $sql = $common + "`n" + (Get-Content -LiteralPath $sqlPath -Raw -Encoding UTF8)
        if ([Text.Encoding]::UTF8.GetByteCount($sql) -gt 32768) { throw 'analysisQuerySizeLimit' }
        [IO.File]::WriteAllText($query,$sql,(New-Object Text.UTF8Encoding($false)))
        $csvPath = Join-Path $directory ($name + '.csv')
        [SnowDesktop.Performance.BoundedQuery]::Run($processor,('query -f ' + (Quote-ProcessArgument $query) + ' ' + (Quote-ProcessArgument $trace)),
            $csvPath,(Join-Path $directory ($name + '.log')))
        $tables[$name] = @(Import-Csv -LiteralPath $csvPath -Encoding UTF8)
    }
    $queryHashes.common = (Get-FileHash -LiteralPath (Join-Path $sqlRoot 'common.sql') -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($tables.quality.Count -ne 1) { throw 'analysisQualityRowMissing' }
    $q = $tables.quality[0]
    $duration = Number-OrNull $q.durationMs
    $probeErrors = [double]$manifest.probeQuality.droppedOwners + [double]$manifest.probeQuality.droppedCounters +
        [double]$manifest.probeQuality.droppedLinks + [double]$manifest.probeQuality.probeErrors
    if ($manifest.probeQuality.PSObject.Properties['droppedTracks']) { $probeErrors += [double]$manifest.probeQuality.droppedTracks }
    $valid = $null -ne $duration -and $duration -gt 0 -and [int]$q.starts -eq 1 -and [int]$q.stops -eq 1 -and [double]$q.lossOrErrors -eq 0 -and $probeErrors -eq 0
    $truncated = [int]$q.scopeGroups -gt 10000 -or [int]$q.counterTracks -gt 10000 -or [int]$q.animations -gt 10000 -or [int]$q.queuedTasks -gt 10000 -or [int]$q.eventGroups -gt 10000
    $valid = $valid -and -not $truncated
    $valid = $valid -and [int]$q.invalidCpuSlices -eq 0
    $drawCorrelation = $null
    if ($manifest.probeSchemaVersion -ge 3) {
        $drawCorrelation = [ordered]@{}
        foreach ($name in @('drawRequests','drawCoalesced','drawConsumed','drawPending')) {
            $value = [double]$manifest.probeQuality.$name
            if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0 -or $value -ne [math]::Floor($value)) { throw 'invalidDrawAccounting' }
            $drawCorrelation[$name] = $value
        }
        $drawCorrelation.accounted = $drawCorrelation.drawRequests -eq ($drawCorrelation.drawConsumed + $drawCorrelation.drawPending) -and $drawCorrelation.drawCoalesced -le $drawCorrelation.drawRequests
        $valid = $valid -and $drawCorrelation.accounted
    }
    $quality = [ordered]@{valid=$valid;lossOrErrors=[double]$q.lossOrErrors;probeErrors=$probeErrors
        reportTruncated=$truncated
        scopeCoverage=if([int]$q.incompleteSlices -gt 0 -or [int]$q.boundarySlices -gt 0){'partial'}else{'complete_recorded'}
        unattributedServiceDiscardedChunks=[double]$q.serviceChunksDiscarded
        unattributedServiceDiscardedPatches=[double]$q.servicePatchesDiscarded
        incompleteSlices=[int]$q.incompleteSlices;boundarySlices=[int]$q.boundarySlices;missingCpuSlices=[int]$q.missingCpuSlices;invalidCpuSlices=[int]$q.invalidCpuSlices
        drawCorrelation=$drawCorrelation
        window='SDK capture markers; only complete contained slices; resource rates use their actual first/last sample window'}
    $scopes = @($tables.scopes | ForEach-Object {
        [pscustomobject]@{module=$_.module;phase=$_.phase;owner=$_.owner;calls=[int]$_.calls
            wallMs=(Number-OrNull $_.wallMs);selfWallMs=(Number-OrNull $_.selfWallMs)
            p50WallMs=(Number-OrNull $_.p50WallMs);p95WallMs=(Number-OrNull $_.p95WallMs);p99WallMs=(Number-OrNull $_.p99WallMs)
            maxWallMs=(Number-OrNull $_.maxWallMs);inclusiveCpuMs=(Number-OrNull $_.inclusiveCpuMs);selfCpuMs=$null;cpuSamples=[int]$_.cpuSamples}
    })
    $modules = @($tables.modules | ForEach-Object { [pscustomobject]@{module=$_.module;calls=[int]$_.calls;selfWallMs=(Number-OrNull $_.selfWallMs);selfCpuMs=$null
        rootInclusiveCpuMs=(Number-OrNull $_.rootInclusiveCpuMs);rootScopes=[int]$_.rootScopes;rootCpuSamples=[int]$_.rootCpuSamples} })
    $cumulative = @('cpu_total_ms','page_fault_count','io_read_bytes','io_write_bytes','io_other_bytes','io_read_operations','io_write_operations',
        'lua_allocated_bytes_total','lua_freed_bytes_total','lua_allocations_total','lua_frees_total','lua_reallocations_total',
        'surface_reclaim_count','surface_reclaimed_bytes','history_skipped_total','query_failures_total','snapshot.bitmap_allocations',
        'raster_ms_total','ranges_total','samples_total','disjoint_total','errors_total','skipped_total')
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
    $resourceTrends = @($counters | Where-Object {$_.semantics -eq 'gauge' -and ($_.module -like '*memory*' -or $_.metric -in @('handle_count','private_commit_bytes','working_set_bytes'))} | ForEach-Object {
        $growth = if ($valid -and $_.samples -ge 2 -and $_.sampleWindowMs -gt 0) {$_.last-$_.first} else {$null}
        [pscustomobject]@{module=$_.module;metric=$_.metric;owner=$_.owner;first=$_.first;last=$_.last;peak=$_.peak
            sampleWindowMs=$_.sampleWindowMs;growth=$growth;growthPerSecond=if($null -ne $growth){$growth*1000/$_.sampleWindowMs}else{$null}
            interpretation='Observed window only; growth is not a leak diagnosis.'}
    })
    $cpu = if ($cpuRows.Count -eq 1) {$cpuRows[0].ratePerSecond} else {$null}
    $gpuMissingOwners = @()
    $widgetGpu = @($counters | Where-Object module -eq 'widget.gpu' | Group-Object owner | ForEach-Object {
        $metrics = @{}; foreach ($row in $_.Group) { $metrics[$row.metric] = $row }
        $missing = @('raster_ms_total','ranges_total','samples_total','disjoint_total','errors_total','skipped_total') | Where-Object { -not $metrics.ContainsKey($_) }
        if (@($missing).Count) { $gpuMissingOwners += $_.Name; return }
        $samples = $metrics['samples_total'].last
        [pscustomobject]@{owner=$_.Name;rasterMs=$metrics['raster_ms_total'].delta
            sampleWindowMs=$metrics['raster_ms_total'].sampleWindowMs;rasterMsPerSecond=$metrics['raster_ms_total'].ratePerSecond
            samples=$samples;ranges=$metrics['ranges_total'].last;disjoint=$metrics['disjoint_total'].last
            errors=$metrics['errors_total'].last;skipped=$metrics['skipped_total'].last
            pending=$metrics['ranges_total'].last-$samples-$metrics['disjoint_total'].last-$metrics['errors_total'].last-$metrics['skipped_total'].last
            meanObservedMs=if($valid -and $samples -gt 0){$metrics['raster_ms_total'].last/$samples}else{$null}
            interpretation='Observed timestamp intervals for host widget raster submission only; adapter scheduling may stretch them. No GPU utilization percentage, DWM/native-backdrop work, presentation FPS, or extrapolation of pending/skipped/disjoint/error samples.'}
    })
    $summary = [ordered]@{schemaVersion=2;queryVersion=3;probeSchemaVersion=$manifest.probeSchemaVersion;sdkVersion=$manifest.sdkVersion;sessionId=$manifest.sessionId
        hostVersion=$manifest.hostVersion;executableSha256=$manifest.executableSha256;sourceCommit=$manifest.sourceCommit
        environment=$manifest.environment;selectedInstances=$manifest.selectedInstances
        moduleFilter=$moduleFilter;enabledCategories=$enabledCategories;inputDetailSampling=$inputSampling;probeFiltering=$probeFiltering
        finalWidgetSampleConfirmed=$manifest.finalWidgetSampleConfirmed
        threadCpuSource=$manifest.threadCpuSource;widgetGpu=$widgetGpu;widgetGpuMissingOwners=@($gpuMissingOwners)
        processId=$manifest.processId;processRole=$manifest.processRole;preset=$manifest.preset;scopeCpuEnabled=$manifest.scopeCpuEnabled
        configHash=$manifest.configHash;traceSha256=$manifest.traceSha256;manifestSha256=(Get-FileHash -LiteralPath $manifestFile -Algorithm SHA256).Hash.ToLowerInvariant()
        queryHashes=$queryHashes;effectiveDurationMs=$duration;quality=$quality;participants=$manifest.participants
        processCpuMsPerSecond=$cpu;processCpuSingleCorePercent=if($null -ne $cpu){$cpu/10}else{$null}
        processCpuNormalizedPercent=if($null -ne $cpu -and $manifest.logicalProcessors -gt 0){$cpu/10/$manifest.logicalProcessors}else{$null}
        scopes=$scopes;modules=$modules;counters=$counters
        events=@(Sql-Records $tables.events);widgets=@(Sql-Records $tables.widgets);tasks=@(Sql-Records $tables.tasks)
        animations=@(Sql-Records $tables.animations);compositor=@(Sql-Records $tables.compositor)
        resourceTrends=$resourceTrends;processWakeupsPerSecond=$null
        limitations=@('One measured process only; its role and excluded processes are listed in the manifest.',
            'Inclusive thread CPU uses the manifest clock source. Windows GetThreadTimes retains OS accounting quantization: tiny individual scopes may read zero or exceed their wall interval by a quantum. Missing CPU stays null; self CPU is not inferred.',
            'Module and instance CPU use outermost synchronous scopes per group. They remain inclusive views, overlap across groups, include native/shared child work, and must not be added as exclusive CPU. Lua protected calls are not pure VM CPU.',
            'Wall time includes waiting and nested work. App callbacks, updates and commits do not measure OS wakeups or presented frames.',
            'Process GPU-engine counters are not divided among widgets. Separate D3D11 timestamp ranges measure observed host raster intervals per component, exclude DWM/native-backdrop work, and report pending, skipped, disjoint and error samples explicitly.',
            'Compositor Clock observes display composition only; independent flip may be missing. It cannot attribute presented FPS or dropped frames to an animation.',
            'Lua allocation traffic counts successful logical allocator requests, including realloc old/new sizes. It is not physical allocation traffic or proof of a leak.',
            'Opaque instance IDs are local to this session. Small or empty observation windows cannot prove idle overhead or a leak.')}
    $summaryJson = ConvertTo-Json $summary -Depth 12
    if ([Text.Encoding]::UTF8.GetByteCount($summaryJson) -gt 16777216) { throw 'analysisSummaryLimit' }
    [IO.File]::WriteAllText((Join-Path $directory 'summary.json'),$summaryJson,(New-Object Text.UTF8Encoding($false)))
    $report = @('# Perfetto analysis', '', ('Trace: `' + $trace + '`'), ('Session: `' + $manifest.sessionId + '`'),
        ('Effective observation window: ' + $duration + ' ms. Evidence valid: ' + $valid + '.'),
        ('Process CPU: ' + $(if($null -eq $cpu){'unavailable'}else{$cpu.ToString('F3',[Globalization.CultureInfo]::InvariantCulture)+' ms/s'}) + '.'),
        ('Module filter: ' + $(if($null -eq $moduleFilter){'unknown (older capture)'}elseif($moduleFilter.Count){$moduleFilter -join ', '}else{'preset default'}) + '; resource and profiler markers are retained.'),
        ('Input detail sampling: ' + $(if($null -eq $inputSampling){'unknown (older capture)'}else{'one in '+$inputSampling.oneIn+' by operation hash; '+$inputSampling.operationsSelected+' / '+$inputSampling.operationsObserved+' operation-group observations selected; category-filtered probe calls '+$probeFiltering.categoryFilteredCalls+'; owner-filtered probe calls '+$probeFiltering.ownerFilteredCalls}) + '. No extrapolation to full input counts or latency distributions.'),
        '', 'Queries and CSV results are saved beside summary.json. Query hashes bind this report to its fixed SQL package.', '',
        'Open questions: workload, display configuration, actual desktop behavior, and unrecorded participants require separate validation.', '')
    $report += $summary.limitations | ForEach-Object {'- ' + $_}
    $report += @('', '## Recorded module work', '', '| Module | Calls | Recorded self wall (ms) | Root inclusive CPU (ms) |', '| --- | ---: | ---: | ---: |')
    foreach ($row in @($modules | Select-Object -First 20)) { $report += ('| ' + $row.module + ' | ' + $row.calls + ' | ' + $row.selfWallMs + ' | ' + $row.rootInclusiveCpuMs + ' |') }
    if ($null -ne $drawCorrelation) {
        $report += @('', ('Draw requests: ' + $drawCorrelation.drawRequests + '; coalesced: ' + $drawCorrelation.drawCoalesced + '; consumed: ' + $drawCorrelation.drawConsumed + '; pending at boundary: ' + $drawCorrelation.drawPending + '; accounted: ' + $drawCorrelation.accounted + '.'))
    }
    if ($widgetGpu.Count) {
        $report += @('', '## Widget raster GPU timestamp intervals', '', '| Anonymous owner | Observed GPU ms | Valid samples | Pending | Disjoint | Errors | Skipped |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: |')
        foreach ($row in $widgetGpu) { $report += ('| ' + $row.owner + ' | ' + $row.rasterMs + ' | ' + $row.samples + ' | ' + $row.pending + ' | ' + $row.disjoint + ' | ' + $row.errors + ' | ' + $row.skipped + ' |') }
    }
    if ($gpuMissingOwners.Count) { $report += @('', ('GPU owners with incomplete counter sets (no inferred values): ' + ($gpuMissingOwners -join ', '))) }
    $report += @('', '## Resource observations', '', '| Module / metric / anonymous owner | First | Last | Peak | Growth |', '| --- | ---: | ---: | ---: | ---: |')
    foreach ($row in @($resourceTrends | Sort-Object peak -Descending | Select-Object -First 20)) { $report += ('| ' + $row.module + ' / ' + $row.metric + ' / ' + $row.owner + ' | ' + $row.first + ' | ' + $row.last + ' | ' + $row.peak + ' | ' + $row.growth + ' |') }
    $report += @('', '## Animation and queue evidence', '', ('Animation groups: ' + $tables.animations.Count + '; task groups: ' + $tables.tasks.Count + '; composed display targets: ' + $tables.compositor.Count + '.'),
        'animations.csv reports application update cadence and deadline misses. compositor.csv reports separate display composition evidence. Presented FPS and dropped frames per animation remain unavailable.',
        'tasks.csv reports queue wait, synchronous execution wall and completion latency where both boundaries exist. Scope CPU is reported separately; elapsed execution includes waits.')
    [IO.File]::WriteAllLines((Join-Path $directory 'perfetto_analysis_report.md'),[string[]]$report,(New-Object Text.UTF8Encoding($false)))
    [pscustomobject]@{schemaVersion=2;sessionId=$manifest.sessionId;summary=(Join-Path $directory 'summary.json');report=(Join-Path $directory 'perfetto_analysis_report.md');quality=$quality} | ConvertTo-Json -Depth 6 -Compress
}
catch { throw }
finally { if ($null -ne $storageLease) { $storageLease.Dispose() } }
