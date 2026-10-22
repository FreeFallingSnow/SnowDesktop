[CmdletBinding()]
param(
    # The batch router forwards its literal leading verb without rebuilding argv.
    [ValidateSet('run')][string]$Verb='run',
    [ValidateSet('start','invoke','status','wait','cancel','unsubscribe','worker','budget')][string]$Command='status',
    [ValidatePattern('^[a-f0-9]{32}$')][string]$RunId,
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$Task,
    [ValidateSet('release','debug','tests','ide','batch')][string]$Action='tests',
    [ValidateSet('full','fast','core','tools','label','name','list','plan','verify')][string]$Mode='list',
    [string]$Filter='',
    [ValidateSet('Release','Debug')][string]$Configuration='Release',
    [string[]]$Targets=@(),
    [ValidatePattern('^[a-f0-9]{32}$')][string]$PlanBatch,
    [ValidatePattern('^[a-f0-9]{32}$')][string]$Batch,
    [long]$EditRevision=-1,
    [ValidateRange(0,60)][int]$Seconds=30,
    [switch]$ReloadShell,[switch]$CloseApplication,
    [string]$Reason='', [switch]$ConfirmSharedCancellation,
    [ValidateRange(0,4096)][int]$CpuSlots=0,[ValidateRange(0,1048576)][int]$MemoryMiB=0,
    [ValidateRange(0,4096)][int]$CpuCapacity=0,[ValidateRange(0,1048576)][int]$MemoryCapacityMiB=0
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$directory=Join-Path $root '.build/collaboration/runs'
[void][IO.Directory]::CreateDirectory($directory)
. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_telemetry.ps1')
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
. (Join-Path $PSScriptRoot 'build_inputs.ps1')
. (Join-Path $PSScriptRoot 'build_evidence.ps1')
. (Join-Path $PSScriptRoot 'build_verification.ps1')
$script:runStageClocks=@{}

function Run-Path([string]$Id,[string]$Suffix){return Join-Path $directory ($Id+$Suffix)}
function Read-RunJson([string]$Path){
    # Read size and content through one shared handle. A provider stat followed
    # by a second open can straddle an atomic replacement of this same Run.
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while($true){
        $stream=$null;$reader=$null
        try {
            $stream=[IO.File]::Open($Path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
            if($stream.Length -gt 1MB){throw 'Run metadata exceeds its size budget.'}
            $reader=New-Object IO.StreamReader($stream,[Text.Encoding]::UTF8)
            return $reader.ReadToEnd()|ConvertFrom-Json
        } catch [IO.IOException] {
            $native=$_.Exception.HResult -band 0xffff
            if($native -notin 2,3,32,33 -or $clock.Elapsed.TotalSeconds -ge 1){throw}
            # A bounded observation retry never relaunches or cancels execution.
            Start-Sleep -Milliseconds 20
        } finally {if($reader){$reader.Dispose()}elseif($stream){$stream.Dispose()}}
    }
}
function Write-RunJson($Value,[string]$Path){
    $temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 30),(New-Object Text.UTF8Encoding($false)))
        if([IO.File]::Exists($Path)){[IO.File]::Replace($temporary,$Path,$temporary+'.bak')}else{[IO.File]::Move($temporary,$Path)}
    } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)};if([IO.File]::Exists($temporary+'.bak')){[IO.File]::Delete($temporary+'.bak')}}
}
function Write-NewRunRequest($Request){
    $path=Run-Path $Request.id '.request.json'
    $temporary=$path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    $bytes=[Text.Encoding]::UTF8.GetBytes(($Request|ConvertTo-Json -Depth 12))
    try {
        $stream=[IO.File]::Open($temporary,'CreateNew','Write','None')
        try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        # Same-directory publication is create-only. A reader can discover the
        # final name only after its complete bytes have been flushed and closed.
        [IO.File]::Move($temporary,$path)
    } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
}
function Run-OwnerState($Owner){
    if(-not $Owner){return 'none'}
    try {$process=Get-Process -Id $Owner.pid -ErrorAction Stop; if($process.StartTime.ToUniversalTime().Ticks.ToString() -eq $Owner.startTicks){return 'alive'};return 'exited'}
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] {return 'exited'}
    catch {return 'unknown'}
}
function Read-RunRequest([string]$Id){
    $request=Read-RunJson (Run-Path $Id '.request.json')
    if($request.schemaVersion -ne 1 -or $request.id -ne $Id -or $request.repositoryRoot -ne $root -or $request.nonce -notmatch '^[a-f0-9]{32}$'){
        throw 'Run request identity/root/version mismatch.'
    }
    return $request
}
function Get-RunView([string]$Id){
    $request=Read-RunRequest $Id
    $record=Read-RunJson (Run-Path $Id '.json')
    if($record.id -ne $Id -or $record.repositoryRoot -ne $root -or $record.nonce -ne $request.nonce){throw 'Run result identity mismatch.'}
    $ownerState=Run-OwnerState $record.owner
    if($ownerState -eq 'none' -and [IO.File]::Exists((Run-Path $Id '.launch.json'))){$ownerState=Run-OwnerState (Read-RunJson (Run-Path $Id '.launch.json'))}
    if($ownerState -eq 'none'){$ownerState=Run-OwnerState $request.launcher}
    $terminal=$record.status -in 'passed','failed','cancelled','start-failed','invalidated','interrupted'
    $observed=if(-not $terminal -and $ownerState -eq 'exited'){'interrupted'}else{$record.status}
    $metrics=if([IO.File]::Exists((Run-Path $Id '.resources.json'))){Read-RunJson (Run-Path $Id '.resources.json')}else{$null}
    $stages=@();$stageError=''
    try {$stages=@(Read-BuildStageEvents $root $Id)}catch{$stageError=$_.Exception.Message}
    $subscriptions=@()
    if($request.mode -eq 'verify'){
        $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
        $ledger=Get-Field $state 'verificationLedger'
        if($ledger){$subscriptions=@($ledger.entries|Where-Object runId -eq $Id|ForEach-Object subscriptions)}
    }
    return [pscustomobject]@{id=$Id;task=$request.task;status=$observed;persistedStatus=$record.status;terminal=($terminal -or $observed -eq 'interrupted');ownerState=$ownerState;subscriptions=$subscriptions;
        request=$request;record=$record;resources=$metrics;stageEvents=$stages;stageError=$stageError;observation='Closing an observer does not cancel the Run. An exited owner requires an explicit new Run, never automatic takeover.'}
}
function Run-CancellationRequested($Request){
    $path=Run-Path $Request.id '.cancel'
    return [IO.File]::Exists($path) -and [IO.File]::ReadAllText($path).Trim() -ceq $Request.nonce
}
function Write-RunRecord($Record){$Record.updatedUtc=[DateTime]::UtcNow.ToString('o');Write-RunJson $Record (Run-Path $Record.id '.json')}
function Start-RunStage($Record,[string]$Name){
    $stage=[pscustomobject]@{id=[Guid]::NewGuid().ToString('N');name=$Name;startedUtc=[DateTime]::UtcNow.ToString('o');completedUtc=$null;elapsedSeconds=$null;exitCode=$null}
    $script:runStageClocks[$stage.id]=[Diagnostics.Stopwatch]::StartNew()
    $Record.stages=@($Record.stages)+@($stage);Write-RunRecord $Record;return $stage
}
function Complete-RunStage($Record,$Stage,[int]$Code){$Stage.completedUtc=[DateTime]::UtcNow.ToString('o');$Stage.elapsedSeconds=$script:runStageClocks[$Stage.id].Elapsed.TotalSeconds;$Stage.exitCode=$Code;Write-RunRecord $Record}
function Enter-BatchRun($Request){
    $metadata=$null;$lease=$null
    try {
        $metadata=[IO.File]::Open((Join-Path $root '.build/collaboration/state.lock'),'OpenOrCreate','ReadWrite','None')
        $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
        $current=$state.current
        if(-not $current -or $current.id -ne $Request.batch -or $current.phase -ne 'building' -or
            $current.runId -ne $Request.id -or $current.protocolVersion -lt 3){throw 'The frozen batch no longer matches this Run.'}
        $planPath=Join-Path $root ('.build/collaboration/'+$Request.batch+'.plan.json')
        if((Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash -ne $Request.planHash){throw 'The immutable frozen plan changed before execution.'}
        if($current.owner.pid -ne $Request.launcher.pid -or $current.owner.startTicks -ne $Request.launcher.startTicks){throw 'The batch execution reservation already changed owner.'}
        $lease=[IO.File]::Open((Join-Path $root '.build/collaboration/build.lock'),'OpenOrCreate','ReadWrite','None')
        $current.owner=[pscustomobject]@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()}
        Write-RunJson $state (Join-Path $root '.build/collaboration/state.json')
        $old=Start-EntryCredential $root $Request.batch
        return [pscustomobject]@{lease=$lease;oldToken=$old}
    } catch {
        if($lease){$lease.Dispose()}
        if(Test-EntrySharingViolation $_.Exception){throw 'Shared output lease/transaction is busy; the frozen Run is queued.'}
        throw
    } finally {if($metadata){$metadata.Dispose()}}
}

