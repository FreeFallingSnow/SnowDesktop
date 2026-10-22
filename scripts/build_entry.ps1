[CmdletBinding()]
param([Alias('Action')][ValidateSet('release','debug','ide','tests','verify')][string]$EntryAction='verify', [Alias('ReloadShell')][switch]$EntryReloadShell,
    [Alias('CloseApplication')][switch]$EntryCloseApplication,
    [Alias('Configuration')][ValidateSet('Release','Debug')][string]$EntryConfiguration='Release', [Alias('Targets')][string[]]$EntryTargets=@(),
    [Alias('Mode')][ValidateSet('full','fast','core','tools','label','name','list','plan','verify')][string]$EntryMode='list', [Alias('Filter')][string]$EntryFilter='',
    [Alias('PlanBatch')][ValidatePattern('^[a-f0-9]{32}$')][string]$EntryPlanBatch,
    [Alias('Task')][ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$')][string]$EntryTask)

function Get-BuildPowerShell {
    $env:PSExecutionPolicyPreference='Bypass'
    # Pin a real executable, not a command alias. Selection never starts a probe
    # shell and grants no build authority; the live lease is still required.
    $selected=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_ENTRY_POWERSHELL','Process')
    if($selected -and [IO.Path]::IsPathRooted($selected) -and [IO.File]::Exists($selected)){return $selected}
    $candidates=@((Join-Path $PSHOME 'pwsh.exe'))
    foreach($variable in @('ProgramFiles','ProgramW6432')) {
        $folder=[Environment]::GetEnvironmentVariable($variable,'Process')
        if($folder){$candidates += Join-Path $folder 'PowerShell/7/pwsh.exe'}
    }
    foreach($candidate in $candidates) {
        if([IO.File]::Exists($candidate)) {
            $env:SNOWDESKTOP_ENTRY_POWERSHELL=[IO.Path]::GetFullPath($candidate)
            return $env:SNOWDESKTOP_ENTRY_POWERSHELL
        }
    }
    $command=Get-Command pwsh.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $selected=if($command){$command.Source}else{Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell/v1.0/powershell.exe'}
    if([IO.File]::Exists($selected)){$env:SNOWDESKTOP_ENTRY_POWERSHELL=$selected;return $selected}
    throw 'Neither PowerShell 7 nor Windows PowerShell 5.1 is available.'
}

function Start-BuildPowerShellScript {
    param([string]$Script,[string]$Arguments='',[string]$WorkingDirectory,
        [string]$OutputPath,[string]$ErrorPath,[int]$StartupSeconds=6,[switch]$Breakaway)
    # A two-file handshake separates engine initialization from script execution.
    # A child cannot execute the target until this parent grants permission. This
    # makes a startup-only retry safe even if readiness races with the timeout.
    $runtime=Get-BuildPowerShell
    # A background waiter must not inherit its short-lived parent's pipe/lease
    # handles. Redirect directly to files and whitelist only its three streams.
    if(-not ('SnowDesktop.Entry.PowerShellChild' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
namespace SnowDesktop.Entry {
 public static class PowerShellChild {
  [StructLayout(LayoutKind.Sequential)] struct Security {public int size;public IntPtr descriptor;public int inherit;}
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct Startup {
   public int size;public string reserved,desktop,title;public int x,y,width,height,charsX,charsY,fill,flags;
   public short show,reservedSize;public IntPtr reservedData,input,output,error;
  }
  [StructLayout(LayoutKind.Sequential)] struct ExtendedStartup {public Startup startup;public IntPtr attributes;}
  [StructLayout(LayoutKind.Sequential)] struct ProcessInfo {public IntPtr process,thread;public int pid,tid;}
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern IntPtr CreateFile(string path,uint access,uint share,ref Security security,uint creation,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)] static extern bool CreateProcess(string app,StringBuilder command,IntPtr ps,IntPtr ts,bool inherit,uint flags,IntPtr environment,string directory,ref ExtendedStartup startup,out ProcessInfo process);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool InitializeProcThreadAttributeList(IntPtr list,int count,int flags,ref IntPtr size);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool UpdateProcThreadAttribute(IntPtr list,uint flags,IntPtr attribute,IntPtr value,IntPtr size,IntPtr previous,IntPtr returned);
  [DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
  [DllImport("shell32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CommandLineToArgvW(string command,out int count);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
  public static string[] Arguments(string arguments) {
   int count;IntPtr memory=CommandLineToArgvW("SnowDesktop.exe "+arguments,out count);
   if(memory==IntPtr.Zero)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
   try {var result=new string[count-1];for(int i=1;i<count;i++)result[i-1]=Marshal.PtrToStringUni(Marshal.ReadIntPtr(memory,i*IntPtr.Size));return result;}
   finally {LocalFree(memory);}
  }
  public static Process Start(string executable,string arguments,string directory,string output,string error) {
   return Start(executable,arguments,directory,output,error,false);
  }
  public static Process Start(string executable,string arguments,string directory,string output,string error,bool breakaway) {
   return Start(executable,arguments,directory,output,error,breakaway,false);
  }
  public static Process Start(string executable,string arguments,string directory,string output,string error,bool breakaway,bool resetModulePath) {
   // Reject corrupt executables before entering Windows app-compat/error UI.
   // The loader can block here even when CreateProcess requested no window.
   using(var image=new BinaryReader(File.Open(executable,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete))) {
    if(image.BaseStream.Length<64||image.ReadUInt16()!=0x5a4d)throw new InvalidDataException("PowerShell runtime is not a valid Windows PE executable");
    image.BaseStream.Position=0x3c;int offset=image.ReadInt32();
    if(offset<64||offset>image.BaseStream.Length-24)throw new InvalidDataException("PowerShell runtime has an invalid PE header");
    image.BaseStream.Position=offset;
    if(image.ReadUInt32()!=0x4550)throw new InvalidDataException("PowerShell runtime has an invalid PE signature");
   }
   IntPtr input=IntPtr.Zero,stdout=IntPtr.Zero,stderr=IntPtr.Zero,attributes=IntPtr.Zero,handles=IntPtr.Zero,environment=IntPtr.Zero;
   bool initialized=false;ProcessInfo child=new ProcessInfo();
   var security=new Security{size=Marshal.SizeOf(typeof(Security)),inherit=1};
   try {
    input=CreateFile("NUL",0x80000000,3,ref security,3,0x80,IntPtr.Zero);
    stdout=CreateFile(output,0x40000000,3,ref security,2,0x80,IntPtr.Zero);
    stderr=CreateFile(error,0x40000000,3,ref security,2,0x80,IntPtr.Zero);
    if(input==new IntPtr(-1)||stdout==new IntPtr(-1)||stderr==new IntPtr(-1))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    IntPtr size=IntPtr.Zero;InitializeProcThreadAttributeList(IntPtr.Zero,1,0,ref size);
    attributes=Marshal.AllocHGlobal(size);
    if(!InitializeProcThreadAttributeList(attributes,1,0,ref size))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    initialized=true;handles=Marshal.AllocHGlobal(3*IntPtr.Size);
    Marshal.WriteIntPtr(handles,0,input);Marshal.WriteIntPtr(handles,IntPtr.Size,stdout);Marshal.WriteIntPtr(handles,2*IntPtr.Size,stderr);
    if(!UpdateProcThreadAttribute(attributes,0,new IntPtr(0x20002),handles,new IntPtr(3*IntPtr.Size),IntPtr.Zero,IntPtr.Zero))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    var startup=new ExtendedStartup();startup.startup.size=Marshal.SizeOf(typeof(ExtendedStartup));startup.startup.flags=0x100;
    startup.startup.input=input;startup.startup.output=stdout;startup.startup.error=stderr;startup.attributes=attributes;
    uint creation=0x80000|0x8000000;
    if(resetModulePath) {
     // Native CreateProcess does not perform PowerShell 7's WinPS module-path
     // filtering. Let Windows PowerShell construct its own default paths,
     // without changing the launching process or its other environment values.
     var values=new System.Collections.Generic.SortedDictionary<string,string>(StringComparer.OrdinalIgnoreCase);
     foreach(System.Collections.DictionaryEntry entry in Environment.GetEnvironmentVariables())
      if(!String.Equals((string)entry.Key,"PSModulePath",StringComparison.OrdinalIgnoreCase))values[(string)entry.Key]=(string)entry.Value;
     var block=new StringBuilder();foreach(var entry in values)block.Append(entry.Key).Append('=').Append(entry.Value).Append('\0');
     block.Append('\0');environment=Marshal.StringToHGlobalUni(block.ToString());creation|=0x400; // CREATE_UNICODE_ENVIRONMENT
    }
    if(breakaway)creation|=0x01000000; // CREATE_BREAKAWAY_FROM_JOB; refusal is not a persistent launch.
    if(!CreateProcess(executable,new StringBuilder("\""+executable+"\" "+arguments),IntPtr.Zero,IntPtr.Zero,true,creation,environment,directory,ref startup,out child))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    var process=Process.GetProcessById(child.pid);GC.KeepAlive(process.Handle);return process;
   } finally {
    foreach(var handle in new[]{input,stdout,stderr,child.process,child.thread})if(handle!=IntPtr.Zero&&handle!=new IntPtr(-1))CloseHandle(handle);
    if(initialized)DeleteProcThreadAttributeList(attributes);
    if(attributes!=IntPtr.Zero)Marshal.FreeHGlobal(attributes);if(handles!=IntPtr.Zero)Marshal.FreeHGlobal(handles);if(environment!=IntPtr.Zero)Marshal.FreeHGlobal(environment);
   }
  }
 }
}
'@
    }
    $fallback=Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell/v1.0/powershell.exe'
    for($attempt=0;$attempt -lt 2;$attempt++) {
        $prefix=Join-Path ([IO.Path]::GetTempPath()) ('SnowDesktop-powershell-'+[Guid]::NewGuid().ToString('N'))
        $ready=$prefix+'.ready';$permit=$prefix+'.permit'
        $bootstrapPath=$prefix+'.ps1'
        $quote={param($value) "'"+$value.Replace("'","''")+"'"}
        $tokens=[SnowDesktop.Entry.PowerShellChild]::Arguments($Arguments)
        $expression=(@($tokens | ForEach-Object {if($_ -match '^-[A-Za-z][A-Za-z0-9-]*(?::\$(?:true|false))?$'){$_}else{& $quote $_}}) -join ' ')
        $bootstrap="[Console]::OutputEncoding=[Text.UTF8Encoding]::new(`$false); `$ProgressPreference='SilentlyContinue'; [IO.File]::WriteAllText($(& $quote $ready),'ready'); `$startupWait=[Diagnostics.Stopwatch]::StartNew(); while(-not [IO.File]::Exists($(& $quote $permit))){if(`$startupWait.Elapsed.TotalSeconds -gt 15){[IO.File]::Delete($(& $quote $ready)); [IO.File]::Delete($(& $quote $bootstrapPath)); exit 124}; [Threading.Thread]::Sleep(20)}; [IO.File]::Delete($(& $quote $ready)); [IO.File]::Delete($(& $quote $permit)); [IO.File]::Delete($(& $quote $bootstrapPath)); & $(& $quote $Script) $expression; if(-not `$? -and `$null -eq `$LASTEXITCODE){exit 1}; exit `$LASTEXITCODE"
        [IO.File]::WriteAllText($bootstrapPath,$bootstrap,(New-Object Text.UTF8Encoding($true)))
        $out=$OutputPath; $err=$ErrorPath
        if($attempt -gt 0){$out=$OutputPath+'.retry';$err=$ErrorPath+'.retry'}
        $process=$null;$granted=$false
        $timer=[Diagnostics.Stopwatch]::StartNew()
        try {
            $env:SNOWDESKTOP_ENTRY_POWERSHELL=$runtime
            $resetModulePath=$PSVersionTable.PSEdition -eq 'Core' -and [IO.Path]::GetFileName($runtime) -ieq 'powershell.exe'
            $process=[SnowDesktop.Entry.PowerShellChild]::Start($runtime,('-NoProfile -NonInteractive -File "'+$bootstrapPath+'"'),$WorkingDirectory,$out,$err,[bool]$Breakaway,[bool]$resetModulePath)
            $null=$process.Handle
            while(-not $process.HasExited -and -not [IO.File]::Exists($ready) -and $timer.Elapsed.TotalSeconds -lt $StartupSeconds){Start-Sleep -Milliseconds 20}
            if([IO.File]::Exists($ready) -and -not $process.HasExited) {
                [IO.File]::WriteAllText($permit,'execute');$granted=$true
                # The caller owns the returned handle and original business timeout.
                $process | Add-Member NoteProperty StartupOutput $out
                $process | Add-Member NoteProperty StartupError $err
                return $process
            }
            if(-not $process.HasExited){$process.Kill();$process.WaitForExit()}
            $evidence=[pscustomobject]@{runtime=$runtime;pid=$process.Id;attempt=$attempt+1;target=$Script;arguments=$Arguments;elapsedSeconds=$timer.Elapsed.TotalSeconds;exitCode=$process.ExitCode;scriptStarted=$false;bootstrap=$bootstrap;stdout=$out;stderr=$err;utc=[DateTime]::UtcNow.ToString('o')}
            $evidence | ConvertTo-Json | Set-Content -LiteralPath ($OutputPath+'.startup-'+($attempt+1)+'.json') -Encoding UTF8
            [Console]::Error.WriteLine('PowerShell initialization failed before target execution; evidence: '+$OutputPath+'.startup-'+($attempt+1)+'.json')
        } catch {
            if($granted){throw}
            if($process -and -not $process.HasExited){$process.Kill();$process.WaitForExit()}
            [pscustomobject]@{runtime=$runtime;attempt=$attempt+1;target=$Script;scriptStarted=$false;error=$_.Exception.Message;utc=[DateTime]::UtcNow.ToString('o')} |
                ConvertTo-Json | Set-Content -LiteralPath ($OutputPath+'.startup-'+($attempt+1)+'.json') -Encoding UTF8
            [Console]::Error.WriteLine('PowerShell initialization failed before target execution; evidence: '+$OutputPath+'.startup-'+($attempt+1)+'.json')
            $failure=$_.Exception.GetBaseException()
            if($Breakaway -and $failure -is [ComponentModel.Win32Exception] -and $failure.NativeErrorCode -eq 5){
                throw 'Persistent worker launch was refused by the parent Job. No non-persistent fallback or build was started; the request is retained.'
            }
        } finally {
            # Never remove the permit while the granted child is still reading it.
            # The child removes both handshake files before entering the target.
            if(-not $granted){if($process){$process.Dispose()};[IO.File]::Delete($ready);[IO.File]::Delete($permit);[IO.File]::Delete($bootstrapPath)}
        }
        if($attempt -eq 0 -and [IO.File]::Exists($fallback)){$runtime=$fallback}
    }
    throw 'PowerShell initialization failed twice; target script was not executed.'
}

# A credential is useful only in the live lease owner's process tree. Merely
# setting an environment variable never grants execution or clears a batch.
$null=Get-BuildPowerShell

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
function Test-CandidateSourceRoot([string]$Root) {
    $state=Read-EntryJson (Join-Path ([IO.Path]::GetDirectoryName($Root)) 'materialization.json')
    return $state -and $state.source -eq $Root
}
function Get-CandidateEntryContext([string]$Root) {
    if(-not (Test-CandidateSourceRoot $Root)){return $null}
    $credential=Read-EntryJson (Join-Path $Root '.build/collaboration/execution.json')
    if(-not (Test-EntryDelegation $Root) -or -not $credential -or -not $credential.PSObject.Properties['candidate']){throw 'Candidate source requires its live private execution credential.'}
    $context=$credential.candidate
    if($context.schemaVersion -ne 1 -or $context.source -ne $Root -or $context.runId -notmatch '^[a-f0-9]{32}$' -or
        -not [IO.Path]::IsPathRooted($context.runRoot)){throw 'Invalid candidate execution context.'}
    $directory=Join-Path $context.runRoot '.build/collaboration/runs'
    $request=Read-EntryJson (Join-Path $directory ($context.runId+'.request.json'))
    $record=Read-EntryJson (Join-Path $directory ($context.runId+'.json'))
    if(-not $request -or -not $record -or $request.repositoryRoot -ne $context.runRoot -or $request.action -ne 'candidate' -or
        $request.snapshot -ne $context.snapshot -or $request.snapshotManifestDigest -ne $context.manifestDigest -or
        $request.nonce -ne $context.nonce -or $record.nonce -ne $context.nonce -or $record.status -notin 'queued','running' -or
        $record.owner.pid -ne $credential.owner.pid -or $record.owner.startTicks -ne $credential.owner.startTicks){throw 'Candidate execution has no matching live parent Run.'}
    return $context
}
function Get-EntryRunRoot([string]$Root) {
    $candidate=Get-CandidateEntryContext $Root
    if($candidate){return $candidate.runRoot}
    return $Root
}
function Start-EntryCredential([string]$Root,[string]$BatchId='',$Candidate=$null) {
    $directory=Join-Path $Root '.build/collaboration'
    $token=[Guid]::NewGuid().ToString('N')+[Guid]::NewGuid().ToString('N')
    $record=[pscustomobject]@{schemaVersion=1;repositoryRoot=$Root;batchId=$BatchId;tokenHash=(Get-EntryHash $token);
        owner=[pscustomobject]@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()};startedUtc=[DateTime]::UtcNow.ToString('o')}
    if($Candidate){$record|Add-Member NoteProperty candidate $Candidate}
    $path=Join-Path $directory 'execution.json';$temporary=$path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try {
        [IO.File]::WriteAllText($temporary,($record|ConvertTo-Json -Depth 6),(New-Object Text.UTF8Encoding($false)))
        if([IO.File]::Exists($path)){[IO.File]::Replace($temporary,$path,$temporary+'.bak')}else{[IO.File]::Move($temporary,$path)}
    } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)};if([IO.File]::Exists($temporary+'.bak')){[IO.File]::Delete($temporary+'.bak')}}
    $old=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')
    [Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$token,'Process')
    return $old
}
function Test-EntrySharingViolation([Exception]$Exception) {
    # Static .NET calls may wrap IOException in MethodInvocationException.
    # Retry only Windows sharing/lock violations, never other I/O failures.
    while($Exception){
        if($Exception -is [IO.IOException] -and ($Exception.HResult -band 0xffff) -in 32,33){return $true}
        $Exception=$Exception.InnerException
    }
    return $false
}
function Open-EntryStateTransaction([string]$Root,[ValidateRange(0,30000)][int]$TimeoutMilliseconds=30000) {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while($true){
        try {return [IO.File]::Open((Join-Path $Root '.build/collaboration/state.lock'),'OpenOrCreate','ReadWrite','None')}
        catch {
            if(-not (Test-EntrySharingViolation $_.Exception)){throw}
            if($timer.ElapsedMilliseconds -ge $TimeoutMilliseconds){throw 'Shared output lease/transaction is busy; state reservation timed out; no Run was submitted.'}
            Start-Sleep -Milliseconds 50
        }
    }
}
function Enter-BuildEntry([string]$Root) {
    if(Test-CandidateSourceRoot $Root){$null=Get-CandidateEntryContext $Root}
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
        if(Test-EntrySharingViolation $_.Exception){throw 'Shared output lease/transaction is busy; no configure, build, arrangement or process action started. Read scripts/build.bat status and retry after the owner finishes.'}
        throw
    } finally {if($metadata){$metadata.Dispose()}}
}
function Exit-BuildEntry($Entry) {
    if($Entry){[Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$Entry.oldToken,'Process');$Entry.lease.Dispose()}
}
function Get-ProjectResourceBinding([string]$Root,[bool]$Required,[int]$CpuSlots=0,[int]$MemoryMiB=0,[bool]$Exclusive=$false) {
    if(($CpuSlots -eq 0) -ne ($MemoryMiB -eq 0) -or $CpuSlots -lt 0 -or $MemoryMiB -lt 0){throw 'CPU slots and memory MiB must be supplied together.'}
    if(-not $Required -and $CpuSlots){throw 'Inspection and empty plans do not accept a weighted execution budget.'}
    if(-not $Required){return [pscustomobject]@{policy='inspection-v1';required=$false;commonGitDirectory=$null;lockPath=$null}}
    if(-not (Get-Command Invoke-InputGit -ErrorAction SilentlyContinue)){. (Join-Path $PSScriptRoot 'build_inputs.ps1')}
    $git=Invoke-InputGit $Root 'rev-parse --path-format=absolute --git-common-dir'
    if($git.code -ne 0 -or -not [IO.Path]::IsPathRooted($git.output.Trim())){throw 'Cannot resolve the shared Git resource scope; no heavy execution was admitted.'}
    $common=[IO.Path]::GetFullPath($git.output.Trim())
    if(-not [IO.Directory]::Exists($common)){throw 'Shared Git resource scope is unavailable; no heavy execution was admitted.'}
    $binding=[pscustomobject]@{policy='project-heavy-v1';required=$true;commonGitDirectory=$common;lockPath=(Join-Path $common 'snowdesktop-execution/heavy.lock')}
    if(-not $CpuSlots){return $binding}
    $state=Get-WeightedResourcePolicy $Root -Persist
    $policy=$state.policy
    $reserved=[int]([Math]::Ceiling($MemoryMiB/256.0)*256)
    if($CpuSlots -gt $policy.cpuCapacity -or $MemoryMiB -lt 256 -or $reserved -gt $policy.memoryCapacityMiB){throw 'Requested CPU or committed-memory budget exceeds project policy capacity.'}
    $binding.policy='project-weighted-v1'
    foreach($pair in ([ordered]@{compatibilityMode=$(if($Exclusive){'exclusive'}else{'shared'});policyHash=$state.hash;policyPath=$state.path;cpuSlots=$CpuSlots;memoryMiB=$MemoryMiB;reservedMemoryMiB=$reserved;
        logicalProcessors=$policy.logicalProcessors;cpuCapacity=$policy.cpuCapacity;memoryCapacityMiB=$policy.memoryCapacityMiB;headroomMiB=$policy.headroomMiB}).GetEnumerator()){$binding|Add-Member NoteProperty $pair.Key $pair.Value}
    return $binding
}
function Initialize-ResourceRuntime {
    if(-not ('SnowDesktop.Build.Job' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'build_job.cs')}
    if(-not ('SnowDesktop.Build.ResourceBroker' -as [type])){throw 'This PowerShell process loaded an older resource runtime; start a new process before requesting weighted execution.'}
}
function New-WeightedResourcePolicy {
    Initialize-ResourceRuntime
    $logical=[int][SnowDesktop.Build.Job]::LogicalProcessors()
    $physical=[long][Math]::Floor([SnowDesktop.Build.Job]::PhysicalMemoryBytes()/1048576.0)
    $memory=[int]([Math]::Floor($physical*0.70/256.0)*256)
    if($logical -gt 4096 -or $memory -lt 256 -or $memory -gt 1048576){throw 'Machine resource capacity is outside the supported policy range.'}
    return [pscustomobject]@{schemaVersion=1;policy='project-weighted-v1';generation=[Guid]::NewGuid().ToString('N');logicalProcessors=$logical;physicalMemoryMiB=$physical;
        cpuCapacity=[Math]::Max(1,$logical-2);memoryCapacityMiB=$memory;memorySlotMiB=256;headroomMiB=[int][Math]::Max(1024,[Math]::Floor($physical/8.0));
        configuredUtc=[DateTime]::UtcNow.ToString('o');configuredBy='automatic-default';reason='Reserve two logical CPUs when available and physical-memory headroom; opt-in weighted requests only.'}
}
function Assert-WeightedResourcePolicy($Policy,[switch]$Historical) {
    foreach($name in @('schemaVersion','policy','generation','logicalProcessors','physicalMemoryMiB','cpuCapacity','memoryCapacityMiB','memorySlotMiB','headroomMiB')){
        if(-not $Policy -or -not $Policy.PSObject.Properties[$name]){throw 'Resource policy is incomplete; preserve it for diagnosis.'}
    }
    foreach($name in @('schemaVersion','logicalProcessors','physicalMemoryMiB','cpuCapacity','memoryCapacityMiB','memorySlotMiB','headroomMiB')){
        $value=$Policy.$name
        if($value -isnot [ValueType] -or $value -is [bool] -or [double]$value -ne [Math]::Truncate([double]$value)){throw 'Resource policy numeric fields must be integers.'}
    }
    if($Policy.schemaVersion -ne 1 -or $Policy.policy -ne 'project-weighted-v1' -or $Policy.generation -notmatch '^[a-f0-9]{32}$' -or
        $Policy.logicalProcessors -lt 1 -or $Policy.logicalProcessors -gt 4096 -or $Policy.cpuCapacity -lt 1 -or $Policy.cpuCapacity -gt [Math]::Max(1,$Policy.logicalProcessors-2) -or
        $Policy.memorySlotMiB -ne 256 -or $Policy.memoryCapacityMiB -lt 256 -or $Policy.memoryCapacityMiB -gt 1048576 -or $Policy.memoryCapacityMiB%256 -ne 0 -or
        $Policy.memoryCapacityMiB -gt [Math]::Floor($Policy.physicalMemoryMiB*0.70/256)*256 -or $Policy.headroomMiB -lt [Math]::Max(1024,[Math]::Floor($Policy.physicalMemoryMiB/8)) -or
        $Policy.headroomMiB -gt $Policy.physicalMemoryMiB-256){throw 'Resource policy has unsupported or unsafe capacity values.'}
    if(-not $Historical){
        Initialize-ResourceRuntime
        if($Policy.logicalProcessors -ne [SnowDesktop.Build.Job]::LogicalProcessors() -or
            $Policy.physicalMemoryMiB -ne [long][Math]::Floor([SnowDesktop.Build.Job]::PhysicalMemoryBytes()/1048576.0)){throw 'Resource policy hardware changed; reconfigure only in a quiescent execution window.'}
    }
}
function Read-ResourcePolicySnapshot([string]$Path) {
    if(-not [IO.File]::Exists($Path)){return $null}
    $stream=[IO.File]::Open($Path,'Open','Read',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=New-Object IO.StreamReader($stream,[Text.Encoding]::UTF8)
    try {if($stream.Length -gt 64KB){throw 'Resource policy exceeds 64 KiB; preserve it.'};$raw=$reader.ReadToEnd()}finally{$reader.Dispose()}
    return [pscustomobject]@{policy=($raw|ConvertFrom-Json);hash=(Get-EntryHash $raw);path=$Path;persisted=$true}
}
function Open-ResourceTransaction([string]$Directory,[int]$Milliseconds=1000) {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while($true){
        try {return [IO.File]::Open((Join-Path $Directory 'budget.lock'),'OpenOrCreate','ReadWrite','None')}
        catch {if(-not (Test-EntrySharingViolation $_.Exception) -or $timer.ElapsedMilliseconds -ge $Milliseconds){throw};Start-Sleep -Milliseconds 20}
    }
}
function Write-ResourcePolicy($Policy,[string]$Path) {
    $temporary=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try {
        if([IO.File]::Exists($Path)){
            $history=Join-Path ([IO.Path]::GetDirectoryName($Path)) 'policies';[void][IO.Directory]::CreateDirectory($history)
            [IO.File]::Copy($Path,(Join-Path $history ([Guid]::NewGuid().ToString('N')+'.json')),$false)
        }
        $bytes=[Text.Encoding]::UTF8.GetBytes(($Policy|ConvertTo-Json -Depth 8))
        $stream=[IO.File]::Open($temporary,'CreateNew','Write','None')
        try {$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        if([IO.File]::Exists($Path)){[IO.File]::Replace($temporary,$Path,$temporary+'.bak')}else{[IO.File]::Move($temporary,$Path)}
    }finally{if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)};if([IO.File]::Exists($temporary+'.bak')){[IO.File]::Delete($temporary+'.bak')}}
}
function Get-WeightedResourcePolicy([string]$Root,[switch]$Persist) {
    Initialize-ResourceRuntime
    $scope=Get-ProjectResourceBinding $Root $true
    $directory=[IO.Path]::GetDirectoryName($scope.lockPath);$path=Join-Path $directory 'budget.json'
    if(-not $Persist){
        $snapshot=Read-ResourcePolicySnapshot $path
        if($snapshot){Assert-WeightedResourcePolicy $snapshot.policy;return $snapshot}
        if([IO.File]::Exists((Join-Path $directory 'budget.lock'))){throw 'Resource policy is missing after initialization; preserve tokens and configure only while quiescent.'}
        return [pscustomobject]@{policy=(New-WeightedResourcePolicy);hash=$null;path=$path;persisted=$false}
    }
    [void][IO.Directory]::CreateDirectory($directory)
    $initialized=[IO.File]::Exists((Join-Path $directory 'budget.lock'))
    $transaction=Open-ResourceTransaction $directory
    try {
        $snapshot=Read-ResourcePolicySnapshot $path
        if(-not $snapshot){
            if($initialized){throw 'Resource policy is missing after initialization; preserve tokens and configure only while quiescent.'}
            Write-ResourcePolicy (New-WeightedResourcePolicy) $path;$snapshot=Read-ResourcePolicySnapshot $path
        }
        Assert-WeightedResourcePolicy $snapshot.policy;return $snapshot
    }finally{$transaction.Dispose()}
}
function Set-WeightedResourcePolicy([string]$Root,[int]$CpuCapacity,[int]$MemoryCapacityMiB,[string]$Task,[string]$Reason) {
    $policy=New-WeightedResourcePolicy
    if($CpuCapacity -lt 1 -or $CpuCapacity -gt $policy.cpuCapacity -or $MemoryCapacityMiB -lt 256 -or $MemoryCapacityMiB -gt $policy.memoryCapacityMiB -or $MemoryCapacityMiB%256){throw 'Configured capacity must fit hardware reserves; memory capacity uses multiples of 256 MiB.'}
    $scope=Get-ProjectResourceBinding $Root $true;$directory=[IO.Path]::GetDirectoryName($scope.lockPath)
    [void][IO.Directory]::CreateDirectory($directory)
    $compatibility=$null;$transaction=$null
    try {
        $compatibility=[IO.File]::Open($scope.lockPath,'OpenOrCreate','ReadWrite','None')
        $transaction=Open-ResourceTransaction $directory
        $path=Join-Path $directory 'budget.json';$previous=Read-ResourcePolicySnapshot $path
        if($previous){Assert-WeightedResourcePolicy $previous.policy -Historical}
        $policy.cpuCapacity=$CpuCapacity;$policy.memoryCapacityMiB=$MemoryCapacityMiB;$policy.configuredBy=$Task;$policy.reason=$Reason
        Write-ResourcePolicy $policy $path;return (Read-ResourcePolicySnapshot $path)
    }catch{if(Test-EntrySharingViolation $_.Exception){throw 'Resource policy is busy; no capacity change was applied while a lease or transaction is active.'};throw}
    finally{if($transaction){$transaction.Dispose()};if($compatibility){$compatibility.Dispose()}}
}
function Enter-ProjectResources($Binding) {
    # The handle is the authority. No PID, heartbeat age, stale JSON or timeout
    # releases somebody else's capacity. It is never inherited by a child.
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Binding.lockPath))
    $compatibility=$null;$transaction=$null
    try {
        if($Binding.policy -ne 'project-weighted-v1'){return [IO.File]::Open($Binding.lockPath,'OpenOrCreate','ReadWrite','None')}
        Initialize-ResourceRuntime
        # Shared readers exclude the original v1 exclusive writer. Keep that
        # reader until all weighted tokens are released, including on crash.
        if($Binding.compatibilityMode -eq 'exclusive'){$compatibility=[IO.File]::Open($Binding.lockPath,'OpenOrCreate','ReadWrite','None')}
        else{$compatibility=[IO.File]::Open($Binding.lockPath,'OpenOrCreate','Read','Read')}
        $transaction=Open-ResourceTransaction ([IO.Path]::GetDirectoryName($Binding.lockPath)) 0
        $snapshot=Read-ResourcePolicySnapshot $Binding.policyPath
        if(-not $snapshot -or $snapshot.hash -ne $Binding.policyHash){throw 'Resource policy changed before admission.'}
        Assert-WeightedResourcePolicy $snapshot.policy
        $attempt=[SnowDesktop.Build.ResourceBroker]::TryReserve([IO.Path]::GetDirectoryName($Binding.lockPath),$Binding.cpuCapacity,[int]($Binding.memoryCapacityMiB/256),
            $Binding.cpuSlots,[int]($Binding.reservedMemoryMiB/256),[SnowDesktop.Build.Job]::AvailableMemoryBytes(),([ulong]$Binding.headroomMiB*1048576))
        if(-not $attempt.Lease){
            $busyError=New-Object InvalidOperationException ('Shared project resources busy: '+$attempt.BusyKind+'; requested '+$Binding.cpuSlots+' CPU slots / '+$Binding.memoryMiB+' MiB')
            $busyError.Data['resourceObservation']=$attempt;throw $busyError
        }
        $attempt.Lease.AttachCompatibility($compatibility);$compatibility=$null
        return $attempt.Lease
    }
    catch {
        if(Test-EntrySharingViolation $_.Exception){throw ('Shared project resources busy: heavy execution lane in '+$Binding.commonGitDirectory)}
        throw
    }
    finally{if($transaction){$transaction.Dispose()};if($compatibility){$compatibility.Dispose()}}
}
function Get-EntryResourceBudget([string]$Root) {
    $id=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
    if(-not $id){return $null}
    if($id -notmatch '^[a-f0-9]{32}$'){throw 'Invalid budget Run identity.'}
    $runRoot=Get-EntryRunRoot $Root
    $directory=Join-Path $runRoot '.build/collaboration/runs'
    $request=Read-EntryJson (Join-Path $directory ($id+'.request.json'))
    if(-not $request -or -not $request.PSObject.Properties['projectResources'] -or $request.projectResources.policy -ne 'project-weighted-v1'){return $null}
    $record=Read-EntryJson (Join-Path $directory ($id+'.json'))
    if(-not $record -or $request.repositoryRoot -ne $runRoot -or $record.repositoryRoot -ne $runRoot -or $record.id -ne $id -or $record.nonce -ne $request.nonce -or
        $record.status -ne 'running' -or -not (Test-EntryDelegation $Root) -or -not (Test-EntryAncestor $record.owner) -or
        -not $record.resourceAdmission.acquiredUtc -or -not $record.resourceAdmission.bindingValid){throw 'Weighted commands require their live admitted Run and output authority.'}
    return $request.projectResources
}
function Set-EntryRunControl([string]$Root) {
    [SnowDesktop.Build.Job]::ConfigureResourceBudget(0,0,0)
    $id=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_RUN_ID','Process')
    if(-not $id){return}
    if($id -notmatch '^[a-f0-9]{32}$'){throw 'Invalid Run identity.'}
    $runRoot=Get-EntryRunRoot $Root
    $directory=Join-Path $runRoot '.build/collaboration/runs'
    $request=Read-EntryJson (Join-Path $directory ($id+'.request.json'))
    $record=Read-EntryJson (Join-Path $directory ($id+'.json'))
    if(-not $request -or -not $record -or $request.repositoryRoot -ne $runRoot -or $record.repositoryRoot -ne $runRoot -or
        $record.id -ne $id -or $record.nonce -ne $request.nonce -or $record.status -ne 'running'){
        throw 'Run control has no live matching request and execution record.'
    }
    if($record.owner.pid -ne $PID){return} # Delegated children are contained by the outer Run Job.
    if($record.owner.startTicks -ne (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()){
        throw 'Run executor process identity changed.'
    }
    Assert-EntryExecutionOwner $Root
    $budget=Get-EntryResourceBudget $Root
    if($budget){[SnowDesktop.Build.Job]::ConfigureResourceBudget($budget.cpuSlots,$budget.logicalProcessors,$budget.memoryMiB)}
    [SnowDesktop.Build.Job]::ConfigureRunControl((Join-Path $directory ($id+'.cancel')),$request.nonce,(Join-Path $directory ($id+'.resources.json')))
}
function Assert-EntryExecutionOwner([string]$Root) {
    $record=Read-EntryJson (Join-Path $Root '.build/collaboration/execution.json')
    if(-not (Test-EntryDelegation $Root) -or -not $record -or $record.owner.pid -ne $PID -or
        $record.owner.startTicks -ne (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()) {
        throw 'Process actions require the live execution lease owner, outside the private build Job.'
    }
}
function Invoke-EntryPreflight([string]$Root,[bool]$ReloadShell,[bool]$CloseApplication,[string]$Configuration='Release',[string]$LogPath='') {
    if(-not $ReloadShell -and -not $CloseApplication){return 0}
    Assert-EntryExecutionOwner $Root
    $options=@{}
    if($ReloadShell){$options.ReloadShell=$true}
    if($CloseApplication){$options.CloseApplication=$true}
    if($Configuration -ne 'Release'){$options.Configuration=$Configuration}
    # Run in the lease-owning PowerShell process. A restored user Shell must
    # survive build success, failure and KILL_ON_JOB_CLOSE on interruption.
    if(-not $LogPath){$LogPath=Join-Path $Root ('.build/collaboration/preflight-'+[Guid]::NewGuid().ToString('N')+'.log')}
    $receipt=[pscustomobject]@{ownerPid=$PID;configuration=$Configuration;reloadShell=$ReloadShell;closeApplication=$CloseApplication;
        startedUtc=[DateTime]::UtcNow.ToString('o');completedUtc=$null;exitCode=0;error=''}
    $global:LASTEXITCODE=0
    try {
        & (Join-Path $Root 'scripts/build_preflight.ps1') @options | Out-Host
        $receipt.exitCode=$global:LASTEXITCODE
    } catch {
        $receipt.error=$_.Exception.ToString()
        $receipt.exitCode=3
        [Console]::Error.WriteLine($receipt.error)
    } finally {
        $receipt.completedUtc=[DateTime]::UtcNow.ToString('o')
        [IO.File]::WriteAllText([IO.Path]::ChangeExtension($LogPath,'.preflight.json'),($receipt|ConvertTo-Json -Depth 6),(New-Object Text.UTF8Encoding($false)))
    }
    if($receipt.exitCode -ne 0){
        [IO.File]::WriteAllText($LogPath,('Output preflight exited with code '+$receipt.exitCode+'. '+$receipt.error),(New-Object Text.UTF8Encoding($false)))
    }
    return $receipt.exitCode
}

if($MyInvocation.InvocationName -ne '.') {
    $ErrorActionPreference='Stop';$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'));$entry=$null
    $previousNodeReuse=[Environment]::GetEnvironmentVariable('MSBUILDDISABLENODEREUSE','Process')
    try {
        if($EntryAction -eq 'verify') {
            if(-not (Test-EntryDelegation $root)){throw 'Shared CMake/IDE output requires a live leased entry. Use scripts/build.bat, scripts/test.bat, or scripts/build_entry.ps1 -Action ide; independent output remains available for diagnostics.'}
            exit 0
        }
        if(-not (Test-EntryDelegation $root) -and -not [Environment]::GetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN','Process')) {
            # Preserve fail-fast busy behavior. Releasing this probe performs no
            # preflight/process action; the detached worker reacquires authority.
            $probe=Enter-BuildEntry $root
            Exit-BuildEntry $probe
            $submit=@{Command='invoke';Action=$EntryAction;Mode=$EntryMode;Filter=$EntryFilter;
                Configuration=$EntryConfiguration;Targets=$EntryTargets;ReloadShell=$EntryReloadShell;CloseApplication=$EntryCloseApplication}
            if($EntryTask){$submit.Task=$EntryTask};if($EntryPlanBatch){$submit.PlanBatch=$EntryPlanBatch}
            & (Join-Path $PSScriptRoot 'build_run.ps1') @submit
            exit $LASTEXITCODE
        }
        $entry=Enter-BuildEntry $root
        $log=Join-Path $root ('.build/collaboration/entry-'+[Guid]::NewGuid().ToString('N')+'.log')
        . (Join-Path $PSScriptRoot 'build_telemetry.ps1')
        if($EntryCloseApplication -and ($EntryAction -ne 'release' -or $EntryReloadShell)){throw 'CloseApplication requires a Release build without ReloadShell.'}
        if($EntryAction -in 'release','debug') {
            $configuration=if($EntryAction -eq 'debug'){'Debug'}else{'Release'}
            $preflightStage=Start-BuildStage $root 'preflight' $log
            $preflightCode=2
            try {$preflightCode=Invoke-EntryPreflight $root ([bool]$EntryReloadShell) ([bool]$EntryCloseApplication) $configuration $log}
            finally {Complete-BuildStage $root $preflightStage $preflightCode}
            if($preflightCode -ne 0){[Console]::Error.WriteLine('Preflight log: '+$log);exit 3}
        }
        Push-Location $root
        try {
            # All leased subprocesses belong to this entry's private Job. An
            # interrupted entry cannot release the lease while its CMake child
            # keeps writing. No user process is attached to this Job.
            Add-Type -Path (Join-Path $PSScriptRoot 'build_job.cs')
            [Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE','1','Process')
            Set-EntryRunControl $root
            if($EntryAction -eq 'ide') {
                $preset=if($EntryConfiguration -eq 'Debug'){'debug'}else{'release'}
                foreach($target in $EntryTargets){if($target -notmatch '^[A-Za-z0-9_.-]+$'){throw 'IDE targets must be literal target names.'}}
                $wrapper='"'+(Get-BuildPowerShell)+'" -NoProfile -File scripts\build_command.ps1'
                $pipeline=$wrapper+' -Operation configure -Preset '+$preset+' && '+$wrapper+' -Operation build -Preset '+$preset
                if($EntryTargets.Count){$pipeline+=' -Targets '+($EntryTargets -join ',')}
            } elseif($EntryAction -eq 'tests') {
                $testScript=Join-Path $PSScriptRoot 'test_manager.ps1'
                $invoke="& '"+$testScript.Replace("'","''")+"' -Mode '"+$EntryMode+"' -Filter '"+$EntryFilter.Replace("'","''")+"'"
                if($EntryPlanBatch){$invoke+=" -PlanBatch '"+$EntryPlanBatch+"'"}
                $invoke='try { '+$invoke+'; if($null -ne $LASTEXITCODE){exit $LASTEXITCODE}else{exit 0} } catch {[Console]::Error.WriteLine($_.ScriptStackTrace);throw}'
                $invokePath=[IO.Path]::ChangeExtension($log,'.ps1')
                [IO.File]::WriteAllText($invokePath,"$"+"ErrorActionPreference='Stop'`n"+$invoke,(New-Object Text.UTF8Encoding($true)))
                $pipeline='"'+(Get-BuildPowerShell)+'" -NoProfile -File "'+$invokePath+'"'
            } else {
                $scriptName=if($EntryAction -eq 'debug'){'build_debug.bat'}else{'build.bat'}
                $pipeline='call scripts\'+$scriptName
            }
            [Console]::Error.WriteLine('Shared entry log: '+$log)
            $pipelineStage=Start-BuildStage $root ($EntryAction+'-pipeline') $log
            $previousParent=[Environment]::GetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT','Process')
            $code=2
            try {
                if($pipelineStage){[Environment]::SetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT',$pipelineStage.id,'Process')}
                $code=[SnowDesktop.Build.Job]::RunLeasedCommand($root,$log,$pipeline)
            } finally {
                [Environment]::SetEnvironmentVariable('SNOWDESKTOP_STAGE_PARENT',$previousParent,'Process')
                Complete-BuildStage $root $pipelineStage $code
            }
            if([IO.File]::Exists($log)){Get-Content -LiteralPath $log}
            exit $code
        } finally {Pop-Location}
    } catch {[Console]::Error.WriteLine($_.Exception.Message);exit 2}
    finally {[Environment]::SetEnvironmentVariable('MSBUILDDISABLENODEREUSE',$previousNodeReuse,'Process');Exit-BuildEntry $entry}
}
