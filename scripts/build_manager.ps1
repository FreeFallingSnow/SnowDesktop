[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('begin', 'ready', 'ready-and-wait', 'finish', 'wait', 'check', 'plan', 'claim', 'commit', 'issue', 'status', 'recover', 'repair', 'repair-abandon', 'task-open', 'task-stage', 'task-close', 'task-list', 'event-post', 'event-read', 'event-wait', 'event-ack', 'edit-verify', 'execute', 'publish-run', 'evidence-list', 'acceptance-start', 'acceptance-complete', 'acceptance-list')][string]$Command,
    [Parameter(Position = 1)][ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$Participant,
    [ValidatePattern('^[a-f0-9]{32}$')][string]$Batch,
    [ValidatePattern('^[a-f0-9]{32}$')][string]$ExecutionRun,
    [ValidateRange(1, 86400)][int]$WaitSeconds = 86400,
    [switch]$ConfirmStopped,
    [switch]$ReloadShell,
    [switch]$CloseApplication,
    [string]$Reason,
    [long]$Revision = -1,
    [ValidateSet('unknown','module','docs','component','tool','public','infrastructure')][string]$Scope = 'unknown',
    [string]$Suites = 'selected', [string]$Tests = '', [string]$Inputs = '',
    [switch]$AdoptExistingChanges, [string]$Files = '', [string]$MessageFile = '',
    [string]$IssueId = '', [string]$Assignee = '', [ValidateSet('open','resolved','deferred')][string]$IssueState = 'open',
    [ValidateRange(0,4096)][int]$ResourceCpuSlots=0,[ValidateRange(0,1048576)][int]$ResourceMemoryMiB=0,
    [switch]$ResourceShared,
    [switch]$AutoCheck,
    [ValidateLength(0,160)][string]$TaskName='',
    [ValidateSet('investigating','editing','waiting','awaiting_input','blocked','validating','completed','cancelled')][string]$TaskPhase='investigating',
    [long]$TaskRevision=-1, [long]$RequirementsRevision=-1,
    [ValidateLength(0,2000)][string]$Progress='', [ValidateLength(0,2000)][string]$Blocker='',
    [ValidateSet('cli','codex','dsh')][string]$Adapter='cli',
    [ValidatePattern('^(\*|[a-zA-Z0-9][a-zA-Z0-9._-]{0,79})$')][string]$Target='*',
    [ValidateSet('requirements','conflict','input-invalidated','owner-lost','result','user-cancel','progress')][string]$EventKind='progress',
    [ValidateSet('normal','urgent')][string]$Importance='normal',
    [ValidateLength(0,4000)][string]$Text='', [ValidatePattern('^[a-zA-Z0-9._-]{1,100}$')][string]$EventKey,
    [long]$AfterSequence=0, [long]$Sequence=0,
    [ValidateSet('adapter','agent')][string]$AckLevel='adapter',
    [ValidatePattern('^[a-f0-9]{32}$')][string]$AcceptanceId,
    [ValidatePattern('^[a-f0-9]{32}$')][string]$CandidateRun,
    [ValidateLength(0,1000)][string]$AcceptanceCriterion='',
    [ValidateSet('passed','failed','cancelled')][string]$AcceptanceOutcome='passed',
    [ValidateSet('user','agent')][string]$Reporter='agent',
    [ValidateLength(0,2000)][string]$Reference='',
    [ValidateLength(0,4000)][string]$Notes='',
    [string]$WorkerRoot=''
)

