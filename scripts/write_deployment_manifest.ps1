[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$TargetDirectory,
    [Parameter(Mandatory = $true)][string]$FileList,
    [Parameter(Mandatory = $true)][string]$FragmentList,
    [Parameter(Mandatory = $true)][string]$WindowsAppSdkVersion,
    [Parameter(Mandatory = $true)][string]$CppWinRtVersion,
    [Parameter(Mandatory = $true)][string]$WindowsAppSdkLicensePath,
    [Parameter(Mandatory = $true)][string]$WindowsAppSdkNoticePath,
    [Parameter(Mandatory = $true)][string]$CppWinRtLicensePath
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

function ConvertTo-DeploymentPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    $candidate = $Path.Trim().Replace("/", "\")
    if ([string]::IsNullOrWhiteSpace($candidate) -or
        [System.IO.Path]::IsPathRooted($candidate)) {
        throw "Deployment paths must be non-empty and relative: $Path"
    }
    $parts = @($candidate.Split("\", [System.StringSplitOptions]::RemoveEmptyEntries))
    if ($parts.Count -eq 0 -or $parts -contains ".." -or $parts -contains ".") {
        throw "Deployment paths cannot contain traversal segments: $Path"
    }
    return $parts -join "/"
}

$TargetDirectory = [System.IO.Path]::GetFullPath($TargetDirectory)
if (-not (Test-Path -LiteralPath $TargetDirectory -PathType Container)) {
    throw "Build target directory was not found: $TargetDirectory"
}

$entries = [System.Collections.Generic.Dictionary[string, object]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($line in @(Get-Content -LiteralPath $FileList -Encoding UTF8)) {
    if ([string]::IsNullOrWhiteSpace($line)) {
        continue
    }
    $fields = @($line -split "`t", 3)
    if ($fields.Count -ne 3) {
        throw "Invalid deployment item emitted by MSBuild: $line"
    }
    $relative = ConvertTo-DeploymentPath $fields[0]
    $package = $fields[1].Trim()
    $kind = $fields[2].Trim()
    if ($kind -eq "NuGet" -and
        -not ($package.StartsWith("Microsoft.Windows", [System.StringComparison]::OrdinalIgnoreCase) -or
              $package.StartsWith("Microsoft.UI", [System.StringComparison]::OrdinalIgnoreCase))) {
        continue
    }
    if ($kind -notin @("NuGet", "WindowsAppSDK", "Generated")) {
        throw "Unknown deployment item kind '$kind'."
    }
    $source = Join-Path $TargetDirectory $relative.Replace("/", "\")
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "MSBuild declared a deployment file that was not copied: $source"
    }
    $record = [ordered]@{
        path = $relative
        sha256 = Get-Sha256 -Path $source
        source = if ([string]::IsNullOrWhiteSpace($package)) { $kind } else { $package }
    }
    if ($entries.ContainsKey($relative)) {
        if ($entries[$relative].sha256 -cne $record.sha256) {
            throw "Deployment path has conflicting contents: $relative"
        }
        continue
    }
    $entries.Add($relative, $record)
}

$required = @(
    "App.xbf",
    "SettingsShell.xbf",
    "SnowDesktop.pri",
    "SnowDesktop.winmd",
    "Microsoft.WindowsAppRuntime.dll",
    "Microsoft.ui.xaml.dll",
    "Microsoft.UI.Xaml.winmd"
)
foreach ($path in $required) {
    if (-not $entries.ContainsKey($path)) {
        throw "The self-contained deployment manifest is missing required file: $path"
    }
}

$metadataDirectory = Join-Path $TargetDirectory ".deployment"
$fragmentDirectory = Join-Path $metadataDirectory "appxfragments"
if (Test-Path -LiteralPath $metadataDirectory) {
    Remove-Item -LiteralPath $metadataDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $fragmentDirectory -Force | Out-Null

$fragments = [System.Collections.Generic.Dictionary[string, object]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($line in @(Get-Content -LiteralPath $FragmentList -Encoding UTF8)) {
    $source = $line.Trim()
    if ([string]::IsNullOrWhiteSpace($source)) {
        continue
    }
    $source = [System.IO.Path]::GetFullPath($source)
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
        [System.IO.Path]::GetFileName($source) -cne "package.appxfragment") {
        throw "Invalid Windows App SDK package fragment: $source"
    }
    $runtimeDirectory = [System.IO.DirectoryInfo]::new(
        [System.IO.Path]::GetDirectoryName($source))
    $versionDirectory = $runtimeDirectory.Parent
    $packageDirectory = if ($null -eq $versionDirectory) {
        $null
    } else {
        $versionDirectory.Parent
    }
    if ($null -eq $packageDirectory -or
        $packageDirectory.Name -notmatch "^[A-Za-z0-9_.-]+$" -or
        -not $packageDirectory.Name.StartsWith(
            "microsoft.windowsappsdk",
            [System.StringComparison]::OrdinalIgnoreCase) -or
        $versionDirectory.Name -notmatch "^[A-Za-z0-9_.-]+$") {
        throw "Cannot identify the NuGet package for fragment: $source"
    }
    $name = "$($packageDirectory.Name)-$($versionDirectory.Name).appxfragment"
    $destination = Join-Path $fragmentDirectory $name
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $relative = ".deployment/appxfragments/$name"
    $record = [ordered]@{
        path = $relative
        sha256 = Get-Sha256 -Path $destination
        source = "$($packageDirectory.Name)/$($versionDirectory.Name)"
    }
    if ($fragments.ContainsKey($relative)) {
        if ($fragments[$relative].sha256 -cne $record.sha256) {
            throw "Windows App SDK fragment name collision: $relative"
        }
        continue
    }
    $fragments.Add($relative, $record)
}
if ($fragments.Count -eq 0) {
    throw "No official Windows App SDK package.appxfragment files were emitted by MSBuild."
}

$licenseDirectory = Join-Path $metadataDirectory "licenses"
New-Item -ItemType Directory -Path $licenseDirectory -Force | Out-Null
$noticeSpecs = @(
    @($WindowsAppSdkLicensePath, "WindowsAppSDK-LICENSE.txt"),
    @($WindowsAppSdkNoticePath, "WindowsAppSDK-NOTICE.txt"),
    @($CppWinRtLicensePath, "CppWinRT-LICENSE.txt")
)
$notices = foreach ($spec in $noticeSpecs) {
    $source = [System.IO.Path]::GetFullPath([string]$spec[0])
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required NuGet license or notice was not found: $source"
    }
    $destination = Join-Path $licenseDirectory ([string]$spec[1])
    Copy-Item -LiteralPath $source -Destination $destination -Force
    [ordered]@{
        path = ".deployment/licenses/$([string]$spec[1])"
        destination = "licenses/$([string]$spec[1])"
        sha256 = Get-Sha256 -Path $destination
    }
}

$manifest = [ordered]@{
    schemaVersion = 1
    architecture = "x64"
    windowsAppSdkPackageVersion = $WindowsAppSdkVersion
    cppWinRtPackageVersion = $CppWinRtVersion
    files = @($entries.Values | Sort-Object path)
    appxFragments = @($fragments.Values | Sort-Object path)
    notices = @($notices | Sort-Object path)
}
$json = $manifest | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText(
    (Join-Path $TargetDirectory "SnowDesktop.deployment.json"),
    $json + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false))
