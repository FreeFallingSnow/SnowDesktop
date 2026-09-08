[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-Publication {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "GitHub publication: $Message" }
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$managerPath = Join-Path $repositoryRoot "scripts/release_manager.ps1"
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $managerPath, [ref]$null, [ref]$parseErrors)
Assert-Publication ($parseErrors.Count -eq 0) "release manager must parse"
$definition = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq "New-GitHubReleasePublication"
}, $false)
Assert-Publication ($null -ne $definition) "publication plan must exist"
# Load only this local file-planning function, never the release entry point.
. ([scriptblock]::Create($definition.Extent.Text))

$temporaryBase = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::GetTempPath()).TrimEnd("\")
$temporaryRoot = Join-Path $temporaryBase (
    "SnowDesktopReleasePublication-" + [guid]::NewGuid().ToString("N"))
$encoding = [System.Text.UTF8Encoding]::new($false)
try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $context = [pscustomobject]@{
        Version = "1.2.3.0"
        Tag = "v1.2.3.0"
        VersionDirectory = $temporaryRoot
    }
    $portableName = "SnowDesktop-portable-x64-1.2.3.0.zip"
    $portable = Join-Path $temporaryRoot $portableName
    [System.IO.File]::WriteAllText($portable, "abc", $encoding)
    $privateChecksums = Join-Path $temporaryRoot "SHA256SUMS.txt"
    $privateContent = "private Store checksum inventory`n"
    [System.IO.File]::WriteAllText($privateChecksums, $privateContent, $encoding)
    foreach ($extension in @("msix", "msixupload", "appxsym")) {
        [System.IO.File]::WriteAllText(
            (Join-Path $temporaryRoot "SnowDesktop-Store-x64-1.2.3.0.$extension"),
            "local Store fixture", $encoding)
    }

    foreach ($signed in @($false, $true)) {
        @{ msix = @{ signed = $signed; path = "SnowDesktop-Store-x64-1.2.3.0.msix" } } |
            ConvertTo-Json | Set-Content -LiteralPath (
                Join-Path $temporaryRoot "package-info.json") -Encoding UTF8
        $publication = New-GitHubReleasePublication -Context $context
        Assert-Publication ($publication.Title -ceq "1.2.3.0") `
            "title must be the raw four-part version"
        Assert-Publication ($publication.Notes -eq (
                Join-Path $temporaryRoot "release-notes.md")) `
            "notes must come from the current version directory"
        $checksums = Join-Path $temporaryRoot "github-release\SHA256SUMS.txt"
        Assert-Publication ($publication.Assets.Count -eq 2 -and
            $publication.Assets -contains $portable -and
            $publication.Assets -contains $checksums) `
            "unsigned or locally signed Store metadata must not add public assets"
        $expected = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  $portableName`n"
        Assert-Publication ([System.IO.File]::ReadAllText($checksums) -ceq $expected) `
            "public checksums must contain only the actual portable bytes"
        Assert-Publication ([System.IO.File]::ReadAllText($privateChecksums) -ceq
            $privateContent) "complete private checksums must remain intact"
    }

    # A stale generated checksum must be replaced when the portable bytes change.
    [System.IO.File]::WriteAllText($portable, "", $encoding)
    $publication = New-GitHubReleasePublication -Context $context
    $expected = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855  $portableName`n"
    Assert-Publication ([System.IO.File]::ReadAllText($publication.Assets[1]) -ceq
        $expected) "repeated preparation must refresh the public digest"

    # Keep both remote transports connected to the same title/attachment plan.
    $publisher = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -eq "Publish-GitHubRelease"
    }, $false)
    $publisherText = $publisher.Extent.Text -replace '\s|`', ''
    Assert-Publication ($publisherText.Contains(
            '$publication=New-GitHubReleasePublication-Context$context') -and
        $publisherText.Contains('$assets=$publication.Assets') -and
        $publisherText.Contains('--title$publication.Title') -and
        $publisherText.Contains('name=$publication.Title') -and
        -not $publisherText.Contains('$assets+=')) `
        "CLI and API transports must share the public plan without extra attachments"

    Write-Output "GitHub release publication checks passed."
}
finally {
    $resolvedRoot = [System.IO.Path]::GetFullPath($temporaryRoot)
    if (-not $resolvedRoot.StartsWith(
            $temporaryBase + "\SnowDesktopReleasePublication-",
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected publication test cleanup path"
    }
    if (Test-Path -LiteralPath $resolvedRoot) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
