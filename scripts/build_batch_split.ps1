# A scoped no-build result uses the original registration address. Its receipt
# is separate from the result eventually produced by the rest of the batch.
function Independent-ResultPath([string]$BatchId, [string]$TaskId, [long]$EditRevision) {
    if($BatchId -notmatch '^[a-f0-9]{32}$' -or $TaskId -notmatch '^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$' -or $EditRevision -lt 0){
        throw 'Independent result requires its original batch, participant and edit revision.'
    }
    return Join-Path $stateRoot ('split-{0}-{1}-{2}.json' -f $BatchId.ToLowerInvariant(),$TaskId.ToLowerInvariant(),$EditRevision)
}

function Read-IndependentResult([string]$BatchId, [string]$TaskId, [long]$EditRevision) {
    $receipt=Read-EntryJson (Independent-ResultPath $BatchId $TaskId $EditRevision)
    if(-not $receipt){return}
    $members=@($receipt.participants)
    if($receipt.schemaVersion -ne 1 -or $receipt.protocolVersion -ne 4 -or $receipt.repositoryRoot -ne $repositoryRoot -or
        $receipt.resultScope -ne 'participant' -or $receipt.batchId -ne $BatchId -or $receipt.outcome -ne 'skipped' -or
        $receipt.inputCheck -ne 'stable' -or $receipt.exitCode -ne 0 -or $receipt.runId -or $members.Count -ne 1 -or
        $members[0].id -ne $TaskId -or $members[0].state -ne 'finished' -or (Get-EditRevision $members[0]) -ne $EditRevision){
        throw 'Invalid independent result; preserve the receipt and batch for diagnosis.'
    }
    return $receipt
}

function Sync-IndependentResults($Current) {
    if(-not $Current -or $Current.phase -ne 'editing' -or (Get-Field $Current 'protocolVersion' 1) -ne 4){return}
    foreach($member in $Current.participants){
        if($member.state -ne 'finished' -or -not (Get-Field (Get-Field $member 'testPlan') 'independent' $false)){continue}
        $revision=Get-EditRevision $member
        $receipt=Read-IndependentResult $Current.id $member.id $revision
        if($receipt){
            # If the publisher stopped after writing the receipt, resume the
            # retirement in memory before evaluating any common obligations.
            Set-Field $member 'independentResult' ([IO.Path]::GetFileName((Independent-ResultPath $Current.id $member.id $revision)))
        }elseif(Get-Field $member 'independentResult'){
            throw 'Independent result marker has no durable receipt; preserve the batch for diagnosis.'
        }
    }
}

function Get-IndependentSplitBlocker($State, $Current, $Member) {
    $plan=Get-Field $Member 'testPlan'
    if((Get-Field $Current 'protocolVersion' 1) -ne 4 -or -not (Get-Field $plan 'independent' $false)){return 'The plan is not opted in.'}
    if((Get-RepairSources $Current) -or @(Get-Field $Member 'repairRequirements' @()).Count){return 'A repair obligation requires the common batch.'}
    if((Get-Field $Current 'reloadShellRequested') -or (Get-Field $Current 'closeApplicationRequested')){return 'The batch has an explicit output-owner action.'}
    $task=Find-VisibleTask (Get-TaskLedger $State) $Member.id
    if($task.phase -in 'completed','cancelled' -or $task.closedUtc){return 'The member task is closed.'}
    $revision=Get-EditRevision $Member
    if($plan.editRevision -ne $revision -or (Get-Field $plan 'requirementsRevision' -1) -ne $task.requirementsRevision){
        return 'Plan or requirements changed; reopen and declare the current plan.'
    }
    try {Assert-IndependentPlan $plan $Member} catch {return $_.Exception.Message}
    $check=Get-Field $Member 'check'
    if(-not $check -or $check.status -ne 'passed' -or $check.editRevision -ne $revision){return 'The member check has not passed.'}
    foreach($pending in @('resource-write-pending.json','resource-commit-pending.json')){
        if([IO.File]::Exists((Join-Path $stateRoot $pending))){return 'A shared language operation needs recovery.'}
    }
    if(@(Get-PendingResourceOwnership | Where-Object participant -eq $Member.id).Count){return 'Pending language keys require the common batch.'}
    foreach($peer in $Current.participants){
        if($peer.id -eq $Member.id -or $peer.state -eq 'withdrawn' -or (Get-Field $peer 'independentResult')){continue}
        $claims=@(Get-Field $peer 'ownedFiles' @()|Where-Object {$_})
        if($peer.state -eq 'editing' -and -not $claims.Count){return "Waiting for $($peer.id) to declare its write paths."}
        foreach($inputPath in $plan.inputs){
            foreach($claim in $claims){
                if($inputPath -ieq $claim -or $inputPath.StartsWith($claim+'/',[StringComparison]::OrdinalIgnoreCase) -or
                    $claim.StartsWith($inputPath+'/',[StringComparison]::OrdinalIgnoreCase)){
                    return "Input $inputPath overlaps $($peer.id)'s write path $claim."
                }
            }
        }
    }
}

function Publish-IndependentResult($State, $Current, $Member) {
    # Caller holds state.lock. The exact input used by the scoped check must
    # still match. A ready member cannot validate later changes by other editors.
    $plan=Get-EffectiveTaskPlan $Member
    $identity=Get-TaskIdentity $plan
    if(-not $plan.inputIdentity -or $identity.digest -ne $plan.inputIdentity.digest -or $identity.digest -ne $Member.check.inputEnd.digest){
        throw 'Independent plan inputs changed after its check; reopen and update the plan.'
    }
    $revision=Get-EditRevision $Member
    $path=Independent-ResultPath $Current.id $Member.id $revision
    if([IO.File]::Exists($path)){throw 'Independent result already exists; read the saved receipt.'}
    $execution=[pscustomobject]@{
        id=$Current.id; protocolVersion=4; createdUtc=$Current.createdUtc; buildStartedUtc=$null; logPath=$null
        participants=@($Member); inputStart=$Member.check.inputStart
    }
    Set-Field $execution 'testingPlan' (New-FrozenPlan $execution)
    $result=New-Result $execution 'skipped' 0 'Independent member passed its scoped check and explicitly exempted host build/tests.'
    Set-Field $result 'resultScope' 'participant'
    $result.commands=@()
    $result.inputEnd=$identity
    $result.inputCheck='stable'
    Set-Field $result 'coverage' ([pscustomobject]@{
        schemaVersion=1; batchId=$Current.id; mode='skipped'; status='not-required'; selected=@(); completed=@()
        tasks=@([pscustomobject]@{participant=$Member.id; requested=@(); status='not-required'; reason=$plan.reason})
    })
    Write-AtomicJson $result $path
    Set-Field $Member 'independentResult' ([IO.Path]::GetFileName($path))
    Write-AtomicJson $State $statePath
    return Read-IndependentResult $Current.id $Member.id $revision
}
