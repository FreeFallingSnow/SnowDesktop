[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Preview", "UploadDev", "UploadPublic")]
    [string]$Mode,

    [string]$SteamCmdPath = "",
    [switch]$SkipPackage,
    [switch]$ReloadShell,
    [switch]$Yes,
    [string]$ConfirmVersion = "",
    [string]$ConfirmPrivateBranch = "",
    [string]$ConfirmPublicBranch = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function ConvertTo-SteamVdfLiteral {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value.IndexOfAny(@([char]0, [char]10, [char]13, [char]34)) -ge 0) {
        throw "SteamPipe VDF values cannot contain NUL, newlines, or quotes."
    }
    return '"' + $Value + '"'
}

function Write-Utf8WithoutBom {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Content
    )

    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $hasher = [System.Security.Cryptography.SHA256]::Create()
        try {
            return [System.BitConverter]::ToString(
                $hasher.ComputeHash($stream)).Replace("-", "")
        }
        finally {
            $hasher.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Resolve-SteamCmdExecutable {
    param([string]$RequestedPath)

    $candidate = $RequestedPath
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = [System.Environment]::GetEnvironmentVariable(
            "SNOWDESKTOP_STEAMCMD_PATH")
    }
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $command = Get-Command "steamcmd.exe" -ErrorAction SilentlyContinue
        if ($null -ne $command) {
            $candidate = $command.Source
        }
    }
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        throw "SteamCMD was not found. Set SNOWDESKTOP_STEAMCMD_PATH to steamcmd.exe."
    }

    $resolved = [System.IO.Path]::GetFullPath($candidate)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf) -or
        [System.IO.Path]::GetFileName($resolved) -ine "steamcmd.exe") {
        throw "SteamCMD path must identify an existing steamcmd.exe: $resolved"
    }
    return $resolved
}

function Get-SteamBuildAccount {
    $account = [System.Environment]::GetEnvironmentVariable(
        "SNOWDESKTOP_STEAM_BUILD_ACCOUNT")
    if ([string]::IsNullOrWhiteSpace($account)) {
        throw "SNOWDESKTOP_STEAM_BUILD_ACCOUNT is required. Log in to SteamCMD manually once; this script never accepts or stores a password."
    }
    if ($account -notmatch "^[A-Za-z0-9_]{3,64}$") {
        throw "SNOWDESKTOP_STEAM_BUILD_ACCOUNT contains unsupported characters."
    }
    return $account
}

function Assert-PrivateDevelopmentBranch {
    param([Parameter(Mandatory = $true)][string]$Branch)

    if ($Branch -notmatch "^[A-Za-z0-9][A-Za-z0-9_-]{2,63}$") {
        throw "The configured Steam development branch name is invalid."
    }
    $normalized = $Branch.ToLowerInvariant()
    if ($normalized -in @("default", "public", "release", "live") -or
        $normalized -notmatch "(dev|internal)") {
        throw "Steam uploads are restricted to an explicitly named private development branch."
    }
}

function Assert-PublicReleaseBranch {
    param([Parameter(Mandatory = $true)][string]$Branch)

    if ($Branch -cne "public") {
        throw "Steam public uploads require the exact public release branch."
    }
}

