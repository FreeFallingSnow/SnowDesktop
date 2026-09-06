[CmdletBinding()]
param(
    [ValidateSet('status', 'capture', 'start', 'stop', 'report', 'compare')]
    [string]$Command = 'status',
    [ValidateRange(1, 600)][int]$Seconds = 60,
    [int]$TargetProcessId = 0,
    [string]$OutputDirectory,
    [string]$Session,
    [string]$ReportPath,
    [string]$BaselinePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Write-JsonFile($Value, [string]$Path) {
    $json = ConvertTo-Json -InputObject $Value -Depth 12
    [IO.File]::WriteAllText($Path, $json, (New-Object Text.UTF8Encoding($false)))
}

function Read-Capture([string]$Path) {
    if (-not $Path) { throw 'ReportPath is required.' }
    $report = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($report.schemaVersion -ne 1) { throw 'Unsupported performance report schema.' }
    return $report
}

function Get-Summary($Report) {
    $seconds = [double]$Report.durationMs / 1000
    $scopes = @($Report.groups | Where-Object { $_.kind -eq 'scope' } | ForEach-Object {
        [pscustomobject]@{
            module = $_.module; phase = $_.phase; owner = $_.owner; calls = $_.count
            callsPerSecond = if ($seconds -gt 0) { $_.count / $seconds } else { $null }
            wallMs = $_.wallMs; selfWallMs = $_.selfWallMs
            selfCpuMs = $_.selfCpuMs; cpuSamples = $_.cpuSamples
            p95WallUpperMs = $_.p95WallUpperMs; maxWallMs = $_.maxWallMs
        }
    } | Sort-Object selfCpuMs -Descending)
    $cpuSamples = @($Report.groups | Where-Object {
        $_.module -eq 'process' -and $_.phase -eq 'cpu_total_ms'
    })
    $cpuPercent = $null
    $cpuWindowMs = $null
    if ($cpuSamples.Count -eq 1 -and $cpuSamples[0].count -ge 2) {
        $cpuWindowMs = $cpuSamples[0].lastAtMs - $cpuSamples[0].firstAtMs
        if ($cpuWindowMs -gt 0 -and $Report.logicalProcessors -gt 0) {
            $cpuPercent = 100 * ($cpuSamples[0].max - $cpuSamples[0].min) /
                $cpuWindowMs / $Report.logicalProcessors
        }
    }
    [pscustomobject]@{
        schemaVersion = 1; session = $Report.session; hostVersion = $Report.hostVersion
        processId = $Report.processId; durationMs = $Report.durationMs
        processCpuPercent = $cpuPercent; processCpuSampleWindowMs = $cpuWindowMs
        droppedEvents = $Report.droppedEvents; droppedGroups = $Report.droppedGroups
        droppedLinks = $Report.droppedLinks; probeErrors = $Report.probeErrors
        scopes = $scopes
        gauges = @($Report.groups | Where-Object { $_.kind -eq 'gauge' })
        limitations = $Report.limitations
    }
}

function Export-Summary([string]$Path) {
    $report = Read-Capture $Path
    $summary = Get-Summary $report
    $directory = Split-Path -Parent ([IO.Path]::GetFullPath($Path))
    $summaryPath = Join-Path $directory 'summary.json'
    Write-JsonFile $summary $summaryPath
    $summary.scopes | Export-Csv -LiteralPath (Join-Path $directory 'scopes.csv') -NoTypeInformation -Encoding UTF8
    $summary.gauges | Export-Csv -LiteralPath (Join-Path $directory 'gauges.csv') -NoTypeInformation -Encoding UTF8
    [pscustomobject]@{ report = [IO.Path]::GetFullPath($Path); summary = $summaryPath
        processCpuPercent = $summary.processCpuPercent; droppedEvents = $summary.droppedEvents }
}

try {
    if ($Command -eq 'report') {
        Export-Summary $ReportPath | ConvertTo-Json -Compress
        exit 0
    }
    if ($Command -eq 'compare') {
        $before = Get-Summary (Read-Capture $BaselinePath)
        $after = Get-Summary (Read-Capture $ReportPath)
        $keys = @{}
        foreach ($row in $before.scopes) {
            $key = ConvertTo-Json -InputObject @($row.module, $row.phase, $row.owner) -Compress
            $keys[$key] = $row
        }
        $rows = @($after.scopes | ForEach-Object {
            $key = ConvertTo-Json -InputObject @($_.module, $_.phase, $_.owner) -Compress
            $old = $keys[$key]
            [pscustomobject]@{
                module = $_.module; phase = $_.phase; owner = $_.owner
                baselineSelfCpuMsPerSecond = if ($old) { $old.selfCpuMs * 1000 / $before.durationMs } else { $null }
                currentSelfCpuMsPerSecond = $_.selfCpuMs * 1000 / $after.durationMs
                baselineCallsPerSecond = if ($old) { $old.callsPerSecond } else { $null }
                currentCallsPerSecond = $_.callsPerSecond
                baselineP95WallUpperMs = if ($old) { $old.p95WallUpperMs } else { $null }
                currentP95WallUpperMs = $_.p95WallUpperMs
            }
        })
        [pscustomobject]@{ schemaVersion = 1; baselineSession = $before.session
            currentSession = $after.session; scopes = $rows
            note = 'Only matched instance IDs are comparable. Keep workload, machine and display configuration equal; timings are instrumented observations, not proof of causality.'
        } | ConvertTo-Json -Depth 8
        exit 0
    }

    if (-not ('SnowDesktopPerformanceClient' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SnowDesktopPerformanceClient {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern uint RegisterWindowMessage(string name);
    [DllImport("user32.dll", SetLastError=true)]
    static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint msg, UIntPtr wp, IntPtr lp,
        uint flags, uint timeout, out UIntPtr result);
    [StructLayout(LayoutKind.Sequential)]
    struct CopyData { public UIntPtr tag; public uint size; public IntPtr data; }
    public static IntPtr Find(int requestedPid) {
        IntPtr found = IntPtr.Zero, cursor = IntPtr.Zero;
        while ((cursor = FindWindowEx(IntPtr.Zero, cursor, "SnowDesktopControlWindow", "SnowDesktopControl")) != IntPtr.Zero) {
            uint pid; GetWindowThreadProcessId(cursor, out pid);
            if (requestedPid != 0 && pid != requestedPid) continue;
            if (found != IntPtr.Zero) throw new InvalidOperationException("Multiple hosts; specify TargetProcessId.");
            found = cursor;
        }
        return found;
    }
    public static uint ProcessId(IntPtr window) { uint pid; GetWindowThreadProcessId(window, out pid); return pid; }
    public static ulong Send(IntPtr window, uint message, IntPtr data) {
        UIntPtr result;
        if (SendMessageTimeout(window, message, UIntPtr.Zero, data, 0x22, 5000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("Host control request timed out or was denied. Win32=" + Marshal.GetLastWin32Error());
        return result.ToUInt64();
    }
    public static ulong Request(IntPtr window, string request) {
        byte[] bytes = Encoding.Unicode.GetBytes(request + "\0");
        IntPtr payload = Marshal.AllocHGlobal(bytes.Length);
        IntPtr copy = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(CopyData)));
        try {
            Marshal.Copy(bytes, 0, payload, bytes.Length);
            Marshal.StructureToPtr(new CopyData { tag = new UIntPtr(0x53445031), size=(uint)bytes.Length, data=payload }, copy, false);
            return Send(window, 0x004A, copy);
        } finally { Marshal.FreeHGlobal(copy); Marshal.FreeHGlobal(payload); }
    }
}
'@
    }
    $window = [SnowDesktopPerformanceClient]::Find($TargetProcessId)
    if ($window -eq [IntPtr]::Zero) { throw 'No running SnowDesktop control endpoint. Start the built application normally first.' }
    $message = [SnowDesktopPerformanceClient]::RegisterWindowMessage('FreeFallingSnow.SnowDesktop.Performance.v1')
    $status = [SnowDesktopPerformanceClient]::Send($window, $message, [IntPtr]::Zero)
    # Keep protocol constants in hexadecimal here to make their wire identity explicit.
    $states = @{}; $states[[ulong]0x53445010] = 'idle'; $states[[ulong]0x53445011] = 'recording'
    $states[[ulong]0x53445012] = 'finishing'; $states[[ulong]0x53445013] = 'failed'
    if (-not $states.ContainsKey($status)) { throw 'The running host does not support performance protocol v1. It may be an older build.' }
    $hostPid = [SnowDesktopPerformanceClient]::ProcessId($window)
    if ($Command -eq 'status') {
        [pscustomobject]@{ schemaVersion = 1; state = $states[$status]; processId = $hostPid } | ConvertTo-Json -Compress
        exit 0
    }
    if ($Command -eq 'stop') {
        if ($Session -notmatch '^[A-Za-z0-9-]{1,64}$') { throw 'A valid Session is required; stop can only stop that session.' }
        $request = @('1', 'stop', $Session, '', '') -join "`n"
        if ([SnowDesktopPerformanceClient]::Request($window, $request) -ne 1) { throw 'No matching active session; nothing was stopped.' }
        [pscustomobject]@{ session = $Session; stopRequested = $true } | ConvertTo-Json -Compress
        exit 0
    }
    if ($states[$status] -in @('recording', 'finishing')) { throw 'The host already has a performance capture. Stop it using its session ID or wait for completion.' }
    $sessionId = [Guid]::NewGuid().ToString()
    if (-not $OutputDirectory) {
        $version = (Get-Content -LiteralPath (Join-Path $repository 'version.json') -Raw | ConvertFrom-Json).version
        $OutputDirectory = Join-Path $repository ("artifacts/v{0}/performance/{1}-{2}" -f $version, (Get-Date -Format 'yyyyMMdd-HHmmss'), $sessionId)
    }
    $OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    if ($OutputDirectory.Contains("`n") -or $OutputDirectory.Contains("`r")) { throw 'OutputDirectory must not contain a newline.' }
    [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
    $capturePath = Join-Path $OutputDirectory 'capture.json'
    $metadataPath = Join-Path $OutputDirectory 'session.json'
    if ((Test-Path -LiteralPath $capturePath) -or (Test-Path -LiteralPath ($capturePath + '.partial')) -or (Test-Path -LiteralPath $metadataPath)) {
        throw 'OutputDirectory already contains a capture; use a new run directory.'
    }
    $hostProcess = Get-Process -Id $hostPid
    $metadata = [pscustomobject]@{ schemaVersion = 1; session = $sessionId; processId = $hostPid
        executable = $hostProcess.Path; startedUtc = [DateTime]::UtcNow.ToString('o')
        executableSha256 = (Get-FileHash -LiteralPath $hostProcess.Path -Algorithm SHA256).Hash
        seconds = $Seconds; report = $capturePath; osVersion = [Environment]::OSVersion.VersionString }
    Write-JsonFile $metadata $metadataPath
    $request = @('1', 'start', $sessionId, [string]$Seconds, $capturePath) -join "`n"
    if ([SnowDesktopPerformanceClient]::Request($window, $request) -ne 1) {
        throw 'Capture was rejected (busy, invalid options, output reservation or sampler setup failure). See session.json and any .partial file.'
    }
    if ($Command -eq 'start') { $metadata | ConvertTo-Json -Compress; exit 0 }

    $completed = $false
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds($Seconds + 30)
        while ([DateTime]::UtcNow -lt $deadline) {
            if (Test-Path -LiteralPath $capturePath) { $completed = $true; break }
            if (-not (Get-Process -Id $hostPid -ErrorAction SilentlyContinue)) { throw 'Host exited before publishing a report; any .partial file is incomplete.' }
            Start-Sleep -Milliseconds 500
        }
        if (-not $completed) { throw 'Timed out waiting for the completed capture report.' }
    } finally {
        if (-not $completed) {
            try { [void][SnowDesktopPerformanceClient]::Request($window, (@('1', 'stop', $sessionId, '', '') -join "`n")) } catch { }
        }
    }
    Export-Summary $capturePath | ConvertTo-Json -Compress
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
