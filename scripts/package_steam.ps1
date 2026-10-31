[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$ReloadShell,
    [string]$OutputDirectory = "",
    [string]$BuildOutputDirectory = "",
    [string]$ArtifactRepositoryRoot = "",
    [ValidatePattern('^[a-f0-9]{32}$')][string]$ArtifactRun = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $algorithm.ComputeHash($stream)
        return [System.BitConverter]::ToString($bytes).Replace("-", "")
    }
    finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][string]$Text)

    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $digest = $algorithm.ComputeHash($bytes)
        return [System.BitConverter]::ToString($digest).Replace("-", "")
    }
    finally {
        $algorithm.Dispose()
    }
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $scriptDirectory ".."))
$runtimeDirectory = "SnowDesktop.Runtime"
Import-Module (Join-Path $scriptDirectory "deployment_payload.psm1") -Force
Import-Module (Join-Path $scriptDirectory "steam_packaging_paths.psm1") -Force
if ($ArtifactRun) {
    if ($ReloadShell) { throw '-ArtifactRun cannot be combined with -ReloadShell.' }
    $SkipBuild = $true
}
if ((Test-SnowDesktopFrozenPackagingSource -Path $repositoryRoot) -and (-not $SkipBuild -or -not $ArtifactRepositoryRoot)) {
    throw 'Frozen candidate packaging requires -SkipBuild and an explicit -ArtifactRepositoryRoot outside the frozen source.'
}
if ($SkipBuild -and $ReloadShell) { throw '-ReloadShell cannot be combined with -SkipBuild.' }
if ($BuildOutputDirectory -and -not $ArtifactRun) { throw 'Explicit build input requires a matching -ArtifactRun.' }
. (Join-Path $scriptDirectory 'packaging_input.ps1')
$originalRepositoryRoot = if ($ArtifactRepositoryRoot) { $ArtifactRepositoryRoot } else { $repositoryRoot }
if (-not $SkipBuild) { $ArtifactRun = Invoke-SnowDesktopPackagingBuild $originalRepositoryRoot }
$packagingContext = Get-SnowDesktopPackagingContext $originalRepositoryRoot $ArtifactRun
try {
if ($BuildOutputDirectory -and [IO.Path]::GetFullPath($BuildOutputDirectory).TrimEnd('\','/') -ne $packagingContext.runtime) {
    throw '-BuildOutputDirectory must match the selected saved ArtifactRun; unbound packaging inputs are rejected.'
}
$repositoryRoot = $packagingContext.source
$version = $packagingContext.version
$buildOutput = $packagingContext.runtime
# Recompute from the captured version, not the live checkout's version.
$requestedOutput = if ($PSBoundParameters.ContainsKey('OutputDirectory')) { $PSBoundParameters['OutputDirectory'] } else { '' }
$paths = Resolve-SnowDesktopSteamPackagingPaths `
    -SourceRepositoryRoot $originalRepositoryRoot -Version $version `
    -BuildOutputDirectory $buildOutput -ArtifactRepositoryRoot $originalRepositoryRoot `
    -OutputDirectory $requestedOutput -SkipBuild
$versionRoot = $paths.versionRoot
$OutputDirectory = $paths.outputDirectory
if ($repositoryRoot -eq $OutputDirectory -or $repositoryRoot.StartsWith($OutputDirectory+'\',[StringComparison]::OrdinalIgnoreCase) -or
    $OutputDirectory.StartsWith($repositoryRoot+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Steam staging must not overlap its saved packaging source.'
}
$steamIdentity = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging/steam-identity.json') -Raw | ConvertFrom-Json
$steamAppId = [uint32]$steamIdentity.appId
$windowsDepotId = [uint32]$steamIdentity.windowsDepotId
if ($steamAppId -eq 0 -or $windowsDepotId -eq 0) { throw 'Invalid saved Steam identity.' }

$runtimeFiles = @(
    "SnowDesktopHookEntry.dll",
    "SnowDesktopHookEntry32.dll",
    "SnowDesktopTaskbarHook.dll",
    "SnowDesktopWindowHelper.exe",
    "SnowDesktopDockMinimizeHook.dll",
    "SnowDesktopDockMinimizeHook32.dll",
    "SnowDesktopDockMinimizeHost32.exe",
    "SnowDesktopWallpaperHook.dll",
    "SnowDesktopWallpaperHook32.dll",
    "SnowDesktopWallpaperInjector32.exe",
    "steam_api64.dll"
)
$required = @(
    "SnowDesktop.exe",
    "SnowDesktopHookEntry.dll",
    "SnowDesktopHookEntry32.dll",
    "SnowDesktopTaskbarHook.dll",
    "SnowDesktopWindowHelper.exe",
    "SnowDesktopDockMinimizeHook.dll",
    "SnowDesktopDockMinimizeHook32.dll",
    "SnowDesktopDockMinimizeHost32.exe",
    "SnowDesktopWallpaperHook.dll",
    "SnowDesktopWallpaperHook32.dll",
    "SnowDesktopWallpaperInjector32.exe",
    "SnowDesktopWorkshopManager.exe",
    "SnowDesktopSteamBridge.exe",
    "snowwidget.exe",
    "steam_api64.dll",
    "SnowDesktopSteamBridge-LICENSE.txt",
    "SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md"
)
foreach ($name in $required) {
    $path = if ($runtimeFiles -contains $name) {
        Join-Path (Join-Path $buildOutput $runtimeDirectory) $name
    }
    else {
        Join-Path $buildOutput $name
    }
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required Steam payload file is missing: $path"
    }
}
$launcherSource = Join-Path $buildOutput "SnowDesktopLauncher.exe"
if (-not (Test-Path -LiteralPath $launcherSource -PathType Leaf)) {
    throw "Required Steam launcher is missing: $launcherSource"
}

foreach ($name in @(
        "SnowDesktopWorkshopManager.exe",
        "SnowDesktopSteamBridge.exe")) {
    $path = Join-Path $buildOutput $name
    $ascii = [System.Text.Encoding]::ASCII.GetString(
        [System.IO.File]::ReadAllBytes($path))
    if (-not $ascii.Contains("steam_api64.dll")) {
        throw "$name is an SDK-free build. Configure the external Steamworks SDK before packaging."
    }
}
$bundledSkillCli = Join-Path $buildOutput `
    "widgets\snowdesktop-lua-widget\bin\snowwidget.exe"
if (-not (Test-Path -LiteralPath $bundledSkillCli -PathType Leaf)) {
    throw "Built Agent Skill CLI is missing: $bundledSkillCli"
}
if ((Get-Sha256 -Path $bundledSkillCli) -ne
    (Get-Sha256 -Path (Join-Path $buildOutput "snowwidget.exe"))) {
    throw "The Agent Skill CLI does not match the standalone snowwidget.exe."
}
$bundledSkillBin = Split-Path -Parent $bundledSkillCli
$bundledSkillPublisherFiles = @(
    "SnowDesktopSteamBridge.exe",
    "steam_api64.dll",
    "SnowDesktopSteamBridge-LICENSE.txt",
    "SnowDesktopSteamBridge-THIRD-PARTY-NOTICES.md"
)
foreach ($name in $bundledSkillPublisherFiles) {
    $path = Join-Path $bundledSkillBin $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Built Agent Skill Workshop CLI file is missing: $path"
    }
}
if ((Get-Sha256 -Path (Join-Path $bundledSkillBin `
            "SnowDesktopSteamBridge.exe")) -ne
        (Get-Sha256 -Path (Join-Path $buildOutput `
            "SnowDesktopSteamBridge.exe")) -or
    (Get-Sha256 -Path (Join-Path $bundledSkillBin `
            "steam_api64.dll")) -ne
        (Get-Sha256 -Path (Join-Path (Join-Path $buildOutput `
            $runtimeDirectory) "steam_api64.dll"))) {
    throw "The Agent Skill Workshop CLI or Steam runtime is stale."
}

