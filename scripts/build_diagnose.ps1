[CmdletBinding()]
param([ValidateSet('diagnose')][string]$Verb='diagnose',[switch]$Help)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if($Help){
    'Usage: scripts/build.bat diagnose [-Help]'
    'Checks coordination-directory writes and the standard persistent child launcher in THIS process context. No configure/build, ACL changes, task changes, or automatic replay.'
    exit 0
}
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$stateRoot=Join-Path $root '.build/collaboration'
$report=[ordered]@{schemaVersion=1;repositoryRoot=$root;contextPid=$PID;writeProbe=$null;persistentLaunch=$null;next=@()}
$probe=$null;$child=$null
try {
    # Unique generated paths only; never open/alter the live state or its lock.
    [void][IO.Directory]::CreateDirectory($stateRoot)
    $probe=Join-Path $stateRoot ('diagnose-'+[Guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($probe)
    [IO.File]::WriteAllText((Join-Path $probe 'write.txt'),'probe')
    [IO.File]::Delete((Join-Path $probe 'write.txt'))
    $report.writeProbe=[pscustomobject]@{status='passed';path=$stateRoot;observation='Only this caller context was checked; Host permission does not establish confined tool permission.'}
} catch {
    $report.writeProbe=[pscustomobject]@{status='denied';path=$stateRoot;error=$_.Exception.Message}
    $report.next=@('Check the caller sandbox writable roots and effective permissions. Use an authorized full-access context or external terminal. Do not infer an ACL defect or automatically change ACLs from this denial.')
}
if($report.writeProbe.status -eq 'passed'){
    try {
        . (Join-Path $PSScriptRoot 'build_entry.ps1')
        $target=Join-Path $probe 'child.ps1';$marker=Join-Path $probe 'completed.txt'
        [IO.File]::WriteAllText($target,"[IO.File]::WriteAllText((Join-Path `$PSScriptRoot 'completed.txt'),'completed'); exit 0")
        $child=Start-BuildPowerShellScript -Script $target -WorkingDirectory $root -Breakaway -OutputPath (Join-Path $probe 'stdout.log') -ErrorPath (Join-Path $probe 'stderr.log')
        if(-not $child.WaitForExit(10000)){throw 'Diagnostic child exceeded its 10-second completion budget.'}
        if($child.ExitCode -ne 0 -or -not [IO.File]::Exists($marker)){throw 'Diagnostic child did not complete its marker.'}
        $report.persistentLaunch=[pscustomobject]@{status='passed';pid=$child.Id;evidenceDirectory=$probe}
        $report.next=@('The write and persistent-launch probes passed in this context. Continue with the existing coordinator batch/lease; this is not a build or activation result.')
    } catch {
        $report.persistentLaunch=[pscustomobject]@{status='failed';error=$_.Exception.Message;evidenceDirectory=$probe}
        $report.next=@('A Job breakaway refusal requires a launch context that permits persistent workers. Use the standard ready entry from a capable adapter Host or an external terminal; diagnose that context first. No fallback build was started.','For start-failed: preserve the original request/result, begin again, claim reviewed paths with -AdoptExistingChanges, plan, then ready-and-wait using the NEW batch/revision. See scripts/README.md#构建环境诊断与启动恢复.')
    } finally {
        if($child){try{if(-not $child.HasExited){$child.Kill();$child.WaitForExit()}}finally{$child.Dispose()}}
    }
}else{$report.persistentLaunch=[pscustomobject]@{status='not-run';reason='Coordination directory is not writable.'}}
$report|ConvertTo-Json -Depth 8
if($report.writeProbe.status -ne 'passed' -or $report.persistentLaunch.status -ne 'passed'){exit 2}
exit 0
