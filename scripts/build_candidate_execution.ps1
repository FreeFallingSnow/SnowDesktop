# Explicit candidate Runs execute a captured toolset and source tree. The
# originating checkout is only the durable request/result/event home.
function New-CandidateRunner([string]$Root,[string]$Id) {
    $directory=Resolve-CandidatePath $Root ('.build/collaboration/runners/'+$Id)
    [void][IO.Directory]::CreateDirectory((Join-Path $directory 'scripts'))
    $handles=@();$files=@()
    try {
        foreach($file in @(Get-ChildItem -LiteralPath (Join-Path $Root 'scripts') -File|Sort-Object Name)){
            if($file.Extension -notin '.ps1','.psm1','.cs','.bat','.json','.py'){continue}
            $stream=[IO.File]::Open($file.FullName,'Open','Read',[IO.FileShare]::Read)
            $handles+=@([pscustomobject]@{name=$file.Name;stream=$stream})
        }
        foreach($item in $handles){
            $path=Join-Path $directory ('scripts/'+$item.name)
            $output=[IO.File]::Open($path,'CreateNew','Write','None')
            try{$item.stream.CopyTo($output);$output.Flush($true)}finally{$output.Dispose()}
            $files+=@([pscustomobject]@{name=$item.name;sha256=(Get-FileDigest $path)})
        }
        return [pscustomobject]@{schemaVersion=1;directory=$directory;files=$files;digest=(Get-VerificationTextDigest ($files|ConvertTo-Json -Depth 4 -Compress))}
    }finally{foreach($item in $handles){$item.stream.Dispose()}}
}
function Assert-CandidateRunner([string]$Root,$Request,[string]$ScriptDirectory) {
    $runtime=$Request.runnerRuntime
    $expected=Resolve-CandidatePath $Root ('.build/collaboration/runners/'+$Request.id)
    if($runtime.schemaVersion -ne 1 -or $runtime.directory -ne $expected -or
        $ScriptDirectory -ne (Join-Path $expected 'scripts') -or
        $runtime.digest -ne (Get-VerificationTextDigest ($runtime.files|ConvertTo-Json -Depth 4 -Compress))){throw 'Candidate runner identity mismatch.'}
    $locks=@()
    try {
        foreach($file in $runtime.files){
            if($file.name -ne [IO.Path]::GetFileName($file.name) -or $file.sha256 -notmatch '^[a-f0-9]{64}$'){throw 'Invalid candidate runner file.'}
            $path=Resolve-CandidatePath $expected ('scripts/'+$file.name)
            $stream=[IO.File]::Open($path,'Open','Read',[IO.FileShare]::Read);$locks+=@($stream)
            if((Get-FileDigest $path) -ne $file.sha256){throw 'Candidate runner files changed after submission.'}
        }
        return ,$locks
    }catch{foreach($stream in $locks){$stream.Dispose()};throw}
}
function Assert-CandidateToolset($Snapshot,$Runtime) {
    $required=@('build_entry.ps1','build_inputs.ps1','build_telemetry.ps1','build_candidate_execution.ps1','build_candidate.ps1','build_job.cs','build_command.ps1','test_manager.ps1','build_run.ps1','build_protocol.ps1')
    foreach($name in @($required)+@($Runtime.files.name)|Sort-Object -Unique){
        $source=@($Snapshot.manifest.files|Where-Object path -eq ('scripts/'+$name))
        $runner=@($Runtime.files|Where-Object name -eq $name)
        if($source.Count -ne 1 -or $source[0].state -ne 'present' -or $runner.Count -ne 1 -or $source[0].sha256 -ne $runner[0].sha256){
            throw 'Snapshot requires a matching candidate execution toolset; capture the updated runtime first.'
        }
    }
}
function Initialize-CandidateOutput($Slot,[string]$Configuration,[switch]$Existing) {
    # Standard presets/scripts keep their .build paths. Both aliases resolve to
    # this configuration-specific slot output, never the originating checkout.
    foreach($name in @('.build','.build_debug')){
        $path=[IO.Path]::GetFullPath((Join-Path $Slot.source $name))
        $expected=[IO.Path]::GetFullPath($Slot.output)
        if(-not $path.StartsWith($Slot.source+'\',[StringComparison]::OrdinalIgnoreCase) -or
            -not $expected.StartsWith($Slot.directory+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Candidate output escaped its leased slot.'}
        if(Test-Path -LiteralPath $path){
            $item=Get-Item -LiteralPath $path -Force
            if(-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or @($item.Target).Count -ne 1 -or
                [IO.Path]::GetFullPath([string]@($item.Target)[0]) -ne $expected){throw 'Candidate output alias changed; preserve the slot for investigation.'}
        }elseif($Existing){throw 'Verification candidate output alias is missing.'}
        else{New-Item -ItemType Junction -Path $path -Target $expected -ErrorAction Stop|Out-Null}
    }
    if(-not $Existing){[void][IO.Directory]::CreateDirectory((Join-Path $Slot.output 'collaboration/runs'))}
}
function Enter-CandidateExecution([string]$Root,$Request,[switch]$RequireExisting) {
    $snapshot=Read-CandidateManifest $Root $Request.snapshot
    if($snapshot.digest -ne $Request.snapshotManifestDigest -or $snapshot.manifest.context.binding.configuration -ne $Request.configuration){throw 'Candidate snapshot/configuration identity changed.'}
    Assert-CandidateToolset $snapshot $Request.runnerRuntime
    $slot=$null;$entry=$null;$inputLocks=@()
    try {
        $slot=Enter-CandidateSlot $snapshot $Request.candidateSlot
        $snapshot=Read-CandidateManifest $Root $Request.snapshot -VerifyObjects -StoreContext $snapshot.store
        if($RequireExisting){
            $old=Read-CandidateJson $slot.state
            if($old.snapshot -ne $Request.snapshot -or $old.manifestDigest -ne $Request.snapshotManifestDigest){throw 'Verification candidate slot changed; use a matching new baseline.'}
            $prepared=[pscustomobject]@{snapshot=$old.snapshot;slot=$slot.key;source=$slot.source;output=$slot.output;copied=0;kept=@($old.files).Count;removed=0;noBuild=$true}
        }else{$prepared=Set-CandidateSlotSource $snapshot $slot}
        Initialize-CandidateOutput $slot $Request.configuration -Existing:$RequireExisting
        $buildLock=[IO.File]::Open((Join-Path $slot.output 'collaboration/build.lock'),'OpenOrCreate','ReadWrite','None')
        $entry=[pscustomobject]@{lease=$buildLock;oldToken=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')}
        $context=[pscustomobject]@{schemaVersion=1;runRoot=$Root;runId=$Request.id;nonce=$Request.nonce;snapshot=$snapshot.manifest.id;
            manifest=$snapshot.path;manifestDigest=$snapshot.digest;source=$slot.source;output=$slot.output;slot=$slot.key;configuration=$Request.configuration}
        if((Get-Field $Request 'candidateBatch' $false)){Set-Field $context 'batchId' $Request.batch}
        $null=Start-EntryCredential $slot.source '' $context
        # The caller retains every captured source handle for the whole Run.
        foreach($file in $snapshot.manifest.files){
            $path=Resolve-CandidatePath $slot.source $file.path
            if($file.state -eq 'present'){$inputLocks+=@([IO.File]::Open($path,'Open','Read',[IO.FileShare]::Read))}
        }
        $identity=Get-CandidateExecutionInputIdentity $slot.source
        if($identity.digest -ne $snapshot.manifest.sourceDigest){throw 'Candidate source changed before execution.'}
        return [pscustomobject]@{entry=$entry;slot=$slot;sourceLocks=$inputLocks;context=$context;prepared=$prepared;inputIdentity=$identity}
    }catch{
        foreach($stream in $inputLocks){$stream.Dispose()}
        if($entry){Exit-BuildEntry $entry}
        if($slot){$slot.lease.Dispose()}
        throw
    }
}
function Exit-CandidateExecution($Execution) {
    if(-not $Execution){return}
    foreach($stream in $Execution.sourceLocks){$stream.Dispose()}
    Exit-BuildEntry $Execution.entry
    $Execution.slot.lease.Dispose()
}
function Get-CandidateExecutionInputIdentity([string]$Root) {
    $context=Get-CandidateEntryContext $Root
    if(-not $context){throw 'Candidate inputs require their live private execution credential.'}
    Initialize-CandidateOutput ([pscustomobject]@{source=$context.source;output=$context.output;directory=[IO.Path]::GetDirectoryName($context.source)}) $context.configuration
    return Get-CandidateSourceIdentity $Root $context
}
function Get-CandidateSourceIdentity([string]$Root,$Context) {
    # This read-only provenance check installs no execution credential.
    $context=$Context
    $snapshot=Read-CandidateManifest $context.runRoot $context.snapshot
    $directory=Resolve-CandidatePath $snapshot.store.slots $context.slot
    if($Root -ne (Resolve-CandidatePath $directory 'source') -or $context.source -ne $Root -or
        $context.output -ne (Resolve-CandidatePath $directory 'output')){throw 'Verification candidate path changed.'}
    $material=Read-CandidateJson (Resolve-CandidatePath $directory 'materialization.json')
    if($material.snapshot -ne $context.snapshot -or $material.manifestDigest -ne $context.manifestDigest -or
        $material.sourceDigest -ne $snapshot.manifest.sourceDigest){throw 'Verification candidate slot changed; use a matching new baseline.'}
    Initialize-CandidateOutput ([pscustomobject]@{source=$context.source;output=$context.output;directory=$directory}) $context.configuration -Existing
    if($snapshot.digest -ne $context.manifestDigest -or $snapshot.path -ne $context.manifest){throw 'Candidate source manifest changed.'}
    $actual=Get-CandidateContext $snapshot.manifest.files $context.configuration
    if($actual.digest -ne $snapshot.manifest.context.digest){throw 'Candidate observed environment/tools changed.'}
    $files=@();$known=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($file in $snapshot.manifest.files){
        $path=Resolve-CandidatePath $Root $file.path;[void]$known.Add($file.path.Replace('/','\'))
        if($file.state -eq 'present'){
            if(-not [IO.File]::Exists($path)){throw 'Candidate source input is missing.'}
            $digest=Get-FileDigest $path
            if($digest -ne $file.sha256){throw 'Candidate source input changed during execution.'}
        }elseif(Test-Path -LiteralPath $path){throw 'Candidate deleted input was recreated.'}
        $files+=@($file)
    }
    $directories=[Collections.Generic.Stack[string]]::new();$directories.Push($Root)
    while($directories.Count){
        $directory=$directories.Pop()
        foreach($item in [IO.Directory]::EnumerateFileSystemEntries($directory)){
            $relative=$item.Substring($Root.Length+1)
            if($directory -eq $Root -and $relative -in '.build','.build_debug'){continue}
            $attributes=[IO.File]::GetAttributes($item)
            if($attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Unexpected candidate source reparse point.'}
            if($attributes -band [IO.FileAttributes]::Directory){$directories.Push($item)}elseif(-not $known.Contains($relative)){throw 'Unexpected candidate source input.'}
        }
    }
    return [pscustomobject]@{algorithm='sha256-candidate-source-v1';digest=(Get-CandidateSourceDigest $files $actual.digest);fileCount=$files.Count;
        head=$snapshot.manifest.head;candidate=$snapshot.manifest.id;manifestDigest=$snapshot.digest;externalDependenciesClosed=$false}
}
function Write-CandidateTestCoverage([string]$Root,$Selection,[string]$Report,[int]$Code) {
    $context=Get-CandidateEntryContext $Root
    if(-not $context){return}
    $definitions=@(Get-VerificationDefinitions $Selection.Tests)
    $binaries=@(foreach($test in $Selection.Tests){
        $command=@(Get-Field $test 'command' @())
        if($command.Count -and [IO.File]::Exists([string]$command[0])){[pscustomobject]@{test=$test.name;path=[string]$command[0];sha256=(Get-FileDigest ([string]$command[0]))}}
    })
    $data=[pscustomobject]@{schemaVersion=1;candidate=$context.snapshot;runId=$context.runId;sourceDigest=(Get-CandidateExecutionInputIdentity $Root).digest;
        selected=@($Selection.Tests.name);definitions=$definitions;binaries=$binaries;runtimeFiles=@(Get-VerificationRuntimeFiles $Root $Selection.Tests);
        report=$Report;reportSha256=$(if([IO.File]::Exists($Report)){Get-FileDigest $Report}else{$null});exitCode=$Code;reuseAllowed=$false}
    $suffix=if(Get-Field $context 'batchId'){'.candidate-tests.'+(Get-VerificationTextDigest $Report)+'.json'}else{'.candidate-tests.json'}
    Write-CandidateJson $data (Join-Path $context.runRoot ('.build/collaboration/runs/'+$context.runId+$suffix)) -Immutable
}