$bridgePath = Join-Path $buildOutput "SnowDesktopSteamBridge.exe"
$configurationText = & $bridgePath configuration
if ($LASTEXITCODE -ne 0) {
    throw "Steam bridge configuration query failed with exit code $LASTEXITCODE."
}
$configuration = $configurationText | ConvertFrom-Json
if (-not $configuration.ok -or
    -not $configuration.steamworksCompiled -or
    [uint32]$configuration.expectedAppId -ne $steamAppId -or
    [uint32]$configuration.windowsDepotId -ne $windowsDepotId) {
    throw "Steam bridge identity does not match packaging\steam-identity.json."
}

$payload = Join-Path $OutputDirectory "SnowDesktop"
if (Test-Path -LiteralPath $OutputDirectory) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $payload -Force | Out-Null
$distribution = Join-Path $payload "distribution"
New-Item -ItemType Directory -Path $distribution -Force | Out-Null
$runtimeRoot = Join-Path $distribution $runtimeDirectory
New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
foreach ($name in $required) {
    $destination = if ($runtimeFiles -contains $name) {
        Join-Path $runtimeRoot $name
    }
    else {
        Join-Path $distribution $name
    }
    $source = if ($runtimeFiles -contains $name) {
        Join-Path (Join-Path $buildOutput $runtimeDirectory) $name
    }
    else {
        Join-Path $buildOutput $name
    }
    Copy-Item -LiteralPath $source `
        -Destination $destination -Force
}
foreach ($name in @("LICENSE", "THIRD_PARTY_NOTICES.md", "README.md", "README.en.md")) {
    Copy-Item -LiteralPath (Join-Path $repositoryRoot $name) `
        -Destination (Join-Path $distribution $name) -Force
}
Copy-Item -LiteralPath (Join-Path $buildOutput "widgets") `
    -Destination (Join-Path $distribution "widgets") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $buildOutput "lang") `
    -Destination (Join-Path $distribution "lang") -Recurse -Force
