Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The production coordinator runs in separate Windows PowerShell processes.
# Only the expensive build.bat/test.bat boundary is replaced, with file gates
# controlling overlap. Counts and durable results are independent expectations.
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryRoot ('SnowDesktop-collaboration-' + [Guid]::NewGuid().ToString('N'))
$scripts = Join-Path $fixture 'scripts'
[void][IO.Directory]::CreateDirectory($scripts)
$utf8 = New-Object Text.UTF8Encoding($false)
$processes = New-Object 'System.Collections.Generic.List[System.Diagnostics.Process]'
$manager = Join-Path $scripts 'build_manager.ps1'
$powershell = Join-Path $PSHOME 'powershell.exe'
$runs = New-Object 'System.Collections.Generic.List[object]'
$fixtureTimer = [Diagnostics.Stopwatch]::StartNew()
$script:stage = 'setup'
$succeeded = $false
$events = Join-Path $fixture 'commands.jsonl'

function Record($Event) {
    $Event.utc = [DateTime]::UtcNow.ToString('o')
    $Event.elapsedSeconds = $fixtureTimer.Elapsed.TotalSeconds
    [IO.File]::AppendAllText($events, (($Event | ConvertTo-Json -Depth 8 -Compress) + "`r`n"), $utf8)
}
function Stage([string]$Name) {
    $script:stage = $Name
    Record @{ event = 'stage'; stage = $Name }
    Write-Output ("STAGE {0} elapsed={1:N2}s" -f $Name, $fixtureTimer.Elapsed.TotalSeconds)
}
function Read-SharedText([string]$Path) {
    if (-not [IO.File]::Exists($Path)) { return '' }
    $stream = $null
    $reader = $null
    try {
        $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $reader = New-Object IO.StreamReader($stream)
        return $reader.ReadToEnd()
    }
    catch { return ('Evidence read failed: ' + $_.Exception.Message) }
    finally {
        if ($null -ne $reader) { $reader.Dispose() }
        elseif ($null -ne $stream) { $stream.Dispose() }
    }
}
function Snapshot([string]$Reason) {
    # Read-only evidence from this exclusively owned disposable fixture. Never
    # acquire the production coordinator lease or inspect unrelated processes.
    $summary = @($runs | ForEach-Object {
        $run = $_
        $exited = $run.process.HasExited
        @{ pid = $run.process.Id; arguments = $run.arguments; stage = $run.stage;
           startedUtc = $run.startedUtc; exited = $exited;
           exitCode = $(if ($exited) { $run.process.ExitCode } else { $null });
           stdout = $run.output; stderr = $run.errorFile;
           stdoutText = Read-SharedText $run.output; stderrText = Read-SharedText $run.errorFile }
        if (-not $exited) {
            Write-Host ("WAITING pid={0} args={1} stage={2} stdout={3} stderr={4}" -f
                $run.process.Id, $run.arguments, $run.stage,
                (Read-SharedText $run.output), (Read-SharedText $run.errorFile))
        }
    })
    [IO.File]::WriteAllText((Join-Path $fixture 'failure.json'),
        (@{ reason = $Reason; stage = $script:stage; utc = [DateTime]::UtcNow.ToString('o');
            elapsedSeconds = $fixtureTimer.Elapsed.TotalSeconds; commands = $summary;
            gates = @(Get-ChildItem -LiteralPath $fixture -File | Where-Object Name -Match '^(hold-|fail-|.*-started|child.pid)' | Select-Object Name, Length, LastWriteTimeUtc)
        } | ConvertTo-Json -Depth 10), $utf8)
    Write-Output ("EVIDENCE retained at {0}; stage={1}; reason={2}" -f $fixture, $script:stage, $Reason)
}

