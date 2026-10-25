[CmdletBinding()]
param([Alias('ReloadShell')][switch]$ExecuteReloadShell, [Alias('CloseApplication')][switch]$ExecuteCloseApplication,
    [ValidateSet('Release','Debug')][string]$Configuration='Release')
if(-not (Get-Command Get-BuildOutputDirectories -ErrorAction SilentlyContinue)){. (Join-Path $PSScriptRoot 'build_entry.ps1')}
function Get-PreflightProcessPath($Process) {
    try {if($Process.Path){return $Process.Path}} catch {}
    # MainModule/Process.Path can be unavailable while a native helper is
    # starting. Query the kernel image with limited rights, and bind the handle
    # to the observed creation time so a recycled PID cannot become an owner.
    try {
        if(-not ('SnowDesktop.Preflight.ProcessImage' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace SnowDesktop.Preflight {
 public static class ProcessImage {
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint access,bool inherit,uint id);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder path,ref uint size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetProcessTimes(IntPtr process,out long creation,out long exit,out long kernel,out long user);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  public static string Read(uint id,long expectedStartTicks) {
   IntPtr process=OpenProcess(0x1000,false,id);
   if(process==IntPtr.Zero)return null;
   try {
    long creation,exit,kernel,user;
    if(!GetProcessTimes(process,out creation,out exit,out kernel,out user) ||
       DateTime.FromFileTimeUtc(creation).Ticks!=expectedStartTicks)return null;
    uint size=32768;var path=new StringBuilder((int)size);
    return QueryFullProcessImageName(process,0,path,ref size)?path.ToString():null;
   } finally {CloseHandle(process);}
  }
 }
}
'@
        }
        return [SnowDesktop.Preflight.ProcessImage]::Read(
            [uint32]$Process.Id,$Process.StartTime.ToUniversalTime().Ticks)
    } catch {return $null}
}
function Get-ReadOnlyPreflight([string]$Root,[string]$Configuration='Release') {
    $releases=@(Get-BuildOutputDirectories $Root $Configuration)
    $owners=@();$unknown=@()
    foreach($process in @(Get-Process -Name SnowDesktop,snowwidget,SnowDesktopWorkshopManager,SnowDesktopSteamBridge,SnowDesktopLauncher,SnowDesktopWallpaperInjector32 -ErrorAction SilentlyContinue)){
        try { $path=Get-PreflightProcessPath $process; if(-not $path){$unknown += $process.Id}elseif(@($releases|Where-Object {$path.StartsWith($_+'\',[StringComparison]::OrdinalIgnoreCase)}).Count){$owners += [pscustomobject]@{pid=$process.Id;name=$process.ProcessName;kind='application';startTicks=$process.StartTime.ToUniversalTime().Ticks.ToString()}} } catch{$unknown += $process.Id}
    }
    $hooks=@($releases|ForEach-Object {(Join-Path $_ 'SnowDesktopTaskbarHook.dll');(Join-Path $_ 'SnowDesktop.Runtime\SnowDesktopTaskbarHook.dll')})
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
    $state=Get-ReadOnlyPreflight $root $Configuration
    if($ExecuteReloadShell -and $ExecuteCloseApplication){throw 'Choose only one output-owner action.'}
    if(-not $ExecuteReloadShell -and -not $ExecuteCloseApplication){$state | ConvertTo-Json -Depth 6;exit $(if($state.status -eq 'clear'){0}else{3})}
    . (Join-Path $PSScriptRoot 'build_entry.ps1')
    Assert-EntryExecutionOwner $root
    if($state.status -eq 'unknown'){throw 'Output owner cannot be identified; explicit reload does not authorize terminating unknown processes.'}
    if($ExecuteCloseApplication -and @($state.owners | Where-Object kind -eq 'hook').Count){throw 'Explorer hook is occupied; close-application never terminates or restarts Explorer.'}
    foreach($owner in $state.owners | Where-Object kind -eq 'application'){
        $process=Get-Process -Id $owner.pid -ErrorAction SilentlyContinue
        if($process){[void]$process.CloseMainWindow()}
    }
    $deadline=[DateTime]::UtcNow.AddSeconds(5)
    while([DateTime]::UtcNow -lt $deadline -and @((Get-ReadOnlyPreflight $root $Configuration).owners | Where-Object kind -eq 'application').Count){Start-Sleep -Milliseconds 100}
    $remaining=Get-ReadOnlyPreflight $root $Configuration
    if($remaining.status -eq 'unknown'){throw 'Owner identity became unknown; reload stopped.'}
    if($ExecuteCloseApplication -and @($remaining.owners | Where-Object kind -eq 'hook').Count){throw 'Explorer hook became occupied; close-application stopped.'}
    foreach($owner in @($remaining.owners | Where-Object { $_.kind -eq 'application' -or $ExecuteReloadShell })){
        $process=Get-Process -Id $owner.pid -ErrorAction SilentlyContinue
        if($process){
            if($process.StartTime.ToUniversalTime().Ticks.ToString() -ne $owner.startTicks){throw 'PID identity changed; process termination refused.'}
            Stop-Process -InputObject $process -ErrorAction Stop
        }
    }
    if($ExecuteReloadShell -and @($remaining.owners | Where-Object kind -eq 'hook').Count){
        # The restored user Shell must neither enter our private Job nor grant
        # its subsequently launched terminals delegated build authority.
        $oldToken=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')
        try {
            [Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$null,'Process')
            Start-Process -FilePath (Join-Path $env:WINDIR 'explorer.exe') -WindowStyle Hidden
        } finally {[Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$oldToken,'Process')}
    }
}
