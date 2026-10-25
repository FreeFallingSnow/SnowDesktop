[CmdletBinding()]
param(
    [ValidateSet('setup','status','capture','start','stop','wait','analyze','report','compare')][string]$Command = 'status',
    [ValidateRange(1,600)][int]$Seconds = 120,
    [int]$TargetProcessId = 0,
    [string]$Session,
    [string]$RequestId,
    [UInt64]$Generation = 0,
    [ValidateSet('idle','interaction','widget-detail')][string]$Preset = 'idle',
    [ValidateSet('summary','trace')][string]$Mode,
    [string[]]$WidgetIds = @(),
    [ValidateRange(64,65536)][int]$BufferKiB = 32768,
    [string]$OutputDirectory,
    [string]$ReportPath,
    [string]$BaselinePath,
    [switch]$ScopeCpu,
    [switch]$Offline,
    [switch]$NoAnalyze
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $repository 'tools/performance/ensure_trace_processor.ps1')
. (Join-Path $repository 'tools/performance/legacy_report.ps1')

function New-RequestId { return [Guid]::NewGuid().ToString('N') }
function Invoke-Control([hashtable]$Request) {
    $Request.protocolVersion = 3
    if (-not $Request.requestId) { $Request.requestId = New-RequestId }
    $reply = [SnowDesktopPerformanceClientV3]::Request($script:hostPid, (ConvertTo-Json $Request -Depth 5 -Compress)) | ConvertFrom-Json
    if ($reply.protocolVersion -ne 3 -or $reply.processId -ne $script:hostPid -or $reply.requestId -ne $Request.requestId) { throw 'controlReplyIdentityMismatch' }
    if ($Request.ContainsKey('sessionId') -and $reply.sessionId -ne $Request.sessionId) { throw 'controlReplySessionMismatch' }
    if ($reply.errorCode) { throw ($reply.errorCode + ': ' + (ConvertTo-Json $reply -Depth 6 -Compress)) }
    return $reply
}
function Wait-Capture([string]$Id, [UInt64]$ExpectedGeneration) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds + 75)
    do {
        $snapshot = Invoke-Control @{command='status';requestId=(New-RequestId);sessionId=$Id}
        if ($ExpectedGeneration -and $snapshot.generation -ne $ExpectedGeneration) { throw 'captureGenerationChanged' }
        if ($snapshot.state -in @('completed','failed')) { return $snapshot }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'captureWaitTimeout: session continues independently; use wait with the same Session to resume.'
}
try {
    if ($Mode) { $Preset = if ($Mode -eq 'summary') {'idle'} else {'interaction'} }
    if ($Command -eq 'setup') {
        [pscustomobject]@{version='v58.2';path=(Get-SnowDesktopTraceProcessor -Offline:$Offline)} | ConvertTo-Json -Compress
        exit 0
    }
    if ($Command -in @('analyze','report')) {
        if (-not $ReportPath) { throw 'ReportPath is required.' }
        $legacy = [IO.Path]::GetExtension($ReportPath) -eq '.json' -and
            (Get-Content -LiteralPath $ReportPath -Raw -Encoding UTF8 | ConvertFrom-Json).schemaVersion -eq 1
        if ($legacy) { Export-Summary $ReportPath | ConvertTo-Json -Compress }
        else { & (Join-Path $PSScriptRoot 'profile_analyze.ps1') -ManifestPath $ReportPath -OutputDirectory $OutputDirectory -Offline:$Offline }
        exit 0
    }
    if ($Command -eq 'compare') {
        $before = Get-Content -LiteralPath $BaselinePath -Raw -Encoding UTF8 | ConvertFrom-Json
        $after = Get-Content -LiteralPath $ReportPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($before.schemaVersion -ne 2 -or $after.schemaVersion -ne 2) { throw 'comparisonRequiresNativeSummaryV2: historical reports remain readable through report.' }
        if (-not $before.quality.valid -or -not $after.quality.valid -or $before.configHash -ne $after.configHash -or
            $before.queryVersion -ne $after.queryVersion) { throw 'incompatibleComparison: capture configuration, query version and complete evidence must match.' }
        foreach ($query in $before.queryHashes.PSObject.Properties) {
            if ($after.queryHashes.($query.Name) -ne $query.Value) { throw 'incompatibleQueryPackage' }
        }
        $keys = @{}; foreach ($row in $before.modules) { $keys[$row.module] = $row }
        $rows = @($after.modules | ForEach-Object {
            $old = $keys[$_.module]
            [pscustomobject]@{module=$_.module;baselineWallMsPerSecond=if($old){[double]$old.selfWallMs*1000/$before.effectiveDurationMs}else{$null}
                currentWallMsPerSecond=[double]$_.selfWallMs*1000/$after.effectiveDurationMs
                baselineCallsPerSecond=if($old){[double]$old.calls*1000/$before.effectiveDurationMs}else{$null}
                currentCallsPerSecond=[double]$_.calls*1000/$after.effectiveDurationMs}
        })
        [pscustomobject]@{schemaVersion=2;modules=$rows;note='Module observations only. Instance identities are session-local. Match machine, display, packages, layout and workload before interpreting differences.'} | ConvertTo-Json -Depth 6
        exit 0
    }
    if (-not ('SnowDesktopPerformanceClientV3' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
public static class SnowDesktopPerformanceClientV3 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint pid);
    public static uint Connect(int requestedPid) {
        IntPtr found=IntPtr.Zero, cursor=IntPtr.Zero; uint target=0;
        while ((cursor=FindWindowEx(IntPtr.Zero,cursor,"SnowDesktopControlWindow","SnowDesktopControl"))!=IntPtr.Zero) {
            uint pid; GetWindowThreadProcessId(cursor,out pid);
            if(requestedPid!=0 && pid!=requestedPid) continue;
            if(found!=IntPtr.Zero) throw new InvalidOperationException("Multiple hosts; specify TargetProcessId.");
            found=cursor; target=pid;
        }
        if(found==IntPtr.Zero) throw new InvalidOperationException("No running SnowDesktop control endpoint.");
        UIntPtr result;
        if(SendMessageTimeout(found,RegisterWindowMessage("FreeFallingSnow.SnowDesktop.Performance.v3"),UIntPtr.Zero,IntPtr.Zero,0x22,3000,out result)==IntPtr.Zero || result.ToUInt64()<0x53445010 || result.ToUInt64()>0x53445014)
            throw new InvalidOperationException("Performance protocol v3 unavailable; check the running build and integrity level.");
        return target;
    }
    static byte[] Read(NamedPipeClientStream pipe,int length) {
        byte[] data=new byte[length]; int offset=0; var end=DateTime.UtcNow.AddSeconds(5);
        while(offset<length) {
            int remaining=(int)Math.Max(0,(end-DateTime.UtcNow).TotalMilliseconds);
            var task=pipe.ReadAsync(data,offset,length-offset);
            if(!task.Wait(remaining)) { pipe.Dispose(); throw new TimeoutException("controlReadTimeout"); }
            if(task.Result==0) throw new EndOfStreamException("controlReplyIncomplete");
            offset+=task.Result;
        }
        return data;
    }
    public static string Request(uint target,string json) {
        using(var pipe=new NamedPipeClientStream(".","SnowDesktop.Performance.v3."+target,PipeDirection.InOut,PipeOptions.Asynchronous)) {
            pipe.Connect(3000); uint actual;
            if(!GetNamedPipeServerProcessId(pipe.SafePipeHandle,out actual) || actual!=target) throw new InvalidOperationException("controlServerIdentityMismatch");
            byte[] data=new UTF8Encoding(false,true).GetBytes(json);
            if(data.Length==0 || data.Length>16384) throw new InvalidOperationException("controlRequestLimit");
            byte[] frame=new byte[data.Length+4]; Array.Copy(BitConverter.GetBytes(data.Length),frame,4); Array.Copy(data,0,frame,4,data.Length);
            if(!pipe.WriteAsync(frame,0,frame.Length).Wait(3000)) {pipe.Dispose();throw new TimeoutException("controlWriteTimeout");}
            int length=BitConverter.ToInt32(Read(pipe,4),0);
            if(length<=0 || length>65536) throw new InvalidOperationException("controlReplyLimit");
            string result=new UTF8Encoding(false,true).GetString(Read(pipe,length));
            if(!pipe.WriteAsync(new byte[]{1},0,1).Wait(1000)) {pipe.Dispose();throw new TimeoutException("controlAckTimeout");}
            return result;
        }
    }
}
'@
    }
    $script:hostPid = [SnowDesktopPerformanceClientV3]::Connect($TargetProcessId)
    if ($Command -eq 'status') { Invoke-Control @{command='status';requestId=(New-RequestId);sessionId=[string]$Session} | ConvertTo-Json -Depth 8 -Compress; exit 0 }
    if ($Command -in @('start','capture')) {
        if (-not $Session) { $Session = New-RequestId }
        if (-not $RequestId) { $RequestId = $Session }
        $snapshot = Invoke-Control @{command='start';requestId=$RequestId;sessionId=$Session;preset=$Preset;seconds=$Seconds;bufferKiB=$BufferKiB;threadCpu=[bool]$ScopeCpu;widgetIds=@($WidgetIds)}
        $Generation = [UInt64]$snapshot.generation
        if ($Command -eq 'start') { $snapshot | ConvertTo-Json -Depth 8 -Compress; exit 0 }
    }
    if (-not $Session) { throw 'Session is required.' }
    if ($Command -eq 'stop') {
        if (-not $Generation) { $Generation = [UInt64](Invoke-Control @{command='status';requestId=(New-RequestId);sessionId=$Session}).generation }
        Invoke-Control @{command='stop';requestId=(New-RequestId);sessionId=$Session;generation=$Generation} | ConvertTo-Json -Depth 8 -Compress
        exit 0
    }
    $snapshot = Wait-Capture $Session $Generation
    if ($OutputDirectory) {
        $destination = [IO.Path]::GetFullPath($OutputDirectory)
        [IO.Directory]::CreateDirectory($destination) | Out-Null
        foreach ($file in @($snapshot.paths.trace,$snapshot.paths.manifest)) { [IO.File]::Copy($file,(Join-Path $destination ([IO.Path]::GetFileName($file))),$false) }
        $snapshot.paths.manifest = Join-Path $destination 'session.json'
        $snapshot.paths.trace = Join-Path $destination 'trace.pftrace'
    }
    if (-not $NoAnalyze) { & (Join-Path $PSScriptRoot 'profile_analyze.ps1') -ManifestPath $snapshot.paths.manifest -Offline:$Offline }
    else { $snapshot | ConvertTo-Json -Depth 8 -Compress }
}
catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }
