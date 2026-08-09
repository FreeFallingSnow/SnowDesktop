# Package the release\ folder into a portable zip and write its SHA256
# checksum file (for the in-app update feed). Run from the repository root
# or the release staging folder.
param(
    [string]$StageDir = "release"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path | Split-Path -Parent
$stage = Join-Path $repoRoot $StageDir
if (-not (Test-Path -LiteralPath $stage -PathType Container)) {
    Write-Error "Staging directory not found: $stage"
    exit 1
}
$version = (Get-Content (Join-Path $repoRoot "version.json") -Raw |
    ConvertFrom-Json).version
$zipName = "SparkDesktop-portable-x64-$version.zip"
$zipPath = Join-Path $repoRoot $zipName
$shaPath = "$zipPath.sha256"

if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $shaPath) {
    Remove-Item -LiteralPath $shaPath -Force
}

# Exclude the zip itself (it lives outside the stage, but be safe).
$items = @(Get-ChildItem -LiteralPath $stage -Force |
    Where-Object { $_.Name -ne $zipName })
Compress-Archive -Path $items.FullName -DestinationPath $zipPath `
    -CompressionLevel Optimal -Force

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
"$hash  $zipName" | Set-Content -LiteralPath $shaPath -Encoding ascii

Write-Host "Portable zip:  $zipPath"
Write-Host "SHA256 file:   $shaPath"
