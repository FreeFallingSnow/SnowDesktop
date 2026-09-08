[CmdletBinding()]
param()

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

function Assert-Contract {
    param(
        [Parameter(Mandatory = $true)][bool]$Condition,
        [Parameter(Mandatory = $true)][string]$Message
    )

    if (-not $Condition) {
        throw "Steam local deploy contract failed: $Message"
    }
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Action,
        [Parameter(Mandatory = $true)][string]$Message
    )

    $failed = $false
    try {
        & $Action | Out-Null
    }
    catch {
        $failed = $true
    }
    Assert-Contract -Condition $failed -Message $Message
}

$testDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $testDirectory ".."))
$deployScript = Join-Path $repositoryRoot "scripts\steam_local_deploy.ps1"
Assert-Contract -Condition `
    (Test-Path -LiteralPath $deployScript -PathType Leaf) `
    -Message "deployment script is missing"

$temporaryBase = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::GetTempPath()).TrimEnd("\")
$temporaryRoot = Join-Path $temporaryBase `
    ("SnowDesktopSteamLocalDeployContract-" + [guid]::NewGuid().ToString("N"))
$steamRoot = Join-Path $temporaryRoot "Steam"
$steamApps = Join-Path $steamRoot "steamapps"
$installRoot = Join-Path $steamApps "common\SnowDesktop"
$payloadRoot = Join-Path $temporaryRoot "payload"
$manifestPath = Join-Path $steamApps "appmanifest_5080330.acf"
$libraryPath = Join-Path $steamApps "libraryfolders.vdf"

try {
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    New-Item -ItemType Directory -Path `
        (Join-Path $payloadRoot "lang\en-US") -Force | Out-Null

    $escapedSteamRoot = $steamRoot.Replace("\", "\\")
    @"
"libraryfolders"
{
    "0"
    {
        "path" "$escapedSteamRoot"
        "apps"
        {
            "5080330" "0"
        }
    }
}
"@ | Set-Content -LiteralPath $libraryPath -Encoding UTF8
    @'
"AppState"
{
    "appid" "5080330"
    "name" "SnowDesktop"
    "StateFlags" "4"
    "installdir" "SnowDesktop"
    "buildid" "0"
    "InstalledDepots"
    {
    }
}
'@ | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    "contract executable" | Set-Content -LiteralPath `
        (Join-Path $payloadRoot "SnowDesktop.exe") -Encoding UTF8
    "contract translation" | Set-Content -LiteralPath `
        (Join-Path $payloadRoot "lang\en-US\contract.txt") -Encoding UTF8
    '{"schemaVersion":1,"kind":"steam-managed"}' | Set-Content `
        -LiteralPath (Join-Path $payloadRoot `
            "SnowDesktop.runtime-context.json") -Encoding UTF8

    $manifestHashBefore = Get-Sha256 -Path $manifestPath
    $libraryHashBefore = Get-Sha256 -Path $libraryPath

    $dryOutput = & $deployScript -SteamRoot $steamRoot `
        -PayloadDirectory $payloadRoot -BuildId "contract-build" `
        -ProfileId "contract-profile" -Json
    $dryJson = $dryOutput -join [Environment]::NewLine
    $dry = $dryJson | ConvertFrom-Json
    Assert-Contract -Condition (-not [bool]$dry.applied) `
        -Message "dry-run reported itself as applied"
    Assert-Contract -Condition `
        ([string]$dry.deploymentKind -ceq "steam-local-dev") `
        -Message "dry-run did not identify the local development flavor"
    Assert-Contract -Condition (-not [bool]$dry.steamManaged) `
        -Message "dry-run claimed to be Steam-managed"
    Assert-Contract -Condition (-not [bool]$dry.changesActiveDeployment) `
        -Message "dry-run claimed it would activate the staged build"
    Assert-Contract -Condition ([int]$dry.fileCount -eq 2) `
        -Message "source deployment identity was treated as payload content"
    Assert-Contract -Condition `
        (-not (Test-Path -LiteralPath (Join-Path $installRoot ".snowdesktop"))) `
        -Message "dry-run changed the Steam install directory"

    $applyOutput = & $deployScript -SteamRoot $steamRoot `
        -PayloadDirectory $payloadRoot -BuildId "contract-build" -Apply `
        -ProfileId "contract-profile" -Json
    $applyJson = $applyOutput -join [Environment]::NewLine
    $applied = $applyJson | ConvertFrom-Json
    $destination = [string]$applied.destination
    Assert-Contract -Condition ([bool]$applied.applied) `
        -Message "apply did not report success"
    $expectedDestination = Join-Path $installRoot `
        ".snowdesktop\dev\contract-build"
    Assert-Contract -Condition ($destination -ceq $expectedDestination) `
        -Message "payload was staged outside the owned development directory"
    $stagedExecutable = Join-Path $destination "SnowDesktop.exe"
    Assert-Contract -Condition `
        (Test-Path -LiteralPath $stagedExecutable -PathType Leaf) `
        -Message "staged executable is missing"
    $stagedTranslation = Join-Path $destination "lang\en-US\contract.txt"
    Assert-Contract -Condition `
        (Test-Path -LiteralPath $stagedTranslation -PathType Leaf) `
        -Message "nested payload file is missing"

    $contextPath = Join-Path $destination `
        "SnowDesktop.runtime-context.json"
    $marker = Get-Content -LiteralPath $contextPath -Encoding UTF8 -Raw |
        ConvertFrom-Json
    Assert-Contract -Condition `
        ([string]$marker.kind -ceq "steam-local-dev") `
        -Message "staged marker has the wrong deployment kind"
    Assert-Contract -Condition `
        ([string]$marker.installRootRelative -ceq "../../..") `
        -Message "staged marker does not resolve the Steam install root"
    Assert-Contract -Condition `
        ([string]$marker.dataRootRelative -ceq `
            ".snowdesktop/dev-data/contract-profile") `
        -Message "staged marker does not isolate development data"
    Assert-Contract -Condition `
        ([string]$marker.launcherRelative -ceq `
            ".snowdesktop/dev/contract-build/SnowDesktop.exe") `
        -Message "staged marker points at the depot launcher"
    Assert-Contract -Condition `
        ([string]$marker.profileId -ceq "contract-profile") `
        -Message "staged marker has the wrong development profile"

    $content = Get-Content -LiteralPath `
        (Join-Path $destination "content-manifest.json") -Encoding UTF8 -Raw |
        ConvertFrom-Json
    Assert-Contract -Condition (@($content.files).Count -eq 2) `
        -Message "content manifest does not describe the copied payload"
    $manifestHashAfter = Get-Sha256 -Path $manifestPath
    Assert-Contract -Condition ($manifestHashAfter -ceq $manifestHashBefore) `
        -Message "Steam app manifest was modified"
    $libraryHashAfter = Get-Sha256 -Path $libraryPath
    Assert-Contract -Condition ($libraryHashAfter -ceq $libraryHashBefore) `
        -Message "Steam library configuration was modified"
    foreach ($protectedName in @("data", "distribution", "runtime", "state")) {
        Assert-Contract -Condition `
            (-not (Test-Path -LiteralPath (Join-Path $installRoot $protectedName))) `
            -Message "local deploy created or changed $protectedName"
    }
    Assert-Contract -Condition `
        (-not (Test-Path -LiteralPath `
            (Join-Path $installRoot ".snowdesktop\dev-data"))) `
        -Message "staging a build created its development data profile"

    Invoke-ExpectedFailure -Message `
        "an existing development build was overwritten" -Action {
        & $deployScript -SteamRoot $steamRoot -PayloadDirectory $payloadRoot `
            -BuildId "contract-build" -ProfileId "contract-profile" `
            -Apply -Json
    }
    $stagedExecutableText = (Get-Content -LiteralPath $stagedExecutable `
        -Encoding UTF8 -Raw).Trim()
    Assert-Contract -Condition `
        ($stagedExecutableText -ceq "contract executable") `
        -Message "failed redeploy changed the existing payload"

    $badPayload = Join-Path $temporaryRoot "bad-payload"
    New-Item -ItemType Directory -Path (Join-Path $badPayload "data") `
        -Force | Out-Null
    "contract executable" | Set-Content -LiteralPath `
        (Join-Path $badPayload "SnowDesktop.exe") -Encoding UTF8
    "user data" | Set-Content -LiteralPath `
        (Join-Path $badPayload "data\settings.json") -Encoding UTF8
    Invoke-ExpectedFailure -Message `
        "a payload containing user data was accepted" -Action {
        & $deployScript -SteamRoot $steamRoot -PayloadDirectory $badPayload `
            -BuildId "bad-payload" -ProfileId "contract-profile" -Json
    }
    $badDestination = Join-Path $installRoot `
        ".snowdesktop\dev\bad-payload"
    Assert-Contract -Condition `
        (-not (Test-Path -LiteralPath $badDestination)) `
        -Message "rejected payload left a development build behind"

    Invoke-ExpectedFailure -Message "an unsafe build ID was accepted" -Action {
        & $deployScript -SteamRoot $steamRoot -PayloadDirectory $payloadRoot `
            -BuildId "..\escape" -ProfileId "contract-profile" -Json
    }

    Invoke-ExpectedFailure -Message "an unsafe profile ID was accepted" -Action {
        & $deployScript -SteamRoot $steamRoot -PayloadDirectory $payloadRoot `
            -BuildId "safe-build" -ProfileId "..\escape" -Json
    }

    Write-Output "steam_local_deploy_contract_tests: passed"
}
finally {
    $fullTemporaryRoot = [System.IO.Path]::GetFullPath($temporaryRoot)
    $expectedPrefix = "$temporaryBase\"
    if ($fullTemporaryRoot.StartsWith(
            $expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Path]::GetFileName($fullTemporaryRoot).StartsWith(
            "SnowDesktopSteamLocalDeployContract-",
            [System.StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $fullTemporaryRoot -Recurse -Force
    }
}
