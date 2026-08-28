[CmdletBinding()]
param(
    [ValidateSet("manager", "app", "bridge")]
    [string]$Target = "manager",

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$CommandArguments = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function ConvertTo-NativeArgument {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Value
    )

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            ++$backslashes
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append(('\' * (($backslashes * 2) + 1)))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append(('\' * ($backslashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $scriptDirectory ".."))
$buildOutput = [System.IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot ".build\Release"))
$identityPath = Join-Path $repositoryRoot "packaging\steam-identity.json"
$identity = Get-Content -LiteralPath $identityPath -Encoding UTF8 -Raw |
    ConvertFrom-Json
$appId = [uint32]$identity.appId
if ($appId -eq 0) {
    throw "packaging\steam-identity.json contains an invalid App ID."
}

$executables = @{
    manager = "SnowDesktopWorkshopManager.exe"
    app = "SnowDesktop.exe"
    bridge = "SnowDesktopSteamBridge.exe"
}
$executable = Join-Path $buildOutput $executables[$Target]
$bridge = Join-Path $buildOutput "SnowDesktopSteamBridge.exe"
$steamApi = Join-Path $buildOutput "steam_api64.dll"
foreach ($path in @($executable, $bridge, $steamApi)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required Steam development file is missing: $path`nRun scripts\build.bat first."
    }
}

$configurationText = & $bridge configuration
if ($LASTEXITCODE -ne 0) {
    throw "SnowDesktopSteamBridge.exe configuration failed with exit code $LASTEXITCODE."
}
$configuration = $configurationText | ConvertFrom-Json
if (-not $configuration.ok -or
    -not $configuration.steamworksCompiled -or
    [uint32]$configuration.expectedAppId -ne $appId) {
    throw "The Release bridge is not an SDK-enabled build for Steam App ID $appId."
}

$appIdFile = Join-Path $buildOutput "steam_appid.txt"
$createdAppIdFile = $false
$processExitCode = 0
if (Test-Path -LiteralPath $appIdFile -PathType Leaf) {
    $existingAppId = (Get-Content -LiteralPath $appIdFile -Raw).Trim()
    if ($existingAppId -ne [string]$appId) {
        throw "Existing steam_appid.txt contains '$existingAppId'; expected '$appId'."
    }
}
else {
    Set-Content -LiteralPath $appIdFile -Value ([string]$appId) `
        -Encoding ascii -NoNewline
    $createdAppIdFile = $true
}

try {
    $nativeArguments = ($CommandArguments |
        ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $process = Start-Process -FilePath $executable `
        -ArgumentList $nativeArguments `
        -WorkingDirectory $buildOutput `
        -NoNewWindow -Wait -PassThru
    $processExitCode = $process.ExitCode
}
finally {
    if ($createdAppIdFile -and
        (Test-Path -LiteralPath $appIdFile -PathType Leaf)) {
        $currentAppId = (Get-Content -LiteralPath $appIdFile -Raw).Trim()
        if ($currentAppId -eq [string]$appId) {
            Remove-Item -LiteralPath $appIdFile -Force
        }
        else {
            Write-Warning "steam_appid.txt changed while the process was running; it was left in place."
        }
    }
}

exit $processExitCode
