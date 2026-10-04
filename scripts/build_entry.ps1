[CmdletBinding()]
param([Alias('Action')][ValidateSet('release','debug','ide','tests','verify')][string]$EntryAction='verify', [Alias('ReloadShell')][switch]$EntryReloadShell,
    [Alias('CloseApplication')][switch]$EntryCloseApplication,
    [Alias('Configuration')][ValidateSet('Release','Debug')][string]$EntryConfiguration='Release', [Alias('Targets')][string[]]$EntryTargets=@(),
    [Alias('Mode')][ValidateSet('full','fast','core','label','name','list','plan')][string]$EntryMode='full', [Alias('Filter')][string]$EntryFilter='',
    [Alias('PlanBatch')][ValidatePattern('^[a-f0-9]{32}$')][string]$EntryPlanBatch)

# A credential is useful only in the live lease owner's process tree. Merely
# setting an environment variable never grants execution or clears a batch.
function Get-EntryHash([string]$Value) {
    $sha=[Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Value))).Replace('-','').ToLowerInvariant() } finally {$sha.Dispose()}
}
function Read-EntryJson([string]$Path) {
    if(-not [IO.File]::Exists($Path)){return $null}
    $stream=[IO.File]::Open($Path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=New-Object IO.StreamReader($stream,[Text.Encoding]::UTF8)
    try {return $reader.ReadToEnd() | ConvertFrom-Json} finally {$reader.Dispose()}
}
function Test-EntryAncestor($Owner) {
    if(-not ('SnowDesktop.Entry.ProcessTree' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
namespace SnowDesktop.Entry {
 public static class ProcessTree {
  [StructLayout(LayoutKind.Sequential)] struct Basic { public IntPtr a,b,c,d,pid,parent; }
  [DllImport("ntdll.dll")] static extern int NtQueryInformationProcess(IntPtr h,int kind,ref Basic b,int size,out int length);
  public static bool Contains(int owner,long ticks) {
   int current=Process.GetCurrentProcess().Id;
   for(int i=0;i<64 && current>0;i++) {
    using(Process p=Process.GetProcessById(current)) {
     if(current==owner)return p.StartTime.ToUniversalTime().Ticks==ticks;
     Basic b=new Basic();int length;
     if(NtQueryInformationProcess(p.Handle,0,ref b,Marshal.SizeOf(typeof(Basic)),out length)!=0)return false;
     current=b.parent.ToInt32();
    }
   }
   return false;
  }
 }
}
'@
    }
    try {return [SnowDesktop.Entry.ProcessTree]::Contains([int]$Owner.pid,[long]$Owner.startTicks)} catch {return $false}
}
function Test-EntryDelegation([string]$Root) {
    $token=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')
    if(-not $token){return $false}
    $directory=Join-Path $Root '.build/collaboration'
    $record=Read-EntryJson (Join-Path $directory 'execution.json')
    if(-not $record -or $record.repositoryRoot -ne $Root -or $record.tokenHash -ne (Get-EntryHash $token) -or -not (Test-EntryAncestor $record.owner)){return $false}
    $probe=$null
    try {$probe=[IO.File]::Open((Join-Path $directory 'build.lock'),'Open','ReadWrite','None');return $false}
    catch [IO.IOException] {if(($_.Exception.HResult -band 0xffff) -notin 32,33){throw}}
    finally {if($probe){$probe.Dispose()}}
    $state=Read-EntryJson (Join-Path $directory 'state.json')
    if($record.batchId) {
        return $state -and $state.current -and $state.current.id -eq $record.batchId -and $state.current.phase -eq 'building' -and
            $state.current.owner.pid -eq $record.owner.pid -and $state.current.owner.startTicks -eq $record.owner.startTicks
    }
    return -not $state -or -not $state.current
}
function Start-EntryCredential([string]$Root,[string]$BatchId='') {
    $directory=Join-Path $Root '.build/collaboration'
    $token=[Guid]::NewGuid().ToString('N')+[Guid]::NewGuid().ToString('N')
    $record=[pscustomobject]@{schemaVersion=1;repositoryRoot=$Root;batchId=$BatchId;tokenHash=(Get-EntryHash $token);
        owner=[pscustomobject]@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()};startedUtc=[DateTime]::UtcNow.ToString('o')}
    $path=Join-Path $directory 'execution.json';$temporary=$path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($record|ConvertTo-Json -Depth 6),(New-Object Text.UTF8Encoding($false)))
        if([IO.File]::Exists($path)){[IO.File]::Replace($temporary,$path,$temporary+'.bak')}else{[IO.File]::Move($temporary,$path)}
    } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)};if([IO.File]::Exists($temporary+'.bak')){[IO.File]::Delete($temporary+'.bak')}}
    $old=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')
    [Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$token,'Process')
    return $old
}
function Enter-BuildEntry([string]$Root) {
    if(Test-EntryDelegation $Root){return $null}
    if([Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')){throw 'Invalid/stale execution credential; no shared output or process action was started.'}
    $directory=Join-Path $Root '.build/collaboration';[void][IO.Directory]::CreateDirectory($directory)
    $metadata=$null;$lease=$null
    try {
        $metadata=[IO.File]::Open((Join-Path $directory 'state.lock'),'OpenOrCreate','ReadWrite','None')
        $state=Read-EntryJson (Join-Path $directory 'state.json')
        if($state -and ($state.schemaVersion -ne 1 -or $state.repositoryRoot -ne $Root)){throw ('Invalid collaboration state/root (recorded '+$state.repositoryRoot+'; entry '+$Root+'); preserve it for diagnosis.')}
        if($state -and $state.current){throw ('Shared output busy: batch '+$state.current.id+' / '+$state.current.phase+' / '+(($state.current.participants|ForEach-Object {$_.id+':'+$_.state}) -join ', ')+'. Read scripts/build.bat status; join begin/plan/ready-and-wait or wait for retirement.')}
        $lease=[IO.File]::Open((Join-Path $directory 'build.lock'),'OpenOrCreate','ReadWrite','None')
        $old=Start-EntryCredential $Root
        return [pscustomobject]@{lease=$lease;oldToken=$old}
    } catch {
        if($lease){$lease.Dispose()}
        if($_.Exception -is [IO.IOException]){throw 'Shared output lease/transaction is busy; no configure, build, arrangement or process action started. Read scripts/build.bat status and retry after the owner finishes.'}
        throw
    } finally {if($metadata){$metadata.Dispose()}}
}
function Exit-BuildEntry($Entry) {
    if($Entry){[Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$Entry.oldToken,'Process');$Entry.lease.Dispose()}
}

if($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference='Stop';$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'));$entry=$null
    try {
        if($EntryAction -eq 'verify') {
            if(-not (Test-EntryDelegation $root)){throw 'Shared CMake/IDE output requires a live leased entry. Use scripts/build.bat, scripts/test.bat, or scripts/build_entry.ps1 -Action ide; independent output remains available for diagnostics.'}
            exit 0
        }
        $entry=Enter-BuildEntry $root
        Push-Location $root
        try {
            # All leased subprocesses belong to this entry's private Job. An
            # interrupted entry cannot release the lease while its CMake child
            # keeps writing. No user process is attached to this Job.
            Add-Type -Path (Join-Path $PSScriptRoot 'build_job.cs')
            $log=Join-Path $root ('.build/collaboration/entry-'+[Guid]::NewGuid().ToString('N')+'.log')
            if($EntryAction -eq 'ide') {
                $preset=if($EntryConfiguration -eq 'Debug'){'debug'}else{'release'}
                foreach($target in $EntryTargets){if($target -notmatch '^[A-Za-z0-9_.-]+$'){throw 'IDE targets must be literal target names.'}}
                $pipeline='cmake --preset '+$preset+' && cmake --build --preset '+$preset
                if($EntryTargets.Count){$pipeline+=' --target '+($EntryTargets -join ' ')}
            } elseif($EntryAction -eq 'tests') {
                $testScript=Join-Path $PSScriptRoot 'test_manager.ps1'
                $invoke="& '"+$testScript.Replace("'","''")+"' -Mode '"+$EntryMode+"' -Filter '"+$EntryFilter.Replace("'","''")+"'"
                if($EntryPlanBatch){$invoke+=" -PlanBatch '"+$EntryPlanBatch+"'"}
                $invoke='try { '+$invoke+'; if($null -ne $LASTEXITCODE){exit $LASTEXITCODE}else{exit 0} } catch {[Console]::Error.WriteLine($_.ScriptStackTrace);throw}'
                $invokePath=[IO.Path]::ChangeExtension($log,'.ps1')
                [IO.File]::WriteAllText($invokePath,"$"+"ErrorActionPreference='Stop'`n"+$invoke,(New-Object Text.UTF8Encoding($true)))
                $pipeline='"'+(Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe')+'" -NoProfile -ExecutionPolicy Bypass -File "'+$invokePath+'"'
            } else {
                $scriptName=if($EntryAction -eq 'debug'){'build_debug.bat'}else{'build.bat'}
                $pipeline='call scripts\'+$scriptName
                if($EntryReloadShell){$pipeline+=' --reload-shell'}
                if($EntryCloseApplication){
                    if($EntryAction -ne 'release' -or $EntryReloadShell){throw 'CloseApplication requires a Release build without ReloadShell.'}
                    $pipeline+=' --close-application'
                }
            }
            [Console]::Error.WriteLine('Shared entry log: '+$log)
            $code=[SnowDesktop.Build.Job]::RunLeasedCommand($root,$log,$pipeline)
            if([IO.File]::Exists($log)){Get-Content -LiteralPath $log}
            exit $code
        } finally {Pop-Location}
    } catch {[Console]::Error.WriteLine($_.Exception.Message);exit 2}
    finally {Exit-BuildEntry $entry}
}