function Get-RelativePayloadFiles {
    param([Parameter(Mandatory = $true)][string]$PayloadRoot)

    return @(Get-ChildItem -LiteralPath $PayloadRoot -Recurse -File |
        ForEach-Object {
            $_.FullName.Substring($PayloadRoot.Length + 1).Replace("\", "/")
        } | Sort-Object)
}

function Assert-SteamPayloadManifest {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$PayloadRoot,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][uint32]$AppId,
        [Parameter(Mandatory = $true)][uint32]$DepotId
    )

    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $PayloadRoot -PathType Container)) {
        throw "Steam payload is missing. Run scripts/package_steam.ps1 first."
    }
    $manifest = Get-Content -LiteralPath $ManifestPath -Encoding UTF8 -Raw |
        ConvertFrom-Json
    if ([uint32]$manifest.schemaVersion -lt 2 -or
        [string]$manifest.version -ne $Version -or
        [uint32]$manifest.steamAppId -ne $AppId -or
        [uint32]$manifest.windowsDepotId -ne $DepotId) {
        throw "Steam payload manifest does not match version.json and Steam identity."
    }

    $listed = @($manifest.files | ForEach-Object { [string]$_ } | Sort-Object)
    $actual = @(Get-RelativePayloadFiles -PayloadRoot $PayloadRoot)
    $difference = @(Compare-Object -ReferenceObject $listed -DifferenceObject $actual)
    if ($listed.Count -eq 0 -or $difference.Count -ne 0) {
        throw "Steam payload files do not match manifest.json; rebuild the package."
    }
    $metadata = @($manifest.fileMetadata)
    if ($metadata.Count -ne $actual.Count) {
        throw "Steam payload metadata is incomplete; rebuild the package."
    }
    $metadataPaths = @($metadata | ForEach-Object { [string]$_.path } |
        Sort-Object)
    if (@(Compare-Object -ReferenceObject $actual `
            -DifferenceObject $metadataPaths).Count -ne 0) {
        throw "Steam payload metadata paths do not match the payload."
    }
    $payloadPrefix = [System.IO.Path]::GetFullPath($PayloadRoot).TrimEnd("\") + "\"
    foreach ($entry in $metadata) {
        $relativePath = [string]$entry.path
        $portablePath = $relativePath.Replace("\", "/")
        if ([string]::IsNullOrWhiteSpace($relativePath) -or
            [System.IO.Path]::IsPathRooted($relativePath) -or
            $relativePath.Contains(":") -or
            $portablePath -match '(^|/)\.\.(/|$)') {
            throw "Steam payload metadata contains an unsafe path: $relativePath"
        }
        $payloadPath = [System.IO.Path]::GetFullPath(
            (Join-Path $PayloadRoot $portablePath.Replace("/", "\")))
        if (-not $payloadPath.StartsWith(
                $payloadPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Steam payload metadata escapes the payload root: $relativePath"
        }
        if (-not (Test-Path -LiteralPath $payloadPath -PathType Leaf) -or
            [uint64](Get-Item -LiteralPath $payloadPath).Length -ne
                [uint64]$entry.size -or
            (Get-Sha256 -Path $payloadPath) -ine [string]$entry.sha256) {
            throw "Steam payload metadata mismatch: $relativePath"
        }
    }
    if ($actual -contains "steam_appid.txt" -or
        -not ($actual -contains "SnowDesktopLauncher.exe") -or
        -not ($actual -contains "SnowDesktop.steam.json") -or
        -not ($actual -contains "distribution/SnowDesktop.exe") -or
        ($actual -contains "SnowDesktop.exe")) {
        throw "Steam payload contains a development App ID file or has an invalid launcher/distribution layout."
    }
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $scriptDirectory ".."))
$version = [string](Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "version.json") -Encoding UTF8 -Raw |
    ConvertFrom-Json).version
if ($version -notmatch "^[1-9][0-9]*\.[0-9]+\.[0-9]+\.0$") {
    throw "version.json must use Store-compatible A.B.C.0 format."
}

$identity = Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "packaging\steam-identity.json") `
    -Encoding UTF8 -Raw | ConvertFrom-Json
$appId = [uint32]$identity.appId
$depotId = [uint32]$identity.windowsDepotId
if ($appId -eq 0 -or $depotId -eq 0) {
    throw "packaging\steam-identity.json contains an invalid Steam identity."
}

$pipeConfiguration = Get-Content -LiteralPath `
    (Join-Path $repositoryRoot "packaging\steam-pipe.json") `
    -Encoding UTF8 -Raw | ConvertFrom-Json
if ([uint32]$pipeConfiguration.schemaVersion -ne 2) {
    throw "packaging\steam-pipe.json uses an unsupported schema version."
}
$privateBranch = [string]$pipeConfiguration.privateDevelopmentBranch
Assert-PrivateDevelopmentBranch -Branch $privateBranch
$publicBranch = [string]$pipeConfiguration.publicReleaseBranch
Assert-PublicReleaseBranch -Branch $publicBranch

if ($Mode -eq "UploadDev") {
    if (-not $Yes -or $ConfirmVersion -ne $version -or
        $ConfirmPrivateBranch -cne $privateBranch) {
        throw "UploadDev requires -Yes -ConfirmVersion $version -ConfirmPrivateBranch $privateBranch."
    }
}
if ($Mode -eq "UploadPublic") {
    if (-not $Yes -or $ConfirmVersion -ne $version -or
        $ConfirmPublicBranch -cne $publicBranch) {
        throw "UploadPublic requires -Yes -ConfirmVersion $version -ConfirmPublicBranch $publicBranch."
    }
}
if ($SkipPackage -and $ReloadShell) {
    throw "-ReloadShell cannot be combined with -SkipPackage."
}

if (-not $SkipPackage) {
    $packageArguments = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $scriptDirectory "package_steam.ps1")
    )
    if ($ReloadShell) {
        $packageArguments += "-ReloadShell"
    }
    & (Join-Path $PSHOME "powershell.exe") @packageArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Steam package generation failed with exit code $LASTEXITCODE."
    }
}

