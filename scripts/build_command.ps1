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
