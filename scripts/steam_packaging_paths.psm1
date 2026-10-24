Set-StrictMode -Version Latest

function Test-SnowDesktopFrozenPackagingSource {
    param([string]$Path)

    $normalized = $Path.Replace('/', '\').TrimEnd('\')
    if ($normalized -match '(?i)\\\.build\\(?:candidate-slots\\[^\\]+|build-workspace)\\source(?:\\|$)') {
        return $true
    }
    return ([IO.Path]::GetFileName($normalized) -eq 'source' -and
        [IO.File]::Exists((Join-Path ([IO.Path]::GetDirectoryName($normalized)) 'materialization.json')))
}

function Assert-SnowDesktopPackagingOutputPath {
    param([string]$Path)

    # Writes and recursive staging replacement must not follow a linked parent.
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force -ErrorAction Stop
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw 'Steam packaging output cannot traverse a reparse point.'
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor)
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
}

function Resolve-SnowDesktopSteamPackagingPaths {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourceRepositoryRoot,
        [Parameter(Mandatory = $true)][ValidatePattern('^[1-9][0-9]*\.[0-9]+\.[0-9]+\.0$')][string]$Version,
        [string]$BuildOutputDirectory = '',
        [string]$ArtifactRepositoryRoot = '',
        [string]$OutputDirectory = '',
        [switch]$SkipBuild,
        [switch]$ReloadShell
    )

    $sourceRoot = [IO.Path]::GetFullPath($SourceRepositoryRoot).TrimEnd('\', '/')
    $frozen = Test-SnowDesktopFrozenPackagingSource -Path $sourceRoot
    if ($SkipBuild -and $ReloadShell) {
        throw '-ReloadShell cannot be combined with -SkipBuild.'
    }
    if ($BuildOutputDirectory -and -not $SkipBuild) {
        throw '-BuildOutputDirectory requires -SkipBuild so packaging cannot rebuild a different input.'
    }
    if ($frozen -and (-not $SkipBuild -or -not $ArtifactRepositoryRoot)) {
        throw 'Frozen candidate packaging requires -SkipBuild and an explicit -ArtifactRepositoryRoot outside the frozen source.'
    }
    $artifactRoot = if ($ArtifactRepositoryRoot) {
        [IO.Path]::GetFullPath($ArtifactRepositoryRoot).TrimEnd('\', '/')
    } else { $sourceRoot }
    if (-not [IO.File]::Exists((Join-Path $artifactRoot 'version.json')) -or
        -not [IO.File]::Exists((Join-Path $artifactRoot 'packaging/steam-identity.json'))) {
        throw '-ArtifactRepositoryRoot must identify the original SnowDesktop repository.'
    }
    if ((Test-SnowDesktopFrozenPackagingSource -Path $artifactRoot) -or
        ($frozen -and ($artifactRoot -eq $sourceRoot -or
            $artifactRoot.StartsWith($sourceRoot + '\', [StringComparison]::OrdinalIgnoreCase)))) {
        throw 'Steam artifacts cannot be written inside a frozen candidate source tree.'
    }
    $buildOutput = if ($BuildOutputDirectory) {
        [IO.Path]::GetFullPath($BuildOutputDirectory).TrimEnd('\', '/')
    } else { Join-Path $sourceRoot '.build/Release' }
    $versionRoot = Join-Path $artifactRoot "artifacts/v$Version"
    $output = if ($OutputDirectory) {
        [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\', '/')
    } else { Join-Path $versionRoot 'steam' }
    if (-not $output.StartsWith($versionRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Steam artifacts must remain under artifacts\v$Version\."
    }
    if ($output -eq $buildOutput -or
        $output.StartsWith($buildOutput + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $buildOutput.StartsWith($output + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Steam packaging input and output directories must not overlap.'
    }
    Assert-SnowDesktopPackagingOutputPath -Path $output
    $zip = Join-Path $versionRoot "SnowDesktop-Steam-x64-$Version.zip"
    Assert-SnowDesktopPackagingOutputPath -Path $zip
    Assert-SnowDesktopPackagingOutputPath -Path ($zip + '.sha256')
    return [pscustomobject]@{
        sourceRoot = $sourceRoot
        buildOutput = $buildOutput
        artifactRoot = $artifactRoot
        versionRoot = $versionRoot
        outputDirectory = $output
        zip = $zip
    }
}

Export-ModuleMember -Function Resolve-SnowDesktopSteamPackagingPaths
