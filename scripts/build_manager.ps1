[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('begin', 'ready', 'ready-and-wait', 'finish', 'wait', 'check', 'plan', 'claim', 'commit', 'issue', 'status', 'recover', 'repair', 'repair-abandon')][string]$Command,
    [Parameter(Position = 1)][ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$Participant,
    [ValidatePattern('^[a-f0-9]{32}$')][string]$Batch,
    [ValidateRange(1, 86400)][int]$WaitSeconds = 86400,
    [switch]$ConfirmStopped,
    [switch]$ReloadShell,
    [string]$Reason,
    [long]$Revision = -1,
    [ValidateSet('unknown','module','docs','component','tool','public','infrastructure')][string]$Scope = 'unknown',
    [string]$Suites = 'full', [string]$Tests = '', [string]$Inputs = '',
    [switch]$AdoptExistingChanges, [string]$Files = '', [string]$MessageFile = '',
    [string]$IssueId = '', [string]$Assignee = '', [ValidateSet('open','resolved','deferred')][string]$IssueState = 'open',
    [switch]$AutoCheck
)

# Keep one foreground tool process alive; the shared ready worker is reused.
if($Command -eq 'ready-and-wait') {
    if(-not $PSBoundParameters.ContainsKey('WaitSeconds')){$WaitSeconds=1800}
    if($Revision -lt 0){throw 'ready-and-wait requires the latest -Revision.'}
    & $PSCommandPath ready $Participant -Batch $Batch -Revision $Revision -WaitSeconds $WaitSeconds -ReloadShell:$ReloadShell | Out-Null
    if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
    $python=Get-Command python.exe -ErrorAction SilentlyContinue
    if($python){
        & $python.Source (Join-Path $PSScriptRoot 'build_wait_tasks.py') watch wait $Participant --condition result --batch $Batch --revision $Revision --timeout $WaitSeconds
    }else{
        # Base collaboration remains usable without installing Python.
        & $PSCommandPath finish $Participant -Batch $Batch -Revision $Revision -WaitSeconds $WaitSeconds -AutoCheck
    }
    exit $LASTEXITCODE
}
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stateRoot = Join-Path $repositoryRoot '.build\collaboration'
$statePath = Join-Path $stateRoot 'state.json'
$clock = [Diagnostics.Stopwatch]::StartNew()
$lastNotice = -30
$hasFinishedCurrentBatch = $false
$finishedRevision = $null
$utf8 = New-Object Text.UTF8Encoding($false)
. (Join-Path $PSScriptRoot 'build_inputs.ps1')
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
. (Join-Path $PSScriptRoot 'build_ownership.ps1')
. (Join-Path $PSScriptRoot 'build_preflight.ps1')
$observedRevision = $null

function Write-AtomicJson($Value, [string]$Path) {
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        $bytes = $utf8.GetBytes((ConvertTo-Json -InputObject $Value -Depth 20))
        $stream = [IO.File]::Open($temporary, 'CreateNew', 'Write', 'None')
        try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
        finally { $stream.Dispose() }
        if ([IO.File]::Exists($Path)) { [IO.File]::Replace($temporary, $Path, $temporary + '.bak') }
        else { [IO.File]::Move($temporary, $Path) }
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
        if ([IO.File]::Exists($temporary + '.bak')) { [IO.File]::Delete($temporary + '.bak') }
    }
}

function Try-Lease([string]$Name) {
    try { return [IO.File]::Open((Join-Path $stateRoot $Name), 'OpenOrCreate', 'ReadWrite', 'None') }
    catch [IO.IOException] {
        if (($_.Exception.HResult -band 0xffff) -in 32, 33) { return $null }
        throw
    }
}

function Wait-Again([string]$Message) {
    if ($clock.Elapsed.TotalSeconds -ge $WaitSeconds) {
        throw "Wait timed out. Registration was retained; use status and repeat the same command. $Message"
    }
    if ($clock.Elapsed.TotalSeconds - $script:lastNotice -ge 30) {
        [Console]::Error.WriteLine($Message)
        $script:lastNotice = $clock.Elapsed.TotalSeconds
    }
    Start-Sleep -Milliseconds 200
}

function Lock-State {
    while ($true) {
        $lease = Try-Lease 'state.lock'
        if ($null -ne $lease) { return $lease }
        Wait-Again 'Waiting for the collaboration state transaction.'
    }
}

function Read-State {
    if (-not [IO.File]::Exists($statePath)) {
        return [pscustomobject]@{ schemaVersion = 1; repositoryRoot = $repositoryRoot; current = $null }
    }
    $state = [IO.File]::ReadAllText($statePath, $utf8) | ConvertFrom-Json
    if ($state.schemaVersion -ne 1 -or $state.repositoryRoot -ne $repositoryRoot) {
        throw 'Collaboration state version/root mismatch. Preserve the state and diagnose it; do not delete registrations.'
    }
    if ($null -ne $state.current) {
        $batchState = $state.current
        if ($batchState.id -notmatch '^[a-f0-9]{32}$' -or $batchState.phase -notin 'editing', 'building' -or
            @($batchState.participants).Count -eq 0 -or
            @($batchState.participants | Where-Object { $_.state -notin 'editing', 'finished', 'withdrawn' }).Count -gt 0 -or
            @($batchState.participants.id | Sort-Object -Unique).Count -ne @($batchState.participants).Count) {
            throw 'Invalid collaboration state. Preserve it for diagnosis; registrations were not changed.'
        }
    }
    return $state
}

