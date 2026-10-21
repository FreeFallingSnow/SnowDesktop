[CmdletBinding()]
param(
    # The batch router forwards its literal leading verb without rebuilding argv.
    [ValidateSet('run')][string]$Verb='run',
    [ValidateSet('start','invoke','status','wait','cancel','worker')][string]$Command='status',
    [ValidatePattern('^[a-f0-9]{32}$')][string]$RunId,
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$Task,
    [ValidateSet('release','debug','tests','ide')][string]$Action='tests',
    [ValidateSet('full','fast','core','tools','label','name','list','plan')][string]$Mode='list',
    [string]$Filter='',
    [ValidateSet('Release','Debug')][string]$Configuration='Release',
    [string[]]$Targets=@(),
    [ValidatePattern('^[a-f0-9]{32}$')][string]$PlanBatch,
    [ValidateRange(0,60)][int]$Seconds=30,
    [switch]$ReloadShell,[switch]$CloseApplication
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$directory=Join-Path $root '.build/collaboration/runs'
[void][IO.Directory]::CreateDirectory($directory)
. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_telemetry.ps1')

function Run-Path([string]$Id,[string]$Suffix){return Join-Path $directory ($Id+$Suffix)}
function Read-RunJson([string]$Path){
    $file=Get-Item -LiteralPath $Path -ErrorAction Stop
    if($file.Length -gt 1MB){throw 'Run metadata exceeds its size budget.'}
    return Read-EntryJson $Path
}
function Write-RunJson($Value,[string]$Path){
    $temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($Value|ConvertTo-Json -Depth 30),(New-Object Text.UTF8Encoding($false)))
        if([IO.File]::Exists($Path)){[IO.File]::Replace($temporary,$Path,$temporary+'.bak')}else{[IO.File]::Move($temporary,$Path)}
    } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)};if([IO.File]::Exists($temporary+'.bak')){[IO.File]::Delete($temporary+'.bak')}}
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
    $terminal=$record.status -in 'passed','failed','cancelled','start-failed','invalidated'
    $observed=if(-not $terminal -and $ownerState -eq 'exited'){'interrupted'}else{$record.status}
    $metrics=if([IO.File]::Exists((Run-Path $Id '.resources.json'))){Read-RunJson (Run-Path $Id '.resources.json')}else{$null}
    $stages=@();$stageError=''
    try {$stages=@(Read-BuildStageEvents $root $Id)}catch{$stageError=$_.Exception.Message}
    return [pscustomobject]@{id=$Id;task=$request.task;status=$observed;persistedStatus=$record.status;terminal=($terminal -or $observed -eq 'interrupted');ownerState=$ownerState;
        request=$request;record=$record;resources=$metrics;stageEvents=$stages;stageError=$stageError;observation='Closing an observer does not cancel the Run. An exited owner requires an explicit new Run, never automatic takeover.'}
}
function Run-CancellationRequested($Request){
    $path=Run-Path $Request.id '.cancel'
    return [IO.File]::Exists($path) -and [IO.File]::ReadAllText($path).Trim() -ceq $Request.nonce
}
function Write-RunRecord($Record){$Record.updatedUtc=[DateTime]::UtcNow.ToString('o');Write-RunJson $Record (Run-Path $Record.id '.json')}
function Start-RunStage($Record,[string]$Name){
    $stage=[pscustomobject]@{name=$Name;startedUtc=[DateTime]::UtcNow.ToString('o');completedUtc=$null;exitCode=$null}
    $Record.stages=@($Record.stages)+@($stage);Write-RunRecord $Record;return $stage
}
function Complete-RunStage($Record,$Stage,[int]$Code){$Stage.completedUtc=[DateTime]::UtcNow.ToString('o');$Stage.exitCode=$Code;Write-RunRecord $Record}

