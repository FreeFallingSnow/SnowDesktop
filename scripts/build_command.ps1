[CmdletBinding()]
param([ValidateSet('configure','build','arrange')][string]$Operation,
    [string]$Preset='', [string]$Directory='', [string]$Source='', [string]$Architecture='',
    [ValidateSet('Release','Debug')][string]$Configuration='Release', [string]$Targets='', [string]$Output='')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_telemetry.ps1')
if(-not (Test-EntryDelegation $root)){throw 'Observed build commands require the standard leased entry.'}
$file='cmake';$arguments=@();$name=$Operation
$reset=$null;$resetPath=$null;$configureKey=$null
if($Operation -eq 'configure'){
    if($Preset){$arguments=@('--preset',$Preset)}
    else{$arguments=@('-B',$Directory,'-S',$Source,'-A',$Architecture,('-DSNOWDESKTOP_OUTPUT_DIR='+$Output))}
    # A product+test Run must not turn BUILD_TESTING off and back on between
    # its two builds: regenerating the native project invalidates WinUI inputs.
    # Only the live Run's hash-bound plan can opt the Release configure in.
    $runId=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
    if($Preset -eq 'release' -and $runId){
        $runRoot=Get-EntryRunRoot $root
        $request=Read-EntryJson (Join-Path $runRoot ('.build/collaboration/runs/'+$runId+'.request.json'))
        if($request -and $request.action -eq 'batch'){
            if($request.batch -notmatch '^[a-f0-9]{32}$'){throw 'Invalid batch configure identity.'}
            $planPath=Join-Path $runRoot ('.build/collaboration/'+$request.batch+'.plan.json')
            if((Get-FileHash -LiteralPath $planPath -Algorithm SHA256).Hash -ne $request.planHash){throw 'Frozen configure plan changed.'}
            $plan=Read-EntryJson $planPath
            if($plan.schemaVersion -ne 1 -or $plan.batchId -ne $request.batch -or $plan.configuration -ne 'Release' -or
                $plan.mode -notin 'selected','full','skipped'){throw 'Invalid batch configure plan.'}
            if($plan.mode -ne 'skipped'){$arguments+=@('-DBUILD_TESTING:BOOL=ON')}
        }
    }
    $context=Get-CandidateEntryContext $root
    if($context){
        . (Join-Path $PSScriptRoot 'build_candidate.ps1')
        $resetPath=Resolve-CandidatePath $context.output 'collaboration/configuration-reset.json'
        if([IO.File]::Exists($resetPath)){
            $reset=Read-CandidateJson $resetPath
            $material=Read-CandidateJson (Join-Path ([IO.Path]::GetDirectoryName($context.source)) 'materialization.json')
            if($reset.schemaVersion -ne 1 -or $reset.compilerContextDigest -ne $material.compilerContextDigest){throw 'Compiler configuration reset identity changed.'}
            $configureKey=if($Preset){'preset-output'}else{[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($Directory)){$Directory}else{Join-Path $root $Directory}))}
            if(@($reset.configured) -notcontains $configureKey){$arguments=@('--fresh')+$arguments}else{$reset=$null}
        }
    }
}elseif($Operation -eq 'build'){
    $name='build-targets'
    $arguments=if($Preset){@('--build','--preset',$Preset)}else{@('--build',$Directory,'--config',$Configuration)}
    if($Targets){$items=@($Targets.Split(','));foreach($item in $items){if($item -notmatch '^[A-Za-z0-9_.-]+$'){throw 'Build targets must be literal names.'}};$arguments+=@('--target')+$items}
}else{
    $file=Get-BuildPowerShell
    $arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'arrange_build_output.ps1'),'-BuildOutput',$Output)
}
$code=2
Invoke-BuildStageCommand $root $name $file $arguments ([ref]$code)
if($code -eq 0 -and $reset){
    $reset.configured=@($reset.configured)+@($configureKey)
    Write-CandidateJson $reset $resetPath
}
exit $code