function Result-Path([string]$Id) { return Join-Path $stateRoot ($Id + '.json') }
function Participant-Revision($Entry) {
    if ($Entry.PSObject.Properties['editRevision']) { return [long]$Entry.editRevision }
    return [long]0 # Registrations made before reopen support remain readable.
}
function Read-Result([string]$Id) {
    $path = Result-Path $Id
    if (-not [IO.File]::Exists($path)) { return $null }
    $result = [IO.File]::ReadAllText($path, $utf8) | ConvertFrom-Json
    if ($result.schemaVersion -ne 1 -or $result.batchId -ne $Id -or $result.repositoryRoot -ne $repositoryRoot) {
        throw 'Invalid batch result; preserve it for diagnosis.'
    }
    return $result
}
function Owner-State($Owner) {
    if ($null -eq $Owner) { return 'none' }
    $process = Get-Process -Id $Owner.pid -ErrorAction SilentlyContinue
    if ($null -eq $process) { return 'exited' }
    try {
        if ($process.StartTime.ToUniversalTime().Ticks.ToString() -eq $Owner.startTicks) { return 'alive' }
        return 'exited' # PID reuse is not ownership.
    }
    catch { return 'unknown' }
}
function New-Result($Current, [string]$Outcome, [int]$Code, [string]$ErrorText) {
    return [pscustomobject]@{
        schemaVersion = 1; protocolVersion=(Get-Field $Current 'protocolVersion' 1); batchId = $Current.id; repositoryRoot = $repositoryRoot
        logicalBatchId=(Get-Field $Current 'logicalBatchId' $Current.id); attempt=[int](Get-Field $Current 'attempt' 1); repairOf=(Get-Field $Current 'repairOf'); testingPlan=(Get-Field $Current 'testingPlan')
        outcome = $Outcome; exitCode = $Code; error = $ErrorText
        createdUtc = $Current.createdUtc; buildStartedUtc = $Current.buildStartedUtc
        completedUtc = [DateTime]::UtcNow.ToString('o'); participants = @($Current.participants)
        logPath = $Current.logPath; commands = if(Get-Field $Current 'testingPlan'){@($(if($Current.testingPlan.buildRequired){'scripts\build.bat'});'scripts\build_batch_tests.ps1 -Batch '+$Current.id)}else{@('scripts\build.bat','scripts\test.bat')}
        inputStart = if ($Current.PSObject.Properties['inputStart']) { $Current.inputStart } else { $null }
        inputEnd = $null; inputCheck = 'not-run'; pipelineExitCode = $null
    }
}
function Publish-Result($State, $Result) {
    # Called with the metadata lease. Only this frozen batch is retired, and
    # only after its immutable result is durable. New begin cannot enter here.
    if ($State.current.id -ne $Result.batchId) { throw 'Batch changed before publication.' }
    $existing = Read-Result $Result.batchId
    if ($null -eq $existing) { Write-AtomicJson $Result (Result-Path $Result.batchId) }
    else { throw 'A batch result already exists; use recover to finish its publication.' }
    $State.current = $null
    Write-AtomicJson $State $statePath
}
function Start-ReadyWorker($Current, $Entry) {
    $existing = Get-Field $Entry 'waiter'
    if ($existing -and (Get-Field $existing 'editRevision' -1) -eq (Get-EditRevision $Entry) -and (Owner-State $existing) -eq 'alive') { return $existing }
    $revision = Get-EditRevision $Entry
    $waiterScript=Join-Path $PSScriptRoot 'build_waiter.ps1'
    $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$waiterScript+'" -Participant '+$Entry.id+' -Batch '+$Current.id+' -Revision '+$revision
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=Join-Path $PSHOME 'powershell.exe';$info.Arguments=$arguments
    $info.UseShellExecute=$true;$info.WindowStyle='Hidden';$info.WorkingDirectory=$repositoryRoot
    $worker=[Diagnostics.Process]::Start($info)
    try {
        $null = $worker.Handle
        $owner = [pscustomobject]@{ pid=$worker.Id; startTicks=$worker.StartTime.ToUniversalTime().Ticks.ToString(); editRevision=$revision }
        Set-Field $Entry 'waiter' $owner
        return $owner
    } finally { $worker.Dispose() }
}
function Start-CheckRecord($Current, $Entry) {
    $plan = Get-Field $Entry 'testPlan'
    if (-not $plan) { $plan=Default-TaskPlan $Entry }
    $checks=@(Get-Field $plan 'lightChecks' @('builtin-basic'))
    if($checks.Count -ne 1 -or $checks[0] -ne 'builtin-basic'){throw 'Unsupported lightweight check plan; no arbitrary command was executed.'}
    $start = Get-TaskIdentity $plan
    $record = [pscustomobject]@{ operationId=[Guid]::NewGuid().ToString('N'); status='running'; source='builtin-basic';
        editRevision=(Get-EditRevision $Entry); startedUtc=[DateTime]::UtcNow.ToString('o'); completedUtc=$null;
        owner=[pscustomobject]@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()};
        inputStart=$start; inputEnd=$null; summary=$null; reason='' }
    Set-Field $record 'plannedChecks' $checks
    Set-Field $Entry 'check' $record
    return [pscustomobject]@{ batchId=$Current.id; participant=$Entry.id; revision=(Get-EditRevision $Entry); record=$record; plan=$plan }
}
function Complete-CheckRecord($Selection) {
    $report=$Selection.record; $code=3
    try {
        $report.summary=Invoke-LightCheck $Selection.plan
        Set-Field $report.summary 'preflight' (Get-ReadOnlyPreflight $repositoryRoot)
        $report.inputEnd=Get-TaskIdentity $Selection.plan
        $report.status=if($report.inputStart.digest -ne $report.inputEnd.digest){'invalidated'}elseif($report.summary.passed){'passed'}else{'failed'}
        $code=if($report.status -eq 'passed'){0}elseif($report.status -eq 'invalidated'){5}else{3}
    } catch { $report.status='failed'; $report.reason=$_.Exception.Message }
    $report.completedUtc=[DateTime]::UtcNow.ToString('o')
    $lease=Lock-State
    try {
        $latest=Read-State; $entry=$null
        if ($latest.current -and $latest.current.id -eq $Selection.batchId) { $entry=@($latest.current.participants | Where-Object id -eq $Selection.participant) | Select-Object -First 1 }
        $active=Get-Field $entry 'check'
        if (-not $entry -or (Get-EditRevision $entry) -ne $Selection.revision -or -not $active -or $active.operationId -ne $report.operationId) {
            $report.status='superseded'; $report.reason='Registration/check changed; this operation has no completion authority.'; $code=2
        } else { Set-Field $entry 'check' $report; Write-AtomicJson $latest $statePath }
        Write-AtomicJson $report (Join-Path $stateRoot ($report.operationId + '.check.json'))
    } finally { $lease.Dispose() }
    return [pscustomobject]@{ check=$report; exitCode=$code }
}
function Emit-Result($Result) {
    ConvertTo-Json -InputObject $Result -Depth 20
    exit $Result.exitCode
}

