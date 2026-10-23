# Compiled test intents belong to the actual owning Run. They are not completed
# baselines, execution credentials, or permission to re-enter a candidate slot.
function Write-CompiledSelectionState($State,[string]$Path) {
    $text=$State|ConvertTo-Json -Depth 40 -Compress
    if($text.Length -gt 7MB){throw 'Verification metadata budget requires explicit maintenance.'}
    $temp=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp';$backup=$temp+'.bak'
    try {
        [IO.File]::WriteAllText($temp,$text,[Text.UTF8Encoding]::new($false))
        [IO.File]::Replace($temp,$Path,$backup)
    }finally{foreach($file in @($temp,$backup)){if([IO.File]::Exists($file)){[IO.File]::Delete($file)}}}
}
function Get-VerificationOriginView([string]$Root,$Entry) {
    $id=[string]$Entry.runId
    if($id -notmatch '^[a-f0-9]{32}$'){throw 'Invalid reference origin Run.'}
    $directory=Join-Path $Root '.build/collaboration/runs'
    $request=Read-EntryJson (Join-Path $directory ($id+'.request.json'))
    $record=Read-EntryJson (Join-Path $directory ($id+'.json'))
    if(-not $request -or -not $record -or $request.id -ne $id -or $record.id -ne $id -or
        $request.repositoryRoot -ne $Root -or $record.repositoryRoot -ne $Root -or $request.nonce -ne $record.nonce){throw 'Reference origin Run binding changed.'}
    $terminal=$record.status -in 'passed','failed','cancelled','start-failed','invalidated','interrupted'
    $coveragePath=Join-Path $directory ($id+'.verification.json')
    $origin=Get-Field $request 'verification';$evidence=Get-Field $record 'evidence'
    $proofPassed=$record.status -eq 'passed' -and $record.exitCode -eq 0 -and $request.mode -eq 'verify' -and $origin -and $origin.key -eq $Entry.key
    $status=$record.status
    if((Get-Field $Entry 'kind') -eq 'compiled-test-origin-v1'){
        $intentPath=Join-Path $directory ($id+'.test-intent.json')
        if((Get-FileDigest $intentPath) -ne $Entry.intentSha256){throw 'Compiled test intent changed.'}
        $intent=Read-EntryJson $intentPath
        $batch=Read-EntryJson (Join-Path $Root ('.build/collaboration/'+$intent.batchId+'.json'))
        if(-not $terminal -or -not $batch){return [pscustomobject]@{terminal=$terminal;status=$record.status;proofPassed=$false}}
        if($intent.schemaVersion -ne 1 -or $intent.runId -ne $id -or $intent.requestSha256 -ne (Get-FileDigest (Join-Path $directory ($id+'.request.json'))) -or
            $request.action -ne 'batch' -or $request.batch -ne $intent.batchId -or $intent.nonce -ne $request.nonce -or
            -not $batch -or $batch.runId -ne $id -or $batch.batchId -ne $intent.batchId -or $batch.inputCheck -ne 'stable' -or
            $batch.outcome -notin 'passed','failed' -or $batch.candidateExecution.snapshot -ne $intent.candidate.snapshot -or
            $batch.inputEnd.digest -ne $intent.sourceDigest){throw 'Compiled test origin has no matching stable completed batch.'}
        $matches=@($intent.tests|Where-Object {$_.key -eq $Entry.key -and $_.binding.names.Count -eq 1 -and $_.binding.names[0] -ceq $Entry.test})
        if($matches.Count -ne 1){throw 'Compiled test origin identity is unavailable.'}
        $origin=$matches[0];$evidence=$batch.evidence
        $coveragePath=Join-Path $Root ('.build/collaboration/'+$intent.batchId+'.coverage.json')
        if((Get-FileDigest $coveragePath) -ne $Entry.coverageSha256){throw 'Compiled test origin coverage changed.'}
        $observed=@($evidence.tests|Where-Object test -CEQ $Entry.test)
        $status=if($observed.Count -eq 1){$observed[0].status}else{'not-run'}
        $proofPassed=$terminal -and $record.status -in 'passed','failed' -and $observed.Count -eq 1 -and $status -eq 'passed' -and
            $origin.key -eq (Get-VerificationTextDigest ($origin.binding|ConvertTo-Json -Depth 20 -Compress))
    }
    return [pscustomobject]@{request=$request;record=$record;terminal=$terminal;status=$status;proofPassed=[bool]$proofPassed;
        verification=$origin;evidence=$evidence;coveragePath=$coveragePath}
}
function New-CompiledTestIntent([string]$Root,$Tests,$Coverage,$Budget) {
    $candidate=Get-CandidateEntryContext $Root
    if(-not $candidate -or -not (Get-Field $candidate 'batchId')){return $null}
    Assert-EntryExecutionOwner $Root
    . (Join-Path $PSScriptRoot 'build_candidate.ps1')
    . (Join-Path $PSScriptRoot 'build_candidate_execution.ps1')
    $bindingCandidate=[pscustomobject]@{snapshot=$candidate.snapshot;manifestDigest=$candidate.manifestDigest;source=$candidate.source;
        output=$candidate.output;slot=$candidate.slot;runRoot=$candidate.runRoot}
    $snapshots=@();$source=$null
    foreach($test in @($Tests|Sort-Object name)){
        if($test.name -notmatch '^[A-Za-z0-9_-]{1,160}$'){continue}
        $review=Get-ReviewedVerificationContext $Root @($test) $bindingCandidate
        if(-not $review -or (Get-Field $review 'candidateCompatibility') -ne 'same-output-reviewed-inputs-v1'){continue}
        if(-not $source){$source=Get-CandidateExecutionInputIdentity $Root}
        $name=$test.name
        $binding=[pscustomobject]@{sourceDigest=$source.digest;configuration='Release';names=@($name);
            definitions=@($Coverage.testDefinitionsBeforeRun|Where-Object name -CEQ $name);
            artifacts=@($Coverage.testExecutablesBeforeRun|Where-Object test -CEQ $name);
            runtimeFiles=@($Coverage.testRuntimeFilesBeforeRun|Where-Object test -CEQ $name);
            environmentDigest=$Coverage.executionContext.digest;candidate=$bindingCandidate;reviewedReuse=$review}
        if($Budget){Set-Field $binding 'resourceBudget' $Budget}
        $snapshot=[pscustomobject]@{schemaVersion=1;key=(Get-VerificationTextDigest ($binding|ConvertTo-Json -Depth 20 -Compress));baseline=$candidate.batchId;
            binding=$binding;source=$source;executionContext=$Coverage.executionContext;tests=@($test);noBuild=$false;reuseAllowed=$false;consumerRunId=$candidate.runId}
        $compatibility=Get-VerificationCompatibility $snapshot
        if(-not $compatibility){continue}
        Set-Field $snapshot 'compatibility' $compatibility
        $snapshots+=@($snapshot)
        if($snapshots.Count -ge 128){break}
    }
    if(-not $snapshots.Count){return $null}
    $requestPath=Join-Path $candidate.runRoot ('.build/collaboration/runs/'+$candidate.runId+'.request.json')
    $intent=[pscustomobject]@{schemaVersion=1;kind='compiled-test-intent';runId=$candidate.runId;nonce=$candidate.nonce;batchId=$candidate.batchId;
        requestSha256=(Get-FileDigest $requestPath);candidate=$bindingCandidate;sourceDigest=$source.digest;
        selected=@($Tests.name);tests=$snapshots;createdUtc=(Format-VerificationUtcTimestamp ([DateTime]::UtcNow))}
    $path=Join-Path $candidate.runRoot ('.build/collaboration/runs/'+$candidate.runId+'.test-intent.json')
    Write-CandidateJson $intent $path -Immutable
    $transaction=Open-EntryStateTransaction $candidate.runRoot
    try {
        $statePath=Join-Path $candidate.runRoot '.build/collaboration/state.json';$state=Read-EntryJson $statePath;$ledger=Get-VerificationLedger $state
        foreach($snapshot in $snapshots){
            $ledger.entries+=@([pscustomobject]@{kind='compiled-test-origin-v1';key=$snapshot.key;runId=$candidate.runId;test=$snapshot.binding.names[0];
                createdUtc=$intent.createdUtc;compatibleTests=@($snapshot.compatibility.tests|Select-Object test,key);subscriptions=@();references=@();
                intentSha256=(Get-FileDigest $path);coverageSha256=$null;execution='pending'})
        }
        if(@($ledger.entries).Count -gt 4096){throw 'Verification ledger count budget requires explicit maintenance.'}
        Write-CompiledSelectionState $state $statePath
    }finally{$transaction.Dispose()}
    return [pscustomobject]@{intent=$intent;path=$path;sha256=(Get-FileDigest $path);context=$candidate}
}
function Get-CompiledTestSelection([string]$Root,$Tests,$Coverage,$Budget) {
    $compiled=New-CompiledTestIntent $Root $Tests $Coverage $Budget
    $result=[pscustomobject]@{tests=@($Tests);references=@();intent=$null}
    if(-not $compiled){return $result}
    $result.intent=[pscustomobject]@{policyVersion=1;runId=$compiled.intent.runId;sha256=$compiled.sha256;path=$compiled.path}
    $selected=@($Tests.name);$context=$compiled.context
    # The authenticated parent owns the slot and source handles throughout this
    # pipeline. Proof lookup is read-only and never re-acquires that same lease.
    foreach($snapshot in $compiled.intent.tests){
        $proposal=Find-CompletedVerificationProof $context.runRoot $snapshot -BatchOrigins
        if(-not $proposal){continue}
        $transaction=Open-EntryStateTransaction $context.runRoot
        try {
            $statePath=Join-Path $context.runRoot '.build/collaboration/state.json'
            $state=Read-EntryJson $statePath;$ledger=Get-VerificationLedger $state
            $matches=@($ledger.entries|Where-Object {$_.runId -eq $proposal.runId -and $_.key -eq $proposal.originKey})
            if($matches.Count -ne 1 -or (Get-FileDigest (Join-Path $context.runRoot ('.build/collaboration/runs/'+$proposal.runId+'.json'))) -ne $proposal.runSha256){continue}
            $name=$snapshot.binding.names[0];$receipts=@()
            foreach($requester in @($Coverage.tasks|Where-Object {$_.requested -contains $name})){
                $tasks=@($state.taskLedger.tasks|Where-Object {$_.id -eq $requester.participant -and -not $_.closedUtc})
                if($tasks.Count -ne 1){throw 'Compiled test requester is no longer active.'}
                $receipt=Add-CompletedVerificationReference $context.runRoot $matches[0] $tasks[0] $context.batchId $proposal
                $receiptPath=Join-Path $context.runRoot ('.build/verification/references/'+$receipt.id+'.json')
                $receipts+=@([pscustomobject]@{id=$receipt.id;task=$receipt.task;sha256=(Get-FileDigest $receiptPath)})
            }
            if(-not $receipts.Count){throw 'Compiled test reference has no declared requester.'}
            if(($state|ConvertTo-Json -Depth 40 -Compress).Length -gt 7MB){throw 'Verification metadata budget requires explicit maintenance.'}
            Write-CompiledSelectionState $state $statePath
            $result.references+=@([pscustomobject]@{test=$name;execution='referenced';newExecution=$false;sourceRunId=$proposal.runId;
                sourceRunSha256=$proposal.runSha256;coverageSha256=$proposal.coverageSha256;originKey=$proposal.originKey;key=$proposal.key;
                originKind=$proposal.originKind;originCoverage=$proposal.originCoverage;receipts=$receipts;checkedUtc=$proposal.checkedUtc})
            $selected=@($selected|Where-Object {$_ -cne $name})
        }finally{$transaction.Dispose()}
    }
    $result.tests=@($Tests|Where-Object {$selected -ccontains $_.name})
    return $result
}
function Assert-CompiledTestReferences([string]$Root,$Coverage) {
    $context=Get-CandidateEntryContext $Root
    if(-not $context){throw 'Reference validation requires the actual live candidate owner.'}
    Assert-EntryExecutionOwner $Root
    $intentPath=Join-Path $context.runRoot ('.build/collaboration/runs/'+$context.runId+'.test-intent.json')
    if($Coverage.compiledIntent.sha256 -ne (Get-FileDigest $intentPath)){throw 'Current compiled test intent changed.'}
    $intent=Read-EntryJson $intentPath
    foreach($reference in @(Get-Field $Coverage 'references' @())){
        if($reference.sourceRunId -notmatch '^[a-f0-9]{32}$' -or $reference.newExecution -ne $false -or
            (Get-FileDigest (Join-Path $context.runRoot ('.build/collaboration/runs/'+$reference.sourceRunId+'.json'))) -ne $reference.sourceRunSha256 -or
            (Get-FileDigest $reference.originCoverage) -ne $reference.coverageSha256){throw 'Referenced origin changed during the test pipeline.'}
        foreach($receipt in $reference.receipts){
            if($receipt.id -notmatch '^[a-f0-9]{32}$' -or (Get-FileDigest (Join-Path $context.runRoot ('.build/verification/references/'+$receipt.id+'.json'))) -ne $receipt.sha256){throw 'Reference receipt changed during the test pipeline.'}
        }
        $snapshots=@($intent.tests|Where-Object {$_.binding.names[0] -ceq $reference.test})
        if($snapshots.Count -ne 1){throw 'Referenced test is missing from the compiled intent.'}
        $snapshot=$snapshots[0]
        $review=Get-ReviewedVerificationContext $Root $snapshot.tests $intent.candidate
        if(-not $review -or ($review|ConvertTo-Json -Depth 12 -Compress) -cne ($snapshot.binding.reviewedReuse|ConvertTo-Json -Depth 12 -Compress) -or
            (Get-VerificationEnvironment $Root).digest -ne $snapshot.executionContext.digest){throw 'Referenced test inputs or environment changed during execution.'}
        $proof=Find-CompletedVerificationProof $context.runRoot $snapshot -BatchOrigins -OnlyRun $reference.sourceRunId
        if(-not $proof -or $proof.runId -ne $reference.sourceRunId -or $proof.originKey -ne $reference.originKey){throw 'Referenced original evidence no longer validates.'}
    }
}
function Publish-CompiledTestHistory([string]$Root,$Request,$Record) {
    if($Request.action -ne 'batch' -or -not (Get-Field $Request 'candidateBatch' $false)){return}
    $path=Join-Path $Root ('.build/collaboration/runs/'+$Request.id+'.test-intent.json')
    if(-not [IO.File]::Exists($path)){return}
    $intent=Read-EntryJson $path;$batch=Read-EntryJson (Join-Path $Root ('.build/collaboration/'+$Request.batch+'.json'))
    if($intent.runId -ne $Request.id -or $intent.nonce -ne $Request.nonce -or $intent.batchId -ne $Request.batch -or
        $intent.requestSha256 -ne (Get-FileDigest (Join-Path $Root ('.build/collaboration/runs/'+$Request.id+'.request.json'))) -or
        -not $batch -or $batch.runId -ne $Request.id -or $batch.inputCheck -ne 'stable' -or $batch.outcome -notin 'passed','failed'){return}
    $transaction=Open-EntryStateTransaction $Root
    try {
        $statePath=Join-Path $Root '.build/collaboration/state.json';$state=Read-EntryJson $statePath;$ledger=Get-VerificationLedger $state
        foreach($snapshot in $intent.tests){
            $name=$snapshot.binding.names[0]
            $executed=@($batch.coverage.completed|Where-Object name -CEQ $name)
            $entries=@($ledger.entries|Where-Object {$_.runId -eq $Request.id -and $_.key -eq $snapshot.key})
            if($entries.Count -ne 1){throw 'Compiled test intent registration changed.'}
            Set-Field $entries[0] 'coverageSha256' (Get-FileDigest (Join-Path $Root ('.build/collaboration/'+$Request.batch+'.coverage.json')))
            $referenced=@(Get-Field $batch.coverage 'references' @()|Where-Object test -CEQ $name)
            Set-Field $entries[0] 'execution' $(if($executed.Count -eq 1){'executed'}elseif($referenced.Count -eq 1){'referenced'}else{'not-run'})
            # Retain cancelled/failed pending history. A fulfilled reference is
            # never a new origin and must not create an indirect citation chain.
            if($executed.Count -eq 0 -and $referenced.Count -eq 1){Set-Field $entries[0] 'compatibleTests' @();Set-Field $entries[0] 'key' ('reference-only-'+$Request.id+'-'+$name)}
        }
        if(@($ledger.entries).Count -gt 4096 -or ($state|ConvertTo-Json -Depth 40 -Compress).Length -gt 7MB){throw 'Verification metadata budget requires explicit maintenance.'}
        Write-CompiledSelectionState $state $statePath
    }finally{$transaction.Dispose()}
}