try {
    if($Command -in 'start','invoke'){
        $automaticTask=$false
        if($Command -eq 'invoke' -and -not $Task){
            $Task='entry-'+[Guid]::NewGuid().ToString('N');$automaticTask=$true
            $null=& (Join-Path $PSScriptRoot 'build_manager.ps1') task-open $Task -TaskName ('Standard '+$Action+' '+$Mode) -Adapter cli
            if($LASTEXITCODE -ne 0){throw 'Could not register the standard entry task.'}
        }
        if(-not $Task){throw 'run start requires -Task from task-open.'}
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
            completedUtc=$null;exitCode=$null;error='';waitingFor='';stages=@();inputStart=$null;inputEnd=$null;logPath=(Run-Path $id '.stdout.log')}
        # Identity is fresh. Request publication is create-only, never a mutable plan.
        $stream=[IO.File]::Open((Run-Path $id '.request.json'),'CreateNew','Write','Read')
        try {$bytes=[Text.Encoding]::UTF8.GetBytes(($request|ConvertTo-Json -Depth 12));$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        Write-RunRecord $record
        $launched=$false
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
        if($Command -eq 'start'){
            [pscustomobject]@{id=$id;task=$Task;requestPath=(Run-Path $id '.request.json');status='submitted';next='run status/wait; only explicit run cancel cancels execution'}|ConvertTo-Json -Depth 8
            exit 0
        }
        $RunId=$id
        [Console]::Error.WriteLine('Persistent Run '+$RunId+'; task '+$Task+'. Closing this observer preserves execution; use run cancel explicitly.')
    }
    if(-not $RunId){throw 'This command requires -RunId.'}
    $request=Read-RunRequest $RunId
    if($Command -eq 'cancel'){
        if(-not $Task -or $request.task -ne $Task){throw 'Explicit cancellation requires the Run owning task ID.'}
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
            $urgent=@($state.taskLedger.events | Where-Object {$_.importance -eq 'urgent' -and $_.recipients -contains $request.task -and @($_.receipts | Where-Object {$_.task -eq $request.task -and $_.level -eq 'agent'}).Count -eq 0})
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
    $ownerLease=$null;$entry=$null;$record=$null;$ownsRun=$false
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
        $record.status='queued';Write-RunRecord $record;$queue=Start-RunStage $record 'queue'
        $queueNotice=[Diagnostics.Stopwatch]::StartNew()
        while(-not $entry){
            if(Run-CancellationRequested $request){$record.status='cancelled';$record.exitCode=4;Complete-RunStage $record $queue 4;break}
            try {$entry=Enter-BuildEntry $root}
            catch {
                if($_.Exception.Message -notmatch '^Shared output (busy:|lease/transaction is busy;)'){throw}
                if($record.waitingFor -ne $_.Exception.Message){$record.waitingFor=$_.Exception.Message;Write-RunRecord $record}
                if($queueNotice.Elapsed.TotalSeconds -ge 30){[Console]::Error.WriteLine('Run '+$RunId+' is queued: '+$record.waitingFor);$queueNotice.Restart()}
                Start-Sleep -Milliseconds 200
            }
        }
        if($entry){
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
                & (Join-Path $PSScriptRoot 'build_entry.ps1') @options
                $record.exitCode=$global:LASTEXITCODE
            } finally {[Environment]::SetEnvironmentVariable('SNOWDESKTOP_RUN_ID',$previousRun,'Process')}
            Complete-RunStage $record $stage $record.exitCode
            $record.inputEnd=Get-BuildInputIdentity $root
            $cancelled=('SnowDesktop.Build.Job' -as [type]) -and [SnowDesktop.Build.Job]::RunWasCancelled
            $record.status=if($cancelled){'cancelled'}elseif($record.exitCode -eq 0){'passed'}else{'failed'}
            if($record.status -eq 'cancelled'){$record.exitCode=4}
            elseif($record.inputStart.digest -ne $record.inputEnd.digest){$record.status='invalidated';$record.exitCode=5;$record.error='Build inputs changed while this Run was executing.'}
        }
    } catch {
        if(-not $ownsRun){throw}
        $record.status='failed';$record.exitCode=2;$record.error=$_.Exception.ToString()
    } finally {
        # Terminal means that the leased output and automatic task transaction
        # have both finished. Observers may submit their next entry immediately.
        Exit-BuildEntry $entry
        if($ownsRun -and $request.PSObject.Properties['automaticTask'] -and $request.automaticTask){
            try {
                $state=Read-EntryJson (Join-Path $root '.build/collaboration/state.json')
                $taskRecord=@($state.taskLedger.tasks|Where-Object id -eq $request.task)[0]
                $null=& (Join-Path $PSScriptRoot 'build_manager.ps1') task-close $request.task -TaskRevision $taskRecord.revision -TaskPhase $(if($record.status -eq 'cancelled'){'cancelled'}else{'completed'})
            } catch {[Console]::Error.WriteLine('Run is terminal; automatic task closure needs attention: '+$_.Exception.Message)}
        }
        if($ownsRun){$record.completedUtc=[DateTime]::UtcNow.ToString('o');Write-RunRecord $record}
        if($ownerLease){$ownerLease.Dispose()}
    }
    $record|ConvertTo-Json -Depth 30;exit ([int]$record.exitCode)
} catch {[Console]::Error.WriteLine($_.Exception.Message);exit 2}
