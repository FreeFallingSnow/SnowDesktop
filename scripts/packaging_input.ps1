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

function Get-SnowDesktopPackagingContext([string]$Root,[string]$ArtifactRun='') {
    $Root=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    if(-not $ArtifactRun){
        $active=Read-EntryJson (Join-Path $Root '.build/collaboration/activation-current.json')
        if(-not $active -or -not $active.runId){throw 'No saved build is selected. Specify -ArtifactRun after a successful standard build.'}
        $ArtifactRun=$active.runId
    }
    if($ArtifactRun -notmatch '^[a-f0-9]{32}$'){throw 'Invalid packaging ArtifactRun.'}
    $run=Read-EntryJson (Join-Path $Root ('.build/collaboration/runs/'+$ArtifactRun+'.json'))
    if(-not $run -or $run.id -ne $ArtifactRun -or $run.repositoryRoot -ne $Root -or
        $run.status -ne 'passed' -or $run.exitCode -ne 0 -or -not $run.completedUtc -or
        -not $run.inputStart -or -not $run.inputEnd -or $run.inputStart.digest -ne $run.inputEnd.digest -or
        -not $run.artifact){throw 'Packaging requires a passed, stable build Run with saved artifacts.'}
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
        'packaging/steam-identity.json','packaging/AppxManifest.xml.in','assets/icon/icon.png','assets/icon/icon_small.png',
        'third_party/everything/LICENSE.txt','third_party/fluentui-system-icons/LICENSE','third_party/microsoft-ui-xaml/LICENSE',
        'third_party/font-awesome/LICENSE.txt','third_party/imgui/LICENSE.txt','third_party/lua/LICENSE.txt',
        'third_party/minhook/LICENSE.txt','third_party/pinyin-data/LICENSE')
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
        capturedHead=$artifact.manifest.sourceHead;artifactManifestDigest=$artifact.digest}
    return [pscustomobject]@{root=$Root;source=$source;runtime=$artifact.runtime;version=$version;
        provenance=$provenance;sourceFiles=$selected}
}

function Assert-SnowDesktopPackagingContext($Context) {
    $null=Read-CandidateArtifact $Context.root $Context.provenance.runId $Context.provenance.artifactManifestDigest -VerifyFiles
    foreach($file in $Context.sourceFiles){
        if((Get-FileDigest (Resolve-CandidatePath $Context.source $file.path)) -ne $file.sha256){throw 'Packaging source changed during packaging.'}
    }
}
