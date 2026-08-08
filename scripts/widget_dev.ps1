param(
    [Parameter(Position = 0)]
    [string]$WidgetPath,

    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",

    [switch]$Once,
    [switch]$RestartHost
)

$ErrorActionPreference = "Stop"

function Write-Usage {
    Write-Host "Usage:"
    Write-Host "  scripts\widget-dev.bat widgets\my-widget"
    Write-Host "  scripts\widget-dev.bat widgets\my-widget -Once"
    Write-Host "  scripts\widget-dev.bat widgets\my-widget -RestartHost"
}

if ([string]::IsNullOrWhiteSpace($WidgetPath)) {
    Write-Usage
    exit 2
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$sourceCandidate = if ([System.IO.Path]::IsPathRooted($WidgetPath)) {
    $WidgetPath
}
else {
    Join-Path $repositoryRoot $WidgetPath
}
$sourceRoot = [System.IO.Path]::GetFullPath($sourceCandidate)
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
    throw "Widget directory does not exist: $sourceRoot"
}

$manifestPath = Join-Path $sourceRoot "widget.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "widget.json is missing: $manifestPath"
}

$hostRoot = Join-Path $repositoryRoot ".build\$Configuration"
$hostExecutable = Join-Path $hostRoot "SnowDesktop.exe"
$packageTool = Join-Path $hostRoot "snowwidget.exe"
if (-not (Test-Path -LiteralPath $hostExecutable -PathType Leaf) -or
    -not (Test-Path -LiteralPath $packageTool -PathType Leaf)) {
    throw "The $Configuration host is missing. Build it once before starting widget development."
}

function Test-WidgetPackage {
    & $packageTool validate $sourceRoot | Write-Host
    return $LASTEXITCODE -eq 0
}

if (-not (Test-WidgetPackage)) {
    throw "Widget validation failed."
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
    ConvertFrom-Json
$entry = [string]$manifest.entry
$slug = [string]$manifest.slug
if ([string]::IsNullOrWhiteSpace($entry) -or
    [string]::IsNullOrWhiteSpace($slug)) {
    throw "widget.json must declare entry and slug."
}

$developmentRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $hostRoot "data\widgets\dev"))
$targetRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $developmentRoot $slug))
$safePrefix = $developmentRoot.TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar) +
    [System.IO.Path]::DirectorySeparatorChar
if (-not $targetRoot.StartsWith(
    $safePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe development target: $targetRoot"
}

$targetAlreadyExisted = Test-Path -LiteralPath (
    Join-Path $targetRoot "widget.json") -PathType Leaf

function Get-ChildRelativePath(
    [string]$ParentPath,
    [string]$ChildPath
) {
    $parentPrefix = [System.IO.Path]::GetFullPath($ParentPath).TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    $childFullPath = [System.IO.Path]::GetFullPath($ChildPath)
    if (-not $childFullPath.StartsWith(
        $parentPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the expected parent: $childFullPath"
    }
    return $childFullPath.Substring($parentPrefix.Length)
}

function Get-SourceSnapshot {
    $parts = Get-ChildItem -LiteralPath $sourceRoot -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            $relative = Get-ChildRelativePath $sourceRoot $_.FullName
            "$relative|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)"
        }
    return $parts -join "`n"
}

function Sync-WidgetPackage {
    New-Item -ItemType Directory -Path $targetRoot -Force | Out-Null
    $sourceFiles = @(
        Get-ChildItem -LiteralPath $sourceRoot -Recurse -File
    )
    $expectedFiles = @{}

    foreach ($sourceFile in $sourceFiles) {
        $relative = Get-ChildRelativePath $sourceRoot $sourceFile.FullName
        $expectedFiles[$relative.ToLowerInvariant()] = $true
        $destination = Join-Path $targetRoot $relative
        $destinationDirectory = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $destinationDirectory -Force |
            Out-Null
        Copy-Item -LiteralPath $sourceFile.FullName `
            -Destination $destination -Force
    }

    foreach ($targetFile in @(
        Get-ChildItem -LiteralPath $targetRoot -Recurse -File
    )) {
        $relative = Get-ChildRelativePath $targetRoot $targetFile.FullName
        if (-not $expectedFiles.ContainsKey($relative.ToLowerInvariant())) {
            Remove-Item -LiteralPath $targetFile.FullName -Force
        }
    }

    foreach ($targetDirectory in @(
        Get-ChildItem -LiteralPath $targetRoot -Recurse -Directory |
            Sort-Object FullName -Descending
    )) {
        if (-not (Get-ChildItem -LiteralPath $targetDirectory.FullName -Force)) {
            Remove-Item -LiteralPath $targetDirectory.FullName -Force
        }
    }

    $targetEntry = Join-Path $targetRoot $entry
    if (-not (Test-Path -LiteralPath $targetEntry -PathType Leaf)) {
        throw "Manifest entry was not copied: $entry"
    }
    (Get-Item -LiteralPath $targetEntry).LastWriteTimeUtc =
        [System.DateTime]::UtcNow
    Write-Host ("[{0}] Synced {1} -> {2}" -f
        (Get-Date -Format "HH:mm:ss"), $slug, $targetRoot)
}

Sync-WidgetPackage

$hostProcesses = @(Get-Process -Name "SnowDesktop" -ErrorAction SilentlyContinue)
$needsRestart = $RestartHost -or
    ($hostProcesses.Count -gt 0 -and -not $targetAlreadyExisted)
if ($needsRestart) {
    Write-Host "Restarting SnowDesktop once to activate the development override..."
    $hostProcesses | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Start-Process -FilePath $hostExecutable -WorkingDirectory $hostRoot
}
elseif ($hostProcesses.Count -eq 0) {
    Write-Host "Starting SnowDesktop with the development override..."
    Start-Process -FilePath $hostExecutable -WorkingDirectory $hostRoot
}

if ($Once) {
    Write-Host "One-time sync complete."
    exit 0
}

Write-Host "Watching for widget changes. Save a file to hot-reload; press Ctrl+C to stop."
$snapshot = Get-SourceSnapshot
while ($true) {
    Start-Sleep -Milliseconds 350
    $nextSnapshot = Get-SourceSnapshot
    if ($nextSnapshot -eq $snapshot) {
        continue
    }
    $snapshot = $nextSnapshot
    try {
        if (Test-WidgetPackage) {
            Sync-WidgetPackage
        }
        else {
            Write-Warning "Validation failed; keeping the last synced package."
        }
    }
    catch {
        Write-Warning $_
    }
}