# Foreground execution needs neither Python nor a hidden worker launch. It uses
# the same finish/check/freeze/lease path and returns the original batch code.
if($Command -eq 'ready-and-wait') {
    if(-not $PSBoundParameters.ContainsKey('WaitSeconds')){$WaitSeconds=1800}
    if($Revision -lt 0){throw 'ready-and-wait requires the latest -Revision.'}
    & $PSCommandPath finish $Participant -Batch $Batch -Revision $Revision -WaitSeconds $WaitSeconds -AutoCheck -ReloadShell:$ReloadShell -CloseApplication:$CloseApplication
    exit $LASTEXITCODE
}
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if($WorkerRoot){
    $repositoryRoot=[IO.Path]::GetFullPath($WorkerRoot).TrimEnd('\','/')
    if($Command -ne 'publish-run' -or -not $ExecutionRun -or $PSScriptRoot -ne (Join-Path $repositoryRoot ('.build/collaboration/runners/'+$ExecutionRun+'/scripts'))){throw 'WorkerRoot is reserved for the exact frozen batch publisher.'}
}
$stateRoot = Join-Path $repositoryRoot '.build\collaboration'
$statePath = Join-Path $stateRoot 'state.json'
$clock = [Diagnostics.Stopwatch]::StartNew()
$lastNotice = -30
$hasFinishedCurrentBatch = $false
$finishedRevision = $null
$utf8 = New-Object Text.UTF8Encoding($false)
. (Join-Path $PSScriptRoot 'build_entry.ps1')
if(Test-CandidateSourceRoot $repositoryRoot){throw 'Candidate source is executor-owned; register tasks and edit the originating checkout.'}
. (Join-Path $PSScriptRoot 'build_inputs.ps1')
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
. (Join-Path $PSScriptRoot 'build_ownership.ps1')
. (Join-Path $PSScriptRoot 'build_preflight.ps1')
if($Command -eq 'plan' -and ($ResourceCpuSlots -or $ResourceMemoryMiB)){Initialize-ResourceRuntime}
if(-not $WorkerRoot -and $Command -notin 'status','task-list','event-read','event-wait','event-ack','edit-verify','evidence-list','acceptance-list' -and [IO.File]::Exists((Join-Path $PSScriptRoot 'build_runtime.ps1'))) {
    . (Join-Path $PSScriptRoot 'build_runtime.ps1')
    Ensure-BuildDashboard
}
$observedRevision = $null

function Write-AtomicJson($Value, [string]$Path) {
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        $bytes = $utf8.GetBytes((ConvertTo-Json -InputObject $Value -Depth 20))
        if($Path -eq $statePath -and $bytes.Length -gt 6MB){throw 'Collaboration metadata reached its byte budget; preserve state and archive through a reviewed migration. No event or task was discarded.'}
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

function Get-BatchWaitMessage($Current) {
    if(-not $Current){return 'Waiting for batch retirement and its common result.'}
    $message='Waiting for batch '+$Current.id
    if($Current.phase -eq 'building'){
        $runId=Get-Field $Current 'runId' ''
        if($runId -match '^[a-f0-9]{32}$'){
            try {
                $directory=Join-Path $stateRoot 'runs'
                $request=Read-EntryJson (Join-Path $directory ($runId+'.request.json'))
                $record=Read-EntryJson (Join-Path $directory ($runId+'.json'))
                if(-not $record -or -not $request -or $record.id -ne $runId -or $request.id -ne $runId -or
                    $record.repositoryRoot -ne $repositoryRoot -or $request.repositoryRoot -ne $repositoryRoot -or
                    $request.batch -ne $Current.id -or $record.nonce -ne $request.nonce){throw 'Run observation identity unavailable.'}
                $status=[string]$record.status
                if($status -eq 'running'){
                    $message+=': executing Run '+$runId
                    try {
                        . (Join-Path $PSScriptRoot 'build_telemetry.ps1')
                        $events=@(Read-BuildStageEvents $repositoryRoot $runId|Where-Object {$_.kind -in 'start','end'})
                        if($events.Count){$latest=$events[-1];$message+='; latest stage '+$latest.kind+' '+$latest.name}
                        else{$message+='; stage unavailable (no observation yet)'}
                    } catch {$message+='; stage unavailable (observation failed)'}
                } elseif($status -eq 'queued'){
                    $waiting=[string](Get-Field $record 'waitingFor' '')
                    $message+=': Run '+$runId+' queued; waiting for '+$(if($waiting){$waiting}else{'resource reason not recorded'})
                } elseif($status -eq 'starting'){$message+=': Run '+$runId+' starting'}
                elseif($status -in 'passed','failed','cancelled','start-failed','invalidated','interrupted'){$message+=': Run '+$runId+' '+$status+'; common result pending'}
                else{throw 'Run observation status unavailable.'}
            } catch {$message+=': Run '+$runId+' state unavailable (observation failed)'}
        } else {$message+=': executing retained build/test pipeline'}
    } elseif($Current.phase -eq 'editing'){
        $editors=@($Current.participants|Where-Object state -eq 'editing'|ForEach-Object id)
        $checks=@($Current.participants|Where-Object {$_.state -eq 'finished' -and (Get-Field (Get-Field $_ 'check') 'status') -in 'pending','running'}|ForEach-Object id)
        $attention=@($Current.participants|Where-Object {$_.state -eq 'finished' -and (Get-Field (Get-Field $_ 'check') 'status') -in 'failed','invalidated','interrupted'}|ForEach-Object id)
        $message+=': editing phase'
        if($editors.Count){$message+='; editing peers '+(($editors|Select-Object -First 8) -join ', ')}
        if($checks.Count){$message+='; checks running/pending '+(($checks|Select-Object -First 8) -join ', ')}
        if($attention.Count){$message+='; checks need attention '+(($attention|Select-Object -First 8) -join ', ')}
        if((Get-Field $Current 'planStatus') -eq 'blocked'){$message+='; plan blocked'}
    } else {$message+=': phase '+$Current.phase}
    return $message+'.'
}

function Wait-Again([string]$Message,$Current=$null) {
    if ($clock.Elapsed.TotalSeconds -ge $WaitSeconds) {
        if($Current){$Message=Get-BatchWaitMessage $Current}
        throw "Wait timed out. Registration was retained; use status and repeat the same command. $Message"
    }
    if ($clock.Elapsed.TotalSeconds - $script:lastNotice -ge 30) {
        # Stage reads happen only when reporting, after releasing the state
        # transaction. Observing must never relaunch or cancel the Run.
        if($Current){$Message=Get-BatchWaitMessage $Current}
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
    $state = Read-EntryJson $statePath
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
    $active=@(Get-Field $state 'activeCandidates' @())
    if($active.Count -gt 32 -or @($active|ForEach-Object id|Sort-Object -Unique).Count -ne $active.Count){throw 'Invalid active candidate registry; preserve it for diagnosis.'}
    foreach($attempt in $active){
        if($attempt.id -notmatch '^[a-f0-9]{32}$' -or $attempt.runId -notmatch '^[a-f0-9]{32}$' -or
            $attempt.protocolVersion -ne 4 -or $attempt.phase -ne 'building' -or -not (Get-Field $attempt 'candidateBinding') -or
            ($state.current -and $state.current.id -eq $attempt.id)){throw 'Invalid captured candidate attempt; preserve the editing window and registry.'}
    }
    return $state
}

function Result-Path([string]$Id) { return Join-Path $stateRoot ($Id + '.json') }
function Find-BoundBatch($State,[string]$Id) {
    if($State.current -and $State.current.id -eq $Id){return $State.current}
    return @(Get-Field $State 'activeCandidates' @()|Where-Object id -eq $Id)|Select-Object -First 1
}
function Retire-BoundBatch($State,[string]$Id) {
    if($State.current -and $State.current.id -eq $Id){$State.current=$null;return}
    if(-not (Find-BoundBatch $State $Id)){throw 'No matching batch to retire.'}
    Set-Field $State 'activeCandidates' @(Get-Field $State 'activeCandidates' @()|Where-Object id -ne $Id)
}
function Participant-Revision($Entry) {
    if ($Entry.PSObject.Properties['editRevision']) { return [long]$Entry.editRevision }
    return [long]0 # Registrations made before reopen support remain readable.
}
function Read-Result([string]$Id) {
    $path = Result-Path $Id
    if (-not [IO.File]::Exists($path)) { return $null }
    $result = Read-EntryJson $path
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
        runId=(Get-Field $Current 'runId')
        candidateBinding=(Get-Field $Current 'candidateBinding');capturedUtc=(Get-Field $Current 'capturedUtc')
        logPath = $Current.logPath; commands = if(Get-Field $Current 'testingPlan'){@($(if($Current.testingPlan.buildRequired){'scripts\build.bat'});'scripts\build_batch_tests.ps1 -Batch '+$Current.id)}else{@('scripts\build.bat','scripts\test.bat')}
        inputStart = if ($Current.PSObject.Properties['inputStart']) { $Current.inputStart } else { $null }
        inputEnd = $null; inputCheck = 'not-run'; pipelineExitCode = $null
    }
}
function Publish-Result($State, $Result) {
    # Called with the metadata lease. Only this frozen batch is retired, and
    # only after its immutable result is durable. New begin cannot enter here.
    if (-not (Find-BoundBatch $State $Result.batchId)) { throw 'Batch changed before publication.' }
    $existing = Read-Result $Result.batchId
    if ($null -eq $existing) { Write-AtomicJson $Result (Result-Path $Result.batchId) }
    else { throw 'A batch result already exists; use recover to finish its publication.' }
    Retire-BoundBatch $State $Result.batchId
    Write-AtomicJson $State $statePath
}
function Assert-BatchRunCaller($Current,[switch]$Leased){
    if(-not $ExecutionRun -or -not $Current -or $Current.id -ne $Batch -or $Current.phase -ne 'building' -or
        (Get-Field $Current 'runId') -ne $ExecutionRun -or (Get-Field $Current 'protocolVersion' 1) -lt 3){throw 'No matching durable batch reservation.'}
    $request=Read-EntryJson (Join-Path $stateRoot ('runs/'+$ExecutionRun+'.request.json'))
    $record=Read-EntryJson (Join-Path $stateRoot ('runs/'+$ExecutionRun+'.json'))
    if($WorkerRoot -and -not (Get-Field $request 'candidateBatch' $false)){throw 'Frozen publication requires a candidate batch request.'}
    if(-not $request -or -not $record -or $request.repositoryRoot -ne $repositoryRoot -or $request.id -ne $ExecutionRun -or
        $request.action -ne 'batch' -or $request.batch -ne $Batch -or $request.task -ne $Participant -or $request.editRevision -ne $Revision -or
        $record.id -ne $ExecutionRun -or $record.repositoryRoot -ne $repositoryRoot -or $record.nonce -ne $request.nonce -or $record.owner.pid -ne $PID -or
        $record.owner.startTicks -ne (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString() -or $record.status -notin 'queued','running'){
        throw 'Batch execution/publication requires its live immutable Run owner.'
    }
    if($Leased -and (-not (Test-EntryDelegation $repositoryRoot) -or $Current.owner.pid -ne $PID -or $Current.owner.startTicks -ne $record.owner.startTicks)){
        throw 'Batch execution requires the Run-owned output lease.'
    }
    return $record
}
function Start-ReadyWorker($Current, $Entry) {
    $existing = Get-Field $Entry 'waiter'
    if ($existing -and (Get-Field $existing 'pid') -and (Get-Field $existing 'editRevision' -1) -eq (Get-EditRevision $Entry) -and (Owner-State $existing) -eq 'alive') { return $existing }
    $revision = Get-EditRevision $Entry
    $waiterScript=Join-Path $PSScriptRoot 'build_waiter.ps1'
    $arguments='-Participant '+$Entry.id+' -Batch '+$Current.id+' -Revision '+$revision
    $output=Join-Path $stateRoot ('waiter-'+[Guid]::NewGuid().ToString('N'))
    try {$worker=Start-BuildPowerShellScript -Script $waiterScript -Arguments $arguments -WorkingDirectory $repositoryRoot -OutputPath ($output+'.out') -ErrorPath ($output+'.err')} catch {
        $unavailable=[pscustomobject]@{status='unavailable';editRevision=$revision;reason=$_.Exception.Message;
            next=('scripts/build.bat ready-and-wait '+$Entry.id+' -Batch '+$Current.id+' -Revision '+$revision)}
        Set-Field $Entry 'waiter' $unavailable
        return $unavailable
    }
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
    if ($CloseApplication -and $Command -notin 'ready','finish') { throw '-CloseApplication is only valid with ready/finish.' }
    if ($ReloadShell -and $CloseApplication) { throw 'Choose only one output-owner action.' }
    if ($Command -in 'begin', 'ready', 'finish', 'wait', 'check', 'plan', 'claim', 'commit', 'issue', 'repair' -and -not $Participant) { throw "$Command requires a participant ID." }
    if ($Command -in 'ready', 'finish', 'wait', 'check', 'plan', 'claim', 'commit', 'issue', 'recover', 'repair', 'repair-abandon' -and -not $Batch) { throw "$Command requires -Batch from begin; this prevents mixing batches." }
    if ($Command -eq 'begin' -and $Batch) { throw 'begin allocates its batch ID; do not supply -Batch.' }
    if ($Command -eq 'recover' -and (-not $ConfirmStopped -or [string]::IsNullOrWhiteSpace($Reason))) {
        throw 'Recovery requires -ConfirmStopped and -Reason after inspecting status and confirming the editor/build is stopped.'
    }
    if ($Command -in 'ready','check','plan','claim','commit','repair' -and $Revision -lt 0) { throw "$Command requires -Revision from begin; stale commands must not finish a new edit round." }
    if($Command -eq 'status') {
        $state=Read-State;$current=if($Batch){Find-BoundBatch $state $Batch}else{$state.current};$result=if($Batch){Read-Result $Batch}else{$null}
        if($result){$result|ConvertTo-Json -Depth 20;exit 0}
        if($Batch -and (-not $current -or $current.id -ne $Batch)){throw 'Unknown batch.'}
        $diagnostic=if($current){[pscustomobject]@{batch=$current;ownerState=(Owner-State $current.owner);persistedResult=(Read-Result $current.id);
            registrationAgesSeconds=@($current.participants|ForEach-Object {[pscustomobject]@{participant=$_.id;state=$_.state;ageSeconds=[int](Get-TimestampAge $_.registeredUtc)}})}}else{$null}
        [pscustomobject]@{schemaVersion=1;stateRoot=$stateRoot;current=$diagnostic;activeCandidates=@(Get-Field $state 'activeCandidates' @());tasks=(Get-Field (Get-Field $state 'taskLedger') 'tasks' @());execution=(Read-EntryJson (Join-Path $stateRoot 'execution.json'));observation='Atomic read-only snapshot; execution record may be historical.'}|ConvertTo-Json -Depth 20
        exit 0
    }
    if($Command -in 'evidence-list','acceptance-list'){
        $state=Read-State
        if($Command -eq 'evidence-list'){
            if(-not $Batch){throw 'evidence-list requires a saved -Batch.'}
            $saved=Read-Result $Batch;if(-not $saved){throw 'No immutable result is available for this batch.'}
            $evidence=Get-Field $saved 'evidence'
            [pscustomobject]@{batchId=$Batch;status=(Get-Field $evidence 'status' 'not-recorded');tests=@(Get-Field $evidence 'tests' @()|Where-Object {-not $Participant -or $_.requesters -contains $Participant});observation='A recorded identity does not establish dependency closure or automatic reuse.'}|ConvertTo-Json -Depth 20
        }else{
            [pscustomobject]@{acceptances=@(Get-Field (Get-Field $state 'taskLedger') 'acceptances' @()|Where-Object {(-not $Participant -or $_.task -eq $Participant) -and (-not $Batch -or $_.batchId -eq $Batch)})}|ConvertTo-Json -Depth 20
        }
        exit 0
    }
    if($Command -in 'acceptance-start','acceptance-complete'){
        if(-not $Participant){throw 'Acceptance requires a registered task ID.'}
        . (Join-Path $PSScriptRoot 'build_acceptance.ps1')
        $metadata=Lock-State
        try{
            $state=Read-State;$task=Find-VisibleTask (Get-TaskLedger $state) $Participant;Assert-TaskRevision $task $TaskRevision
            if($task.phase -in 'completed','cancelled'){throw 'A closed task cannot open or complete acceptance.'}
            if($Command -eq 'acceptance-start'){
                if($CandidateRun){
                    if($Batch -or $Revision -ge 0){throw 'CandidateRun acceptance cannot also name a batch/edit revision.'}
                    $request=Read-EntryJson (Join-Path $stateRoot ('runs/'+$CandidateRun+'.request.json'))
                    $saved=Read-EntryJson (Join-Path $stateRoot ('runs/'+$CandidateRun+'.json'))
                    $record=Start-RunAcceptance $repositoryRoot $state $task $CandidateRun $request $saved $AcceptanceCriterion $Files
                }else{
                    if(-not $Batch -or $Revision -lt 0){throw 'Acceptance start requires CandidateRun or the candidate batch and edit revision.'}
                    $saved=Read-Result $Batch;if(-not $saved){throw 'Candidate result is not available.'}
                    $record=Start-TaskAcceptance $repositoryRoot $state $task $saved $Revision $AcceptanceCriterion $Files
                }
            }else{
                if(-not $AcceptanceId){throw 'Acceptance completion requires -AcceptanceId.'}
                $record=Complete-TaskAcceptance $repositoryRoot $state $task $AcceptanceId $AcceptanceOutcome $Reporter $Reference $Notes
            }
            if([Text.Encoding]::UTF8.GetByteCount(($state|ConvertTo-Json -Depth 30)) -gt 7MB){throw 'Acceptance would exceed the metadata budget; preserve records for reviewed archival.'}
            Write-AtomicJson $state $statePath
            [pscustomobject]@{acceptance=$record;editingAuthority=$false;observation='Reported manual acceptance does not alter build/test results or publish a release.'}|ConvertTo-Json -Depth 20
        }finally{$metadata.Dispose()}
        exit 0
    }
    if($Command -in 'task-list','event-read') {
        $state=Read-State;$ledger=Get-Field $state 'taskLedger'
        if($Command -eq 'task-list') {[pscustomobject]@{tasks=@(Get-Field $ledger 'tasks' @());sequence=(Get-Field $ledger 'sequence' 0);sourceChanges=(Get-SourceChangeAudit $state)}|ConvertTo-Json -Depth 20}
        else {
            if(-not $Participant){throw 'event-read requires a task ID.'}
            [pscustomobject]@{events=@(Get-TaskEvents $ledger $Participant $AfterSequence | Select-Object -First 100);sequence=(Get-Field $ledger 'sequence' 0);delivery='Reading is not acknowledgement.'}|ConvertTo-Json -Depth 20
        }
        exit 0
    }
    if($Command -in 'task-open','task-stage','task-close','event-post','event-wait','event-ack','edit-verify' -and -not $Participant){throw "$Command requires a task ID."}
    if($Command -eq 'event-wait' -and -not $PSBoundParameters.ContainsKey('WaitSeconds')){$WaitSeconds=30}
    [void][IO.Directory]::CreateDirectory($stateRoot)
    while ($true) {
        $metadata = Lock-State
        $buildLease = $null
        $selected = $null
        $checkSelected = $null
        $commitSelected = $null; $gitLease = $null; $waitingForGit = $false
        $cohortSealed=$false
        try {
            $state = Read-State
            $current = $state.current
            if($Batch -and (-not $current -or $current.id -ne $Batch)){$bound=Find-BoundBatch $state $Batch;if($bound){$current=$bound}}
            if($Command -in 'task-open','task-stage','task-close','event-post','event-wait','event-ack') {
                $ledger=Get-TaskLedger $state
                if($Command -eq 'task-open') {
                    $task=Open-VisibleTask $ledger $Participant $TaskName $Adapter 'task-open'
                    $task.lastHeartbeatUtc=[DateTime]::UtcNow.ToString('o')
                } else { $task=Find-VisibleTask $ledger $Participant }
                if($Command -in 'task-stage','task-close') {
                    Assert-TaskRevision $task $TaskRevision
                    if($task.phase -in 'completed','cancelled'){throw 'Closed task cannot change phase.'}
                    if($TaskPhase -in 'completed','awaiting_input','cancelled') {
                        $sourceLease=Try-Lease 'git.lock';if(-not $sourceLease){throw 'Git transaction is active; retry task closure without changing its revision.'}
                        try {
                            $audit=Get-SourceChangeAudit $state
                            $pending=@($audit.changes|Where-Object task -eq $Participant)
                            if($pending.Count -and $TaskPhase -ne 'cancelled'){throw ('Uncommitted task paths: '+(($pending|ForEach-Object path) -join ', ')+'. Commit or explicitly hand off these paths before awaiting input/completion.')}
                            Set-Field $task 'sourceChanges' ([pscustomobject]@{checkedUtc=$audit.checkedUtc;pending=$pending})
                        } finally {$sourceLease.Dispose()}
                    }
                    if($Command -eq 'task-close') {
                        if($TaskPhase -notin 'completed','cancelled'){throw 'task-close requires completed/cancelled.'}
                        if($state.current -and @($state.current.participants | Where-Object {$_.id -eq $Participant -and $_.state -ne 'withdrawn'}).Count){throw 'Close only after the edit batch has ended; task-close never withdraws editing authority.'}
                        if(@(Get-Field $state 'activeCandidates' @()|ForEach-Object participants|Where-Object {$_.id -eq $Participant -and $_.state -ne 'withdrawn'}).Count){throw 'Close only after the captured candidate attempts have ended; task-close never cancels their Runs.'}
                        if(@((Get-Field $ledger 'acceptances' @())|Where-Object {$_.task -eq $Participant -and $_.status -eq 'running'}).Count){throw 'Complete or cancel the open acceptance interval before closing its task.'}
                        $task.closedUtc=[DateTime]::UtcNow.ToString('o')
                    } elseif($TaskPhase -in 'completed','cancelled'){throw 'Use task-close for terminal phases.'}
                    $task.phase=$TaskPhase;$task.progress=$Progress;$task.blocker=$Blocker
                    $task.lastHeartbeatUtc=[DateTime]::UtcNow.ToString('o');$task.lastProgressUtc=$task.lastHeartbeatUtc
                    $task.revision=[long]$task.revision+1
                    if($RequirementsRevision -ge 0){
                        if($RequirementsRevision -lt $task.requirementsRevision){throw 'Requirements revision cannot move backwards.'}
                        $task.requirementsRevision=$RequirementsRevision
                    }
                }
                if($Command -eq 'event-post') {
                    if(-not $EventKey -or [string]::IsNullOrWhiteSpace($Text)){throw 'event-post requires EventKey and Text.'}
                    $existing=@($ledger.events | Where-Object key -eq $EventKey)
                    if($existing.Count){
                        $event=$existing[0]
                        if($event.sender -ne $Participant -or $event.target -ne $Target -or $event.kind -ne $EventKind -or $event.importance -ne $Importance -or $event.text -cne $Text -or $event.requirementsRevision -ne $RequirementsRevision){throw 'EventKey already belongs to a different event.'}
                    } else {
                        if(@($ledger.events).Count -ge 4096){throw 'Event ledger is full; preserve unconsumed events and archive through a reviewed migration.'}
                        $recipients=if($Target -eq '*'){@($ledger.tasks | Where-Object {$_.phase -notin 'completed','cancelled'} | ForEach-Object {$_.id})}else{@((Find-VisibleTask $ledger $Target).id)}
                        $ledger.sequence=[long]$ledger.sequence+1
                        $event=[pscustomobject]@{sequence=$ledger.sequence;key=$EventKey;sender=$Participant;target=$Target;recipients=@($recipients);kind=$EventKind;importance=$Importance;text=$Text;
                            requirementsRevision=$RequirementsRevision;createdUtc=[DateTime]::UtcNow.ToString('o');receipts=@();delivery='persisted'}
                        $ledger.events=@($ledger.events)+@($event)
                    }
                    Write-AtomicJson $state $statePath;$event|ConvertTo-Json -Depth 20;exit 0
                }
                if($Command -eq 'event-ack') {
                    $events=@(Get-TaskEvents $ledger $Participant | Where-Object sequence -eq $Sequence)
                    if($events.Count -ne 1){throw 'Event is not addressed to this task.'}
                    $event=$events[0]
                    if(@($event.receipts | Where-Object {$_.task -eq $Participant -and $_.level -eq $AckLevel}).Count -eq 0){$event.receipts=@($event.receipts)+@([pscustomobject]@{task=$Participant;level=$AckLevel;utc=[DateTime]::UtcNow.ToString('o')})}
                    Write-AtomicJson $state $statePath;$event|ConvertTo-Json -Depth 20;exit 0
                }
                if($Command -eq 'event-wait') {
                    $events=@(Get-TaskEvents $ledger $Participant $AfterSequence | Select-Object -First 100)
                    if($events.Count -or $clock.Elapsed.TotalSeconds -ge $WaitSeconds){[pscustomobject]@{events=$events;sequence=$ledger.sequence;timedOut=($events.Count -eq 0);delivery='No automatic agent acknowledgement or host wake.'}|ConvertTo-Json -Depth 20;exit 0}
                } else {
                    Write-AtomicJson $state $statePath
                    [pscustomobject]@{task=$task;editingAuthority=$false;next=@('task-stage','begin then claim before writing');observation='Task presence does not join the edit barrier.'}|ConvertTo-Json -Depth 20;exit 0
                }
            }
            if($Command -in 'finish','wait' -and $current -and $current.id -eq $Batch -and ($hasFinishedCurrentBatch -or $Command -eq 'wait')) {
                $ledger=Get-Field $state 'taskLedger'
                if($ledger){
                    $urgent=@(Get-TaskEvents $ledger $Participant 0 -UrgentUnacknowledged)
                    if($urgent.Count){[pscustomobject]@{attention='urgent-event';events=$urgent;batchId=$Batch;editRevision=$Revision;registrationRetained=$true;next='Read event, acknowledge explicitly, then re-evaluate current revision.'}|ConvertTo-Json -Depth 20;exit 6}
                }
            }
            if($Command -eq 'edit-verify') {
                if(-not $current -or $current.id -ne $Batch -or $current.phase -ne 'editing'){throw 'No active editing window.'}
                $entries=@($current.participants | Where-Object {$_.id -eq $Participant -and $_.state -eq 'editing'})
                if($entries.Count -ne 1){throw 'No active edit grant.'};Assert-EditRevision $entries[0] $Revision
                $paths=@($Files.Split(',') | Where-Object {$_} | ForEach-Object {Normalize-OwnedPath $_})
                if(-not $paths.Count){throw 'edit-verify requires explicit Files.'}
                foreach($path in $paths){if(@(Get-Field $entries[0] 'ownedFiles' @() | Where-Object {$path.Equals($_,[StringComparison]::OrdinalIgnoreCase) -or $path.StartsWith($_+'/',[StringComparison]::OrdinalIgnoreCase)}).Count -eq 0){throw "No edit grant for $path"}}
                [pscustomobject]@{allowed=$true;batchId=$Batch;editRevision=$Revision;paths=$paths;observation='Point-in-time verification; only an integrated write gate enforces subsequent writes.'}|ConvertTo-Json -Depth 10;exit 0
            }
            if(-not $current -and $Command -in 'begin','repair') {
                $outputProbe=Try-Lease 'build.lock'
                if(-not $outputProbe){throw 'A standalone shared-output entry owns build.lock. No editor registered; read status and retry after execution finishes.'}
                $outputProbe.Dispose()
            }
            if($Command -in 'begin','ready','finish','plan','claim','recover','repair','repair-abandon') {
                $gitProbe=Try-Lease 'git.lock'
                if(-not $gitProbe){
                    if($Command -eq 'finish' -and $current -and $current.id -eq $Batch){
                        $members=@($current.participants|Where-Object {$_.id -eq $Participant -and $_.state -ne 'withdrawn'})
                        if($members.Count -ne 1){throw 'Participant is not active.'}
                        Assert-EditRevision $members[0] $Revision
                        $urgent=@(Get-TaskEvents (Get-Field $state 'taskLedger') $Participant 0 -UrgentUnacknowledged)
                        if($urgent.Count){[pscustomobject]@{attention='urgent-event';events=$urgent;batchId=$Batch;editRevision=$Revision;registrationRetained=$true;next='Read event, acknowledge explicitly, then re-evaluate current revision.'}|ConvertTo-Json -Depth 20;exit 6}
                        # Continue only after finally releases the state transaction.
                        # The same observer, revision and deadline remain active.
                        $waitingForGit=$true;continue
                    }
                    throw 'A Git transaction is running; retry after its receipt is durable.'
                }
                $gitProbe.Dispose()
            }
            $result = if ($Batch) { Read-Result $Batch } else { $null }
            if ($Command -eq 'event-wait') { # Its bounded observation loop resumes after releasing metadata, below.
            }
            elseif ($Command -in 'finish','wait' -and $null -ne $result) {
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
                    # Captured v4 results were just checked against their
                    # actual frozen source. Original checkout edits are a new
                    # input set, never a replacement for that snapshot.
                    if((Get-Field $result 'protocolVersion' 1) -lt 4 -or -not (Get-Field $result 'candidateBinding')){
                        $nowInputs = Get-BuildInputIdentity $repositoryRoot
                        if ($nowInputs.digest -ne $result.inputEnd.digest) {
                            throw 'Repository inputs differ from this historical result. Use status for history; begin a new batch for current edits.'
                        }
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
                if($current -and (Get-Field $current 'repairOf') -ne $Batch){throw ('Another batch is active: '+$current.id+' / '+$current.phase+' / '+(($current.participants|ForEach-Object {$_.id+':'+$_.state}) -join ', ')+'. Read scripts/build.bat status; use one window wait before repair. No registration displaced.')}
                if(-not $current){
                    if($chain -and $chain.childBatchId){throw 'Repair mapping exists without active state/result. Preserve metadata and diagnose; no duplicate attempt created.'}
                    $members=@()
                    foreach($old in @($result.participants|Where-Object state -ne 'withdrawn')){
                        $entry=[pscustomobject]@{id=$old.id;state='finished';editRevision=((Get-EditRevision $old)+1);registeredUtc=[DateTime]::UtcNow.ToString('o');finishedUtc=[DateTime]::UtcNow.ToString('o');withdrawalReason=$null;carriedFrom=$Batch;repairRequested=$false}
                        $previous=Get-Field $old 'testPlan';if(-not $previous){$previous=Default-TaskPlan $old}
                        $plan=$previous|ConvertTo-Json -Depth 20|ConvertFrom-Json
                        $plan.source='repair-carried';$plan.editRevision=$entry.editRevision;$plan.inputIdentity=$null
                        # Preserve explicit requirements, including none/manual;
                        # scope alone never escalates a repair to full coverage.
                        $plan.requiredFull = $plan.suites -contains 'full'
                        Set-Field $plan 'originalRequirement' (Get-Field $previous 'originalRequirement' $previous);Set-Field $entry 'testPlan' $plan
                        Set-Field $entry 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=$entry.editRevision;reason='Prior ready evidence is not reused for repaired inputs.'})
                        $members += $entry
                    }
                    $current=[pscustomobject]@{id=[Guid]::NewGuid().ToString('N');protocolVersion=$(if((Get-Field $result 'protocolVersion' 1) -ge 4){4}elseif((Get-Field $result 'protocolVersion' 1) -ge 3){3}else{2});phase='editing';participants=$members;createdUtc=[DateTime]::UtcNow.ToString('o');buildStartedUtc=$null;owner=$null;logPath=$null;inputStart=$null;repairOf=$Batch;logicalBatchId=(Get-Field $result 'logicalBatchId' $Batch);attempt=([int](Get-Field $result 'attempt' 1)+1);repairReason=$Reason}
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
                        id = [Guid]::NewGuid().ToString('N'); protocolVersion = 4; phase = 'editing'; participants = @()
                        createdUtc = [DateTime]::UtcNow.ToString('o'); buildStartedUtc = $null; owner = $null; logPath = $null; inputStart = $null
                    }
                    $state.current = $current
                }
                if ($current.phase -eq 'editing') {
                    if((Get-Field $current 'protocolVersion' 1) -ge 4 -and (Get-Field $current 'membershipSealed' $false) -and
                        @($current.participants|Where-Object id -eq $Participant).Count -eq 0){
                        # A new editor cannot extend a cohort whose first member
                        # is already ready. The outer wait releases metadata.
                        $cohortSealed=$true
                    }else{
                    $ledger=Get-TaskLedger $state
                    $task=Open-VisibleTask $ledger $Participant $TaskName $Adapter 'begin-compatibility'
                    if($task.phase -ne 'editing'){$task.phase='editing';$task.revision=[long]$task.revision+1;$task.lastProgressUtc=[DateTime]::UtcNow.ToString('o')}
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
                    Write-AtomicJson $state $statePath
                    ConvertTo-Json -InputObject ([pscustomobject]@{ participant = $Participant; batchId = $current.id; state = 'editing'; editRevision=(Get-EditRevision (@($current.participants | Where-Object id -eq $Participant)[0]));taskRevision=$task.revision;next='claim explicit paths before writing' })
                    exit 0
                    }
                }
            }
            elseif ($Command -in 'execute','publish-run') {
                $runRecord=Assert-BatchRunCaller $current -Leased:($Command -eq 'execute')
                if($Command -eq 'execute'){$selected=$current}
                else {
                    $releaseProbe=if((Get-Field $current 'protocolVersion' 1) -lt 4){Try-Lease 'build.lock'}else{$null}
                    if((Get-Field $current 'protocolVersion' 1) -ge 4 -or $releaseProbe){
                        if($releaseProbe){$releaseProbe.Dispose()}
                        $pendingPath=Join-Path $stateRoot ($Batch+'.pending-result.json')
                        $pending=Read-EntryJson $pendingPath
                        if($pending){
                            if($pending.batchId -ne $Batch -or $pending.repositoryRoot -ne $repositoryRoot -or $pending.runId -ne $ExecutionRun){throw 'Pending batch result identity mismatch.'}
                            $result=$pending
                        } else {
                            $cancelPath=Join-Path $stateRoot ('runs/'+$ExecutionRun+'.cancel')
                            $request=Read-EntryJson (Join-Path $stateRoot ('runs/'+$ExecutionRun+'.request.json'))
                            $cancelled=$runRecord.exitCode -eq 4 -and [IO.File]::Exists($cancelPath) -and [IO.File]::ReadAllText($cancelPath).Trim() -ceq $request.nonce
                            $resourceInvalidated=$runRecord.exitCode -eq 5 -and (Get-Field (Get-Field $runRecord 'resourceAdmission') 'bindingValid') -eq $false
                            $result=New-Result $current $(if($cancelled){'cancelled'}elseif($resourceInvalidated){'invalidated'}else{'failed'}) $(if($cancelled){4}elseif($resourceInvalidated){5}else{2}) $Reason
                        }
                        Set-Field $result 'publishedUtc' ([DateTime]::UtcNow.ToString('o'))
                        Publish-Result $state $result
                        Emit-Result $result
                    }
                }
            }
            elseif ($Command -eq 'commit') {
                $record=if($current -and $current.id -eq $Batch){$current}else{Read-Result $Batch}
                $entries=@(Get-Field $record 'participants' @()|Where-Object id -eq $Participant)
                if($entries.Count -ne 1 -or $entries[0].state -eq 'withdrawn'){throw 'No matching Git checkpoint registration.'}
                $entry=$entries[0];Assert-EditRevision $entry $Revision
                $frozen=-not ($state.current -and $state.current.id -eq $Batch -and $state.current.phase -eq 'editing' -and $entry.state -eq 'editing')
                if($frozen){
                    if($entry.state -ne 'finished'){throw 'Frozen checkpoint requires a finished edit registration.'}
                    Assert-CommitIdentity $entry
                    foreach($claim in @(Get-Field (Get-Field $state 'taskLedger') 'sourceOwnership' @()|Where-Object {$_.sequence -gt (Get-Field $entry 'ownershipSequence' 0)})){
                        foreach($path in @(Get-Field $entry 'ownedFiles' @())){
                            if($path -ieq $claim.path -or $path.StartsWith($claim.path+'/',[StringComparison]::OrdinalIgnoreCase) -or $claim.path.StartsWith($path+'/',[StringComparison]::OrdinalIgnoreCase)){throw 'Checkpoint scope was handed off after ready; commit through the current owner.'}
                        }
                    }
                    foreach($other in @(Get-Field $state.current 'participants' @())){
                        if($other.id -eq $Participant -and $state.current.id -eq $Batch){continue}
                        if($other.state -eq 'withdrawn'){continue}
                        foreach($mine in @(Get-Field $entry 'ownedFiles' @())){foreach($theirs in @(Get-Field $other 'ownedFiles' @())){
                            if($mine -ieq $theirs -or $mine.StartsWith($theirs+'/',[StringComparison]::OrdinalIgnoreCase) -or $theirs.StartsWith($mine+'/',[StringComparison]::OrdinalIgnoreCase)){throw 'A current editor owns this checkpoint scope; use its reviewed handoff.'}
                        }}
                    }
                }
                $gitLease=Try-Lease 'git.lock';if(-not $gitLease){throw 'A cooperating Git transaction is running. Retry; shared index was not changed.'}
                $commitSelected=[pscustomobject]@{batchId=$Batch;participant=$Participant;editRevision=$Revision;ownedFiles=@(Get-Field $entry 'ownedFiles' @());frozen=$frozen;commitIdentity=(Get-Field $entry 'commitIdentity')}
            }
            elseif ($Command -in 'plan','ready','check','claim') {
                if (-not $current -or $current.id -ne $Batch -or $current.phase -ne 'editing') { throw 'Batch is frozen/retired. Use begin and wait for the next permitted editing window; no current input was changed.' }
                $entries=@($current.participants | Where-Object id -eq $Participant)
                if ($entries.Count -ne 1 -or $entries[0].state -eq 'withdrawn') { throw 'Participant is not active.' }
                $entry=$entries[0]; Assert-EditRevision $entry $Revision
                if ($Command -in 'ready','check','plan' -and (Get-Field $current 'protocolVersion' 1) -lt 2) { throw 'Legacy active batch: keep finish contract; advanced ready/plan/check begin with the next batch. No live migration.' }
                if ($Command -eq 'claim') {
                    if($entry.state -ne 'editing'){throw 'Claim requires begin/reopen before changing files.'}
                    if($Command -eq 'claim'){
                        $claimLease=Try-Lease 'git.lock';if(-not $claimLease){throw 'Git checkpoint is active; retry claim. No ownership was changed.'}
                        try {
                            Set-Ownership $current $entry $Files $AdoptExistingChanges
                            Register-SourceOwnership $state $entry $Batch
                            Write-AtomicJson $state $statePath
                        } finally {$claimLease.Dispose()}
                        $entry | ConvertTo-Json -Depth 10; exit 0
                    }
                } else {

                if ($Command -eq 'plan') {
                    if ($entry.state -ne 'editing') { throw 'Plan changes require atomic begin/reopen before editing.' }
                    Set-Field $entry 'testPlan' (New-TaskPlan $entry $Scope $Suites $Tests $Inputs $Reason)
                    if($ResourceShared -and ($Scope -ne 'tool' -or $entry.testPlan.suites -contains 'none' -or $entry.testPlan.suites -contains 'full' -or -not $ResourceCpuSlots -or -not $ResourceMemoryMiB)){
                        throw 'Shared resource admission requires a selected tool plan with explicit CPU and memory budgets.'
                    }
                    if($ResourceCpuSlots -or $ResourceMemoryMiB){
                        if($entry.testPlan.suites -contains 'none'){throw 'An empty plan cannot reserve weighted resources.'}
                        Set-Field $entry.testPlan 'projectResources' (Get-ProjectResourceBinding $repositoryRoot $true $ResourceCpuSlots $ResourceMemoryMiB (-not $ResourceShared))
                    }
                    $visible=Find-VisibleTask (Get-TaskLedger $state) $Participant
                    Set-Field $entry.testPlan 'requirementsRevision' $visible.requirementsRevision
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
                        if(@(Get-Field $entry 'ownedFiles' @()).Count){Set-Field $entry 'commitIdentity' (Get-OwnedCommitIdentity $entry)}
                        $entry.state='finished'; $entry.finishedUtc=[DateTime]::UtcNow.ToString('o')
                        if((Get-Field $current 'protocolVersion' 1) -ge 4){Set-Field $current 'membershipSealed' $true}
                        Set-Field $entry 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=$Revision;reason='Post-ready lightweight check is queued.'})
                        Write-AtomicJson $state $statePath # crash leaves an explicit pending check, never permission to build
                    }
                    $check=Get-Field $entry 'check'
                    if ($check -and $check.status -in 'failed','invalidated','interrupted') { throw 'Check requires attention. Use begin/reopen for repairs, or check for a bounded read-only reassessment.' }
                    if($ReloadShell){Set-Field $current 'reloadShellRequested' $true}
                    if($CloseApplication){Set-Field $current 'closeApplicationRequested' $true}
                    $waiter=Start-ReadyWorker $current $entry
                    Write-AtomicJson $state $statePath
                    ConvertTo-Json -InputObject ([pscustomobject]@{participant=$Participant;batchId=$Batch;editRevision=$Revision;state='ready';checkStatus=(Get-Field (Get-Field $entry 'check') 'status');waiter=$waiter}) -Depth 8
                    if((Get-Field $waiter 'status') -eq 'unavailable'){[Console]::Error.WriteLine('Background launch unavailable; pending registration retained. Continue with '+$waiter.next);exit 2}
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
                    Retire-BoundBatch $state $current.id
                    Write-AtomicJson $state $statePath
                }
                elseif ($current.phase -eq 'building') {
                    if ($Participant) { throw 'A building batch is frozen; recover the batch without a participant.' }
                    if ((Owner-State $current.owner) -ne 'exited') { throw 'Build owner is alive or cannot be inspected; recovery refused.' }
                    if((Get-Field $current 'protocolVersion' 1) -lt 4){
                        $buildLease = Try-Lease 'build.lock'
                        if ($null -eq $buildLease) { throw 'Build lease is still held; recovery refused.' }
                    }
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
                    if (-not (Get-Field $entries[0] 'testPlan')) { [void](Default-TaskPlan $entries[0]) }
                    if($CloseApplication){Set-Field $current 'closeApplicationRequested' $true; Write-AtomicJson $state $statePath}
                    # Remember an explicit request for this batch, even when
                    # another participant eventually owns the build. Shell
                    # cleanup remains inside that owner's build process.
                    if ($ReloadShell -and (-not $current.PSObject.Properties['reloadShellRequested'] -or -not $current.reloadShellRequested)) {
                        $current | Add-Member -NotePropertyName reloadShellRequested -NotePropertyValue $true -Force
                        Write-AtomicJson $state $statePath
                    }
                    if ($entries[0].state -eq 'editing') {
                        if($AutoCheck){
                            $plan=Get-Field $entries[0] 'testPlan';if(-not $plan){$plan=Default-TaskPlan $entries[0]}
                            if($plan.editRevision -ne $currentRevision){throw 'Plan is stale; reopen/update before waiting.'}
                            $plan.inputIdentity=Get-TaskIdentity $plan;Set-Field $entries[0] 'testPlan' $plan
                            Set-Field $entries[0] 'check' ([pscustomobject]@{status='pending';source='builtin-basic';editRevision=$currentRevision;reason='Foreground lightweight check queued.'})
                        }
                        if(@(Get-Field $entries[0] 'ownedFiles' @()).Count){Set-Field $entries[0] 'commitIdentity' (Get-OwnedCommitIdentity $entries[0])}
                        $entries[0].state = 'finished'
                        if((Get-Field $current 'protocolVersion' 1) -ge 4){Set-Field $current 'membershipSealed' $true}
                        $entries[0].finishedUtc = [DateTime]::UtcNow.ToString('o')
                        Write-AtomicJson $state $statePath
                    }
                    $check=Get-Field $entries[0] 'check'
                    if($AutoCheck -and @($current.participants | Where-Object state -eq 'editing').Count -eq 0 -and $check -and $check.status -eq 'passed' -and (Get-Field $entries[0] 'testPlan').source -in 'legacy-default','repair-carried' -and (Get-TaskIdentity $entries[0].testPlan).digest -ne $check.inputEnd.digest){$check.status='pending';$check.reason='Global input changed while peers edited; repeat read-only checks.'}
                    if ($AutoCheck -and $check -and $check.status -eq 'pending' -and ((Get-Field (Get-Field $entries[0] 'testPlan') 'source') -ne 'repair-carried' -or @($current.participants | Where-Object state -eq 'editing').Count -eq 0)) {
                        $checkSelected=Start-CheckRecord $current $entries[0]
                        Write-AtomicJson $state $statePath
                    }
                    if($AutoCheck -and -not $checkSelected -and @($current.participants|Where-Object state -eq 'editing').Count -eq 0){
                        $pending=@($current.participants|Where-Object {$_.state -eq 'finished' -and (Get-Field (Get-Field $_ 'check') 'status') -eq 'pending'})|Select-Object -First 1
                        if($pending){$checkSelected=Start-CheckRecord $current $pending;Write-AtomicJson $state $statePath}
                    }
                    $badChecks=@($current.participants | Where-Object {$_.state -eq 'finished' -and (Get-Field (Get-Field $_ 'check') 'status') -in 'failed','invalidated','interrupted'})
                    if($AutoCheck -and $badChecks.Count -gt 0){
                        [pscustomobject]@{status='attention';batchId=$Batch;participant=$Participant;editRevision=$observedRevision;reason='A declared check needs explicit repair/reopen; no automatic repeat was started.';checks=@($badChecks | ForEach-Object {[pscustomobject]@{participant=$_.id;check=$_.check}})} | ConvertTo-Json -Depth 10
                        exit 2
                    }
                    if (@($current.participants | Where-Object state -eq 'editing').Count -eq 0 -and -not (Checks-BlockFreeze $current) -and
                        ((Get-Field $current 'protocolVersion' 1) -lt 4 -or @(Get-Field $state 'activeCandidates' @()).Count -lt 32)) {
                        $buildLease = Try-Lease 'build.lock'
                        if ($null -eq $buildLease) { throw 'Unexpected occupied build lease; inspect status without clearing registrations.' }
                        # The metadata lease covers membership freeze and the
                        # input identity, closing the new-begin/start-build race.
                        try {
                            $frozenPlan=New-FrozenPlan $current
                            foreach($request in $frozenPlan.tasks){
                                $required=Get-Field $request.requirement 'requirementsRevision'
                                if($null -ne $required -and $required -ne (Find-VisibleTask (Get-TaskLedger $state) $request.participant).requirementsRevision){throw 'Requirements changed after planning; reopen and update the declared plan before execution.'}
                            }
                        } catch {
                            Set-Field $current 'planStatus' 'blocked'; Set-Field $current 'planError' $_.Exception.Message
                            Write-AtomicJson $state $statePath; throw
                        }
                        $preflight=Get-Field $current 'preflight'
                        if((Get-Field $current 'protocolVersion' 1) -ge 4){
                            $preflight=[pscustomobject]@{status='private-output-pending';owners=@();observedUtc=[DateTime]::UtcNow.ToString('o')}
                        }elseif(-not $preflight -or (Get-TimestampAge $preflight.observedUtc) -gt 10){$preflight=Get-ReadOnlyPreflight $repositoryRoot}
                        Set-Field $current 'preflight' $preflight
                        if((Get-Field $current 'protocolVersion' 1) -lt 4 -and $frozenPlan.buildRequired -and $preflight.status -ne 'clear' -and -not (Get-Field $current 'reloadShellRequested' $false) -and -not (Get-Field $current 'closeApplicationRequested' $false)) {
                            Set-Field $current 'planStatus' 'waiting-output-owner';Write-AtomicJson $state $statePath
                            $buildLease.Dispose();$buildLease=$null
                        } else {
                        Write-AtomicJson $frozenPlan (Join-Path $stateRoot ($current.id + '.plan.json'))
                        Set-Field $current 'testingPlan' $frozenPlan
                        $startInputs = Get-BuildInputIdentity $repositoryRoot
                        $current | Add-Member -NotePropertyName inputStart -NotePropertyValue $startInputs -Force
                        $current.phase = 'building'
                        Set-Field $current 'planStatus' 'executing'
                        Set-Field $current 'planError' ''
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
            if($waitingForGit){Wait-Again 'Waiting for the cooperating Git transaction; registration and edit revision are retained.'}
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
            if((Get-Field $selected 'protocolVersion' 1) -ge 4 -and -not $ExecutionRun -and
                -not $selected.testingPlan.buildRequired -and $selected.testingPlan.mode -eq 'skipped'){
                # An explicit empty requirement needs no copied source tree,
                # compiler slot, worker, or heavy-resource reservation.
                $metadata=Lock-State
                try{
                    $state=Read-State;$ownedBatch=Find-BoundBatch $state $selected.id
                    if(-not $ownedBatch -or $ownedBatch.owner.pid -ne $PID){throw 'Empty batch reservation changed.'}
                    $result=New-Result $ownedBatch 'skipped' 0 'All frozen task requirements explicitly exempt host build/tests.'
                    $result.commands=@();$result.inputEnd=Get-BuildInputIdentity $repositoryRoot
                    $result.inputCheck=if($result.inputStart.digest -eq $result.inputEnd.digest){'stable'}else{'changed'}
                    if($result.inputCheck -ne 'stable'){$result.outcome='invalidated';$result.exitCode=5}
                    Set-Field $result 'coverage' ([pscustomobject]@{schemaVersion=1;batchId=$ownedBatch.id;mode='skipped';status='not-required';selected=@();completed=@();tasks=@($ownedBatch.testingPlan.tasks|ForEach-Object {[pscustomobject]@{participant=$_.participant;requested=@();status='not-required';reason=$_.requirement.reason}})})
                    Publish-Result $state $result
                }finally{$metadata.Dispose();if($buildLease){$buildLease.Dispose();$buildLease=$null}}
                Emit-Result $result
            }
            if((Get-Field $selected 'protocolVersion' 1) -ge 3 -and -not $ExecutionRun){
                try {
                    $submitted=& (Join-Path $PSScriptRoot 'build_run.ps1') -Command start -Task $Participant -Action batch -Batch $selected.id -EditRevision $observedRevision
                    if($LASTEXITCODE -ne 0){throw ('Persistent batch submission failed: '+($submitted -join "`n"))}
                } catch {
                    $metadata=Lock-State
                    try {
                        $submissionError=$_.Exception.Message;$state=Read-State;$ownedBatch=Find-BoundBatch $state $selected.id
                        $result=Read-Result $selected.id
                        if(-not $result){
                            if(-not $ownedBatch){throw 'Batch reservation changed during submission; no other editing window was retired.'}
                            $reserved=Get-Field $ownedBatch 'runId'
                            $runRecord=if($reserved){Read-EntryJson (Join-Path $stateRoot ('runs/'+$reserved+'.json'))}else{$null}
                            if($runRecord -and $runRecord.status -ne 'start-failed'){throw ('Run '+$reserved+' is already reserved; inspect its owner and saved result. Submission observation failed, so no replacement outcome was published.')}
                            $result=New-Result $ownedBatch 'failed' 2 $submissionError;Publish-Result $state $result
                        }
                    }
                    finally {$metadata.Dispose()}
                    $buildLease.Dispose();$buildLease=$null
                    Emit-Result $result
                } finally {if($buildLease){$buildLease.Dispose();$buildLease=$null}}
                continue
            }
            try {
                $code = 1; $errorText = ''; $outcome = 'failed'
                $oldNodeReuse = [Environment]::GetEnvironmentVariable('MSBUILDDISABLENODEREUSE', 'Process')
                $oldEntryToken=Start-EntryCredential $repositoryRoot $selected.id
                try {
                    # Reused MSBuild workers from a prior, unrelated build are
                    # outside our job. Use fresh workers in this batch only.
                    [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE', '1', 'Process')
                    Add-Type -Path (Join-Path $PSScriptRoot 'build_job.cs')
                    if($ExecutionRun){Set-EntryRunControl $repositoryRoot;. (Join-Path $PSScriptRoot 'build_telemetry.ps1')}
                    [Console]::Error.WriteLine("Building batch $($selected.id). Log: $($selected.logPath)")
                    # Destructive preflight belongs to the outer execution owner;
                    # all configure/build/test children retain strict Job containment.
                    $code=0
                    if($selected.testingPlan.buildRequired){
                        $preflightStage=$null
                        if($ExecutionRun){$preflightStage=Start-BuildStage $repositoryRoot 'preflight' $selected.logPath}
                        try {$code=Invoke-EntryPreflight $repositoryRoot ([bool](Get-Field $selected 'reloadShellRequested' $false)) ([bool](Get-Field $selected 'closeApplicationRequested' $false)) 'Release' $selected.logPath}
                        finally {if($ExecutionRun){Complete-BuildStage $repositoryRoot $preflightStage $code}}
                        if($code -ne 0){$code=3}
                    }
                    $pipelineStage=$null
                    if($ExecutionRun){$pipelineStage=Start-BuildStage $repositoryRoot 'batch-pipeline' $selected.logPath}
                    $previousParent=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT','Process')
                    if($pipelineStage){[Environment]::SetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT',$pipelineStage.id,'Process')}
                    try {if($code -eq 0){$code = [SnowDesktop.Build.Job]::RunBatch($repositoryRoot, $selected.logPath, $selected.id, $selected.testingPlan.buildRequired, $false, $false)}}
                    finally {[Environment]::SetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT',$previousParent,'Process');if($ExecutionRun){Complete-BuildStage $repositoryRoot $pipelineStage $code}}
                    if ($code -eq 0) { $outcome = if($selected.testingPlan.mode -eq 'skipped'){'skipped'}else{'passed'} }
                    elseif($ExecutionRun -and [SnowDesktop.Build.Job]::RunWasCancelled){$outcome='cancelled';$code=4}
                    else { $errorText = "Build/test pipeline exited with code $code. See the batch log." }
                }
                catch { $errorText = $_.Exception.ToString() }
                finally { [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE', $oldNodeReuse, 'Process');[Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$oldEntryToken,'Process') }
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
                    . (Join-Path $PSScriptRoot 'build_evidence.ps1')
                    Set-Field $result 'evidence' (New-BatchTestEvidence $repositoryRoot $result)
                    if($ExecutionRun){Write-AtomicJson $result (Join-Path $stateRoot ($selected.id+'.pending-result.json'))}
                    else {Publish-Result $state $result}
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
            finally { if($buildLease){$buildLease.Dispose()} }
            Emit-Result $result
        }
        if($Command -eq 'event-wait'){Start-Sleep -Milliseconds 200}
        elseif($cohortSealed){Wait-Again 'The ready cohort is sealed; waiting for source capture to release the next editing window.' $current}
        else {Wait-Again 'Waiting for batch retirement and its common result.' $current}
    }
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 2
}
