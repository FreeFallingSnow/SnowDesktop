[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildOutput,
    [switch]$AllowMissingFirstPartyRuntime
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Build layout paths must be non-empty and relative: $RelativePath"
    }
    $normalized = $RelativePath.Replace("/", "\")
    $parts = @($normalized.Split(
        "\", [System.StringSplitOptions]::RemoveEmptyEntries))
    if ($parts.Count -eq 0 -or $parts -contains "." -or
        $parts -contains "..") {
        throw "Build layout path contains traversal: $RelativePath"
    }
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd("\") + "\"
    $fullPath = [System.IO.Path]::GetFullPath(
        (Join-Path $fullRoot ($parts -join "\")))
    if (-not $fullPath.StartsWith(
            $fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Build layout path escapes its root: $RelativePath"
    }
    return $fullPath
}

function Get-DeploymentSourcePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)]$Entry
    )

    $relativePath = if ($null -ne $Entry.PSObject.Properties["buildPath"] -and
        -not [string]::IsNullOrWhiteSpace([string]$Entry.buildPath)) {
        [string]$Entry.buildPath
    }
    else {
        [string]$Entry.path
    }
    return Resolve-ContainedPath -Root $Root -RelativePath $relativePath
}

function Copy-RequiredRuntimeFile {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$ExistingRuntimeRoot,
        [Parameter(Mandatory = $true)][string]$StagingRoot,
        [switch]$Optional
    )

    $source = Join-Path $Root $Name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        $source = Join-Path $ExistingRuntimeRoot $Name
    }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        if ($Optional) {
            return $false
        }
        throw "Required SnowDesktop runtime file is missing: $Name"
    }
    Copy-Item -LiteralPath $source `
        -Destination (Join-Path $StagingRoot $Name) -Force
    return $true
}

function Remove-EmptyBuildParents {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string[]]$Paths
    )

    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd("\")
    $prefix = $fullRoot + "\"
    foreach ($start in @($Paths | Sort-Object Length -Descending -Unique)) {
        $current = [System.IO.Path]::GetFullPath($start)
        while ($current.StartsWith(
                $prefix, [System.StringComparison]::OrdinalIgnoreCase) -and
            $current -cne $fullRoot -and
            (Test-Path -LiteralPath $current -PathType Container) -and
            [System.IO.Directory]::GetFileSystemEntries($current).Count -eq 0) {
            Remove-Item -LiteralPath $current -Force
            $current = Split-Path -Parent $current
        }
    }
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $scriptDirectory ".."))
$BuildOutput = [System.IO.Path]::GetFullPath($BuildOutput)
$allowedRelease = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot ".build\Release"))
$allowedDebug = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot ".build_debug\Debug"))
if (-not [string]::Equals(
        $BuildOutput, $allowedRelease,
        [System.StringComparison]::OrdinalIgnoreCase) -and
    -not [string]::Equals(
        $BuildOutput, $allowedDebug,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Build output arrangement is limited to the repository Release and Debug output directories: $BuildOutput"
}

$version = (Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "version.json") -Encoding UTF8 -Raw |
    ConvertFrom-Json).version
if ($version -notmatch "^[1-9][0-9]*\.[0-9]+\.[0-9]+\.0$") {
    throw "version.json must use Store-compatible A.B.C.0 format."
}
$application = Join-Path $BuildOutput "SnowDesktop.exe"
if (-not (Test-Path -LiteralPath $application -PathType Leaf)) {
    throw "SnowDesktop build executable was not found: $application"
}

$runtimeDirectory = "SnowDesktop.Runtime"
$runtimeRoot = Join-Path $BuildOutput $runtimeDirectory
$deploymentModule = Join-Path $scriptDirectory "deployment_payload.psm1"
Import-Module $deploymentModule -Force
$deployment = Read-SnowDesktopDeploymentManifest -BuildOutput $BuildOutput

$stagingParent = Join-Path $BuildOutput ".deployment\build-layout"
$stagingRoot = Join-Path $stagingParent ([guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
$rootRuntimeSources = [System.Collections.Generic.List[string]]::new()
try {
    foreach ($entry in @($deployment.files)) {
        if ([string]$entry.source -ceq "SnowDesktop") {
            continue
        }
        $source = Get-DeploymentSourcePath `
            -Root $BuildOutput -Entry $entry
        $target = Resolve-ContainedPath `
            -Root $stagingRoot -RelativePath ([string]$entry.path)
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) `
            -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
        $rootSource = Resolve-ContainedPath `
            -Root $BuildOutput -RelativePath ([string]$entry.path)
        if (Test-Path -LiteralPath $rootSource -PathType Leaf) {
            $rootRuntimeSources.Add($rootSource)
        }
    }

    $existingPrivateManifest = Join-Path $runtimeRoot `
        "$runtimeDirectory.manifest"
    if (Test-Path -LiteralPath $existingPrivateManifest -PathType Leaf) {
        Copy-Item -LiteralPath $existingPrivateManifest `
            -Destination (Join-Path $stagingRoot `
                "$runtimeDirectory.manifest") -Force
    }

    $firstPartyRuntimeFiles = @(
        "SnowDesktopTaskbarHook.dll",
        "SnowDesktopWallpaperHook.dll",
        "SnowDesktopWallpaperHook32.dll",
        "SnowDesktopWallpaperInjector32.exe"
    )
    foreach ($name in $firstPartyRuntimeFiles) {
        $present = Copy-RequiredRuntimeFile `
            -Name $name -Root $BuildOutput `
            -ExistingRuntimeRoot $runtimeRoot -StagingRoot $stagingRoot `
            -Optional:$AllowMissingFirstPartyRuntime
        if (-not $present -and -not $AllowMissingFirstPartyRuntime) {
            throw "Required SnowDesktop runtime file is missing: $name"
        }
    }
    $additionalRuntimeDlls = [System.Collections.Generic.List[string]]::new()
    foreach ($name in @("Microsoft.WindowsAppRuntime.Bootstrap.dll")) {
        if (Copy-RequiredRuntimeFile `
                -Name $name -Root $BuildOutput `
                -ExistingRuntimeRoot $runtimeRoot -StagingRoot $stagingRoot `
                -Optional) {
            $additionalRuntimeDlls.Add($name)
        }
    }
    $hasSteamRuntime = Copy-RequiredRuntimeFile `
        -Name "steam_api64.dll" -Root $BuildOutput `
        -ExistingRuntimeRoot $runtimeRoot -StagingRoot $stagingRoot -Optional
    if ($hasSteamRuntime) {
        $additionalRuntimeDlls.Add("steam_api64.dll")
    }

    if (Test-Path -LiteralPath $runtimeRoot -PathType Container) {
        Remove-Item -LiteralPath $runtimeRoot -Recurse -Force
    }
    Move-Item -LiteralPath $stagingRoot -Destination $runtimeRoot

    $privateAssemblyArguments = @{
        BuildOutput = $BuildOutput
        PackageRoot = $BuildOutput
        Version = $version
        RuntimeDirectory = $runtimeDirectory
    }
    if ($additionalRuntimeDlls.Count -ne 0) {
        $privateAssemblyArguments.AdditionalRuntimeDlls = `
            $additionalRuntimeDlls.ToArray()
    }
    if ($hasSteamRuntime) {
        $privateAssemblyArguments.AdditionalExecutables = @(
            "SnowDesktopSteamBridge.exe",
            "SnowDesktopWorkshopManager.exe")
    }
    Enable-SnowDesktopPrivateRuntimeAssembly @privateAssemblyArguments

    foreach ($entry in @($deployment.files)) {
        if ([string]$entry.source -ceq "SnowDesktop") {
            if ($null -ne $entry.PSObject.Properties["buildPath"]) {
                $entry.PSObject.Properties.Remove("buildPath")
            }
            continue
        }
        $buildPath = "$runtimeDirectory/$([string]$entry.path)"
        if ($null -eq $entry.PSObject.Properties["buildPath"]) {
            $entry | Add-Member -NotePropertyName "buildPath" `
                -NotePropertyValue $buildPath
        }
        else {
            $entry.buildPath = $buildPath
        }
    }
    $manifestPath = Join-Path $BuildOutput "SnowDesktop.deployment.json"
    $temporaryManifest = "$manifestPath.$([guid]::NewGuid().ToString('N')).tmp"
    [System.IO.File]::WriteAllText(
        $temporaryManifest,
        ($deployment | ConvertTo-Json -Depth 20),
        [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporaryManifest `
        -Destination $manifestPath -Force

    $emptyDirectoryCandidates = [System.Collections.Generic.List[string]]::new()
    foreach ($source in $rootRuntimeSources) {
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Remove-Item -LiteralPath $source -Force
            $emptyDirectoryCandidates.Add((Split-Path -Parent $source))
        }
    }
    foreach ($name in $firstPartyRuntimeFiles +
            $additionalRuntimeDlls.ToArray()) {
        $source = Join-Path $BuildOutput $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Remove-Item -LiteralPath $source -Force
        }
    }
    Remove-EmptyBuildParents `
        -Root $BuildOutput -Paths $emptyDirectoryCandidates.ToArray()

    $rootDlls = @(Get-ChildItem -LiteralPath $BuildOutput -File -Filter "*.dll")
    if ($rootDlls.Count -ne 0) {
        throw "Build output still contains root-level DLLs: $($rootDlls.Name -join ', ')"
    }
    Write-Host "Build runtime arranged under $runtimeDirectory ($(@(Get-ChildItem -LiteralPath $runtimeRoot -File -Recurse).Count) files)."
}
finally {
    if (Test-Path -LiteralPath $stagingRoot -PathType Container) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
}
