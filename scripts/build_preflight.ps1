[CmdletBinding()]
param([Alias('ReloadShell')][switch]$ExecuteReloadShell, [Alias('CloseApplication')][switch]$ExecuteCloseApplication)
function Get-ReadOnlyPreflight([string]$Root) {
    $release=[IO.Path]::GetFullPath((Join-Path $Root '.build\Release'))
    $owners=@();$unknown=@()
    foreach($process in @(Get-Process -Name SnowDesktop,snowwidget,SnowDesktopWorkshopManager,SnowDesktopSteamBridge,SnowDesktopLauncher,SnowDesktopWallpaperInjector32 -ErrorAction SilentlyContinue)){
        try { $path=$process.Path; if(-not $path){$unknown += $process.Id}elseif($path.StartsWith($release+'\',[StringComparison]::OrdinalIgnoreCase)){$owners += [pscustomobject]@{pid=$process.Id;name=$process.ProcessName;kind='application';startTicks=$process.StartTime.ToUniversalTime().Ticks.ToString()}} } catch{$unknown += $process.Id}
    }
    $hooks=@((Join-Path $release 'SnowDesktopTaskbarHook.dll'),(Join-Path $release 'SnowDesktop.Runtime\SnowDesktopTaskbarHook.dll'))
    if(@($hooks | Where-Object {[IO.File]::Exists($_)}).Count){
        foreach($shell in @(Get-Process -Name explorer -ErrorAction SilentlyContinue)){
            try {if(@($shell.Modules | Where-Object {$hooks -contains $_.FileName}).Count){$owners += [pscustomobject]@{pid=$shell.Id;name='explorer';kind='hook';startTicks=$shell.StartTime.ToUniversalTime().Ticks.ToString()}}}catch{$unknown += $shell.Id}
        }
    }
    return [pscustomobject]@{status=$(if($unknown.Count){'unknown'}elseif($owners.Count){'blocked'}else{'clear'});owners=$owners;unknownPids=$unknown;observedUtc=[DateTime]::UtcNow.ToString('o');note='Read-only observation; no process has been stopped.'}
}
# Dot sourcing defines the read-only observer only. The destructive path is
# available solely through an explicit execution-owner authorization flag.
if($MyInvocation.InvocationName -ne '.'){
    $ErrorActionPreference='Stop'
    $root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $state=Get-ReadOnlyPreflight $root
    if($ExecuteReloadShell -and $ExecuteCloseApplication){throw 'Choose only one output-owner action.'}
    if(-not $ExecuteReloadShell -and -not $ExecuteCloseApplication){$state | ConvertTo-Json -Depth 6;exit $(if($state.status -eq 'clear'){0}else{3})}
    if($state.status -eq 'unknown'){throw 'Output owner cannot be identified; explicit reload does not authorize terminating unknown processes.'}
    if($ExecuteCloseApplication -and @($state.owners | Where-Object kind -eq 'hook').Count){throw 'Explorer hook is occupied; close-application never terminates or restarts Explorer.'}
    foreach($owner in $state.owners | Where-Object kind -eq 'application'){
        $process=Get-Process -Id $owner.pid -ErrorAction SilentlyContinue
        if($process){[void]$process.CloseMainWindow()}
    }
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    while([DateTime]::UtcNow -lt $deadline -and @((Get-ReadOnlyPreflight $root).owners | Where-Object kind -eq 'application').Count){Start-Sleep -Milliseconds 100}
    $remaining=Get-ReadOnlyPreflight $root
    if($remaining.status -eq 'unknown'){throw 'Owner identity became unknown; reload stopped.'}
    if($ExecuteCloseApplication -and @($remaining.owners | Where-Object kind -eq 'hook').Count){throw 'Explorer hook became occupied; close-application stopped.'}
    foreach($owner in @($remaining.owners | Where-Object { $_.kind -eq 'application' -or $ExecuteReloadShell })){
        $process=Get-Process -Id $owner.pid -ErrorAction SilentlyContinue
        if($process){
            if($process.StartTime.ToUniversalTime().Ticks.ToString() -ne $owner.startTicks){throw 'PID identity changed; process termination refused.'}
            Stop-Process -InputObject $process -ErrorAction Stop
        }
    }
    if($ExecuteReloadShell -and @($remaining.owners | Where-Object kind -eq 'hook').Count){Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -WindowStyle Hidden}
}
