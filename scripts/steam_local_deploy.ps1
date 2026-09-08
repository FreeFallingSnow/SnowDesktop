<#
.SYNOPSIS
Stages a SnowDesktop payload as an isolated Steam local development runtime.

.DESCRIPTION
The default operation is a read-only dry-run. With -Apply, files are copied
through a private staging directory and atomically moved below
<Steam install>\.snowdesktop\dev\<BuildId>. The script never edits Steam app
manifests, depot state, the tracked distribution, or the selected runtime.

.EXAMPLE
.\scripts\steam_local_deploy.ps1 -PayloadDirectory <payload> -BuildId test-1

.EXAMPLE
.\scripts\steam_local_deploy.ps1 -PayloadDirectory <payload> -BuildId test-1 -Apply
#>
[CmdletBinding()]
param(
    [string]$PayloadDirectory = "",
    [string]$BuildId = "",
    [string]$ProfileId = "default",
    [string]$SteamRoot = "",
    [switch]$Apply,
    [switch]$Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [System.BitConverter]::ToString(
            $algorithm.ComputeHash($stream)).Replace("-", "")
    }
    finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path).TrimEnd("\")
}

function Test-SameOrDescendantPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $fullPath = Get-NormalizedPath -Path $Path
    $fullRoot = Get-NormalizedPath -Path $Root
    return $fullPath.Equals(
            $fullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith(
            "$fullRoot\", [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeDescendantPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $fullPath = Get-NormalizedPath -Path $Path
    $fullRoot = Get-NormalizedPath -Path $Root
    if ($fullPath.Equals(
            $fullRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
        -not $fullPath.StartsWith(
            "$fullRoot\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description must remain below $fullRoot`: $fullPath"
    }
}

function Get-VdfStringValue {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $pattern = '(?im)^\s*"' + [regex]::Escape($Name) +
        '"\s+"((?:\\.|[^"\\])*)"\s*$'
    $match = [regex]::Match($Text, $pattern)
    if (-not $match.Success) {
        return ""
    }
    return $match.Groups[1].Value.Replace('\\', '\')
}

function Get-SteamRoot {
    param([string]$RequestedRoot)

    if (-not [string]::IsNullOrWhiteSpace($RequestedRoot)) {
        $resolved = Get-NormalizedPath -Path $RequestedRoot
    }
    else {
        $registry = Get-ItemProperty -LiteralPath `
            "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue
        if ($null -eq $registry -or
            [string]::IsNullOrWhiteSpace([string]$registry.SteamPath)) {
            throw "SteamRoot was not supplied and SteamPath was not found in HKCU."
        }
        $resolved = Get-NormalizedPath -Path ([string]$registry.SteamPath)
    }
    if (-not (Test-Path -LiteralPath $resolved -PathType Container)) {
        throw "Steam root does not exist: $resolved"
    }
    return $resolved
}

function Get-SteamLibraryRoots {
    param([Parameter(Mandatory = $true)][string]$ClientRoot)

    $roots = [System.Collections.Generic.List[string]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    if ($seen.Add($ClientRoot)) {
        $roots.Add($ClientRoot)
    }

    $libraryFile = Join-Path $ClientRoot "steamapps\libraryfolders.vdf"
    if (-not (Test-Path -LiteralPath $libraryFile -PathType Leaf)) {
        return $roots.ToArray()
    }
    $content = Get-Content -LiteralPath $libraryFile -Encoding UTF8 -Raw
    foreach ($match in [regex]::Matches(
            $content, '(?im)^\s*"path"\s+"((?:\\.|[^"\\])*)"\s*$')) {
        $candidate = $match.Groups[1].Value.Replace('\\', '\')
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        $candidate = Get-NormalizedPath -Path $candidate
        if ($seen.Add($candidate)) {
            $roots.Add($candidate)
        }
    }
    return $roots.ToArray()
}

function Find-SnowDesktopSteamInstall {
    param(
        [Parameter(Mandatory = $true)][string]$ClientRoot,
        [Parameter(Mandatory = $true)][uint32]$AppId
    )

    foreach ($library in Get-SteamLibraryRoots -ClientRoot $ClientRoot) {
        $manifestPath = Join-Path $library "steamapps\appmanifest_$AppId.acf"
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
            continue
        }
        $manifestText = Get-Content -LiteralPath $manifestPath -Encoding UTF8 -Raw
        $manifestAppId = Get-VdfStringValue -Text $manifestText -Name "appid"
        if ($manifestAppId -cne [string]$AppId) {
            throw "Steam app manifest has an unexpected App ID: $manifestPath"
        }
        $installName = Get-VdfStringValue -Text $manifestText -Name "installdir"
        if ([string]::IsNullOrWhiteSpace($installName) -or
            [System.IO.Path]::GetFileName($installName) -cne $installName -or
            $installName -in @(".", "..")) {
            throw "Steam app manifest has an unsafe install directory: $manifestPath"
        }
        if ($installName -ine "SnowDesktop") {
            throw "Steam app manifest does not target the SnowDesktop install directory: $manifestPath"
        }
        $installRoot = Get-NormalizedPath -Path `
            (Join-Path $library "steamapps\common\$installName")
        $commonRoot = Get-NormalizedPath -Path `
            (Join-Path $library "steamapps\common")
        Assert-SafeDescendantPath -Path $installRoot -Root $commonRoot `
            -Description "Steam install directory"
        return [pscustomobject]@{
            LibraryRoot = $library
            ManifestPath = $manifestPath
            InstallRoot = $installRoot
            BuildId = Get-VdfStringValue -Text $manifestText -Name "buildid"
        }
    }
    throw "Steam does not have an app manifest for SnowDesktop App ID $AppId. Install or register the app in the Steam client first."
}

function Get-DefaultBuildId {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryRoot,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $commit = (& git -C $RepositoryRoot rev-parse --short=12 HEAD 2>$null |
        Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace([string]$commit)) {
        $commit = "unknown"
    }
    else {
        $commit = ([string]$commit).Trim()
    }
    $dirty = @(& git -C $RepositoryRoot status --porcelain `
        --untracked-files=no 2>$null).Count -ne 0
    $dirtySuffix = if ($dirty) { "-dirty" } else { "" }
    $timestamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddHHmmss")
    return "v$Version-$commit$dirtySuffix-$timestamp"
}

function Get-PayloadFiles {
    param([Parameter(Mandatory = $true)][string]$PayloadRoot)

    $payloadItem = Get-Item -LiteralPath $PayloadRoot
    if (($payloadItem.Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Local development payload root must not be a reparse point: $PayloadRoot"
    }
    $items = @(Get-ChildItem -LiteralPath $PayloadRoot -Recurse -Force)
    $reparsePoints = @($items | Where-Object {
        ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0
    })
    if ($reparsePoints.Count -ne 0) {
        throw "Local development payload must not contain reparse points: $($reparsePoints[0].FullName)"
    }

    $files = @($items | Where-Object { -not $_.PSIsContainer })
    if ($files.Count -eq 0) {
        throw "Local development payload is empty: $PayloadRoot"
    }
    $payloadPrefix = (Get-NormalizedPath -Path $PayloadRoot) + "\"
    $relativeFiles = foreach ($file in $files) {
        $fullFile = Get-NormalizedPath -Path $file.FullName
        if (-not $fullFile.StartsWith(
                $payloadPrefix,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Local development payload file escapes its root: $fullFile"
        }
        # PowerShell 5.1 uses .NET Framework, which has no Path.GetRelativePath.
        # Enumeration already proved the file is a descendant, so a checked
        # prefix trim is deterministic and compatible with every supported host.
        $relativePath = $fullFile.Substring($payloadPrefix.Length).Replace(
            "/", "\")
        $segments = @($relativePath.Split(
            "\", [System.StringSplitOptions]::RemoveEmptyEntries))
        $leaf = [System.IO.Path]::GetFileName($relativePath)
        if ($relativePath -ieq "SnowDesktop.runtime-context.json") {
            # Deployment identity belongs to the destination layout. A Steam
            # managed or portable source context must never leak into a local
            # development runtime.
            continue
        }
        if ($segments.Count -eq 0 -or
            $segments[0] -iin @("data", ".snowdesktop") -or
            $segments -icontains ".git" -or
            $leaf -ieq "steam_appid.txt" -or
            $leaf -ieq "libraryfolders.vdf" -or
            $leaf -ieq "steam_dev.cfg" -or
            $leaf -imatch '^appmanifest_\d+\.acf$') {
            throw "Local development payload contains deployment state or user data: $relativePath"
        }
        [pscustomobject]@{
            Source = $file.FullName
            RelativePath = $relativePath
            Length = [int64]$file.Length
        }
    }
    $executable = Join-Path $PayloadRoot "SnowDesktop.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Local development payload must contain SnowDesktop.exe at its root."
    }
    return @($relativeFiles | Sort-Object RelativePath)
}

function Write-Result {
    param(
        [Parameter(Mandatory = $true)]$Result,
        [switch]$AsJson
    )

    if ($AsJson) {
        $Result | ConvertTo-Json -Depth 5
        return
    }
    $verb = if ($Result.applied) { "staged" } else { "would stage" }
    Write-Host "Steam local development payload $verb at: $($Result.destination)"
    Write-Host "Deployment kind: steam-local-dev (never steam-managed)"
    Write-Host "Steam app manifest and active runtime selection were not modified."
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = Get-NormalizedPath -Path (Join-Path $scriptDirectory "..")
$identity = Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "packaging\steam-identity.json") `
    -Encoding UTF8 -Raw | ConvertFrom-Json
$appId = [uint32]$identity.appId
$depotId = [uint32]$identity.windowsDepotId
if ($appId -eq 0 -or $depotId -eq 0) {
    throw "packaging\steam-identity.json contains an invalid Steam identity."
}
$version = [string](Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "version.json") -Encoding UTF8 -Raw |
    ConvertFrom-Json).version

if ([string]::IsNullOrWhiteSpace($PayloadDirectory)) {
    $PayloadDirectory = Join-Path $repositoryRoot `
        "artifacts\v$version\steam\SnowDesktop"
}
$payloadRoot = Get-NormalizedPath -Path $PayloadDirectory
if (-not (Test-Path -LiteralPath $payloadRoot -PathType Container)) {
    throw "Steam payload directory does not exist: $payloadRoot. Generate it first or pass -PayloadDirectory."
}
$clientRoot = Get-SteamRoot -RequestedRoot $SteamRoot
$steamInstall = Find-SnowDesktopSteamInstall -ClientRoot $clientRoot `
    -AppId $appId
$installRoot = $steamInstall.InstallRoot
if (-not (Test-Path -LiteralPath $installRoot -PathType Container)) {
    throw "Steam registered SnowDesktop, but its install directory does not exist: $installRoot"
}
if (Test-SameOrDescendantPath -Path $payloadRoot -Root $installRoot) {
    throw "PayloadDirectory must be outside the Steam SnowDesktop install directory."
}

$payloadFiles = @(Get-PayloadFiles -PayloadRoot $payloadRoot)
$payloadExecutable = Get-Item -LiteralPath `
    (Join-Path $payloadRoot "SnowDesktop.exe")
$payloadVersion = [string]$payloadExecutable.VersionInfo.ProductVersion
if ($payloadVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    $payloadVersion = $version
}
if ([string]::IsNullOrWhiteSpace($BuildId)) {
    $BuildId = Get-DefaultBuildId -RepositoryRoot $repositoryRoot `
        -Version $payloadVersion
}
if ($BuildId -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$' -or
    $BuildId.EndsWith(".", [System.StringComparison]::Ordinal) -or
    $BuildId -imatch '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') {
    throw "BuildId must be a safe 1-96 character Windows filename using ASCII letters, digits, dots, underscores, or hyphens."
}
if ($ProfileId -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$' -or
    $ProfileId.EndsWith(".", [System.StringComparison]::Ordinal) -or
    $ProfileId -imatch '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') {
    throw "ProfileId must be a safe 1-96 character identifier using ASCII letters, digits, dots, underscores, or hyphens."
}
$totalBytes = [int64](($payloadFiles | Measure-Object Length -Sum).Sum)
$ownedRoot = Join-Path $installRoot ".snowdesktop"
$devRoot = Join-Path $ownedRoot "dev"
$stagingRoot = Join-Path $ownedRoot "staging"
$destination = Get-NormalizedPath -Path (Join-Path $devRoot $BuildId)
$staging = Get-NormalizedPath -Path (Join-Path $stagingRoot `
    ("dev-$BuildId-" + [guid]::NewGuid().ToString("N")))
Assert-SafeDescendantPath -Path $destination -Root $devRoot `
    -Description "Local development destination"
Assert-SafeDescendantPath -Path $staging -Root $stagingRoot `
    -Description "Local development staging directory"

$baseResult = [ordered]@{
    schemaVersion = 1
    deploymentKind = "steam-local-dev"
    steamManaged = $false
    changesActiveDeployment = $false
    appId = $appId
    depotId = $depotId
    steamManifestBuildId = [string]$steamInstall.BuildId
    buildId = $BuildId
    profileId = $ProfileId
    version = $payloadVersion
    source = $payloadRoot
    installRoot = $installRoot
    destination = $destination
    fileCount = $payloadFiles.Count
    totalBytes = $totalBytes
    applied = [bool]$Apply
}

if (-not $Apply) {
    Write-Result -Result ([pscustomobject]$baseResult) -AsJson:$Json
    return
}
if (Test-Path -LiteralPath $destination) {
    throw "Local development build already exists and will not be overwritten: $destination"
}

try {
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    $contentEntries = foreach ($file in $payloadFiles) {
        $target = Get-NormalizedPath -Path `
            (Join-Path $staging $file.RelativePath)
        Assert-SafeDescendantPath -Path $target -Root $staging `
            -Description "Payload target"
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) `
            -Force | Out-Null
        Copy-Item -LiteralPath $file.Source -Destination $target
        $sourceHash = Get-Sha256 -Path $file.Source
        $targetHash = Get-Sha256 -Path $target
        if ($sourceHash -cne $targetHash) {
            throw "Copied payload hash mismatch: $($file.RelativePath)"
        }
        [ordered]@{
            path = $file.RelativePath.Replace("\", "/")
            size = $file.Length
            sha256 = $sourceHash
        }
    }

    $createdAt = (Get-Date).ToUniversalTime().ToString("o")
    $contentManifest = [ordered]@{
        schemaVersion = 1
        deploymentKind = "steam-local-dev"
        buildId = $BuildId
        profileId = $ProfileId
        version = $payloadVersion
        createdAt = $createdAt
        files = @($contentEntries)
    }
    $contentManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath `
        (Join-Path $staging "content-manifest.json") -Encoding UTF8
    $runtimeContext = [ordered]@{
        schemaVersion = 1
        kind = "steam-local-dev"
        installRootRelative = "../../.."
        dataRootRelative = ".snowdesktop/dev-data/$ProfileId"
        launcherRelative = ".snowdesktop/dev/$BuildId/SnowDesktop.exe"
        profileId = $ProfileId
    }
    $runtimeContext | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath `
        (Join-Path $staging "SnowDesktop.runtime-context.json") `
        -Encoding UTF8

    New-Item -ItemType Directory -Path $devRoot -Force | Out-Null
    if (Test-Path -LiteralPath $destination) {
        throw "Local development build appeared during staging and will not be overwritten: $destination"
    }
    Assert-SafeDescendantPath -Path $staging -Root $ownedRoot `
        -Description "Move source"
    Assert-SafeDescendantPath -Path $destination -Root $ownedRoot `
        -Description "Move destination"
    Move-Item -LiteralPath $staging -Destination $destination
}
catch {
    if (Test-Path -LiteralPath $staging) {
        Assert-SafeDescendantPath -Path $staging -Root $stagingRoot `
            -Description "Failed staging cleanup target"
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    throw
}

Write-Result -Result ([pscustomobject]$baseResult) -AsJson:$Json