$null = Copy-SnowDesktopDeploymentPayload `
    -BuildOutput $buildOutput `
    -Destination $distribution `
    -RuntimeDirectory $runtimeDirectory
Enable-SnowDesktopPrivateRuntimeAssembly `
    -BuildOutput $buildOutput `
    -PackageRoot $distribution `
    -Version $version `
    -RuntimeDirectory $runtimeDirectory `
    -AdditionalRuntimeDlls @("steam_api64.dll") `
    -AdditionalExecutables @(
        "SnowDesktopSteamBridge.exe",
        "SnowDesktopWorkshopManager.exe")
$packagedConfigurationText = & (Join-Path $distribution `
    "SnowDesktopSteamBridge.exe") configuration
if ($LASTEXITCODE -ne 0) {
    throw "Packaged Steam bridge could not load its private runtime (exit $LASTEXITCODE)."
}
$packagedConfiguration = $packagedConfigurationText | ConvertFrom-Json
if (-not $packagedConfiguration.ok -or
    -not $packagedConfiguration.steamworksCompiled -or
    [uint32]$packagedConfiguration.expectedAppId -ne $steamAppId) {
    throw "Packaged Steam bridge private runtime validation failed."
}
$licensesDestination = Join-Path $distribution "licenses"
Copy-SnowDesktopRepositoryLicenses `
    -RepositoryRoot $repositoryRoot `
    -Destination $licensesDestination
$developerAssets = @(Get-ChildItem -LiteralPath $distribution -Recurse -Force |
    Where-Object {
        $_.FullName.Substring($distribution.Length).TrimStart('\') `
            -match '(^|\\)developer_assets(\\|$)'
    })
if ($developerAssets.Count -ne 0) {
    throw "Steam payload contains developer-only assets: $($developerAssets.FullName -join ', ')"
}
$payloadSkillBin = Join-Path $distribution `
    "widgets\snowdesktop-lua-widget\bin"
New-Item -ItemType Directory -Path $payloadSkillBin -Force | Out-Null
Copy-Item -LiteralPath $bundledSkillCli `
    -Destination (Join-Path $payloadSkillBin "snowwidget.exe") -Force
foreach ($name in $bundledSkillPublisherFiles) {
    Copy-Item -LiteralPath (Join-Path $bundledSkillBin $name) `
        -Destination (Join-Path $payloadSkillBin $name) -Force
}

$forbidden = @(Get-ChildItem -LiteralPath $payload -Recurse -File |
    Where-Object {
        $_.Name -ieq "steam_appid.txt" -or
        $_.Extension -in @(".lib", ".h", ".hpp") -or
        $_.Name -match "^(steam_api|isteam).+\.(h|hpp|lib)$"
    })
if ($forbidden.Count -ne 0) {
    throw "Steam payload contains forbidden SDK material: $($forbidden.FullName -join ', ')"
}
$forbiddenDirectory = @(Get-ChildItem -LiteralPath $payload -Recurse -Directory |
    Where-Object {
        $_.Name -match "^(steamworks_sdk|redistributable_bin|sdk)$"
    })
if ($forbiddenDirectory.Count -ne 0) {
    throw "Steam payload contains an SDK directory: $($forbiddenDirectory.FullName -join ', ')"
}
$steamDlls = @(Get-ChildItem -LiteralPath $payload -Recurse -File |
    Where-Object { $_.Name -like "steam_api*.dll" })
$expectedSteamDlls = @(
    (Join-Path (Join-Path $distribution $runtimeDirectory) `
        "steam_api64.dll"),
    (Join-Path $payloadSkillBin "steam_api64.dll")
)
if ($steamDlls.Count -ne $expectedSteamDlls.Count -or
    @($expectedSteamDlls | Where-Object {
        -not (Test-Path -LiteralPath $_ -PathType Leaf)
    }).Count -ne 0) {
    throw "The payload must contain only the two permitted steam_api64.dll runtime copies."
}
$payloadSkillCli = Join-Path $distribution `
    "widgets\snowdesktop-lua-widget\bin\snowwidget.exe"
if (-not (Test-Path -LiteralPath $payloadSkillCli -PathType Leaf) -or
    (Get-Sha256 -Path $payloadSkillCli) -ne
        (Get-Sha256 -Path (Join-Path $distribution "snowwidget.exe"))) {
    throw "The Steam payload contains an unavailable or stale Agent Skill CLI."
}
$payloadSkillPublisher = Join-Path $payloadSkillBin `
    "SnowDesktopSteamBridge.exe"
