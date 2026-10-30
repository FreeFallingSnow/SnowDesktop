[CmdletBinding()]
param(
    [string]$Method = 'api.describe',
    [string]$ParametersJson = '{}',
    [string]$ParametersFile,
    [string]$ExecutablePath,
    [ValidateRange(100, 30000)][int]$TimeoutMs = 5000,
    # Explicit local endpoint override for isolated protocol tests.
    [string]$PipeName
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$pipe = $null
try {
    if (-not $ExecutablePath) {
        $bindingPath = Join-Path (Split-Path $PSScriptRoot -Parent) 'connection.json'
        if (-not (Test-Path -LiteralPath $bindingPath -PathType Leaf)) {
            throw 'Install this Skill from SnowDesktop, or specify -ExecutablePath for the intended installation.'
        }
        $ExecutablePath = (Get-Content -LiteralPath $bindingPath -Raw -Encoding UTF8 | ConvertFrom-Json).executablePath
    }
    if (-not [IO.Path]::IsPathRooted($ExecutablePath) -or
        -not (Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) {
        throw 'The bound SnowDesktop executable is unavailable. Reinstall the Skill from the intended installation.'
    }
    $expectedExecutable = [IO.Path]::GetFullPath($ExecutablePath)
    if ($ParametersFile) {
        if ($PSBoundParameters.ContainsKey('ParametersJson')) { throw 'Use ParametersJson or ParametersFile, not both.' }
        $ParametersJson = [IO.File]::ReadAllText((Resolve-Path -LiteralPath $ParametersFile).Path, [Text.Encoding]::UTF8)
    }
    $parameters = ConvertFrom-Json -InputObject $ParametersJson
    if ($null -eq $parameters -or $parameters -isnot [PSCustomObject]) { throw 'Parameters must be a JSON object.' }
    $requestId = [Guid]::NewGuid().ToString('N')
    $request = @{ jsonrpc = '2.0'; id = $requestId; method = $Method; params = $parameters } | ConvertTo-Json -Depth 64 -Compress
    $utf8 = New-Object Text.UTF8Encoding($false, $true)
    $payload = $utf8.GetBytes($request)
    if ($payload.Length -gt 1048576) { throw 'Request exceeds the 1 MiB frame limit.' }

    if (-not ('SnowDesktopControl.Native' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace SnowDesktopControl {
    public static class Native {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetNamedPipeServerProcessId(IntPtr pipe, out uint process);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, uint process);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder path, ref uint length);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        public static string ServerPath(IntPtr pipe) {
            uint processId;
            if (!GetNamedPipeServerProcessId(pipe, out processId)) throw new Win32Exception();
            IntPtr process = OpenProcess(0x1000, false, processId);
            if (process == IntPtr.Zero) throw new Win32Exception();
            try {
                uint length = 32768;
                var path = new StringBuilder((int)length);
                if (!QueryFullProcessImageName(process, 0, path, ref length)) throw new Win32Exception();
                return path.ToString();
            } finally { CloseHandle(process); }
        }
    }
}
'@
    }
    if (-not $PipeName) {
        $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
        $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
        $PipeName = "SnowDesktop.Control.v1.$session.$userSid"
    }
    if ($PipeName.Contains('\') -or $PipeName.Contains('/')) { throw 'PipeName must be a local pipe name.' }
    $pipe = New-Object IO.Pipes.NamedPipeClientStream('.', $PipeName,
        [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous,
        [Security.Principal.TokenImpersonationLevel]::Identification)
    $pipe.Connect($TimeoutMs)
    $serverPath = [SnowDesktopControl.Native]::ServerPath($pipe.SafePipeHandle.DangerousGetHandle())
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFullPath($serverPath), $expectedExecutable)) {
        throw "Unexpected IPC server: $serverPath. Expected: $expectedExecutable. No request was sent."
    }
    function Write-FrameBytes([byte[]]$Bytes) {
        $operation = $pipe.WriteAsync($Bytes, 0, $Bytes.Length)
        if (-not $operation.Wait($TimeoutMs)) { throw 'IPC write timed out; query state before retrying a mutation.' }
    }
    function Read-FrameBytes([int]$Count) {
        $buffer = New-Object byte[] $Count
        $offset = 0
        while ($offset -lt $Count) {
            $operation = $pipe.ReadAsync($buffer, $offset, $Count - $offset)
            if (-not $operation.Wait($TimeoutMs)) { throw 'IPC read timed out; query state before retrying a mutation.' }
            $received = $operation.Result
            if ($received -eq 0) { throw 'IPC disconnected; query state before retrying a mutation.' }
            $offset += $received
        }
        return ,$buffer
    }
    Write-FrameBytes ([BitConverter]::GetBytes([uint32]$payload.Length))
    Write-FrameBytes $payload
    $length = [BitConverter]::ToUInt32((Read-FrameBytes 4), 0)
    if ($length -eq 0 -or $length -gt 1048576) { throw 'Invalid IPC response length.' }
    $responseText = $utf8.GetString((Read-FrameBytes ([int]$length)))
    Write-FrameBytes ([byte[]]@(6))
    $response = ConvertFrom-Json -InputObject $responseText
    if ($response.jsonrpc -ne '2.0' -or $response.id -ne $requestId) { throw 'IPC response identity mismatch.' }
    [Console]::OutputEncoding = $utf8
    [Console]::WriteLine($responseText)
    if ($response.PSObject.Properties['error']) { exit 1 }
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
finally {
    if ($null -ne $pipe) { $pipe.Dispose() }
}