try {
    if($Command -eq 'budget'){
        if($CpuSlots -or $MemoryMiB){throw 'Use capacity fields for budget configuration and slots/MiB for run start.'}
        if($CpuCapacity -or $MemoryCapacityMiB){
            if(-not $CpuCapacity -or -not $MemoryCapacityMiB -or -not $Task -or [string]::IsNullOrWhiteSpace($Reason) -or $Reason.Length -gt 2048){throw 'Budget configuration needs both capacities, an active Task and a nonempty Reason up to2048 characters.'}
            Initialize-ResourceRuntime
            $transaction=Open-EntryStateTransaction $root
            try {
                $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
                if(-not $state -or $state.repositoryRoot -ne $root -or @($state.taskLedger.tasks|Where-Object {$_.id -eq $Task -and -not $_.closedUtc}).Count -ne 1){throw 'Budget configuration requires an active Task in this checkout.'}
                Set-WeightedResourcePolicy $root $CpuCapacity $MemoryCapacityMiB $Task $Reason|ConvertTo-Json -Depth 8
            }finally{$transaction.Dispose()}
        }else{Get-WeightedResourcePolicy $root|ConvertTo-Json -Depth 8}
        exit 0
    }
    if($CpuCapacity -or $MemoryCapacityMiB){throw 'Capacity changes require run budget; no execution was submitted.'}
    if($Command -in 'start','invoke'){
        $automaticTask=$false
        if($Mode -eq 'verify' -and ($Action -ne 'tests' -or -not $Task -or -not $PlanBatch -or [string]::IsNullOrWhiteSpace($Reason) -or $Reason.Length -gt 2048 -or $Configuration -ne 'Release')){throw 'Verification requires tests, an explicit active Task, Release, -PlanBatch and a nonempty -Reason (up to 2048 characters).'}
        if($Command -eq 'invoke' -and -not $Task){
            $Task='entry-'+[Guid]::NewGuid().ToString('N');$automaticTask=$true
            $null=& (Join-Path $PSScriptRoot 'build_manager.ps1') task-open $Task -TaskName ('Standard '+$Action+' '+$Mode) -Adapter cli
            if($LASTEXITCODE -ne 0){throw 'Could not register the standard entry task.'}
        }
        if(-not $Task){throw 'run start requires -Task from task-open.'}
        if($Action -eq 'batch' -and ($Command -ne 'start' -or -not $Batch -or $EditRevision -lt 0)){throw 'A batch Run requires a coordinator reservation and exact edit revision.'}
        if($Filter.Length -gt 4096){throw 'Test filter exceeds its length budget.'}
        if($ReloadShell -and ($CloseApplication -or $Action -notin 'release','debug')){throw 'ReloadShell requires release/debug and cannot be combined with CloseApplication.'}
        if($CloseApplication -and $Action -ne 'release'){throw 'CloseApplication requires release.'}
        foreach($target in $Targets){if($target -notmatch '^[A-Za-z0-9_.-]+$'){throw 'IDE targets must be literal target names.'}}
        $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
        if(-not $state -or $state.repositoryRoot -ne $root){throw 'Open the investigation task in this checkout before starting a Run.'}
        $tasks=@($state.taskLedger.tasks | Where-Object {$_.id -eq $Task -and -not $_.closedUtc})
        if($tasks.Count -ne 1){throw 'run start requires one active investigation task in this checkout.'}
        $id=[Guid]::NewGuid().ToString('N');$now=[DateTime]::UtcNow.ToString('o')
        $request=[pscustomobject]@{schemaVersion=1;id=$id;repositoryRoot=$root;task=$Task;taskRevision=$tasks[0].revision;nonce=[Guid]::NewGuid().ToString('N');
            launcher=[pscustomobject]@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()};
            action=$Action;mode=$Mode;filter=$Filter;configuration=$Configuration;targets=@($Targets);planBatch=$PlanBatch;automaticTask=$automaticTask;
            reloadShell=[bool]$ReloadShell;closeApplication=[bool]$CloseApplication;createdUtc=$now}
        $record=[pscustomobject]@{schemaVersion=1;id=$id;repositoryRoot=$root;nonce=$request.nonce;status='starting';owner=$null;createdUtc=$now;updatedUtc=$now;
            completedUtc=$null;exitCode=$null;error='';waitingFor='';waitIntervals=@();waitIntervalsTruncated=$false;stages=@();inputStart=$null;inputEnd=$null;logPath=(Run-Path $id '.stdout.log')}
        $heavy=-not ($request.action -eq 'tests' -and $request.mode -in 'list','plan')
        $frozenResources=$null
        if($request.action -eq 'batch'){
            if($CpuSlots -or $MemoryMiB){throw 'A batch uses only its frozen aggregate resource plan.'}
            $frozenPlan=Read-EntryJson (Join-Path $root ('.build/collaboration/'+$Batch+'.plan.json'))
            if(-not $frozenPlan -or $frozenPlan.batchId -ne $Batch){throw 'Frozen resource plan is unavailable.'}
            $heavy=[bool]$frozenPlan.buildRequired -or $frozenPlan.mode -ne 'skipped'
            $frozenResources=Get-Field $frozenPlan 'projectResources'
        }
        # Configured test execution has unknown cross-process resources until
        # selection. Keep it exclusive; only verified definitions may opt into
        # sharing. Preflight process actions and arbitrary IDE targets also do.
        $exclusive=$request.action -notin 'release','debug' -or $request.reloadShell -or $request.closeApplication
        if($Mode -eq 'verify'){$exclusive=$false}
        $request|Add-Member NoteProperty projectResources $(if($frozenResources){$frozenResources}else{Get-ProjectResourceBinding $root $heavy $CpuSlots $MemoryMiB $exclusive})
        $batchTransaction=$null;$joined=$false;$verification=$null
        if($Mode -eq 'verify'){
            $verification=Get-VerificationSnapshot $root $PlanBatch $Filter $(if($request.projectResources.policy -eq 'project-weighted-v1'){$request.projectResources}else{$null})
            if($request.projectResources.policy -eq 'project-weighted-v1'){
                $shared=@($verification.tests|ForEach-Object properties|Where-Object {$_.name -in 'RESOURCE_LOCK','RESOURCE_GROUPS','RUN_SERIAL' -and $_.value})
                if($shared.Count){$request.projectResources.compatibilityMode='exclusive'}
                $verification.key=Get-VerificationTextDigest ($verification.binding|ConvertTo-Json -Depth 20 -Compress)
            }
            # Inventory is re-read and validated by the worker; retain hashes,
            # not duplicate raw CTest environment/command contents in requests.
            $verification.PSObject.Properties.Remove('tests')
            $request|Add-Member NoteProperty verification $verification
        }
        try {
        if($Mode -eq 'verify'){
            $batchTransaction=Open-EntryStateTransaction $root
            $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
            $active=@($state.taskLedger.tasks|Where-Object {$_.id -eq $Task -and -not $_.closedUtc})
            if($active.Count -ne 1){throw 'Verification subscriber task is no longer active.'}
            $ledger=Get-VerificationLedger $state
            foreach($candidate in @($ledger.entries|Where-Object key -eq $verification.key)){
                $existing=Get-RunView $candidate.runId
                if(-not $existing.terminal -and $existing.ownerState -eq 'alive' -and -not (Run-CancellationRequested $existing.request)){
                    if($existing.request.mode -ne 'verify' -or $existing.request.verification.key -ne $verification.key){throw 'Verification ledger/request identity mismatch.'}
                    Add-VerificationSubscription $candidate $active[0] $PlanBatch $Reason
                    $id=$candidate.runId;$joined=$true;break
                }
                if(-not $existing.terminal -and $existing.ownerState -eq 'unknown'){throw 'Verification owner state is unknown; inspect the existing Run before submitting another attempt.'}
            }
            if(-not $joined){
                $candidate=[pscustomobject]@{key=$verification.key;runId=$id;createdUtc=$now;subscriptions=@()}
                Add-VerificationSubscription $candidate $active[0] $PlanBatch $Reason
                $ledger.entries=@($ledger.entries)+@($candidate)
            }
            if(($state|ConvertTo-Json -Depth 40 -Compress).Length -gt 7MB){throw 'Verification metadata budget requires explicit maintenance.'}
        }
        if($Action -eq 'batch'){
            $batchTransaction=Open-EntryStateTransaction $root
            $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
            $current=$state.current
            if(-not $current -or $current.id -ne $Batch -or $current.phase -ne 'building' -or $current.protocolVersion -lt 3 -or
                $current.owner.pid -ne $PID -or $current.owner.startTicks -ne $request.launcher.startTicks -or $current.PSObject.Properties['runId']){
                throw 'Only the current freezing coordinator can reserve one Run for a new batch.'
            }
            $member=@($current.participants|Where-Object id -eq $Task)
            if($member.Count -ne 1 -or $member[0].editRevision -ne $EditRevision -or $member[0].state -ne 'finished'){throw 'Batch Run participant/revision mismatch.'}
            $request|Add-Member NoteProperty batch $Batch
            $request|Add-Member NoteProperty editRevision $EditRevision
            $request|Add-Member NoteProperty planHash ((Get-FileHash -LiteralPath (Join-Path $root ('.build/collaboration/'+$Batch+'.plan.json')) -Algorithm SHA256).Hash)
            $current|Add-Member NoteProperty runId $id
        }
        # Identity is fresh. Request publication is create-only, never a mutable plan.
        if(-not $joined){Write-NewRunRequest $request;Write-RunRecord $record}
        if($Action -eq 'batch' -or $Mode -eq 'verify'){Write-RunJson $state (Join-Path $root '.build/collaboration/state.json')}
        } finally {if($batchTransaction){$batchTransaction.Dispose()}}
        $launched=$false
        if(-not $joined){
        try {
            $child=Start-BuildPowerShellScript -Script $PSCommandPath -Arguments ('-Command worker -RunId '+$id) -WorkingDirectory $root -Breakaway `
                -OutputPath (Run-Path $id '.stdout.log') -ErrorPath (Run-Path $id '.stderr.log')
            $launched=$true
            try {Write-RunJson ([pscustomobject]@{pid=$child.Id;startTicks=$child.StartTime.ToUniversalTime().Ticks.ToString()}) (Run-Path $id '.launch.json')}
            finally {$child.Dispose()}
        } catch {
            if($launched){throw ('Worker was admitted; observe Run '+$id+'. Launch receipt failed: '+$_.Exception.Message)}
            # No target code was admitted by the launch handshake.
            $record.status='start-failed';$record.exitCode=2;$record.error=$_.Exception.Message;$record.completedUtc=[DateTime]::UtcNow.ToString('o');Write-RunRecord $record
            Get-RunView $id|ConvertTo-Json -Depth 30;exit 2
        }
        }
        if($Command -eq 'start'){
            [pscustomobject]@{id=$id;task=$Task;joined=$joined;requestPath=(Run-Path $id '.request.json');status='submitted';next='run status/wait; unsubscribe withdraws interest, only explicit owner cancellation stops execution'}|ConvertTo-Json -Depth 8
            exit 0
        }
        $RunId=$id
        [Console]::Error.WriteLine('Persistent Run '+$RunId+'; task '+$Task+'. Closing this observer preserves execution; use run cancel explicitly.')
    }
    if(-not $RunId){throw 'This command requires -RunId.'}
    $request=Read-RunRequest $RunId
    if($Command -eq 'unsubscribe'){
        if(-not $Task -or $request.mode -ne 'verify'){throw 'unsubscribe requires a verification Run and subscriber Task.'}
        $transaction=Open-EntryStateTransaction $root
        try {
            $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
            $ledger=Get-VerificationLedger $state
            $entries=@($ledger.entries|Where-Object runId -eq $RunId)
            if($entries.Count -ne 1){throw 'Verification subscription identity mismatch.'}
            $mine=@($entries[0].subscriptions|Where-Object {$_.task -eq $Task -and -not $_.withdrawnUtc})
            if(-not $mine.Count){throw 'No active subscription for this Task.'}
            foreach($subscription in $mine){$subscription.withdrawnUtc=[DateTime]::UtcNow.ToString('o')}
            Write-RunJson $state (Join-Path $root '.build/collaboration/state.json')
        } finally {$transaction.Dispose()}
        [pscustomobject]@{id=$RunId;task=$Task;withdrawn=$true;runPreserved=$true}|ConvertTo-Json;exit 0
    }
    if($Command -eq 'cancel'){
        if(-not $Task -or $request.task -ne $Task){throw 'Explicit cancellation requires the Run owning task ID.'}
        $cancelTransaction=$null
        try {
        if($request.mode -eq 'verify'){
            $cancelTransaction=Open-EntryStateTransaction $root
            $shared=Get-RunView $RunId
            if(@($shared.subscriptions|Where-Object {$_.task -ne $Task -and -not $_.withdrawnUtc}).Count -and -not $ConfirmSharedCancellation){throw 'Other Tasks subscribe to this verification; owner cancellation requires -ConfirmSharedCancellation.'}
        }
        $view=Get-RunView $RunId
        if(-not $view.terminal){
            $temporary=Run-Path $RunId ('.cancel.'+[Guid]::NewGuid().ToString('N')+'.tmp')
            [IO.File]::WriteAllText($temporary,$request.nonce,(New-Object Text.UTF8Encoding($false)))
            $cancelPath=Run-Path $RunId '.cancel'
            try {
                if([IO.File]::Exists($cancelPath)){[IO.File]::Replace($temporary,$cancelPath,$temporary+'.bak')}else{[IO.File]::Move($temporary,$cancelPath)}
            } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)};if([IO.File]::Exists($temporary+'.bak')){[IO.File]::Delete($temporary+'.bak')}}
        }
        [pscustomobject]@{id=$RunId;cancellationRequested=(-not $view.terminal);status=$view.status;observation='A request is not a completed cancellation; observe the terminal result.'}|ConvertTo-Json
        } finally {if($cancelTransaction){$cancelTransaction.Dispose()}}
        exit 0
    }
    if($Command -in 'status','wait','invoke'){
        $timer=[Diagnostics.Stopwatch]::StartNew()
        $notice=[Diagnostics.Stopwatch]::StartNew();$logReaders=@{};$errorReader=$null
        try {
        do {
            $view=Get-RunView $RunId
            if($Command -eq 'invoke'){
                $errorPath=Run-Path $RunId '.stderr.log'
                if(-not $errorReader -and [IO.File]::Exists($errorPath)){
                    $errorStream=[IO.File]::Open($errorPath,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                    $errorReader=New-Object IO.StreamReader($errorStream,[Text.Encoding]::UTF8)
                }
                if($errorReader){$chunk=$errorReader.ReadToEnd();if($chunk){[Console]::Error.Write($chunk)}}
                foreach($event in @($view.stageEvents|Where-Object {$_.kind -eq 'start' -and $_.name -like '*-pipeline' -and $_.logPath})){
                    $path=[IO.Path]::GetFullPath($event.logPath)
                    if(-not $path.StartsWith((Join-Path $root '.build')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Run log escaped this checkout output.'}
                    if(-not $logReaders.ContainsKey($path) -and [IO.File]::Exists($path)){
                        $stream=[IO.File]::Open($path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
                        $logReaders[$path]=New-Object IO.StreamReader($stream,[Text.Encoding]::UTF8)
                    }
                }
                foreach($reader in $logReaders.Values){$chunk=$reader.ReadToEnd();if($chunk){[Console]::Out.Write($chunk)}}
                if($notice.Elapsed.TotalSeconds -ge 30){[Console]::Error.WriteLine('Run '+$RunId+': '+$view.status+' '+$view.record.waitingFor);$notice.Restart()}
            }
            if($view.terminal -or $Command -eq 'status' -or ($Command -ne 'invoke' -and $timer.Elapsed.TotalSeconds -ge $Seconds)){break}
            $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
            $observerTask=if($Task){$Task}else{$request.task}
            # An unavailable/legacy task snapshot is not an execution failure or
            # acknowledgement. Keep observing; a later read retains all events.
            $urgent=@(Get-TaskEvents (Get-Field $state 'taskLedger') $observerTask -UrgentUnacknowledged)
            if($urgent.Count){[pscustomobject]@{id=$RunId;status=$view.status;events=$urgent;runPreserved=$true}|ConvertTo-Json -Depth 25;exit 6}
            Start-Sleep -Milliseconds 500
        } while($true)
        } finally {foreach($reader in $logReaders.Values){$reader.Dispose()};if($errorReader){$errorReader.Dispose()}}
        if($Command -eq 'invoke'){
            [Console]::Error.WriteLine('Run '+$RunId+': '+$view.status+'. Record: '+(Run-Path $RunId '.json'))
            if($view.record.error){[Console]::Error.WriteLine($view.record.error)}
            if($view.status -eq 'interrupted'){exit 4}
            exit ([int]$view.record.exitCode)
        }
        $view|ConvertTo-Json -Depth 30
        if($Command -eq 'wait'){
            if(-not $view.terminal){exit 10}
            if($view.status -eq 'interrupted'){exit 4}
            exit ([int]$view.record.exitCode)
        }
        exit 0
    }
    # The process lifetime lease and immutable owner fence prevent duplicate workers
    # and make owner loss an explicit interrupted outcome rather than a takeover.
    $ownerLease=$null;$entry=$null;$resourceLease=$null;$record=$null;$ownsRun=$false;$queue=$null
    try {
        $ownerLease=[IO.File]::Open((Run-Path $RunId '.owner.lock'),'OpenOrCreate','ReadWrite','None')
        $record=Read-RunJson (Run-Path $RunId '.json')
        if($record.status -ne 'starting' -or $record.owner){throw 'This Run already acquired an executor; start a new Run explicitly.'}
        $record.owner=[pscustomobject]@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()}
        $ownsRun=$true
        # A newly detached executor cannot borrow its launcher's old lease.
        [Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$null,'Process')
        [Environment]::SetEnvironmentVariable('SNOWDESKTOP_RUN_ID',$null,'Process')
        [Environment]::SetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT',$null,'Process')
        # Publish the owner before any fallible resource validation. Even a
        # pre-execution invalidation must retain a verifiable batch publisher.
        $record.status='queued';Write-RunRecord $record;$queue=Start-RunStage $record 'queue'
        $binding=Get-Field $request 'projectResources'
        Set-Field $record 'resourceAdmission' ([pscustomobject]@{policy=$(if($binding){$binding.policy}else{'legacy-unbudgeted'});required=[bool]($binding -and $binding.required);
            commonGitDirectory=$(if($binding){$binding.commonGitDirectory}else{$null});acquiredUtc=$null;releasedUtc=$null;bindingValid=$null;
            accounting='A v1 exclusive lease or v2 weighted reservations. CPU/committed-memory limits are read from the private Job, separately.'})
        if($binding -and $binding.required){
            if($binding.policy -eq 'project-weighted-v1'){
                foreach($name in @('compatibilityMode','cpuSlots','memoryMiB','reservedMemoryMiB','logicalProcessors','cpuCapacity','memoryCapacityMiB','headroomMiB','policyHash')){Set-Field $record.resourceAdmission $name $binding.$name}
                $currentPolicy=Get-WeightedResourcePolicy $root -Persist
                if($currentPolicy.hash -ne $binding.policyHash){$record.resourceAdmission.bindingValid=$false;throw 'Resource policy changed before admission.'}
                $mustBeExclusive=($request.action -notin 'release','debug' -and $request.mode -ne 'verify') -or $request.reloadShell -or $request.closeApplication
                if($binding.compatibilityMode -notin 'shared','exclusive' -or ($mustBeExclusive -and $binding.compatibilityMode -ne 'exclusive')){$record.resourceAdmission.bindingValid=$false;throw 'Resource policy changed before admission.'}
                $actual=Get-ProjectResourceBinding $root $true $binding.cpuSlots $binding.memoryMiB ($binding.compatibilityMode -eq 'exclusive')
            }else{$actual=Get-ProjectResourceBinding $root $true}
            if(($binding|ConvertTo-Json -Depth 8 -Compress) -ne ($actual|ConvertTo-Json -Depth 8 -Compress)){
                $record.resourceAdmission.bindingValid=$false;throw 'Resource policy changed before admission.'
            }
        }
        $record.resourceAdmission.bindingValid=$true;Write-RunRecord $record
        $queueNotice=[Diagnostics.Stopwatch]::StartNew()
        $waitInterval=$null;$waitClock=$null;$queuedCancellation=$false
        while(-not $entry){
            if(Run-CancellationRequested $request){$record.exitCode=4;Complete-RunStage $record $queue 4;$queuedCancellation=$true;break}
            try {$entry=if($request.action -eq 'batch'){Enter-BatchRun $request}else{Enter-BuildEntry $root}}
            catch {
                if($_.Exception.Message -notmatch '^Shared output (busy:|lease/transaction is busy;)'){throw}
                if($record.waitingFor -ne $_.Exception.Message){
                    if($waitInterval){$waitInterval.completedUtc=[DateTime]::UtcNow.ToString('o');$waitInterval.elapsedSeconds=$waitClock.Elapsed.TotalSeconds}
                    $record.waitingFor=$_.Exception.Message
                    $waitInterval=[pscustomobject]@{resource='shared-output';reason=$record.waitingFor;startedUtc=[DateTime]::UtcNow.ToString('o');completedUtc=$null;elapsedSeconds=$null;measurement='First observed busy until acquisition or reason change, including retry/backoff.'}
                    $waitClock=[Diagnostics.Stopwatch]::StartNew()
                    if(@($record.waitIntervals).Count -lt 100){$record.waitIntervals=@($record.waitIntervals)+@($waitInterval)}else{$record.waitIntervalsTruncated=$true}
                    Write-RunRecord $record
                }
                if($queueNotice.Elapsed.TotalSeconds -ge 30){[Console]::Error.WriteLine('Run '+$RunId+' is queued: '+$record.waitingFor);$queueNotice.Restart()}
                Start-Sleep -Milliseconds 200
            }
        }
        if($waitInterval){$waitInterval.completedUtc=[DateTime]::UtcNow.ToString('o');$waitInterval.elapsedSeconds=$waitClock.Elapsed.TotalSeconds;Write-RunRecord $record;$waitInterval=$null}
        # Hold this checkout's output authority while queued for common capacity;
        # do not preflight, configure, compile or launch a test before admission.
        if($entry -and $binding -and $binding.required){
            $observationClock=[Diagnostics.Stopwatch]::StartNew()
            while(-not $resourceLease){
                if(Run-CancellationRequested $request){$record.exitCode=4;Complete-RunStage $record $queue 4;$queuedCancellation=$true;break}
                try {$resourceLease=Enter-ProjectResources $binding;$record.resourceAdmission.acquiredUtc=[DateTime]::UtcNow.ToString('o')}
                catch {
                    if($_.Exception.Message -eq 'Resource policy changed before admission.'){$record.resourceAdmission.bindingValid=$false;throw}
                    if($_.Exception.Message -notmatch '^Shared project resources busy:'){throw}
                    $resourceError=$_.Exception
                    while($resourceError -and -not $resourceError.Data.Contains('resourceObservation')){$resourceError=$resourceError.InnerException}
                    if($resourceError){
                        $observation=$resourceError.Data['resourceObservation']
                        Set-Field $record.resourceAdmission 'lastObservation' ([pscustomobject]@{observedUtc=[DateTime]::UtcNow.ToString('o');occupiedCpuSlots=$observation.OccupiedCpuSlots;
                            occupiedMemoryMiB=($observation.OccupiedMemorySlots*256);availablePhysicalBytes=$observation.AvailablePhysicalBytes;reason=$observation.BusyKind})
                    }
                    if($record.waitingFor -ne $_.Exception.Message){
                        if($waitInterval){$waitInterval.completedUtc=[DateTime]::UtcNow.ToString('o');$waitInterval.elapsedSeconds=$waitClock.Elapsed.TotalSeconds}
                        $record.waitingFor=$_.Exception.Message
                        $waitInterval=[pscustomobject]@{resource=$(if($binding.policy -eq 'project-weighted-v1'){'project-cpu-memory'}else{'project-execution'});reason=$record.waitingFor;startedUtc=[DateTime]::UtcNow.ToString('o');completedUtc=$null;elapsedSeconds=$null;measurement='First observed shared project contention until acquisition, reason change or cancellation, including retry/backoff.'}
                        $waitClock=[Diagnostics.Stopwatch]::StartNew()
                        if(@($record.waitIntervals).Count -lt 100){$record.waitIntervals=@($record.waitIntervals)+@($waitInterval)}else{$record.waitIntervalsTruncated=$true}
                        Write-RunRecord $record
                    }
                    elseif($observationClock.Elapsed.TotalSeconds -ge 1){Write-RunRecord $record;$observationClock.Restart()}
                    if($queueNotice.Elapsed.TotalSeconds -ge 30){[Console]::Error.WriteLine('Run '+$RunId+' is queued: '+$record.waitingFor);$queueNotice.Restart()}
                    Start-Sleep -Milliseconds 200
                }
            }
        }
        if($waitInterval){$waitInterval.completedUtc=[DateTime]::UtcNow.ToString('o');$waitInterval.elapsedSeconds=$waitClock.Elapsed.TotalSeconds;Write-RunRecord $record}
        # Persist completed queue timing while the executor is still active.
        # A terminal status is published only after batch retirement below.
        if($queuedCancellation){$record.status='cancelled';$record.waitingFor=''}
        if($entry -and -not $queuedCancellation){
            $record.waitingFor=''
            Complete-RunStage $record $queue 0
            . (Join-Path $PSScriptRoot 'build_inputs.ps1')
            $record.inputStart=Get-BuildInputIdentity $root
            $record.status='running';Write-RunRecord $record
            $stage=Start-RunStage $record 'pipeline'
            $previousRun=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
            try {
                [Environment]::SetEnvironmentVariable('SNOWDESKTOP_RUN_ID',$RunId,'Process')
                $options=@{EntryAction=$request.action;EntryMode=$request.mode;EntryFilter=$request.filter}
                if($request.PSObject.Properties['configuration']){$options.EntryConfiguration=$request.configuration;$options.EntryTargets=@($request.targets)}
                if($request.PSObject.Properties['planBatch'] -and $request.planBatch){$options.EntryPlanBatch=$request.planBatch}
                if($request.reloadShell){$options.EntryReloadShell=$true};if($request.closeApplication){$options.EntryCloseApplication=$true}
                $global:LASTEXITCODE=0
                if($request.action -eq 'batch'){
                    & (Join-Path $PSScriptRoot 'build_manager.ps1') execute $request.task -Batch $request.batch -Revision $request.editRevision -ExecutionRun $RunId
                } else {& (Join-Path $PSScriptRoot 'build_entry.ps1') @options}
                $record.exitCode=$global:LASTEXITCODE
            } finally {[Environment]::SetEnvironmentVariable('SNOWDESKTOP_RUN_ID',$previousRun,'Process')}
            Complete-RunStage $record $stage $record.exitCode
            $record.inputEnd=Get-BuildInputIdentity $root
            $cancelled=('SnowDesktop.Build.Job' -as [type]) -and [SnowDesktop.Build.Job]::RunWasCancelled
            $record.status=if($cancelled){'cancelled'}elseif($request.mode -eq 'verify' -and $record.exitCode -eq 5){'invalidated'}elseif($record.exitCode -eq 0){'passed'}else{'failed'}
            if($record.status -eq 'cancelled'){$record.exitCode=4}
            elseif($record.inputStart.digest -ne $record.inputEnd.digest){$record.status='invalidated';$record.exitCode=5;$record.error='Build inputs changed while this Run was executing.'}
        }
    } catch {
        if(-not $ownsRun){throw}
        $invalidated=$record.PSObject.Properties['resourceAdmission'] -and $record.resourceAdmission.bindingValid -eq $false
        $record.exitCode=if($invalidated){5}else{2};$record.error=$_.Exception.ToString()
        if($queue -and -not $queue.completedUtc){Complete-RunStage $record $queue $record.exitCode}
        $record.status=if($invalidated){'invalidated'}else{'failed'}
    } finally {
        # Terminal means that the leased output and automatic task transaction
        # have both finished. Observers may submit their next entry immediately.
        if($resourceLease){$resourceLease.Dispose();$record.resourceAdmission.releasedUtc=[DateTime]::UtcNow.ToString('o')}
        Exit-BuildEntry $entry
        if($ownsRun -and $request.action -eq 'batch'){
            try {
                $null=& (Join-Path $PSScriptRoot 'build_manager.ps1') publish-run $request.task -Batch $request.batch -Revision $request.editRevision -ExecutionRun $RunId -Reason $record.error -WaitSeconds 30
                $publicationCode=$LASTEXITCODE
                $batchResult=Read-EntryJson (Join-Path $root ('.build/collaboration/'+$request.batch+'.json'))
                $retiredState=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
                # publish-run returns the batch outcome, including cancellation
                # and failed tests. Validate the retired identity and that code,
                # rather than treating every legitimate nonzero result as failure.
                if(-not $batchResult -or -not $retiredState -or $batchResult.schemaVersion -ne 1 -or
                    $batchResult.repositoryRoot -ne $root -or $batchResult.batchId -ne $request.batch -or
                    $batchResult.runId -ne $RunId -or $publicationCode -ne $batchResult.exitCode -or
                    ($retiredState.current -and $retiredState.current.id -eq $request.batch)){
                    throw 'Batch result publication failed; preserve state and inspect status/recover.'
                }
                $record.status=if($batchResult.outcome -eq 'skipped'){'passed'}else{$batchResult.outcome}
                $record.exitCode=$batchResult.exitCode;$record.error=$batchResult.error
            } catch {$record.status='failed';$record.exitCode=2;$record.error+=' '+$_.Exception.Message}
        }
        if($ownsRun -and $request.PSObject.Properties['automaticTask'] -and $request.automaticTask){
            try {
                $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
                $taskRecord=@($state.taskLedger.tasks|Where-Object id -eq $request.task)[0]
                $null=& (Join-Path $PSScriptRoot 'build_manager.ps1') task-close $request.task -TaskRevision $taskRecord.revision -TaskPhase $(if($record.status -eq 'cancelled'){'cancelled'}else{'completed'})
            } catch {[Console]::Error.WriteLine('Run is terminal; automatic task closure needs attention: '+$_.Exception.Message)}
        }
        if($ownsRun){
            $record.completedUtc=[DateTime]::UtcNow.ToString('o')
            if($request.mode -eq 'verify'){
                try {
                    $coverage=Read-EntryJson (Run-Path $RunId '.verification.json')
                    if($coverage -and $record.status -in 'failed','invalidated' -and $coverage.error){$record.error=$coverage.error}
                    $subscriptions=@((Get-RunView $RunId).subscriptions)
                    if($coverage){Set-Field $coverage 'tasks' @($subscriptions|Select-Object -ExpandProperty task -Unique|ForEach-Object {[pscustomobject]@{participant=$_;requested=$request.verification.binding.names}})}
                    $synthetic=[pscustomobject]@{batchId=$null;runId=$RunId;coverage=$coverage;inputStart=$record.inputStart;inputEnd=$record.inputEnd;inputCheck=$(if($record.status -eq 'invalidated'){'invalidated'}elseif($record.inputStart -and $record.inputEnd -and $record.inputStart.digest -eq $record.inputEnd.digest){'stable'}else{'not-recorded'});pipelineExitCode=$record.exitCode;testingPlan=[pscustomobject]@{configuration='Release'}}
                    Set-Field $record 'evidence' (New-BatchTestEvidence $root $synthetic)
                    Set-Field $record 'verification' ([pscustomobject]@{baseline=$request.planBatch;key=$request.verification.key;names=$request.verification.binding.names;noBuild=$true;reuseAllowed=$false})
                    $projectionRoot=Join-Path $root '.build/verification';[void][IO.Directory]::CreateDirectory($projectionRoot)
                    Write-RunJson ([pscustomobject]@{schemaVersion=2;kind='durable-test-verification';runId=$RunId;task=$request.task;status=$record.status;baseline=$request.planBatch;binding=$request.verification.binding;
                        noBuild=$true;reuseAllowed=$false;createdUtc=$record.createdUtc;completedUtc=$record.completedUtc;exitCode=$record.exitCode;report=(Get-Field $coverage 'report');evidence=$record.evidence;
                        authority=('.build/collaboration/runs/'+$RunId+'.json');logPath=$record.logPath}) (Join-Path $projectionRoot ($RunId+'.json'))
                } catch {$record.status='failed';$record.exitCode=2;$record.error+=' Verification publication: '+$_.Exception.Message}
            }
            Write-RunRecord $record
        }
        if($ownerLease){$ownerLease.Dispose()}
    }
    $record|ConvertTo-Json -Depth 30;exit ([int]$record.exitCode)
} catch {[Console]::Error.WriteLine($_.Exception.Message);exit 2}