function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Wait-Until([scriptblock]$Condition, [string]$Message, $ObservedRun = $null) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not (& $Condition)) {
        if ($null -ne $ObservedRun -and $ObservedRun.process.HasExited) {
            Complete $ObservedRun | Out-Null
            throw "Command exited before expected signal: $Message; PID=$($ObservedRun.process.Id); args=$($ObservedRun.arguments)"
        }
        if ($timer.Elapsed.TotalSeconds -gt 20 -or $fixtureTimer.Elapsed.TotalSeconds -gt 160) {
            Snapshot ("Timed out: $Message")
            throw "Timed out: $Message; stage=$script:stage; evidence=$fixture"
        }
        Start-Sleep -Milliseconds 50
    }
}
function Start-Command([string]$Arguments) {
    if($Arguments -match '^finish ([a-zA-Z0-9._-]+) -Batch ([a-f0-9]{32})' -and $Arguments -notmatch '-Revision') {
        $taskId=$Matches[1];$batchId=$Matches[2]
        $st=State;$part=$null
        if($st.current -and $st.current.id -eq $batchId){$part=@($st.current.participants|Where-Object id -eq $taskId)|Select-Object -First 1}
        else{$rp=Join-Path $fixture ('.build\collaboration\'+$batchId+'.json');if(Test-Path -LiteralPath $rp){$part=@(([IO.File]::ReadAllText($rp)|ConvertFrom-Json).participants|Where-Object id -eq $taskId)|Select-Object -First 1}}
        if($part){$rev=if($part.PSObject.Properties['editRevision']){$part.editRevision}else{0};$Arguments+=' -Revision '+$rev}
    }
    $token = [Guid]::NewGuid().ToString('N')
    $output = Join-Path $fixture ($token + '.out')
    $errorFile = Join-Path $fixture ($token + '.err')
    $process = Start-Process -FilePath $powershell -WindowStyle Hidden -PassThru -ArgumentList (
        '-NoProfile -ExecutionPolicy Bypass -File "' + $manager + '" ' + $Arguments
    ) -RedirectStandardOutput $output -RedirectStandardError $errorFile
    # Keep the native handle before the short-lived process exits; PS 5.1
    # otherwise loses ExitCode when Start-Process only retains its PID.
    $null = $process.Handle
    $processes.Add($process)
    $run = [pscustomobject]@{ process = $process; output = $output; errorFile = $errorFile;
        arguments = $Arguments; stage = $script:stage; startedUtc = [DateTime]::UtcNow.ToString('o') }
    $runs.Add($run)
    Record @{ event = 'start'; pid = $process.Id; arguments = $Arguments; stage = $script:stage;
        stdout = $output; stderr = $errorFile }
    return $run
}
function Complete($Run, [int]$Code = 0) {
    Wait-Until { $Run.process.HasExited } ('command PID ' + $Run.process.Id)
    $Run.process.WaitForExit()
    $actual = $Run.process.ExitCode
    $errorText = [IO.File]::ReadAllText($Run.errorFile)
    $output = [IO.File]::ReadAllText($Run.output)
    if ($actual -ne $Code -and $output.Trim().StartsWith('{')) {
        $reported = $output | ConvertFrom-Json
        if ($reported.PSObject.Properties['logPath'] -and $reported.logPath -and [IO.File]::Exists($reported.logPath)) {
            $errorText += [IO.File]::ReadAllText($reported.logPath)
        }
    }
    Record @{ event = 'complete'; pid = $Run.process.Id; arguments = $Run.arguments;
        stage = $Run.stage; actualExitCode = $actual; expectedExitCode = $Code }
    Check ($actual -eq $Code) "Expected exit $Code, got $actual. PID=$($Run.process.Id); args=$($Run.arguments); stage=$($Run.stage). $errorText $output"
    if ($output.Trim()) { return $output | ConvertFrom-Json }
    return $null
}
function Call([string]$Arguments, [int]$Code = 0) { return Complete (Start-Command $Arguments) $Code }
function Gate([string]$Name) { [IO.File]::WriteAllText((Join-Path $fixture $Name), 'gate') }
function Remove-Gate([string]$Name) { [IO.File]::Delete((Join-Path $fixture $Name)) }
function State {
    # Observers take the same metadata lease; unsynchronized polling readers
    # can themselves prevent Windows File.Replace from committing state.
    $lease = $null
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($null -eq $lease) {
        try { $lease = [IO.File]::Open((Join-Path $fixture '.build\collaboration\state.lock'), 'OpenOrCreate', 'ReadWrite', 'None') }
        catch [IO.IOException] {
            if (($_.Exception.HResult -band 0xffff) -notin 32, 33 -or $timer.Elapsed.TotalSeconds -gt 10) { throw }
            Start-Sleep -Milliseconds 20
        }
    }
    try { return ([IO.File]::ReadAllText((Join-Path $fixture '.build\collaboration\state.json')) | ConvertFrom-Json) }
    finally { $lease.Dispose() }
}
function Counts {
    $path = Join-Path $fixture 'calls.txt'
    if (-not [IO.File]::Exists($path)) { return ,@() }
    return ,@([IO.File]::ReadAllLines($path))
}
function Release-Pipeline { Remove-Gate 'hold-build'; Remove-Gate 'hold-test' }
function Compare-Results($First, $Second) {
    Check (($First | ConvertTo-Json -Depth 20 -Compress) -eq ($Second | ConvertTo-Json -Depth 20 -Compress)) 'Participants must receive the identical persisted batch result'
}

try {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\scripts\build_manager.ps1'),
        (Join-Path $PSScriptRoot '..\scripts\build_inputs.ps1'),
        (Join-Path $PSScriptRoot '..\scripts\build_job.cs'),
        (Join-Path $PSScriptRoot '..\scripts\build_protocol.ps1'),
        (Join-Path $PSScriptRoot '..\scripts\build_ownership.ps1'),
        (Join-Path $PSScriptRoot '..\scripts\build_preflight.ps1'),
        (Join-Path $PSScriptRoot '..\scripts\build_waiter.ps1') -Destination $scripts
    [IO.File]::WriteAllText((Join-Path $scripts 'build_batch_tests.ps1'), 'param([string]$Batch)' + "`r`n" + '& (Join-Path $PSScriptRoot fake.ps1) -Phase test; exit $LASTEXITCODE', $utf8)
    $fake = @'
param([string]$Phase, [string]$BuildArgument = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Directory]::GetParent($PSScriptRoot).FullName
[IO.File]::AppendAllText((Join-Path $root 'calls.txt'), $Phase + "`r`n")
if ($Phase -eq 'build') {
    [IO.File]::WriteAllText((Join-Path $root 'child.pid'), $PID.ToString())
    [IO.File]::WriteAllText((Join-Path $root 'build-arguments.txt'), $BuildArgument)
}
[IO.File]::WriteAllText((Join-Path $root ($Phase + '-started')), 'started')
$timer = [Diagnostics.Stopwatch]::StartNew()
while (Test-Path -LiteralPath (Join-Path $root ('hold-' + $Phase))) {
    if ($timer.Elapsed.TotalSeconds -gt 30) { throw 'Fixture gate was not released' }
    Start-Sleep -Milliseconds 50
}
$failure = Join-Path $root ('fail-' + $Phase)
if (Test-Path -LiteralPath $failure) {
    Write-Output "controlled $Phase failure"
    exit ([int][IO.File]::ReadAllText($failure))
}
Write-Output "controlled $Phase passed"
exit 0
'@
    [IO.File]::WriteAllText((Join-Path $scripts 'fake.ps1'), $fake, $utf8)
    foreach ($phase in @('build', 'test')) {
        $batchScript = '@echo off' + "`r`n" +
            'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0fake.ps1" -Phase ' + $phase + ' -BuildArgument "%~1"' + "`r`n" +
            'exit /b %ERRORLEVEL%' + "`r`n"
        [IO.File]::WriteAllText((Join-Path $scripts ($phase + '.bat')), $batchScript, $utf8)
    }
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'src'))
    [IO.File]::WriteAllText((Join-Path $fixture 'src\fixture.cpp'), 'original source', $utf8)
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'src\winui'))
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'src\core'))
    [IO.File]::WriteAllText((Join-Path $fixture 'src\winui\presenter.cpp'), 'local presenter fixture', $utf8)
    [IO.File]::WriteAllText((Join-Path $fixture 'src\core\desktop.cpp'), 'local renderer fixture', $utf8)
    [IO.File]::WriteAllText((Join-Path $fixture '.gitignore'), "/*`n!/scripts/`n!/src/`n!/.gitignore`n!/CMakePresets.json`n", $utf8)
    & git.exe -C $fixture init --quiet
    Check ($LASTEXITCODE -eq 0) 'The isolated input fixture must initialize Git'
    & git.exe -C $fixture add -- scripts src .gitignore
    Check ($LASTEXITCODE -eq 0) 'The fixture must cover tracked files with uncommitted content changes'
    $readOnly = Call 'status'
    Check ($null -eq $readOnly.current -and -not [IO.File]::Exists((Join-Path $fixture '.build\collaboration\state.json'))) 'A read-only status query must not register or start a build'

    Stage 'overlap'
    # A returns first and waits for B; duplicate finish and begin are harmless.
    $a = Call 'begin task-a'
    $b = Call 'begin task-b'
    Check ($a.batchId -eq $b.batchId) 'Overlapping editors must join one batch'
    $repeatBegin = Call 'begin task-a'
    Check ($a.batchId -eq $repeatBegin.batchId) 'Repeated begin must reuse the editing registration'
    # A reviewed local change and a failed-test supplement retain their exact
    # selection even under native/shared directories. High-risk declarations
    # still escalate; rejected empty selections cannot replace a valid plan.
    $planArgs = 'plan task-a -Batch ' + $a.batchId + ' -Revision 0'
    $mappedInputs = ' -Inputs src/winui/presenter.cpp,src/core/desktop.cpp,scripts/build_protocol.ps1'
    $scoped = Call ($planArgs + ' -Scope module -Suites selected -Tests Alpha' + $mappedInputs + ' -Reason reviewed-local-dependencies-and-failed-test-supplement')
    Check (-not $scoped.testPlan.requiredFull -and $scoped.testPlan.tests.Count -eq 1 -and
        $scoped.testPlan.tests[0] -eq 'Alpha' -and -not (State).current.participants[0].testPlan.requiredFull) 'Reviewed local plans must persist only the requested coverage without directory-based full escalation'
    Call ($planArgs + ' -Scope module -Suites selected' + $mappedInputs + ' -Reason empty-selection') 2 | Out-Null
    Check ((State).current.participants[0].testPlan.tests[0] -eq 'Alpha') 'An empty selection must be rejected without replacing the valid plan'
    foreach ($risk in @('public', 'infrastructure', 'unknown')) {
        $highRisk = Call ($planArgs + ' -Scope ' + $risk + ' -Suites selected -Tests Alpha' + $mappedInputs + ' -Reason declared-high-risk')
        Check $highRisk.testPlan.requiredFull 'Unknown/public/infrastructure plans must retain full automatic coverage'
    }
    # Preserve the original default-full fixture for the following barrier,
    # crash recovery and input invalidation scenarios.
    Call ($planArgs + ' -Scope unknown -Suites full') | Out-Null
    Write-Output 'PASS scoped native/shared-directory selection, nonempty rejection, and high-risk escalation'
    Call ('finish task-a -Batch ' + ('0' * 32)) 2 | Out-Null
    Gate 'hold-build'
    Gate 'hold-test'
    $first = Start-Command ('finish task-a -Batch ' + $a.batchId)
    Wait-Until { (State).current.participants[0].state -eq 'finished' } 'A marks finished'
    $waiting = Call 'status'
    Check ($waiting.current.batch.phase -eq 'editing' -and -not $first.process.HasExited -and
        (Counts).Count -eq 0) 'A must wait without starting a build while B edits'
    Call ('finish task-a -Batch ' + $a.batchId + ' -WaitSeconds 1') 2 | Out-Null
    Check ((State).current.participants.Count -eq 2) 'Wait timeout must retain both registrations'
    $duplicate = Start-Command ('finish task-a -Batch ' + $a.batchId)
    Wait-Until { [IO.File]::Exists($duplicate.errorFile) -and (Read-SharedText $duplicate.errorFile) -match 'Waiting for this batch|Waiting for the collaboration state transaction' } 'duplicate finish waits'
    $originalRegistration = (State).current.participants[0].registeredUtc
    $reopened = Call 'begin task-a'
    $reopenedState = State
    Check ($reopened.batchId -eq $a.batchId -and $reopened.state -eq 'editing' -and
        $reopenedState.current.participants.Count -eq 2 -and
        $reopenedState.current.participants[0].state -eq 'editing' -and
        $null -eq $reopenedState.current.participants[0].finishedUtc -and
        $reopenedState.current.participants[0].registeredUtc -eq $originalRegistration) 'Reopening must preserve the batch and membership while clearing completion'
    Wait-Until { ($first.process.HasExited -and $duplicate.process.HasExited) -or
        (State).current.participants[0].state -ne 'editing' } 'superseded finish waiters retire'
    Check ((State).current.participants[0].state -eq 'editing') 'Reopened task must remain editing while old finish waiters retire'
    Complete $first 2 | Out-Null
    Complete $duplicate 2 | Out-Null
    Check ((State).current.participants[0].state -eq 'editing' -and (Counts).Count -eq 0) 'Superseded finish waiters must not finish the reopened editor or start a build'
    $repeatedReopen = Call 'begin task-a'
    Check ($repeatedReopen.batchId -eq $a.batchId -and (State).current.participants[0].editRevision -eq 1) 'Repeated begin must keep the reopened edit revision stable'
    # The explicit full plan used above belongs to the old edit revision.
    # Re-declare it before completing the reopened fixture participant.
    Call ('plan task-a -Batch ' + $a.batchId + ' -Revision ' + $reopened.editRevision + ' -Scope unknown -Suites full') | Out-Null
    $first = Start-Command ('finish task-a -Batch ' + $a.batchId + ' -ReloadShell')
    Wait-Until { (State).current.participants[0].state -eq 'finished' } 'reopened A finishes'
    Check ((Counts).Count -eq 0 -and -not [IO.File]::Exists((Join-Path $fixture 'build-arguments.txt'))) 'Requesting Shell reload must not start cleanup while another participant edits'
    $duplicate = Start-Command ('finish task-a -Batch ' + $a.batchId)
    $last = Start-Command ('finish task-b -Batch ' + $b.batchId)
    Wait-Until {
        if ($last.process.HasExited) { Complete $last | Out-Null }
        Test-Path -LiteralPath (Join-Path $fixture 'build-started')
    } 'single build starts' $last
    Check ((Counts).Count -eq 1) 'Concurrent finish callers must trigger exactly one build'
    Check ([IO.File]::ReadAllText((Join-Path $fixture 'build-arguments.txt')) -eq '--reload-shell') 'The actual owner must receive the waiting participant explicit reload request'
    $pending = Start-Command 'begin task-c'
    Call 'begin task-d -WaitSeconds 1' 2 | Out-Null
    Check (-not $pending.process.HasExited -and (State).current.participants.Count -eq 2) 'New begin must wait without entering the frozen batch'
    Remove-Gate 'hold-build'
    Wait-Until { Test-Path -LiteralPath (Join-Path $fixture 'test-started') } 'single test starts' $last
    Check (-not $pending.process.HasExited -and (Counts).Count -eq 2) 'begin must also wait throughout testing'
    Remove-Gate 'hold-test'
    $resultA = Complete $first
    $resultB = Complete $last
    Compare-Results $resultA $resultB
    Compare-Results $resultA (Complete $duplicate)
    Compare-Results $resultA (Call ('finish task-a -Batch ' + $a.batchId))
    Check ($resultA.outcome -eq 'passed' -and $resultA.participants.Count -eq 2) 'Successful result must retain the frozen participant list'
    Check ($resultA.inputCheck -eq 'stable' -and $resultA.inputStart.digest -eq $resultA.inputEnd.digest) 'Result must bind a stable content identity to its batch'
    $c = Complete $pending
    Check ($c.batchId -ne $a.batchId) 'Queued begin must start a new batch'
    Compare-Results $resultA (Call ('status -Batch ' + $a.batchId))
    Check ((State).current.participants.Count -eq 1 -and (State).current.participants[0].id -eq 'task-c') 'Publishing the old result must preserve the new registration'
    Call ('finish task-c -Batch ' + $c.batchId) | Out-Null
    Check ([IO.File]::ReadAllText((Join-Path $fixture 'build-arguments.txt')) -eq '') 'A new batch must not inherit Shell reload from the previous batch'
    Check ((Counts).Count -eq 4) 'The next batch must independently build and test once'
    Write-Output 'PASS overlapping editors, reopen before build, superseded finish isolation, duplicate calls, fixed batch, common result, begin/build race, next-batch isolation'

    Stage 'race-and-failure'
    # Simultaneous last finish calls contend for the same atomic transition.
    $raceA = Call 'begin race-a'
    Call 'begin race-b' | Out-Null
    $raceFirst = Start-Command ('finish race-a -Batch ' + $raceA.batchId)
    $raceLast = Start-Command ('finish race-b -Batch ' + $raceA.batchId)
    Compare-Results (Complete $raceFirst) (Complete $raceLast)
    Check ((Counts).Count -eq 6) 'Simultaneous final finish calls must build/test once'

    # Build and test failures preserve identical errors and release both leases.
    foreach ($phase in @('build', 'test')) {
        $before = (Counts).Count
        [IO.File]::WriteAllText((Join-Path $fixture ('fail-' + $phase)), '7')
        $failed = Call ('begin failure-' + $phase)
        $failure = Call ('finish failure-' + $phase + ' -Batch ' + $failed.batchId) 7
        Check ($failure.outcome -eq 'failed' -and $failure.error -match 'code 7') 'Failure must be persisted with its real exit code'
        Check ([IO.File]::ReadAllText($failure.logPath) -match ('controlled ' + $phase + ' failure')) 'Failure log must persist the underlying error'
        Compare-Results $failure (Call ('finish failure-' + $phase + ' -Batch ' + $failed.batchId) 7)
        $expected = if ($phase -eq 'build') { 1 } else { 2 }
        Check ((Counts).Count - $before -eq $expected) 'Build failure must skip tests; test failure must not rerun the build'
        Remove-Gate ('fail-' + $phase)
        $after = Call ('begin after-' + $phase)
        Call ('finish after-' + $phase + ' -Batch ' + $after.batchId) | Out-Null
    }
    Write-Output 'PASS simultaneous finish, build/test failures, persistent error, failure lease release'

    Stage 'stale-recovery'
    # Age cannot imply completion. Recovery withdraws only the selected editor.
    $stale = Call 'begin stale-editor'
    Call 'begin live-editor' | Out-Null
    $statePath = Join-Path $fixture '.build\collaboration\state.json'
    $aged = State
    $aged.current.participants[0].registeredUtc = '2000-01-01T00:00:00Z'
    [IO.File]::WriteAllText($statePath, ($aged | ConvertTo-Json -Depth 20), $utf8)
    $before = (Counts).Count
    $liveFinish = Start-Command ('finish live-editor -Batch ' + $stale.batchId)
    Wait-Until { (State).current.participants[1].state -eq 'finished' } 'live editor finishes'
    $diagnostic = Call 'status'
    Check ($diagnostic.current.registrationAgesSeconds[0].ageSeconds -gt 86400 -and (Counts).Count -eq $before) 'A stale editor must be diagnosed and must keep the barrier closed'
    Call ('recover stale-editor -Batch ' + $stale.batchId + ' -Reason inspected') 2 | Out-Null
    Call ('recover live-editor -Batch ' + $stale.batchId + ' -ConfirmStopped -Reason inspected') 2 | Out-Null
    Call ('recover stale-editor -Batch ' + $stale.batchId + ' -ConfirmStopped -Reason inspected') | Out-Null
    $staleResult = Complete $liveFinish
    Check ($staleResult.participants[0].state -eq 'withdrawn' -and $staleResult.participants[1].state -eq 'finished') 'Recovery must preserve the completed registration and record withdrawal separately'
    $cancelled = Call 'begin abandoned-editor'
    Call ('recover abandoned-editor -Batch ' + $cancelled.batchId + ' -ConfirmStopped -Reason inspected') | Out-Null
    Check ((Call ('status -Batch ' + $cancelled.batchId)).outcome -eq 'cancelled') 'All-withdrawn batch must persist cancellation rather than success'
    Write-Output 'PASS stale registrations, explicit targeted recovery, no automatic completion'

    Stage 'crash-recovery'
    # Abrupt coordinator death kills its own build tree but leaves an orphan
    # batch blocked. Live-owner recovery and newer-batch recovery are refused.
    Remove-Gate 'build-started'
    Gate 'hold-build'
    $crashed = Call 'begin crash-editor'
    $crashRun = Start-Command ('finish crash-editor -Batch ' + $crashed.batchId)
    Wait-Until {
        if ($crashRun.process.HasExited) { Complete $crashRun | Out-Null }
        Test-Path -LiteralPath (Join-Path $fixture 'build-started')
    } 'crash fixture starts' $crashRun
    $childId = [int][IO.File]::ReadAllText((Join-Path $fixture 'child.pid'))
    $before = (Counts).Count
    Call ('recover -Batch ' + $crashed.batchId + ' -ConfirmStopped -Reason inspected') 2 | Out-Null
    Stop-Process -Id $crashRun.process.Id -Force
    Wait-Until { $null -eq (Get-Process -Id $childId -ErrorAction SilentlyContinue) } 'private build job stops after owner crash'
    $diagnostic = Call 'status'
    Check ($diagnostic.current.ownerState -eq 'exited') 'Orphan status must identify the exited owner'
    Call 'begin blocked-editor -WaitSeconds 1' 2 | Out-Null
    Check ((Counts).Count -eq $before) 'A crashed batch must not automatically rebuild or admit a new editor'
    Remove-Gate 'hold-build'
    Call ('recover -Batch ' + $crashed.batchId + ' -ConfirmStopped -Reason inspected') | Out-Null
    $interrupted = Call ('finish crash-editor -Batch ' + $crashed.batchId) 4
    Check ($interrupted.outcome -eq 'interrupted' -and $interrupted.error -eq 'inspected') 'Recovery must persist interruption for every waiter'
    $new = Call 'begin after-crash'
    Call ('recover nobody -Batch ' + $crashed.batchId + ' -ConfirmStopped -Reason inspected') | Out-Null
    Check ((State).current.id -eq $new.batchId) 'Old result/recovery must not clear a newer batch'
    Call ('finish after-crash -Batch ' + $new.batchId) | Out-Null
    Write-Output 'PASS coordinator crash, private job cleanup, orphan refusal, explicit recovery, newer batch preservation'

    Stage 'published-recovery'
    # Crash after publishing a result but before retiring state is recoverable
    # without rebuilding. Emulate only this disk commit boundary.
    $saved = State
    $saved.current = [pscustomobject]@{
        id = $resultA.batchId; phase = 'building'; participants = @($resultA.participants)
        createdUtc = $resultA.createdUtc; buildStartedUtc = $resultA.buildStartedUtc
        logPath = $resultA.logPath; owner = [pscustomobject]@{ pid = $PID; startTicks = '0' }
    }
    [IO.File]::WriteAllText($statePath, ($saved | ConvertTo-Json -Depth 20), $utf8)
    $before = (Counts).Count
    Call ('recover -Batch ' + $resultA.batchId + ' -ConfirmStopped -Reason published') | Out-Null
    Check ($null -eq (State).current -and (Counts).Count -eq $before) 'Published result recovery must retire only its own batch without rerunning'
    Write-Output 'PASS result-before-retirement crash recovery'

    Stage 'input-invalidation'
    # Unregistered edits cannot be physically stopped. Endpoint content checks
    # must invalidate the result even when the fake build/test both succeeded.
    foreach ($change in @('content', 'addition', 'deletion', 'rename', 'configuration', 'local-configuration')) {
        if ($change -eq 'deletion') {
            & git.exe -C $fixture add -- src/added.h
            Check ($LASTEXITCODE -eq 0) 'Deletion must cover an indexed input, not only an untracked file'
        }
        Remove-Gate 'build-started'
        Gate 'hold-build'
        $editing = Call ('begin input-' + $change)
        $checking = Start-Command ('finish input-' + $change + ' -Batch ' + $editing.batchId)
        Wait-Until { Test-Path -LiteralPath (Join-Path $fixture 'build-started') } ('input test starts: ' + $change) $checking
        switch ($change) {
            'content' { [IO.File]::WriteAllText((Join-Path $fixture 'src\fixture.cpp'), 'modified source', $utf8) }
            'addition' { [IO.File]::WriteAllText((Join-Path $fixture 'src\added.h'), 'new input', $utf8) }
            'deletion' { [IO.File]::Delete((Join-Path $fixture 'src\added.h')) }
            'rename' { [IO.File]::Move((Join-Path $fixture 'src\fixture.cpp'), (Join-Path $fixture 'src\renamed.cpp')) }
            'configuration' { [IO.File]::WriteAllText((Join-Path $fixture 'CMakePresets.json'), '{"version":6}', $utf8) }
            'local-configuration' { [IO.File]::WriteAllText((Join-Path $fixture 'CMakeUserPresets.json'), '{"version":6}', $utf8) }
        }
        Remove-Gate 'hold-build'
        $invalid = Complete $checking 5
        Check ($invalid.outcome -eq 'invalidated' -and $invalid.pipelineExitCode -eq 0 -and
            $invalid.inputStart.digest -ne $invalid.inputEnd.digest) "Successful commands must not validate changed inputs: $change"
        Compare-Results $invalid (Call ('finish input-' + $change + ' -Batch ' + $editing.batchId) 5)
    }
    Call ('finish task-a -Batch ' + $resultA.batchId) 2 | Out-Null
    $newTaskA = Call 'begin task-a'
    Call ('finish task-a -Batch ' + $resultA.batchId) 2 | Out-Null
    Check ((State).current.id -eq $newTaskA.batchId -and (State).current.participants[0].state -eq 'editing') 'An old successful result must neither finish nor verify a newer registration'
    $newResult = Call ('finish task-a -Batch ' + $newTaskA.batchId)
    Check ($newResult.inputCheck -eq 'stable' -and $newResult.inputStart.digest -ne $resultA.inputStart.digest) 'A new batch must bind the actual new inputs'
    Compare-Results $resultA (Call ('status -Batch ' + $resultA.batchId))
    Write-Output 'PASS content/add/delete/rename/config changes invalidate verification, ignored local presets covered, old result refuses newer edits'
    $succeeded = $true
}
catch {
    Snapshot $_.Exception.Message
    throw
}
finally {
    foreach ($process in $processes) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
        $process.Dispose()
    }
    $resolved = [IO.Path]::GetFullPath($fixture)
    Check ($resolved.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith('SnowDesktop-collaboration-')) 'Cleanup must remain within the disposable fixture'
    if ($succeeded) { Remove-Item -LiteralPath $resolved -Recurse -Force }
    else { Write-Output ("EVIDENCE cleanup stopped only fixture-owned processes; directory preserved: " + $resolved) }
}