$versionRoot = Join-Path $repositoryRoot "artifacts\v$version"
$steamPackageRoot = Join-Path $versionRoot "steam"
$payloadRoot = Join-Path $steamPackageRoot "SnowDesktop"
$payloadManifest = Join-Path $steamPackageRoot "manifest.json"
Assert-SteamPayloadManifest `
    -ManifestPath $payloadManifest `
    -PayloadRoot $payloadRoot `
    -Version $version `
    -AppId $appId `
    -DepotId $depotId

$steamPipeRoot = Join-Path $versionRoot "steampipe"
$steamPipeScripts = Join-Path $steamPipeRoot "scripts"
$steamPipeOutput = Join-Path $steamPipeRoot "output"
$steamPipeLogs = Join-Path $steamPipeRoot "logs"
foreach ($path in @($steamPipeScripts, $steamPipeOutput, $steamPipeLogs)) {
    New-Item -ItemType Directory -Path $path -Force | Out-Null
}

$depotScriptPath = Join-Path $steamPipeScripts "depot_build_$depotId.vdf"
$depotScriptName = [System.IO.Path]::GetFileName($depotScriptPath)
$depotScript = @"
"DepotBuild"
{
    "DepotID" $(ConvertTo-SteamVdfLiteral -Value ([string]$depotId))
    "ContentRoot" $(ConvertTo-SteamVdfLiteral -Value $payloadRoot)
    "FileMapping"
    {
        "LocalPath" "*"
        "DepotPath" "."
        "Recursive" "1"
    }
    "FileExclusion" "steam_appid.txt"
    "FileExclusion" ".snowdesktop\*"
    "FileExclusion" "data\*"
    "FileExclusion" "runtime\*"
    "FileExclusion" "state\*"
    "FileExclusion" "logs\*"
}
"@
Write-Utf8WithoutBom -Path $depotScriptPath -Content $depotScript

$modeSlug = switch ($Mode) {
    "Preview" { "preview" }
    "UploadDev" { "upload-dev" }
    "UploadPublic" { "upload-public" }
}
$appScriptPath = Join-Path $steamPipeScripts `
    "app_build_${appId}_$modeSlug.vdf"
