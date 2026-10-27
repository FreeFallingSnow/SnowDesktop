Set-StrictMode -Version Latest

function Get-SnowDesktopTraceProcessor([switch]$Offline) {
    $digest = 'adfa6bad3d72be3ba9b83fa2b17b69fa13b3ab1cad0f42e52b86188bd5f0f997'
    $url = 'https://commondatastorage.googleapis.com/perfetto-luci-artifacts/v58.2/windows-amd64/trace_processor_shell.exe'
    $cache = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'SnowDesktop/tools/perfetto/v58.2'
    $path = Join-Path $cache 'trace_processor_shell.exe'
    if ($env:SNOWDESKTOP_TRACE_PROCESSOR) { $path = [IO.Path]::GetFullPath($env:SNOWDESKTOP_TRACE_PROCESSOR) }
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) {
            throw 'traceProcessorHashMismatch: expected official Perfetto v58.2 Windows x64 analyzer.'
        }
        return $path
    }
    if ($Offline -or $env:SNOWDESKTOP_TRACE_PROCESSOR) { throw 'traceProcessorUnavailable: run profile.bat setup online, or provide the pinned executable through SNOWDESKTOP_TRACE_PROCESSOR.' }
    [IO.Directory]::CreateDirectory($cache) | Out-Null
    $partial = Join-Path $cache ('download-' + [Guid]::NewGuid().ToString('N') + '.partial')
    try {
        # Download only the public executable. Capture data never leaves the machine.
        Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing -TimeoutSec 90
        if ((Get-Item -LiteralPath $partial).Length -ne 14439936 -or
            (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) {
            throw 'traceProcessorDownloadIntegrityFailed'
        }
        try { [IO.File]::Move($partial, $path) }
        catch { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw } }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) { throw 'traceProcessorHashMismatch' }
        return $path
    }
    finally { if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial } }
}
