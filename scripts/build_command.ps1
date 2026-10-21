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
if($Operation -eq 'configure'){
    if($Preset){$arguments=@('--preset',$Preset)}
    else{$arguments=@('-B',$Directory,'-S',$Source,'-A',$Architecture,('-DSNOWDESKTOP_OUTPUT_DIR='+$Output))}
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
exit $code