try {
    if ($ReloadShell -and $Command -notin 'ready','finish') { throw '-ReloadShell is only valid with finish.' }
    if ($Command -in 'begin', 'ready', 'finish', 'wait', 'check', 'plan', 'claim', 'commit', 'issue', 'repair' -and -not $Participant) { throw "$Command requires a participant ID." }
    if ($Command -in 'ready', 'finish', 'wait', 'check', 'plan', 'claim', 'commit', 'issue', 'recover', 'repair', 'repair-abandon' -and -not $Batch) { throw "$Command requires -Batch from begin; this prevents mixing batches." }
    if ($Command -eq 'begin' -and $Batch) { throw 'begin allocates its batch ID; do not supply -Batch.' }
    if ($Command -eq 'recover' -and (-not $ConfirmStopped -or [string]::IsNullOrWhiteSpace($Reason))) {
        throw 'Recovery requires -ConfirmStopped and -Reason after inspecting status and confirming the editor/build is stopped.'
    }
    if ($Command -in 'ready','check','plan','claim','commit','repair' -and $Revision -lt 0) { throw "$Command requires -Revision from begin; stale commands must not finish a new edit round." }
    [void][IO.Directory]::CreateDirectory($stateRoot)
    while ($true) {
        $metadata = Lock-State
        $buildLease = $null
        $selected = $null
        $checkSelected = $null
        $commitSelected = $null; $gitLease = $null
        try {
            $state = Read-State
            $current = $state.current
            if($Command -in 'begin','ready','finish','plan','claim','recover','repair','repair-abandon') {
                $gitProbe=Try-Lease 'git.lock';if(-not $gitProbe){throw 'A Git transaction is running; retry after its receipt is durable.'};$gitProbe.Dispose()
            }
            $result = if ($Batch) { Read-Result $Batch } else { $null }
            if ($Command -eq 'status') {
                if ($null -ne $result) { ConvertTo-Json -InputObject $result -Depth 20; exit 0 }
                if ($Batch -and ($null -eq $current -or $current.id -ne $Batch)) { throw 'Unknown batch.' }
                $diagnostic = if ($null -ne $current) {
                    [pscustomobject]@{
                        batch = $current; ownerState = Owner-State $current.owner
                        registrationAgesSeconds = @($current.participants | ForEach-Object {
                            [pscustomobject]@{ participant = $_.id; state = $_.state
                                ageSeconds = [int]([DateTime]::UtcNow - [DateTime]::Parse($_.registeredUtc).ToUniversalTime()).TotalSeconds }
                        })
                        persistedResult = Read-Result $current.id
                    }
                } else { $null }
                ConvertTo-Json -InputObject ([pscustomobject]@{ schemaVersion = 1; stateRoot = $stateRoot; current = $diagnostic }) -Depth 20
                exit 0
            }
            if ($Command -in 'finish','wait' -and $null -ne $result) {
                if ($null -ne $current -and $current.id -ne $Batch -and
                    @($current.participants | Where-Object id -eq $Participant).Count -gt 0) {
                    throw 'This participant has a newer active batch. Its old result cannot verify new edits; use the batch ID from the latest begin.'
                }
                if (@($result.participants | Where-Object id -eq $Participant).Count -ne 1) { throw 'Participant does not belong to this result.' }
                if((Get-Field $result 'protocolVersion' 1) -ge 2 -and $Revision -lt 0){throw 'Result requires persisted edit revision.'}
                Assert-EditRevision (@($result.participants | Where-Object id -eq $Participant)[0]) $Revision
                $completedEntry = @($result.participants | Where-Object id -eq $Participant)[0]
                if ($null -ne $finishedRevision -and (Participant-Revision $completedEntry) -ne $finishedRevision) {
                    throw 'This finish was superseded by a reopened registration. Use finish for the current edit revision.'
                }
                if (-not $hasFinishedCurrentBatch -and $result.outcome -eq 'passed') {
                    if (-not $result.PSObject.Properties['inputEnd'] -or $null -eq $result.inputEnd) {
                        throw 'This historical result has no input identity. Read it with status; begin a new batch to verify edits.'
                    }
                    Assert-ResultBinaries $result
                    $nowInputs = Get-BuildInputIdentity $repositoryRoot
                    if ($nowInputs.digest -ne $result.inputEnd.digest) {
                        throw 'Repository inputs differ from this historical result. Use status for history; begin a new batch for current edits.'
                    }
                }
                Emit-Result $result
            }
            elseif ($Command -eq 'repair-abandon') {
                if (-not $result -or $result.outcome -notin 'failed','invalidated','interrupted') { throw 'Abandon requires an unsuccessful saved attempt.' }
                if ([string]::IsNullOrWhiteSpace($Reason)) { throw 'Abandon requires a reason; saved evidence is retained.' }
                $chainPath=Join-Path $stateRoot ($Batch+'.repair.json')
                $chain=if([IO.File]::Exists($chainPath)){[IO.File]::ReadAllText($chainPath)|ConvertFrom-Json}else{[pscustomobject]@{parentBatchId=$Batch;childBatchId=$null;abandoned=$false}}
                if($current -and (Get-Field $current 'repairOf') -eq $Batch){throw 'An active repair cannot be abandoned for other participants. Withdraw only your stopped registration with begin/recover.'}
                Set-Field $chain 'abandoned' $true;Set-Field $chain 'reason' $Reason;Set-Field $chain 'updatedUtc' ([DateTime]::UtcNow.ToString('o'))
                Write-AtomicJson $chain $chainPath;$chain|ConvertTo-Json;exit 0
            }
            elseif ($Command -eq 'repair') {
                if (-not $result -or $result.outcome -notin 'failed','invalidated','interrupted') { throw 'Repair requires a failed/invalidated/interrupted saved attempt, never a live frozen batch.' }
                if([string]::IsNullOrWhiteSpace($Reason)){throw 'Repair requires -Reason with the failure/repair handoff.'}
                $prior=@($result.participants|Where-Object id -eq $Participant)
                if($prior.Count -ne 1 -or $prior[0].state -eq 'withdrawn'){throw 'Repair requester must belong to the unsuccessful attempt.'}
                Assert-EditRevision $prior[0] $Revision
                $chainPath=Join-Path $stateRoot ($Batch+'.repair.json')
                $chain=if([IO.File]::Exists($chainPath)){[IO.File]::ReadAllText($chainPath)|ConvertFrom-Json}else{$null}
                if($chain -and (Get-Field $chain 'abandoned' $false)){throw 'This repair was explicitly abandoned. Its evidence remains; begin an independent task if authorized.'}
                if($chain -and $chain.childBatchId -and (Read-Result $chain.childBatchId)){throw ('Repair attempt already ended. Read status for '+$chain.childBatchId+'; a further repair must name that unsuccessful child, preserving every attempt.')}
                if($current -and (Get-Field $current 'repairOf') -ne $Batch){throw 'Another batch is active. Use one local watch window wait, then retry repair; no registration was displaced.'}
                if(-not $current){
                    if($chain -and $chain.childBatchId){throw 'Repair mapping exists without active state/result. Preserve metadata and diagnose; no duplicate attempt created.'}
                    $members=@()
                    foreach($old in @($result.participants|Where-Object state -ne 'withdrawn')){
                        $entry=[pscustomobject]@{id=$old.id;state='finished';editRevision=((Get-EditRevision $old)+1);registeredUtc=[DateTime]::UtcNow.ToString('o');finishedUtc=[DateTime]::UtcNow.ToString('o');withdrawalReason=$null;carriedFrom=$Batch;repairRequested=$false}
                        $previous=Get-Field $old 'testPlan';if(-not $previous){$previous=Default-TaskPlan $old}
                        $plan=Default-TaskPlan $entry;$plan.source='repair-carried';$plan.reason='Repair carries prior requirements and conservatively reruns full automatic coverage.'
                        $plan.tests=@(Get-Field $previous 'tests' @()) # Explicit manual requests must survive the full automatic escalation.
                        Set-Field $plan 'originalRequirement' (Get-Field $previous 'originalRequirement' $previous);Set-Field $entry 'testPlan' $plan
                        Set-Field $entry 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=$entry.editRevision;reason='Prior ready evidence is not reused for repaired inputs.'})
                        $members += $entry
                    }
                    $current=[pscustomobject]@{id=[Guid]::NewGuid().ToString('N');protocolVersion=2;phase='editing';participants=$members;createdUtc=[DateTime]::UtcNow.ToString('o');buildStartedUtc=$null;owner=$null;logPath=$null;inputStart=$null;repairOf=$Batch;logicalBatchId=(Get-Field $result 'logicalBatchId' $Batch);attempt=([int](Get-Field $result 'attempt' 1)+1);repairReason=$Reason}
                    $state.current=$current
                }
                if($current.phase -ne 'editing'){throw 'Repair attempt is frozen. Observe it; no edit permission granted.'}
                $entry=@($current.participants|Where-Object id -eq $Participant)[0]
                if(-not (Get-Field $entry 'repairRequested' $false)){
                    Reopen-Entry $entry;Set-Field $entry 'repairRequested' $true
                    $plan=Get-Field $entry 'testPlan';$plan.editRevision=Get-EditRevision $entry;$plan.inputIdentity=$null
                }
                # State first: a crash before the mapping is saved is reconciled from repairOf under this same lease.
                Write-AtomicJson $state $statePath
                $chain=[pscustomobject]@{parentBatchId=$Batch;childBatchId=$current.id;logicalBatchId=$current.logicalBatchId;attempt=$current.attempt;abandoned=$false;reason=$Reason;updatedUtc=[DateTime]::UtcNow.ToString('o')}
                Write-AtomicJson $chain $chainPath
                foreach($peer in @($current.participants|Where-Object state -eq 'finished')){[void](Start-ReadyWorker $current $peer)}
                Write-AtomicJson $state $statePath
                [pscustomobject]@{participant=$Participant;batchId=$current.id;editRevision=(Get-EditRevision $entry);state=$entry.state;repairOf=$Batch;logicalBatchId=$current.logicalBatchId;attempt=$current.attempt;instruction='Claim files before writing. Prior requests are carried; prior checks/results are not reused. Duplicate repair does not reopen a finished editor.'}|ConvertTo-Json
                exit 0
            }
            elseif ($Command -eq 'begin') {
                if ($null -ne $current -and $null -ne (Read-Result $current.id)) {
                    throw 'The current result is durable but retirement was interrupted. Use status and recover before a new begin.'
                }
                if ($null -eq $current) {
                    $current = [pscustomobject]@{
                        id = [Guid]::NewGuid().ToString('N'); protocolVersion = 2; phase = 'editing'; participants = @()
                        createdUtc = [DateTime]::UtcNow.ToString('o'); buildStartedUtc = $null; owner = $null; logPath = $null; inputStart = $null
                    }
                    $state.current = $current
                }
                if ($current.phase -eq 'editing') {
                    $entry = @($current.participants | Where-Object id -eq $Participant)
                    if ($entry.Count -eq 1 -and $entry[0].state -eq 'finished') { Reopen-Entry $entry[0]; Write-AtomicJson $state $statePath }
                    if ($entry.Count -eq 0) {
                        $current.participants = @($current.participants) + @([pscustomobject]@{
                            id = $Participant; state = 'editing'; editRevision = 0; registeredUtc = [DateTime]::UtcNow.ToString('o')
                            finishedUtc = $null; withdrawalReason = $null
                        })
                        Write-AtomicJson $state $statePath
                    }
                    elseif ($entry[0].state -eq 'finished') {
                        $entry[0] | Add-Member -NotePropertyName editRevision -NotePropertyValue ((Participant-Revision $entry[0]) + 1) -Force
                        $entry[0].state = 'editing'
                        $entry[0].finishedUtc = $null
                        Write-AtomicJson $state $statePath
                    }
                    elseif ($entry[0].state -ne 'editing') { throw 'This participant was withdrawn. Use a different task ID or wait for the batch result.' }
                    ConvertTo-Json -InputObject ([pscustomobject]@{ participant = $Participant; batchId = $current.id; state = 'editing'; editRevision=(Get-EditRevision (@($current.participants | Where-Object id -eq $Participant)[0])) })
                    exit 0
                }
            }
            elseif ($Command -in 'plan','ready','check','claim','commit') {
                if (-not $current -or $current.id -ne $Batch -or $current.phase -ne 'editing') { throw 'Batch is frozen/retired. Use begin and wait for the next permitted editing window; no current input was changed.' }
                $entries=@($current.participants | Where-Object id -eq $Participant)
                if ($entries.Count -ne 1 -or $entries[0].state -eq 'withdrawn') { throw 'Participant is not active.' }
                $entry=$entries[0]; Assert-EditRevision $entry $Revision
                if ($Command -in 'ready','check','plan' -and (Get-Field $current 'protocolVersion' 1) -lt 2) { throw 'Legacy active batch: keep finish contract; advanced ready/plan/check begin with the next batch. No live migration.' }
                if ($Command -in 'claim','commit') {
                    if($entry.state -ne 'editing'){throw 'Claim/commit require begin/reopen before changing files or Git.'}
                    if($Command -eq 'claim'){
                        Set-Ownership $current $entry $Files $AdoptExistingChanges
                        Write-AtomicJson $state $statePath
                        $entry | ConvertTo-Json -Depth 10; exit 0
                    }
                    $gitLease=Try-Lease 'git.lock'; if(-not $gitLease){throw 'A cooperating Git transaction is running. Retry; shared index was not changed.'}
                    $commitSelected=[pscustomobject]@{batchId=$Batch;participant=$Participant;editRevision=$Revision;ownedFiles=@(Get-Field $entry 'ownedFiles' @())}
                } else {

                if ($Command -eq 'plan') {
                    if ($entry.state -ne 'editing') { throw 'Plan changes require atomic begin/reopen before editing.' }
                    Set-Field $entry 'testPlan' (New-TaskPlan $entry $Scope $Suites $Tests $Inputs $Reason)
                    Write-AtomicJson $state $statePath
                    ConvertTo-Json -InputObject ([pscustomobject]@{participant=$Participant;batchId=$Batch;editRevision=$Revision;testPlan=$entry.testPlan}) -Depth 15
                    exit 0
                } elseif ($Command -eq 'check') {
                    if ($entry.state -ne 'finished') { throw 'This recorded check is for ready/waiting tasks. Unregistered read-only work does not need registration.' }
                    $active=Get-Field $entry 'check'
                    if ($active -and $active.status -eq 'running' -and (Owner-State $active.owner) -in 'alive','unknown') { throw 'A check is already running; observe status instead of duplicating it.' }
                    $checkSelected=Start-CheckRecord $current $entry
                    Write-AtomicJson $state $statePath
                } else {
                    if ($entry.state -eq 'editing') {
                        $plan=Get-Field $entry 'testPlan'; if (-not $plan) { $plan=Default-TaskPlan $entry }
                        if ($plan.editRevision -ne $Revision) { throw 'Plan belongs to an older revision; update it before ready.' }
                        $plan.inputIdentity=Get-TaskIdentity $plan
                        Set-Field $entry 'testPlan' $plan
                        $entry.state='finished'; $entry.finishedUtc=[DateTime]::UtcNow.ToString('o')
                        Set-Field $entry 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=$Revision;reason='Post-ready lightweight check is queued.'})
                        Write-AtomicJson $state $statePath # crash leaves an explicit pending check, never permission to build
                    }
                    $check=Get-Field $entry 'check'
                    if ($check -and $check.status -in 'failed','invalidated','interrupted') { throw 'Check requires attention. Use begin/reopen for repairs, or check for a bounded read-only reassessment.' }
                    if($ReloadShell){Set-Field $current 'reloadShellRequested' $true}
                    $waiter=Start-ReadyWorker $current $entry
                    Write-AtomicJson $state $statePath
                    ConvertTo-Json -InputObject ([pscustomobject]@{participant=$Participant;batchId=$Batch;editRevision=$Revision;state='ready';checkStatus=(Get-Field (Get-Field $entry 'check') 'status');waiter=$waiter}) -Depth 8
                    exit 0
                }
                }
            }
            elseif ($Command -eq 'issue') {
                $path=Join-Path $stateRoot ($Batch+'.issues.json')
                if(($null -eq $current -or $current.id -ne $Batch) -and -not $result){throw 'Unknown batch for issue.'}
                if(@($(if($result){$result.participants}else{$current.participants}) | Where-Object id -eq $Participant).Count -ne 1){throw 'Issue reporter must belong to this batch.'}
                if([string]::IsNullOrWhiteSpace($Reason)){throw 'Issue requires -Reason with evidence or handoff.'}
                $ledger=if([IO.File]::Exists($path)){[IO.File]::ReadAllText($path)|ConvertFrom-Json}else{[pscustomobject]@{batchId=$Batch;issues=@()}}
                if($IssueId -and $IssueId -notmatch '^[a-f0-9]{32}$'){throw 'Invalid issue ID.'}
                $item=@($ledger.issues|Where-Object id -eq $IssueId) | Select-Object -First 1
                if(-not $item){if($IssueId){throw 'Unknown issue ID.'};$item=[pscustomobject]@{id=[Guid]::NewGuid().ToString('N');reportedBy=$Participant;assignee=$Assignee;state=$IssueState;reason=$Reason;updatedUtc=$null};$ledger.issues=@($ledger.issues)+@($item)}else{$item.assignee=$Assignee;$item.state=$IssueState;$item.reason=$Reason}
                $item.updatedUtc=[DateTime]::UtcNow.ToString('o');Write-AtomicJson $ledger $path;$item|ConvertTo-Json;exit 0
            }
            elseif ($Command -eq 'wait') {
                if (-not $current -or $current.id -ne $Batch) { throw 'Unknown current batch.' }
                $entry=@($current.participants | Where-Object id -eq $Participant)
                if ($entry.Count -ne 1) { throw 'Participant does not belong to batch.' }
                if((Get-Field $current 'protocolVersion' 1) -ge 2 -and $Revision -lt 0){throw 'wait requires -Revision.'}
                Assert-EditRevision $entry[0] $Revision
                # Observes only. It never marks an editing task ready.
            }
            elseif ($Command -eq 'recover') {
                if ($null -eq $current -or $current.id -ne $Batch) {
                    if ($null -ne $result) { ConvertTo-Json -InputObject $result -Depth 20; exit 0 }
                    throw 'Unknown current batch; recovery cannot touch another batch.'
                }
                if ($null -ne $result) {
                    $members = @($current.participants.id | Sort-Object)
                    $recorded = @($result.participants.id | Sort-Object)
                    $cancelled = $result.outcome -eq 'cancelled' -and $current.phase -eq 'editing' -and
                        @($current.participants | Where-Object state -ne 'withdrawn').Count -eq 0
                    if (($current.phase -ne 'building' -and -not $cancelled) -or (Owner-State $current.owner) -in 'alive', 'unknown' -or
                        @(Compare-Object $members $recorded).Count -ne 0) { throw 'Cannot reconcile a live or mismatched publication.' }
                    $state.current = $null
                    Write-AtomicJson $state $statePath
                }
                elseif ($current.phase -eq 'building') {
                    if ($Participant) { throw 'A building batch is frozen; recover the batch without a participant.' }
                    if ((Owner-State $current.owner) -ne 'exited') { throw 'Build owner is alive or cannot be inspected; recovery refused.' }
                    $buildLease = Try-Lease 'build.lock'
                    if ($null -eq $buildLease) { throw 'Build lease is still held; recovery refused.' }
                    $result = New-Result $current 'interrupted' 4 $Reason
                    Publish-Result $state $result
                }
                else {
                    $entries = @($current.participants | Where-Object id -eq $Participant)
                    if (-not $Participant -or $entries.Count -ne 1 -or $entries[0].state -eq 'finished') {
                        throw 'Editing recovery requires an editing/withdrawn participant; completed registrations are retained.'
                    }
                    $entries[0].state = 'withdrawn'
                    $entries[0].withdrawalReason = $Reason
                    if (@($current.participants | Where-Object state -ne 'withdrawn').Count -eq 0) {
                        $result = New-Result $current 'cancelled' 4 $Reason
                        Publish-Result $state $result
                    }
                    else { Write-AtomicJson $state $statePath }
                }
                ConvertTo-Json -InputObject ([pscustomobject]@{ recoveredBatch = $Batch; participant = $Participant; reason = $Reason; result = $result }) -Depth 20
                exit 0
            }
            else { # finish is idempotent, including a second concurrent waiter.
                if ($observedRevision -eq $null) { $observedRevision=$Revision }
                if ($null -eq $current -or $current.id -ne $Batch) { throw 'Unknown current batch; registration was not changed.' }
                $entries = @($current.participants | Where-Object id -eq $Participant)
                if ($entries.Count -ne 1 -or $entries[0].state -eq 'withdrawn') { throw 'Participant is not registered or was withdrawn.' }
                $currentRevision = Participant-Revision $entries[0]
                if ($null -ne $finishedRevision -and $finishedRevision -ne $currentRevision) {
                    throw 'This finish was superseded by a reopened registration. The task remains editing until its new finish.'
                }
                $finishedRevision = $currentRevision
                Assert-EditRevision $entries[0] $Revision
                if((Get-Field $current 'protocolVersion' 1) -ge 2 -and $observedRevision -lt 0){throw 'Protocol v2 finish requires -Revision from begin.'}
                Assert-EditRevision $entries[0] $observedRevision
                if($observedRevision -lt 0){$observedRevision=Get-EditRevision $entries[0]}
                $hasFinishedCurrentBatch = $true
                if ($current.phase -eq 'editing') {
                    # Remember an explicit request for this batch, even when
                    # another participant eventually owns the build. Shell
                    # cleanup remains inside that owner's build process.
                    if ($ReloadShell -and (-not $current.PSObject.Properties['reloadShellRequested'] -or -not $current.reloadShellRequested)) {
                        $current | Add-Member -NotePropertyName reloadShellRequested -NotePropertyValue $true -Force
                        Write-AtomicJson $state $statePath
                    }
                    if ($entries[0].state -eq 'editing') {
                        $entries[0].state = 'finished'
                        $entries[0].finishedUtc = [DateTime]::UtcNow.ToString('o')
                        Write-AtomicJson $state $statePath
                    }
                    $check=Get-Field $entries[0] 'check'
                    if($AutoCheck -and @($current.participants | Where-Object state -eq 'editing').Count -eq 0 -and $check -and $check.status -eq 'passed' -and (Get-Field $entries[0] 'testPlan').source -in 'legacy-default','repair-carried' -and (Get-TaskIdentity $entries[0].testPlan).digest -ne $check.inputEnd.digest){$check.status='pending';$check.reason='Global input changed while peers edited; repeat read-only checks.'}
                    if ($AutoCheck -and $check -and $check.status -eq 'pending' -and ((Get-Field (Get-Field $entries[0] 'testPlan') 'source') -ne 'repair-carried' -or @($current.participants | Where-Object state -eq 'editing').Count -eq 0)) {
                        $checkSelected=Start-CheckRecord $current $entries[0]
                        Write-AtomicJson $state $statePath
                    }
                    $badChecks=@($current.participants | Where-Object {$_.state -eq 'finished' -and (Get-Field (Get-Field $_ 'check') 'status') -in 'failed','invalidated','interrupted'})
                    if($AutoCheck -and $badChecks.Count -gt 0){
                        [pscustomobject]@{status='attention';batchId=$Batch;participant=$Participant;editRevision=$observedRevision;reason='A declared check needs explicit repair/reopen; no automatic repeat was started.';checks=@($badChecks | ForEach-Object {[pscustomobject]@{participant=$_.id;check=$_.check}})} | ConvertTo-Json -Depth 10
                        exit 2
                    }
                    if (@($current.participants | Where-Object state -eq 'editing').Count -eq 0 -and -not (Checks-BlockFreeze $current)) {
                        $buildLease = Try-Lease 'build.lock'
                        if ($null -eq $buildLease) { throw 'Unexpected occupied build lease; inspect status without clearing registrations.' }
                        # The metadata lease covers membership freeze and the
                        # input identity, closing the new-begin/start-build race.
                        try { $frozenPlan=New-FrozenPlan $current } catch {
                            Set-Field $current 'planStatus' 'blocked'; Set-Field $current 'planError' $_.Exception.Message
                            Write-AtomicJson $state $statePath; throw
                        }
                        $preflight=Get-Field $current 'preflight'
                        if(-not $preflight -or ([DateTime]::UtcNow-[DateTime]::Parse($preflight.observedUtc).ToUniversalTime()).TotalSeconds -gt 10){$preflight=Get-ReadOnlyPreflight $repositoryRoot}
                        Set-Field $current 'preflight' $preflight
                        if($frozenPlan.buildRequired -and $preflight.status -ne 'clear' -and -not (Get-Field $current 'reloadShellRequested' $false)) {
                            Set-Field $current 'planStatus' 'waiting-output-owner';Write-AtomicJson $state $statePath
                            $buildLease.Dispose();$buildLease=$null
                        } else {
                        Write-AtomicJson $frozenPlan (Join-Path $stateRoot ($current.id + '.plan.json'))
                        Set-Field $current 'testingPlan' $frozenPlan
                        $startInputs = Get-BuildInputIdentity $repositoryRoot
                        $current | Add-Member -NotePropertyName inputStart -NotePropertyValue $startInputs -Force
                        $current.phase = 'building'
                        $current.buildStartedUtc = [DateTime]::UtcNow.ToString('o')
                        $current.owner = [pscustomobject]@{ pid = $PID; startTicks = (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString() }
                        $current.logPath = Join-Path $stateRoot ($current.id + '.log')
                        Write-AtomicJson $state $statePath
                        $selected = $current
                        }
                    }
                }
            }
        }
        finally {
            $metadata.Dispose()
            if ($null -eq $selected -and $null -ne $buildLease) { $buildLease.Dispose() }
        }
        if ($commitSelected) {
            try {
                Push-Location $repositoryRoot
                try {$receipt=Invoke-OwnedCommit $commitSelected $Files $MessageFile} finally {Pop-Location}
                $metadata=Lock-State
                try {Write-AtomicJson $receipt (Join-Path $stateRoot ([Guid]::NewGuid().ToString('N')+'.commit.json'))} finally {$metadata.Dispose()}
                $receipt | ConvertTo-Json -Depth 10;exit 0
            } finally {$gitLease.Dispose()}
        }
        if ($null -ne $checkSelected) {
            $checkResult=Complete-CheckRecord $checkSelected
            if ($Command -eq 'check' -or $checkResult.exitCode -ne 0) { ConvertTo-Json -InputObject $checkResult -Depth 15; exit $checkResult.exitCode }
            continue
        }
        if ($null -ne $selected) {
            try {
                $code = 1; $errorText = ''; $outcome = 'failed'
                $oldNodeReuse = [Environment]::GetEnvironmentVariable('MSBUILDDISABLENODEREUSE', 'Process')
                try {
                    # Reused MSBuild workers from a prior, unrelated build are
                    # outside our job. Use fresh workers in this batch only.
                    [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE', '1', 'Process')
                    Add-Type -Path (Join-Path $PSScriptRoot 'build_job.cs')
                    [Console]::Error.WriteLine("Building batch $($selected.id). Log: $($selected.logPath)")
                    $code = [SnowDesktop.Build.Job]::RunBatch($repositoryRoot, $selected.logPath, $selected.id, $selected.testingPlan.buildRequired, [bool](Get-Field $selected 'reloadShellRequested' $false))
                    if ($code -eq 0) { $outcome = if($selected.testingPlan.mode -eq 'skipped'){'skipped'}else{'passed'} }
                    else { $errorText = "Build/test pipeline exited with code $code. See the batch log." }
                }
                catch { $errorText = $_.Exception.ToString() }
                finally { [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE', $oldNodeReuse, 'Process') }
                $pipelineCode = $code
                $endInputs = $null; $inputCheck = 'changed'
                try {
                    $endInputs = Get-BuildInputIdentity $repositoryRoot
                    if ($selected.inputStart.digest -eq $endInputs.digest) { $inputCheck = 'stable' }
                    else { $errorText += ' Repository build inputs changed during verification; outputs may contain mixed inputs. Begin a new batch.' }
                }
                catch { $errorText += ' Final input identity could not be verified: ' + $_.Exception.Message }
                if ($inputCheck -ne 'stable') { $outcome = 'invalidated'; $code = 5 }
                $metadata = Lock-State
                try {
                    $state = Read-State
                    $result = New-Result $selected $outcome $code $errorText
                    Set-Field $result 'testingPlan' $selected.testingPlan
                    $stagePath=Join-Path $stateRoot ($selected.id+'.stage.json');$buildPassed=$false
                    if([IO.File]::Exists($stagePath)){$stageRecord=[IO.File]::ReadAllText($stagePath)|ConvertFrom-Json;$buildPassed=$stageRecord.batchId -eq $selected.id -and $stageRecord.hostBuildPassed}
                    Set-Field $result 'binaryEvidence' (Get-BinaryEvidence $selected $buildPassed)
                    if($outcome -in 'failed','interrupted','invalidated'){Set-Field $result 'responsibility' 'unassigned; coverage identifies affected requests, not proven defect ownership; use issue for handoff.'}
                    $coveragePath=Join-Path $stateRoot ($selected.id + '.coverage.json')
                    if ([IO.File]::Exists($coveragePath)) { Set-Field $result 'coverage' ([IO.File]::ReadAllText($coveragePath,$utf8) | ConvertFrom-Json) }
                    $retryPath=Join-Path $stateRoot ($selected.id+'.retry.json')
                    if([IO.File]::Exists($retryPath)){Set-Field $result 'testRetry' ([IO.File]::ReadAllText($retryPath,$utf8)|ConvertFrom-Json)}
                    $result.inputEnd = $endInputs
                    $result.inputCheck = $inputCheck
                    $result.pipelineExitCode = $pipelineCode
                    Publish-Result $state $result
                }
                catch {
                    # A result committed before retirement failed is still the
                    # common outcome. Keep the batch blocked for explicit recovery.
                    $persisted = Read-Result $selected.id
                    if ($null -eq $persisted) { throw }
                    [Console]::Error.WriteLine('Result is durable; state retirement requires status/recover: ' + $_.Exception.Message)
                    $result = $persisted
                }
                finally { $metadata.Dispose() }
            }
            finally { $buildLease.Dispose() }
            Emit-Result $result
        }
        Wait-Again 'Waiting for this batch: editors must finish; a crashed build requires explicit recover. Do not modify files after finish.'
    }
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 2
}
