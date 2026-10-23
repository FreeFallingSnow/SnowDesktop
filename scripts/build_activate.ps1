[CmdletBinding()]
param([ValidatePattern('^[a-f0-9]{32}$')][string]$ArtifactRun,
    [string]$RepositoryRoot='')

# Activation only arranges a saved runtime. It does not compile live source,
# join the editing barrier, or wait for test acceptance.
function Initialize-BuildActivationControl {
    if('SnowDesktop.Build.ActivationControl' -as [type]){return}
    Add-Type @'
using System;
using System.Runtime.InteropServices;
namespace SnowDesktop.Build {
 public static class ActivationControl {
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent,IntPtr after,string cls,string title);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
  [DllImport("user32.dll",SetLastError=true)] public static extern bool PostMessage(IntPtr window,uint message,IntPtr w,IntPtr l);
 }
}
'@
}
function Start-ActivatedApplication([string]$Executable,[string]$Arguments) {
    # The desktop application must outlive the build Job and must not inherit
    # its execution credential or cancellation identity.
    $saved=@{}
    foreach($name in @('SNOWDESKTOP_EXECUTION_TOKEN','SNOWDESKTOP_RUN_ID')){
        $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
        [Environment]::SetEnvironmentVariable($name,$null,'Process')
    }
    try{
        $options=@{FilePath=$Executable;WorkingDirectory=[IO.Path]::GetDirectoryName($Executable);WindowStyle='Hidden';PassThru=$true}
        if($Arguments){$options.ArgumentList=$Arguments}
        return Start-Process @options
    }finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}
}
function Invoke-BuiltRuntimeActivation([string]$Root,$Artifact,[scriptblock]$CheckCancellation={}) {
    if(-not $Artifact.manifest.productRuntime -or $Artifact.manifest.configuration -ne 'Release'){
        return [pscustomobject]@{status='not-product-release';runId=$Artifact.manifest.runId}
    }
    $targetRoot=Resolve-CandidatePath $Root '.build/Release'
    $executable=Resolve-CandidatePath $targetRoot 'SnowDesktop.exe'
    $stateRoot=Resolve-CandidatePath $Root '.build/collaboration'
    $lease=$null
    $result=[pscustomobject]@{schemaVersion=1;runId=$Artifact.manifest.runId;candidate=$Artifact.manifest.candidate;
        artifactDigest=$Artifact.digest;sourceCapturedUtc=$Artifact.manifest.capturedUtc;artifactCreatedUtc=$Artifact.manifest.createdUtc;
        status='pending';path=$executable;oldPid=$null;newPid=$null;sha256=$null;replacedFiles=0;backup=$null;
        dataDirectory=(Join-Path $targetRoot 'data');completedUtc=$null;error=''}
    try{
        $clock=[Diagnostics.Stopwatch]::StartNew()
        while(-not $lease){
            & $CheckCancellation
            $transaction=Open-EntryStateTransaction $Root
            try{
                try{$lease=[IO.File]::Open((Join-Path $stateRoot 'build.lock'),'OpenOrCreate','ReadWrite','None')}
                catch{if(-not (Test-EntrySharingViolation $_.Exception)){throw}}
            }finally{$transaction.Dispose()}
            if(-not $lease){if($clock.Elapsed.TotalSeconds -ge 15){throw 'Runtime output is busy; activation did not change the running application.'};Start-Sleep -Milliseconds 100}
        }
        # This narrower lease permits runtime arrangement while editors work.
        # It never issues a configure/build credential for the live checkout.
        if(-not $lease.CanWrite -or $lease.Name -ne (Join-Path $stateRoot 'build.lock')){throw 'Runtime activation lost its output lease.'}
        $current=Read-EntryJson (Join-Path $stateRoot 'activation-current.json')
        if($current -and (Get-Field $current 'sourceCapturedUtc') -and
            (ConvertTo-UtcTimestamp $current.sourceCapturedUtc) -gt (ConvertTo-UtcTimestamp $result.sourceCapturedUtc)){$result.status='superseded';return $result}
        Initialize-BuildActivationControl
        $window=[IntPtr]::Zero;$process=$null
        while($true){
            $window=[SnowDesktop.Build.ActivationControl]::FindWindowEx([IntPtr]::Zero,$window,'SnowDesktopControlWindow','SnowDesktopControl')
            if($window -eq [IntPtr]::Zero){break}
            [uint32]$ownerId=0
            [void][SnowDesktop.Build.ActivationControl]::GetWindowThreadProcessId($window,[ref]$ownerId)
            $observed=Get-Process -Id $ownerId -ErrorAction SilentlyContinue
            if($observed -and $observed.Path -eq $executable){$process=$observed;break}
        }
        $arguments=''
        if($process){
            $result.oldPid=$ownerId
            $command=(Get-CimInstance Win32_Process -Filter ('ProcessId = '+$ownerId) -ErrorAction Stop).CommandLine
            $launch=[regex]::Match($command,'^\s*(?:"(?<exe>[^"]+)"|(?<exe>\S+))(?<args>.*)$')
            if(-not $launch.Success -or [IO.Path]::GetFullPath($launch.Groups['exe'].Value) -ne $executable){throw 'Cannot preserve the running application arguments.'}
            $arguments=$launch.Groups['args'].Value.Trim()
            # Internal handoff handles/PIDs describe the previous launch only.
            $arguments=([regex]::Replace($arguments,'(^|\s)--(?:wait-for-pid|watch-process-handle)=\d+(?=\s|$)',' ')).Trim()
        }elseif(@(Get-Process SnowDesktop -ErrorAction SilentlyContinue|Where-Object Path -eq $executable).Count){
            throw 'The target application is running without its control window; runtime was retained.'
        }
        $changes=@($Artifact.manifest.files|Where-Object{
            $target=Resolve-CandidatePath $targetRoot $_.path
            -not [IO.File]::Exists($target) -or (Get-FileDigest $target) -ne $_.sha256
        })
        if(-not $changes.Count){$result.status='already-current';$result.newPid=$result.oldPid;$result.sha256=Get-FileDigest $executable;return $result}
        $backup=Resolve-CandidatePath $Root ('.build/activation-backups/'+$result.runId+'-'+[Guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($backup);$result.backup=$backup
        & $CheckCancellation
        if($process){
            if(-not [SnowDesktop.Build.ActivationControl]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)){throw 'Graceful application exit request failed.'}
            if(-not $process.WaitForExit(15000)){throw 'Application did not exit gracefully; runtime was retained.'}
        }
        $replaced=[Collections.Generic.List[object]]::new()
        try{
            foreach($file in $changes){
                if($file.path.Replace('\','/').Split('/')[0] -in 'data','data.debug','.snowdesktop'){throw 'User data cannot be installed as a runtime.'}
                $target=Resolve-CandidatePath $targetRoot $file.path
                $saved=Resolve-CandidatePath $backup $file.path
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($saved))
                $hadOld=[IO.File]::Exists($target)
                # A mapped Hook may remain loaded in Explorer. Retain its old
                # image by rename; no unrelated process is stopped.
                if($hadOld){[IO.File]::Move($target,$saved)}
                $replaced.Add([pscustomobject]@{target=$target;backup=$saved;hadOld=$hadOld})
                [IO.File]::Copy((Resolve-CandidatePath $Artifact.runtime $file.path),$target,$false)
                if((Get-FileDigest $target) -ne $file.sha256){throw 'Installed runtime hash differs from the saved build.'}
            }
            if($process){
                $started=Start-ActivatedApplication $executable $arguments
                if($started.WaitForExit(3000)){throw ('New application exited during startup: '+$started.ExitCode)}
                $result.newPid=$started.Id
            }
            $result.sha256=Get-FileDigest $executable
            $result.replacedFiles=$changes.Count;$result.status=if($process){'activated'}else{'installed'}
        }catch{
            $installError=$_
            for($index=$replaced.Count-1;$index -ge 0;$index--){
                $item=$replaced[$index]
                if([IO.File]::Exists($item.target)){[IO.File]::Move($item.target,$item.backup+'.failed')}
                if($item.hadOld){[IO.File]::Move($item.backup,$item.target)}
            }
            if($process){
                $restored=Start-ActivatedApplication $executable $arguments
                $result.newPid=$restored.Id
            }
            $result.status='restored-after-failure'
            throw $installError
        }
        return $result
    }catch{$result.error=$_.Exception.Message;if($result.status -eq 'pending'){$result.status='failed'};return $result}
    finally{
        $result.completedUtc=[DateTime]::UtcNow.ToString('o')
        if($lease){
            try{
                Write-CandidateJson $result (Join-Path $stateRoot ($result.runId+'.activation.json'))
                if($result.status -in 'activated','installed','already-current'){Write-CandidateJson $result (Join-Path $stateRoot 'activation-current.json')}
            }finally{
                $lease.Dispose()
            }
        }
    }
}
function Invoke-CandidateRuntimeActivation([string]$Root,[string]$Run,[scriptblock]$CheckCancellation={}) {
    $artifact=Read-CandidateArtifact $Root $Run -VerifyFiles
    # Order by source capture, not completion time: a slow older build must
    # never replace a newer build that is already running.
    $snapshot=Read-CandidateManifest $Root $artifact.manifest.candidate
    Set-Field $artifact.manifest 'capturedUtc' $snapshot.manifest.capturedUtc
    return Invoke-BuiltRuntimeActivation $Root $artifact $CheckCancellation
}
if($MyInvocation.InvocationName -eq '.'){return}
$ErrorActionPreference='Stop'
if(-not $ArtifactRun){throw 'Specify the successful product build Run with -ArtifactRun.'}
$activationRoot=if($RepositoryRoot){[IO.Path]::GetFullPath($RepositoryRoot)}else{[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_protocol.ps1')
. (Join-Path $PSScriptRoot 'build_inputs.ps1')
. (Join-Path $PSScriptRoot 'build_candidate.ps1')
. (Join-Path $PSScriptRoot 'build_candidate_artifacts.ps1')
$activation=Invoke-CandidateRuntimeActivation $activationRoot $ArtifactRun
$activation|ConvertTo-Json -Depth 8
if($activation.status -in 'failed','restored-after-failure'){exit 2}
