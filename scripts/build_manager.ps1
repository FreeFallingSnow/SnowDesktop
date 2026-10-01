[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $true)]
    [ValidateSet('begin', 'finish', 'status', 'recover')][string]$Command,
    [Parameter(Position = 1)][ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$Participant,
    [ValidatePattern('^[a-f0-9]{32}$')][string]$Batch,
    [ValidateRange(1, 86400)][int]$WaitSeconds = 86400,
    [switch]$ConfirmStopped,
    [string]$Reason
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stateRoot = Join-Path $repositoryRoot '.build\collaboration'
$statePath = Join-Path $stateRoot 'state.json'
$clock = [Diagnostics.Stopwatch]::StartNew()
$lastNotice = -30
$hasFinishedCurrentBatch = $false
$utf8 = New-Object Text.UTF8Encoding($false)
. (Join-Path $PSScriptRoot 'build_inputs.ps1')

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
        schemaVersion = 1; batchId = $Current.id; repositoryRoot = $repositoryRoot
        outcome = $Outcome; exitCode = $Code; error = $ErrorText
        createdUtc = $Current.createdUtc; buildStartedUtc = $Current.buildStartedUtc
        completedUtc = [DateTime]::UtcNow.ToString('o'); participants = @($Current.participants)
        logPath = $Current.logPath; commands = @('scripts\build.bat', 'scripts\test.bat')
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
function Emit-Result($Result) {
    ConvertTo-Json -InputObject $Result -Depth 20
    exit $Result.exitCode
}

try {
    if ($Command -in 'begin', 'finish' -and -not $Participant) { throw "$Command requires a participant ID." }
    if ($Command -in 'finish', 'recover' -and -not $Batch) { throw "$Command requires -Batch from begin; this prevents mixing batches." }
    if ($Command -eq 'begin' -and $Batch) { throw 'begin allocates its batch ID; do not supply -Batch.' }
    if ($Command -eq 'recover' -and (-not $ConfirmStopped -or [string]::IsNullOrWhiteSpace($Reason))) {
        throw 'Recovery requires -ConfirmStopped and -Reason after inspecting status and confirming the editor/build is stopped.'
    }
    [void][IO.Directory]::CreateDirectory($stateRoot)
    while ($true) {
        $metadata = Lock-State
        $buildLease = $null
        $selected = $null
        try {
            $state = Read-State
            $current = $state.current
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
            if ($Command -eq 'finish' -and $null -ne $result) {
                if ($null -ne $current -and $current.id -ne $Batch -and
                    @($current.participants | Where-Object id -eq $Participant).Count -gt 0) {
                    throw 'This participant has a newer active batch. Its old result cannot verify new edits; use the batch ID from the latest begin.'
                }
                if (@($result.participants | Where-Object id -eq $Participant).Count -ne 1) { throw 'Participant does not belong to this result.' }
                if (-not $hasFinishedCurrentBatch -and $result.outcome -eq 'passed') {
                    if (-not $result.PSObject.Properties['inputEnd'] -or $null -eq $result.inputEnd) {
                        throw 'This historical result has no input identity. Read it with status; begin a new batch to verify edits.'
                    }
                    $nowInputs = Get-BuildInputIdentity $repositoryRoot
                    if ($nowInputs.digest -ne $result.inputEnd.digest) {
                        throw 'Repository inputs differ from this historical result. Use status for history; begin a new batch for current edits.'
                    }
                }
                Emit-Result $result
            }
            if ($Command -eq 'begin') {
                if ($null -ne $current -and $null -ne (Read-Result $current.id)) {
                    throw 'The current result is durable but retirement was interrupted. Use status and recover before a new begin.'
                }
                if ($null -eq $current) {
                    $current = [pscustomobject]@{
                        id = [Guid]::NewGuid().ToString('N'); phase = 'editing'; participants = @()
                        createdUtc = [DateTime]::UtcNow.ToString('o'); buildStartedUtc = $null; owner = $null; logPath = $null; inputStart = $null
                    }
                    $state.current = $current
                }
                if ($current.phase -eq 'editing') {
                    $entry = @($current.participants | Where-Object id -eq $Participant)
                    if ($entry.Count -eq 0) {
                        $current.participants = @($current.participants) + @([pscustomobject]@{
                            id = $Participant; state = 'editing'; registeredUtc = [DateTime]::UtcNow.ToString('o')
                            finishedUtc = $null; withdrawalReason = $null
                        })
                        Write-AtomicJson $state $statePath
                    }
                    elseif ($entry[0].state -ne 'editing') { throw 'This participant already finished/withdrew. Read its result before editing again.' }
                    ConvertTo-Json -InputObject ([pscustomobject]@{ participant = $Participant; batchId = $current.id; state = 'editing' })
                    exit 0
                }
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
                if ($null -eq $current -or $current.id -ne $Batch) { throw 'Unknown current batch; registration was not changed.' }
                $entries = @($current.participants | Where-Object id -eq $Participant)
                if ($entries.Count -ne 1 -or $entries[0].state -eq 'withdrawn') { throw 'Participant is not registered or was withdrawn.' }
                $hasFinishedCurrentBatch = $true
                if ($current.phase -eq 'editing') {
                    if ($entries[0].state -eq 'editing') {
                        $entries[0].state = 'finished'
                        $entries[0].finishedUtc = [DateTime]::UtcNow.ToString('o')
                        Write-AtomicJson $state $statePath
                    }
                    if (@($current.participants | Where-Object state -eq 'editing').Count -eq 0) {
                        $buildLease = Try-Lease 'build.lock'
                        if ($null -eq $buildLease) { throw 'Unexpected occupied build lease; inspect status without clearing registrations.' }
                        # The metadata lease covers membership freeze and the
                        # input identity, closing the new-begin/start-build race.
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
        finally {
            $metadata.Dispose()
            if ($null -eq $selected -and $null -ne $buildLease) { $buildLease.Dispose() }
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
                    $code = [SnowDesktop.Build.Job]::Run($repositoryRoot, $selected.logPath)
                    if ($code -eq 0) { $outcome = 'passed' }
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
