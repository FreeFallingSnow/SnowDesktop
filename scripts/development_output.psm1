Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-SnowDesktopDevelopmentOutput {
    param([Parameter(Mandatory=$true)][string]$RepositoryRoot,
        [ValidateSet("Release","Debug")][string]$Configuration="Release")
    $presets = Get-Content -LiteralPath (Join-Path $RepositoryRoot "CMakePresets.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $build = @($presets.buildPresets | Where-Object { $_.name -ceq $Configuration.ToLowerInvariant() })
    if ($build.Count -ne 1) { throw "Missing unique development build preset: $Configuration" }
    $name = [string]$build[0].configurePreset
    $seen = [Collections.Generic.HashSet[string]]::new()
    while ($true) {
        if (-not $seen.Add($name)) { throw "Cyclic configure preset: $name" }
        $configure = @($presets.configurePresets | Where-Object { $_.name -ceq $name })
        if ($configure.Count -ne 1) { throw "Missing unique configure preset: $name" }
        if ($null -ne $configure[0].PSObject.Properties['binaryDir']) {
            $binary = [string]$configure[0].binaryDir
            break
        }
        if ($null -eq $configure[0].PSObject.Properties['inherits'] -or @($configure[0].inherits).Count -ne 1) {
            throw "Development configure preset must resolve a single binaryDir: $name"
        }
        $name = [string]$configure[0].inherits
    }
    $binary = $binary.Replace('${sourceDir}', $RepositoryRoot)
    if ($binary.Contains('${')) { throw "Unsupported development binaryDir macro: $binary" }
    if (-not [IO.Path]::IsPathRooted($binary)) { $binary = Join-Path $RepositoryRoot $binary }
    return [IO.Path]::GetFullPath((Join-Path $binary ([string]$build[0].configuration)))
}

function Get-SnowDesktopDevelopmentProcesses {
    param([Parameter(Mandatory=$true)][string]$Executable)
    $expected = [IO.Path]::GetFullPath($Executable)
    foreach ($candidate in @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($expected)) -ErrorAction SilentlyContinue)) {
        try {
            if ([IO.Path]::GetFullPath($candidate.Path).Equals($expected,[StringComparison]::OrdinalIgnoreCase)) {
                [pscustomobject]@{ Process=$candidate; StartTicks=$candidate.StartTime.ToUniversalTime().Ticks }
            }
        } catch { } # An inaccessible process is never a restart target.
    }
}

function Stop-SnowDesktopDevelopmentProcesses {
    param([Parameter(Mandatory=$true)][string]$Executable,[object[]]$Owners)
    $expected=[IO.Path]::GetFullPath($Executable)
    foreach ($owner in $Owners) {
        $process=$owner.Process
        $process.Refresh()
        if ($process.HasExited) { continue }
        if ($process.StartTime.ToUniversalTime().Ticks -ne $owner.StartTicks -or
            -not [IO.Path]::GetFullPath($process.Path).Equals($expected,[StringComparison]::OrdinalIgnoreCase)) {
            throw "Development host identity changed before restart."
        }
        if (-not $process.CloseMainWindow() -or -not $process.WaitForExit(10000)) {
            throw "The development host did not close normally; close it before restarting."
        }
    }
}

Export-ModuleMember -Function Get-SnowDesktopDevelopmentOutput, Get-SnowDesktopDevelopmentProcesses, Stop-SnowDesktopDevelopmentProcesses
