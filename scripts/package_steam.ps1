[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$ReloadShell,
    [string]$OutputDirectory = ""
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
$buildOutput = Join-Path $repositoryRoot ".build\Release"
$runtimeDirectory = "SnowDesktop.Runtime"
Import-Module (Join-Path $scriptDirectory "deployment_payload.psm1") -Force
$steamIdentity = Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "packaging\steam-identity.json") `
    -Encoding UTF8 -Raw | ConvertFrom-Json
$steamAppId = [uint32]$steamIdentity.appId
$windowsDepotId = [uint32]$steamIdentity.windowsDepotId
if ($steamAppId -eq 0 -or $windowsDepotId -eq 0) {
    throw "packaging\steam-identity.json contains an invalid Steam identity."
}
$version = (Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "version.json") -Encoding UTF8 -Raw |
    ConvertFrom-Json).version
if ($version -notmatch "^[1-9][0-9]*\.[0-9]+\.[0-9]+\.0$") {
    throw "version.json must use Store-compatible A.B.C.0 format."
}
$versionRoot = Join-Path $repositoryRoot "artifacts\v$version"
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $versionRoot "steam"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$versionRootFull = [System.IO.Path]::GetFullPath($versionRoot).TrimEnd("\") + "\"
if (-not $OutputDirectory.StartsWith(
        $versionRootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Steam artifacts must remain under artifacts\v$version\."
}

if ($SkipBuild -and $ReloadShell) {
    throw "-ReloadShell cannot be combined with -SkipBuild."
}
if (-not $SkipBuild) {
    $buildCommand = "call `"$repositoryRoot\scripts\build.bat`""
    if ($ReloadShell) {
        $buildCommand += " --reload-shell"
    }
    & cmd.exe /d /c $buildCommand
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/build.bat failed with exit code $LASTEXITCODE."
    }
}

$runtimeFiles = @(
    "SnowDesktopTaskbarHook.dll",
    "SnowDesktopWallpaperHook.dll",
    "SnowDesktopWallpaperHook32.dll",
    "SnowDesktopWallpaperInjector32.exe",
    "steam_api64.dll"
)
$required = @(
    "SnowDesktop.exe",
    "SnowDesktopTaskbarHook.dll",
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
Copy-Item -LiteralPath (Join-Path $repositoryRoot "widgets") `
    -Destination (Join-Path $distribution "widgets") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repositoryRoot "lang") `
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
if ($steamDlls.Count -ne 1 -or $steamDlls[0].Name -cne "steam_api64.dll") {
    throw "The payload must contain exactly one permitted steam_api64.dll."
}
$payloadSkillCli = Join-Path $distribution `
    "widgets\snowdesktop-lua-widget\bin\snowwidget.exe"
if (-not (Test-Path -LiteralPath $payloadSkillCli -PathType Leaf) -or
    (Get-Sha256 -Path $payloadSkillCli) -ne
        (Get-Sha256 -Path (Join-Path $distribution "snowwidget.exe"))) {
    throw "The Steam payload contains an unavailable or stale Agent Skill CLI."
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

$zip = Join-Path $versionRoot "SnowDesktop-Steam-x64-$version.zip"
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