$modeLine = if ($Mode -eq "Preview") {
    '    "Preview" "1"'
}
else {
    $targetBranch = if ($Mode -eq "UploadDev") {
        $privateBranch
    }
    else {
        $publicBranch
    }
    '    "SetLive" ' + (ConvertTo-SteamVdfLiteral -Value $targetBranch)
}
$description = "SnowDesktop v$version $modeSlug"
$appScript = @"
"AppBuild"
{
    "AppID" $(ConvertTo-SteamVdfLiteral -Value ([string]$appId))
    "Desc" $(ConvertTo-SteamVdfLiteral -Value $description)
$modeLine
    "ContentRoot" $(ConvertTo-SteamVdfLiteral -Value $payloadRoot)
    "BuildOutput" $(ConvertTo-SteamVdfLiteral -Value $steamPipeOutput)
    "Depots"
    {
        $(ConvertTo-SteamVdfLiteral -Value ([string]$depotId)) $(ConvertTo-SteamVdfLiteral -Value $depotScriptName)
    }
}
"@
if ($Mode -eq "Preview" -and $appScript -match '(?i)"SetLive"') {
    throw "Preview SteamPipe script unexpectedly contains SetLive."
}
if ($Mode -eq "UploadDev" -and
    ($appScript -notmatch [regex]::Escape('"SetLive" "' + $privateBranch + '"') -or
    $appScript -match '(?i)"SetLive"\s+"(default|public|release|live)"')) {
    throw "Upload SteamPipe script does not target the configured private branch."
}
if ($Mode -eq "UploadPublic" -and
    $appScript -notmatch [regex]::Escape('"SetLive" "' + $publicBranch + '"')) {
    throw "Public SteamPipe script does not target the configured public branch."
}
Write-Utf8WithoutBom -Path $appScriptPath -Content $appScript

$steamCmd = Resolve-SteamCmdExecutable -RequestedPath $SteamCmdPath
$buildAccount = Get-SteamBuildAccount
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logPath = Join-Path $steamPipeLogs "$modeSlug-$timestamp.log"
$requestPath = Join-Path $steamPipeRoot "last-request.json"
$request = [ordered]@{
    schemaVersion = 1
    requestedAt = (Get-Date).ToUniversalTime().ToString("o")
    mode = $Mode
    version = $version
    steamAppId = $appId
    windowsDepotId = $depotId
    targetBranch = if ($Mode -eq "UploadDev") {
        $privateBranch
    } elseif ($Mode -eq "UploadPublic") {
        $publicBranch
    } else { $null }
    appBuildScript = $appScriptPath
    log = $logPath
    accountSource = "SNOWDESKTOP_STEAM_BUILD_ACCOUNT"
    passwordAcceptedOrStored = $false
}
$request | ConvertTo-Json -Depth 3 |
    Set-Content -LiteralPath $requestPath -Encoding UTF8

@(
    "SnowDesktop SteamPipe $Mode",
    "Version: $version",
    "App/Depot: $appId/$depotId",
    "Branch: $(if ($Mode -eq 'UploadDev') { $privateBranch } elseif ($Mode -eq 'UploadPublic') { $publicBranch } else { '(none; preview only)' })",
    "Build account: configured through environment (redacted)",
    "Password: never accepted by this script"
) | Set-Content -LiteralPath $logPath -Encoding UTF8

$steamCmdArguments = @(
    "+@ShutdownOnFailedCommand", "1",
    "+@NoPromptForPassword", "1",
    "+login", $buildAccount,
    "+run_app_build", $appScriptPath,
    "+quit"
)
& $steamCmd @steamCmdArguments 2>&1 |
    Tee-Object -FilePath $logPath -Append
$steamCmdExitCode = $LASTEXITCODE
if ($steamCmdExitCode -ne 0) {
    throw "SteamPipe $Mode failed with exit code $steamCmdExitCode. See $logPath"
}

Write-Host "SteamPipe $Mode completed: $logPath" -ForegroundColor Green
if ($Mode -eq "Preview") {
    Write-Host "No depot content was uploaded and no branch was changed."
}
else {
    if ($Mode -eq "UploadDev") {
        Write-Host "Uploaded only to private development branch '$privateBranch'."
    }
    else {
        Write-Host "Uploaded to public release branch '$publicBranch'."
    }
}
