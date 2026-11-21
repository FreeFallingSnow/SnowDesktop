# Resolve packaging inputs from a completed, saved build, never the active
# runtime or the reusable compiler workspace. Source resources come from the
# same content-addressed snapshot as the binaries.
. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
. (Join-Path $PSScriptRoot 'build_inputs.ps1')
. (Join-Path $PSScriptRoot 'build_candidate.ps1')
. (Join-Path $PSScriptRoot 'build_candidate_artifacts.ps1')
Import-Module (Join-Path $PSScriptRoot 'deployment_payload.psm1') -Force

function Invoke-SnowDesktopPackagingBuild([string]$Root) {
    $task='package-'+[Guid]::NewGuid().ToString('N')
    $shell=Get-BuildPowerShell
    $manager=Join-Path $Root 'scripts/build_manager.ps1'
    $runner=Join-Path $Root 'scripts/build_run.ps1'
    $null=& $shell -NoProfile -File $manager task-open $task -TaskName 'Build saved packaging input'
    if($LASTEXITCODE -ne 0){throw 'Cannot register packaging build.'}
    try{
        # invoke streams arbitrary build stdout and deliberately returns no
        # JSON. Use start receipts and the persisted records, never parse logs
        # as identity metadata.
        $capturedText=(& $shell -NoProfile -File $runner -Command start -Action snapshot -Task $task | Out-String)
        if($LASTEXITCODE -ne 0){throw 'Packaging source capture failed; inspect its Run evidence.'}
        $captureReceipt=$capturedText|ConvertFrom-Json
        Wait-SnowDesktopPackagingRun $Root $task $captureReceipt.id
        $captured=Read-EntryJson (Join-Path $Root ('.build/collaboration/runs/'+$captureReceipt.id+'.json'))
        $builtText=(& $shell -NoProfile -File $runner -Command start -Action candidate -CandidateAction release `
            -Snapshot $captured.candidate.id -Task $task | Out-String)
        if($LASTEXITCODE -ne 0){throw 'Saved packaging build failed; inspect its Run evidence.'}
        $buildReceipt=$builtText|ConvertFrom-Json
        Wait-SnowDesktopPackagingRun $Root $task $buildReceipt.id
        $built=Read-EntryJson (Join-Path $Root ('.build/collaboration/runs/'+$buildReceipt.id+'.json'))
        if($built.status -ne 'passed' -or -not $built.artifact){throw 'Packaging build did not produce a passed saved runtime.'}
        return $built.id
    }finally{
        $state=Read-EntryJson (Join-Path $Root '.build/collaboration/state.json')
        $entry=@($state.taskLedger.tasks|Where-Object id -eq $task)
        if($entry.Count -eq 1){
            $null=& $shell -NoProfile -File $manager task-close $task -TaskRevision $entry[0].revision -TaskPhase completed
        }
    }
}

function Wait-SnowDesktopPackagingRun([string]$Root,[string]$Task,[string]$Run) {
    $shell=Get-BuildPowerShell
    do {
        $null=& $shell -NoProfile -File (Join-Path $Root 'scripts/build_run.ps1') -Command wait -RunId $Run -Task $Task -Seconds 60
        $code=$LASTEXITCODE
        if($code -eq 10){Write-Host "Waiting for saved packaging Run $Run..."}
    }while($code -eq 10)
    if($code -ne 0){throw "Packaging Run $Run did not complete successfully (exit $code); evidence is preserved."}
}

function Get-SnowDesktopPackagingEvidence([string]$Root,$Run) {
    if($Run.status -eq 'passed' -and $Run.exitCode -eq 0){return 'passed-run'}
    $product=Get-Field $Run 'productBuild'
    if($Run.status -notin 'failed','cancelled' -or -not $product -or $product.exitCode -ne 0){
        throw 'Packaging requires a passed Run or independently verified product and full application coverage.'
    }
    $request=Read-EntryJson (Join-Path $Root ('.build/collaboration/runs/'+$Run.id+'.request.json'))
    $batch=Get-Field $request 'batch'
    if($batch -notmatch '^[a-f0-9]{32}$' -or $request.action -ne 'batch' -or -not (Get-Field $request 'candidateBatch') -or
        $request.nonce -ne $Run.nonce -or $request.id -ne $Run.id -or $request.repositoryRoot -ne $Root){
        throw 'The product-only success has no matching batch request.'
    }
    . (Join-Path $PSScriptRoot 'build_candidate_batch.ps1')
    $binding=Read-CandidateBatchBinding $Root $request
    $result=Read-EntryJson (Join-Path $Root ('.build/collaboration/'+$batch+'.json'))
    $coverage=Get-Field $result 'coverage'
    if(-not $result -or $result.batchId -ne $batch -or $result.runId -ne $Run.id -or $result.repositoryRoot -ne $Root -or
        $result.inputCheck -ne 'stable' -or $result.outcome -ne $Run.status -or $binding.snapshot -ne $Run.artifact.candidate -or
        $result.artifact.manifestDigest -ne $Run.artifact.manifestDigest -or $result.inputEnd.digest -ne $Run.inputEnd.digest -or
        (Get-Field $coverage 'batchId') -ne $batch -or (Get-Field (Get-Field (Get-Field $coverage 'postChecks') 'outputIsolation') 'status') -ne 'passed'){
        throw 'The product-only success lacks matching stable application evidence and a passing output-isolation check.'
    }
    $tasks=@(Get-Field $coverage 'tasks' @())
    $mixed=@($tasks|Where-Object {$_.suites -contains 'tools' -and @($_.suites).Count -ne 1})
    $app=@($tasks|Where-Object {$_.suites -notcontains 'tools' -and $_.suites -notcontains 'none'})
    $tools=@($tasks|Where-Object {$_.suites -contains 'tools'})
    if($mixed.Count -or -not $tools.Count -or -not @($app|Where-Object {$_.suites -contains 'full' -and $_.status -eq 'passed' -and @($_.requested).Count}).Count){
        throw 'Packaging cannot separate application coverage from this Run. Use a saved Run with distinct, passing full application coverage.'
    }
    $names=@($app|ForEach-Object requested|Sort-Object -Unique)
    $toolsOnly=@($tools|ForEach-Object requested|Where-Object {$_ -notin $names}|Sort-Object -Unique)
    $selected=@(Get-Field $coverage 'selected' @())
    if(@($selected|Where-Object {$_ -notin $names -and $_ -notin $toolsOnly}).Count){throw 'The selection contains tests without a recorded application or tool obligation.'}
    if(-not @($selected|Where-Object {
        $testName=$_
        $testName -in $toolsOnly -and -not @($coverage.completed|Where-Object {$_.name -eq $testName -and $_.status -eq 'passed'}).Count
    }).Count){throw 'The failed Run has no separately identified unsuccessful tool test.'}
    foreach($name in $names){
        $cases=@($coverage.completed|Where-Object name -eq $name)
        if($name -notin $selected -or $cases.Count -ne 1 -or $cases[0].status -ne 'passed'){throw "Application test $name does not have a unique passing result."}
    }
    return 'passed-product-and-application'
}

function Get-SnowDesktopPackagingContext([string]$Root,[string]$ArtifactRun='') {
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $runtimeLease=Open-CandidateRuntimeLease $Root
    try {
    if(-not $ArtifactRun){
        $active=Read-EntryJson (Join-Path $Root '.build/collaboration/activation-current.json')
        if(-not $active -or -not $active.runId){throw 'No saved build is selected. Specify -ArtifactRun after a successful standard build.'}
        $ArtifactRun=$active.runId
    }
    if($ArtifactRun -notmatch '^[a-f0-9]{32}$'){throw 'Invalid packaging ArtifactRun.'}
    $run=Read-EntryJson (Join-Path $Root ('.build/collaboration/runs/'+$ArtifactRun+'.json'))
    if(-not $run -or $run.id -ne $ArtifactRun -or $run.repositoryRoot -ne $Root -or
        -not $run.completedUtc -or
        -not $run.inputStart -or -not $run.inputEnd -or $run.inputStart.digest -ne $run.inputEnd.digest -or
        -not $run.artifact){throw 'Packaging requires a stable, completed build Run with saved artifacts.'}
    $packagingEvidence=Get-SnowDesktopPackagingEvidence $Root $run
    $artifact=Read-CandidateArtifact $Root $ArtifactRun $run.artifact.manifestDigest -VerifyFiles
    if(-not $artifact.manifest.productRuntime -or $artifact.manifest.configuration -ne 'Release' -or
        $artifact.manifest.sourceDigest -ne $run.inputEnd.digest){throw 'Packaging requires the matching saved Release product runtime.'}
    $snapshot=Read-CandidateManifest $Root $artifact.manifest.candidate
    $versionFile=@($snapshot.manifest.files|Where-Object {$_.path -eq 'version.json' -and $_.state -eq 'present'})
    if($versionFile.Count -ne 1){throw 'Packaging snapshot has no version.json.'}
    $versionObject=Resolve-CandidatePath $snapshot.store.root ('objects/'+$versionFile[0].sha256)
    if((Get-FileDigest $versionObject) -ne $versionFile[0].sha256){throw 'Packaging version object changed.'}
    $version=(Read-CandidateJson $versionObject).version
    if($version -notmatch '^[1-9][0-9]*\.[0-9]+\.[0-9]+\.0$'){throw 'Invalid packaging snapshot version.'}
    foreach($part in $version.Split('.')){if([uint64]$part -gt 65535){throw 'Invalid packaging version component.'}}
    $source=Resolve-CandidatePath $Root ('artifacts/v'+$version+'/package-inputs/'+$ArtifactRun+'/source')
    $required=@('version.json','LICENSE','THIRD_PARTY_NOTICES.md','README.md','README.en.md',
        'packaging/steam-identity.json','packaging/AppxManifest.xml.in','assets/icon/icon.png','assets/icon/icon_small.png')
    $required+=@((Get-SnowDesktopRepositoryLicenseMap).Keys)
    $selected=@($snapshot.manifest.files|Where-Object {$_.state -eq 'present' -and ($_.path -in $required -or $_.path.StartsWith('packaging/'))})
    foreach($name in $required){if($name -notin $selected.path){throw "Packaging snapshot is missing source resource: $name"}}
    foreach($file in $selected){
        $object=Resolve-CandidatePath $snapshot.store.root ('objects/'+$file.sha256)
        if(-not [IO.File]::Exists($object) -or (Get-Item -LiteralPath $object).Length -ne $file.bytes -or
            (Get-FileDigest $object) -ne $file.sha256){throw 'Packaging source object changed.'}
        $target=Resolve-CandidatePath $source $file.path
        if(-not [IO.File]::Exists($target)){
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
            [IO.File]::Copy($object,$target,$false)
        }
        if((Get-FileDigest $target) -ne $file.sha256){throw 'Saved packaging source resource changed.'}
    }
    $provenance=[pscustomobject]@{runId=$ArtifactRun;snapshot=$artifact.manifest.candidate;
        sourceDigest=$artifact.manifest.sourceDigest;sourceManifestDigest=$artifact.manifest.sourceManifestDigest;
        capturedHead=$artifact.manifest.sourceHead;artifactManifestDigest=$artifact.digest;evidence=$packagingEvidence}
    return [pscustomobject]@{root=$Root;source=$source;runtime=$artifact.runtime;version=$version;
        provenance=$provenance;sourceFiles=$selected;runtimeLease=$runtimeLease}
    } catch {$runtimeLease.Dispose();throw}
}

function Assert-SnowDesktopPackagingContext($Context) {
    $null=Read-CandidateArtifact $Context.root $Context.provenance.runId $Context.provenance.artifactManifestDigest -VerifyFiles
    foreach($file in $Context.sourceFiles){
        if((Get-FileDigest (Resolve-CandidatePath $Context.source $file.path)) -ne $file.sha256){throw 'Packaging source changed during packaging.'}
    }
}
function Close-SnowDesktopPackagingContext($Context) {
    if($Context -and $Context.PSObject.Properties['runtimeLease']){$Context.runtimeLease.Dispose()}
}
