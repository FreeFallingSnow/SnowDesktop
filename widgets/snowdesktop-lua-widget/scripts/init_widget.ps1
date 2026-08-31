[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Destination,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9]+(?:-[a-z0-9]+)*$')]
    [string] $Slug,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $Name,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $Author,

    [ValidateNotNullOrEmpty()]
    [string] $Description = 'A SnowDesktop Lua widget.',

    [string] $Id = ([guid]::NewGuid().ToString('D'))
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$skillRoot = Split-Path -Parent $PSScriptRoot
$template = Join-Path $skillRoot 'assets\widget-template'
$destinationFull = [IO.Path]::GetFullPath($Destination)
$manifestPath = Join-Path $destinationFull 'widget.json'
$entryPath = Join-Path $destinationFull 'main.lua'
$ownsDestination = $false

try {
    if (-not (Test-Path -LiteralPath $template -PathType Container)) {
        throw "Bundled widget template is missing: $template"
    }
    if (Test-Path -LiteralPath $destinationFull) {
        throw "Destination already exists: $destinationFull"
    }
    $ownsDestination = $true

    [guid] $parsedId = [guid]::Empty
    if (-not [guid]::TryParse($Id, [ref] $parsedId) -or
        $parsedId -eq [guid]::Empty) {
        throw 'Id must be a non-empty UUID.'
    }

    $parent = Split-Path -Parent $destinationFull
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Copy-Item -LiteralPath $template -Destination $destinationFull -Recurse

    $keyPrefix = 'lua_widget.' + $Slug.Replace('-', '_')
    $manifestText = [IO.File]::ReadAllText($manifestPath)
    $manifestText = $manifestText.Replace('lua_widget.template', $keyPrefix)
    $manifest = $manifestText | ConvertFrom-Json
    $manifest.id = $parsedId.ToString('D')
    $manifest.slug = $Slug
    $manifest.name = $Name
    $manifest.description = $Description
    $manifest.author = $Author

    $englishLocale = $manifest.locales.'en-US'
    $englishLocale.PSObject.Properties[$keyPrefix + '.name'].Value = $Name
    $englishLocale.PSObject.Properties[$keyPrefix + '.description'].Value = $Description
    $manifest.locales = [pscustomobject]@{
        'en-US' = $englishLocale
    }

    $utf8NoBom = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 100) + [Environment]::NewLine,
        $utf8NoBom)

    $entryText = [IO.File]::ReadAllText($entryPath)
    $entryText = $entryText.Replace('lua_widget.template', $keyPrefix)
    [IO.File]::WriteAllText($entryPath, $entryText, $utf8NoBom)

    [ordered]@{
        ok = $true
        directory = $destinationFull
        id = $manifest.id
        slug = $manifest.slug
        keyPrefix = $keyPrefix
    } | ConvertTo-Json -Compress
}
catch {
    if ($ownsDestination -and
        (Test-Path -LiteralPath $destinationFull -PathType Container)) {
        Remove-Item -LiteralPath $destinationFull -Recurse -Force
    }
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