if (-not (Test-Path -LiteralPath $payloadSkillPublisher -PathType Leaf) -or
    (Get-Sha256 -Path $payloadSkillPublisher) -ne
        (Get-Sha256 -Path (Join-Path $distribution `
            "SnowDesktopSteamBridge.exe"))) {
    throw "The Steam payload contains an unavailable or stale Agent Skill Workshop CLI."
}

Copy-Item -LiteralPath $launcherSource `
    -Destination (Join-Path $payload "SnowDesktopLauncher.exe") -Force
$distributionFileMetadata = @(Get-ChildItem -LiteralPath $distribution `
    -Recurse -File | ForEach-Object {
        [pscustomobject][ordered]@{
            path = $_.FullName.Substring(
                $distribution.Length + 1).Replace("\", "/")
            size = [uint64]$_.Length
            sha256 = (Get-Sha256 -Path $_.FullName).ToLowerInvariant()
        }
    } | Sort-Object path)
if ($distributionFileMetadata.Count -eq 0 -or
    -not ($distributionFileMetadata.path -ccontains "SnowDesktop.exe")) {
    throw "The Steam distribution does not contain SnowDesktop.exe."
}
$distributionIdentity = [string]::Join("`n", @(
    $distributionFileMetadata | ForEach-Object {
        "$($_.path)|$($_.size)|$($_.sha256)"
    }))
$distributionDigest = (Get-TextSha256 `
    -Text $distributionIdentity).ToLowerInvariant()
$runtimeBuildId = "$version-$($distributionDigest.Substring(0, 16))"
$runtimeManifest = [ordered]@{
    schemaVersion = 1
    kind = "steam-managed"
    launcherProtocol = 1
    version = $version
    buildId = $runtimeBuildId
    steamAppId = $steamAppId
    windowsDepotId = $windowsDepotId
    distributionDirectory = "distribution"
    runtimeDirectory = ".snowdesktop/runtime"
    dataDirectory = "data"
    files = @($distributionFileMetadata | ForEach-Object {
        [ordered]@{
            path = $_.path
            size = $_.size
            sha256 = $_.sha256
        }
    })
}
$runtimeManifest | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath `
        (Join-Path $payload "SnowDesktop.steam.json") -Encoding utf8

$payloadFileMetadata = @(Get-ChildItem -LiteralPath $payload -Recurse -File |
    ForEach-Object {
        [pscustomobject][ordered]@{
            path = $_.FullName.Substring($payload.Length + 1).Replace("\", "/")
            size = [uint64]$_.Length
            sha256 = Get-Sha256 -Path $_.FullName
        }
    } | Sort-Object path)
$manifest = [ordered]@{
    schemaVersion = 3
    buildProvenance = $packagingContext.provenance
    version = $version
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    steamAppId = $steamAppId
    windowsDepotId = $windowsDepotId
    launchExecutable = "SnowDesktopLauncher.exe"
    distributionManifest = "SnowDesktop.steam.json"
    runtimeBuildId = $runtimeBuildId
    steamworksRedistributable = `
        "distribution/$runtimeDirectory/steam_api64.dll"
    sdkMaterialsIncluded = $false
    files = @($payloadFileMetadata | ForEach-Object { $_.path })
    fileMetadata = $payloadFileMetadata
}
$manifest | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $OutputDirectory "manifest.json") `
        -Encoding utf8

$zip = $paths.zip
Assert-SnowDesktopPackagingContext $packagingContext
if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -LiteralPath $payload -DestinationPath $zip `
    -CompressionLevel Optimal
$hash = Get-Sha256 -Path $zip
"$hash  $([System.IO.Path]::GetFileName($zip))" |
    Set-Content -LiteralPath "$zip.sha256" -Encoding ascii
Write-Host "Steam payload generated: $zip" -ForegroundColor Green
Write-Host "Only $runtimeDirectory\steam_api64.dll from the Steamworks SDK is included."
} finally { Close-SnowDesktopPackagingContext $packagingContext }
