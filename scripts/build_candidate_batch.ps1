# v4 batches keep immutable execution attempts outside the live editing window.
# Capturing needs the original output lease; queuing/building use a private slot.
function Read-CandidateBatchBinding([string]$Root,$Request) {
    $path=Join-Path $Root ('.build/collaboration/runs/'+$Request.id+'.candidate-binding.json')
    $binding=Read-EntryJson $path
    if(-not $binding -or $binding.schemaVersion -ne 1 -or $binding.repositoryRoot -ne $Root -or
        $binding.runId -ne $Request.id -or $binding.nonce -ne $Request.nonce -or $binding.batchId -ne $Request.batch -or
        $binding.planHash -ne $Request.planHash -or $binding.snapshot -notmatch '^[a-f0-9]{32}$' -or
        $binding.manifestDigest -notmatch '^[a-f0-9]{64}$' -or $binding.sourceDigest -notmatch '^[a-f0-9]{64}$'){
        throw 'The candidate batch has no matching immutable source binding.'
    }
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne [IO.File]::ReadAllText($path+'.sha256').Trim()){throw 'Candidate batch source binding changed.'}
    return $binding
}
function New-CandidateBatchCapture([string]$Root,$Request,$Record,[scriptblock]$CheckCancellation) {
    # Enter-BatchRun has already fenced cooperating editors and installed the
    # Run-owned original-checkout credential. No heavy capacity is held here.
    $before=Get-BuildInputIdentity $Root
    if($before.digest -ne $Request.batchInputDigest){throw 'Frozen batch inputs changed before source capture.'}
    $snapshot=New-CandidateSnapshot $Root $Request.task $Request.requirementsRevision 'Release' '' $before.digest $CheckCancellation
    Assert-CandidateToolset (Read-CandidateManifest $Root $snapshot.id) $Request.runnerRuntime
    $binding=[pscustomobject]@{schemaVersion=1;repositoryRoot=$Root;batchId=$Request.batch;runId=$Request.id;nonce=$Request.nonce;
        planHash=$Request.planHash;snapshot=$snapshot.id;manifestDigest=$snapshot.manifestDigest;sourceDigest=$snapshot.sourceDigest;
        originalInputDigest=$before.digest;createdUtc=[DateTime]::UtcNow.ToString('o')}
    $path=Join-Path $Root ('.build/collaboration/runs/'+$Request.id+'.candidate-binding.json')
    Write-CandidateJson $binding $path -Immutable
    [IO.File]::WriteAllText($path+'.sha256',(Get-FileDigest $path),[Text.UTF8Encoding]::new($false))
    & $CheckCancellation
    $metadata=Open-EntryStateTransaction $Root
    try{
        $state=Read-EntryJson (Join-Path $Root '.build/collaboration/state.json');$batch=$state.current
        if(-not $batch -or $batch.id -ne $Request.batch -or $batch.runId -ne $Request.id -or $batch.protocolVersion -ne 4 -or
            $batch.phase -ne 'building' -or $batch.owner.pid -ne $PID -or $batch.owner.startTicks -ne $Record.owner.startTicks){throw 'Candidate batch capture lost its original reservation.'}
        $active=@(Get-Field $state 'activeCandidates' @())
        if($active.Count -ge 32 -or @($active|Where-Object id -eq $batch.id).Count){throw 'Candidate attempt registry is full or already contains this batch.'}
        Set-Field $batch 'candidateBinding' $binding
        Set-Field $batch 'capturedUtc' $binding.createdUtc
        Set-Field $state 'activeCandidates' (@($active)+@($batch));$state.current=$null
        if([Text.Encoding]::UTF8.GetByteCount(($state|ConvertTo-Json -Depth 30)) -gt 6MB){throw 'Candidate registry exceeds the metadata budget; no editing window was released.'}
        Write-RunJson $state (Join-Path $Root '.build/collaboration/state.json')
    }finally{$metadata.Dispose()}
    Set-Field $Record 'candidateBinding' $binding
    return $binding
}
function Get-CandidateBatchExecutionRequest([string]$Root,$Request) {
    $binding=Read-CandidateBatchBinding $Root $Request
    $execution=$Request|ConvertTo-Json -Depth 16|ConvertFrom-Json
    Set-Field $execution 'snapshot' $binding.snapshot
    Set-Field $execution 'snapshotManifestDigest' $binding.manifestDigest
    Set-Field $execution 'candidateSlot' 'batches'
    return $execution
}
function Invoke-CandidateBatchPipeline([string]$Root,$Request,$Context,$Record) {
    $binding=Read-CandidateBatchBinding $Root $Request
    $planPath=Join-Path $Root ('.build/collaboration/'+$Request.batch+'.plan.json')
    if((Get-FileDigest $planPath) -ne $Request.planHash.ToLowerInvariant()){throw 'Frozen candidate plan changed.'}
    $plan=Read-EntryJson $planPath
    if($plan.schemaVersion -ne 1 -or $plan.batchId -ne $Request.batch -or $plan.configuration -ne 'Release'){throw 'Invalid candidate batch plan.'}
    $privatePlan=Join-Path $Context.output ('collaboration/'+$Request.batch+'.plan.json')
    Write-CandidateJson $plan $privatePlan -Immutable
    if(-not ('SnowDesktop.Build.Job' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'build_job.cs')}
    Set-EntryRunControl $Context.source
    $log=Join-Path $Root ('.build/collaboration/'+$Request.batch+'.log')
    $previousNodeReuse=[Environment]::GetEnvironmentVariable('MSBUILDDISABLENODEREUSE','Process')
    try{
        [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE','1','Process')
        $code=0
        if($plan.buildRequired){$code=Invoke-EntryPreflight $Context.source $false $false 'Release' $log}
        if($code -eq 0 -and $plan.buildRequired){
            $productLog=$log+'.product.log'
            $code=[SnowDesktop.Build.Job]::RunLeasedCommand($Context.source,$productLog,'call scripts\build.bat')
            if($Record){
                Set-Field $Record 'productBuild' ([pscustomobject]@{exitCode=$code;completedUtc=[DateTime]::UtcNow.ToString('o');logPath=$productLog})
                Set-Field $Record.productBuild 'resources' (Read-EntryJson (Join-Path $Root ('.build/collaboration/runs/'+$Request.id+'.resources.json')))
                Write-RunRecord $Record
            }
            if($code -eq 0 -and -not [SnowDesktop.Build.Job]::RunWasCancelled){
                if((Get-BuildInputIdentity $Context.source).digest -ne $Record.inputStart.digest){throw 'Candidate source changed before build activation.'}
                . (Join-Path $PSScriptRoot 'build_candidate_artifacts.ps1')
                . (Join-Path $PSScriptRoot 'build_activate.ps1')
                $published=Publish-CandidateRuntime $Context 'release' {
                    if(Run-CancellationRequested $Request){throw [OperationCanceledException]::new('Product publication cancelled.')}
                }
                Set-Field $Record 'artifact' $published
                # Persist a usable build before activation or any later test.
                Write-RunRecord $Record
                $activation=Invoke-CandidateRuntimeActivation $Root $Request.id {
                    if(Run-CancellationRequested $Request){throw [OperationCanceledException]::new('Runtime activation cancelled.')}
                }
                Set-Field $Record 'activation' $activation
                Write-RunRecord $Record
            }
        }
        if($code -eq 0 -and -not [SnowDesktop.Build.Job]::RunWasCancelled){
            $code=[SnowDesktop.Build.Job]::RunBatch($Context.source,$log,$Request.batch,$false,$false,$false)
        }
        return [int]$code
    }finally{[Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE',$previousNodeReuse,'Process')}
}
function Save-CandidateBatchPending([string]$Root,$Request,$Record,$Context) {
    $state=Read-EntryJson (Join-Path $Root '.build/collaboration/state.json')
    $batch=if($state.current -and $state.current.id -eq $Request.batch){$state.current}else{@(Get-Field $state 'activeCandidates' @()|Where-Object id -eq $Request.batch)|Select-Object -First 1}
    if(-not $batch -or $batch.runId -ne $Request.id -or $batch.owner.pid -ne $PID -or $batch.owner.startTicks -ne $Record.owner.startTicks){throw 'Candidate result lost its frozen attempt owner.'}
    $inputCheck=if($Record.inputStart -and $Record.inputEnd){if($Record.inputStart.digest -eq $Record.inputEnd.digest){'stable'}else{'changed'}}else{'not-run'}
    $outcome=if($Record.status -eq 'passed' -and $batch.testingPlan.mode -eq 'skipped'){'skipped'}else{$Record.status}
    $result=[pscustomobject]@{schemaVersion=1;protocolVersion=4;batchId=$batch.id;repositoryRoot=$Root;
        logicalBatchId=(Get-Field $batch 'logicalBatchId' $batch.id);attempt=(Get-Field $batch 'attempt' 1);repairOf=(Get-Field $batch 'repairOf');
        testingPlan=$batch.testingPlan;outcome=$outcome;exitCode=$Record.exitCode;error=$Record.error;createdUtc=$batch.createdUtc;
        buildStartedUtc=$batch.buildStartedUtc;capturedUtc=(Get-Field $batch 'capturedUtc');completedUtc=[DateTime]::UtcNow.ToString('o');
        participants=@($batch.participants);runId=$Request.id;logPath=$batch.logPath;
        commands=@('Captured scripts/build.bat and scripts/build_batch_tests.ps1 in the private candidate output');
        inputStart=$Record.inputStart;inputEnd=$Record.inputEnd;inputCheck=$inputCheck;pipelineExitCode=$Record.exitCode;
        originalInputStart=$batch.inputStart;candidateBinding=(Get-Field $batch 'candidateBinding');artifact=(Get-Field $Record 'artifact');
        productBuild=(Get-Field $Record 'productBuild');activation=(Get-Field $Record 'activation')}
    if($Context){
        Set-Field $result 'candidateExecution' $Context
        $coveragePath=Join-Path $Context.output ('collaboration/'+$Request.batch+'.coverage.json')
        if([IO.File]::Exists($coveragePath)){
            $coverage=Read-EntryJson $coveragePath
            if($coverage.batchId -ne $Request.batch){throw 'Candidate test coverage belongs to another attempt.'}
            Write-CandidateJson $coverage (Join-Path $Root ('.build/collaboration/'+$Request.batch+'.coverage.json')) -Immutable
            Set-Field $result 'coverage' $coverage
        }
        $retryPath=Join-Path $Context.output ('collaboration/'+$Request.batch+'.retry.json')
        if([IO.File]::Exists($retryPath)){
            $retry=Read-EntryJson $retryPath
            if($retry.batchId -ne $Request.batch){throw 'Candidate retry history belongs to another batch.'}
            Write-CandidateJson $retry (Join-Path $Root ('.build/collaboration/'+$Request.batch+'.retry.json')) -Immutable
            Set-Field $result 'testRetry' $retry
        }
        $stage=Read-EntryJson (Join-Path $Context.output ('collaboration/'+$Request.batch+'.stage.json'))
        $buildPassed=$stage -and $stage.batchId -eq $Request.batch -and $stage.hostBuildPassed
        $binary=& {
            param($PrivateRoot,$Selected,$Passed)
            $repositoryRoot=$PrivateRoot
            . (Join-Path $PSScriptRoot 'build_ownership.ps1')
            Get-BinaryEvidence $Selected $Passed
        } $Context.source $batch ([bool]$buildPassed)
        Set-Field $result 'binaryEvidence' $binary
        Set-Field $result 'evidence' (New-BatchTestEvidence $Context.source $result)
    }
    if($outcome -in 'failed','interrupted','invalidated'){Set-Field $result 'responsibility' 'unassigned; preserve original coverage and use issue for handoff.'}
    Write-CandidateJson $result (Join-Path $Root ('.build/collaboration/'+$Request.batch+'.pending-result.json')) -Immutable
}
function Complete-CandidateBatchCoverage($Context) {
    $coverage=Read-EntryJson (Join-Path $Context.output ('collaboration/'+$Context.batchId+'.coverage.json'))
    if(-not $coverage -or $coverage.mode -eq 'skipped'){return}
    $executed=@(Get-Field $coverage 'executedSelection' @($coverage.selected))
    $references=@(Get-Field $coverage 'references' @())
    $union=@($executed)+@($references|ForEach-Object test)
    if($coverage.batchId -ne $Context.batchId -or $coverage.status -notin 'passed','passed-after-retry','fulfilled' -or @($coverage.selected).Count -eq 0 -or
        $union.Count -ne @($coverage.selected).Count -or @($union|Sort-Object -Unique).Count -ne $union.Count -or
        @((Compare-Object @($union|Sort-Object) @($coverage.selected|Sort-Object))).Count -or
        @($coverage.completed).Count -ne $executed.Count -or @($coverage.completed|Where-Object status -notin 'passed','passed-after-retry').Count){throw 'No final fulfilled candidate test coverage is available.'}
    if($references.Count){Assert-CompiledTestReferences $Context.source $coverage}
    $directory=Join-Path $Context.runRoot '.build/collaboration/runs'
    if($executed.Count){
        if(-not $coverage.report){throw 'Executed tests require their original JUnit report.'}
        $suffix=Get-VerificationTextDigest $coverage.report
        $final=Read-EntryJson (Join-Path $directory ($Context.runId+'.candidate-tests.'+$suffix+'.json'))
        if(-not $final -or $final.runId -ne $Context.runId -or $final.candidate -ne $Context.snapshot -or
            $final.report -ne $coverage.report -or $final.reportSha256 -ne (Get-FileDigest $coverage.report) -or
            (@($final.selected|Sort-Object) -join ',') -cne (@($executed|Sort-Object) -join ',')){throw 'Final candidate test report binding changed.'}
    }else{
        if($coverage.report -or -not $references.Count){throw 'Reference-only coverage must not create an execution report.'}
        $final=[pscustomobject]@{schemaVersion=1;candidate=$Context.snapshot;runId=$Context.runId;sourceDigest=(Get-CandidateExecutionInputIdentity $Context.source).digest;
            report=$null;reportSha256=$null;exitCode=0;reuseAllowed=$false}
    }
    $attempts=@()
    if($coverage.status -eq 'passed-after-retry'){
        $retry=Read-EntryJson (Join-Path $Context.output ('collaboration/'+$Context.batchId+'.retry.json'))
        if(-not $retry -or $retry.batchId -ne $Context.batchId -or $retry.status -ne 'passed-after-retry'){throw 'Passing retry aggregate has no matching preserved ledger.'}
        foreach($test in $retry.tests.PSObject.Properties){foreach($attempt in $test.Value.attempts){
            if(-not $attempt.report -or -not [IO.File]::Exists($attempt.report)){throw 'Candidate retry report is missing.'}
            $attempts+=@([pscustomobject]@{test=$test.Name;attempt=$attempt.attempt;status=$attempt.status;exitCode=$attempt.exitCode;report=$attempt.report;sha256=(Get-FileDigest $attempt.report)})
        }}
    }elseif($final.exitCode -ne 0){throw 'Final initial test report failed without a passing retry ledger.'}
    Set-Field $final 'initialExitCode' $final.exitCode
    Set-Field $final 'exitCode' 0
    Set-Field $final 'resultKind' $(if($references.Count){'batch-fulfillment'}else{'batch-aggregate'})
    Set-Field $final 'selected' @($coverage.selected)
    Set-Field $final 'executedSelection' $executed
    Set-Field $final 'references' $references
    Set-Field $final 'aggregateStatus' $coverage.status
    Set-Field $final 'retryAttempts' $attempts
    Write-CandidateJson $final (Join-Path $directory ($Context.runId+'.candidate-tests.json')) -Immutable
}
