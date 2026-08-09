[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$Development,
    [string]$IdentityName = "",
    [string]$Publisher = "",
    [string]$PublisherDisplayName = "",
    [string]$PackageFamilyName = "",
    [string]$PackageSid = "",
    [string]$StoreId = "",
    [string]$CertificatePath = "",
    [string]$CertificateThumbprint = "",
    [ValidateSet("CurrentUser", "LocalMachine")]
    [string]$CertificateStoreLocation = "CurrentUser",
    [string]$OutputDirectory = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $scriptDirectory ".."))
$packagingDirectory = Join-Path $repositoryRoot "packaging"
$buildOutput = Join-Path $repositoryRoot ".build\Release"
$artifactsRoot = Join-Path $repositoryRoot "artifacts"

function Assert-ChildPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Parent
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullParent = [System.IO.Path]::GetFullPath($Parent)
    $fullParent = $fullParent.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith(
            $fullParent, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the package output directory: $fullPath"
    }
}

function Reset-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)

    Assert-ChildPath -Path $Path -Parent $OutputDirectory
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Remove-OutputFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    Assert-ChildPath -Path $Path -Parent $OutputDirectory
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force
    }
}

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

function Copy-Directory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
        throw "Required directory was not found: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
}

function Copy-Payload {
    param([Parameter(Mandatory = $true)][string]$Destination)

    $requiredFiles = @(
        (Join-Path $buildOutput "SparkDesktop.exe"),
        (Join-Path $buildOutput "SnowDesktopTaskbarHook.dll"),
        (Join-Path $repositoryRoot "LICENSE"),
        (Join-Path $repositoryRoot "THIRD_PARTY_NOTICES.md"),
        (Join-Path $repositoryRoot "README.md"),
        (Join-Path $repositoryRoot "README.en.md")
    )
    foreach ($file in $requiredFiles) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "Required package file was not found: $file"
        }
        Copy-Item -LiteralPath $file -Destination $Destination -Force
    }

    $licensesDestination = Join-Path $Destination "licenses"
    New-Item -ItemType Directory -Path $licensesDestination -Force |
        Out-Null
    $fluentIconsLicense = Join-Path $repositoryRoot `
        "third_party\fluentui-system-icons\LICENSE"
    if (-not (Test-Path -LiteralPath $fluentIconsLicense -PathType Leaf)) {
        throw "Required Fluent System Icons license was not found: $fluentIconsLicense"
    }
    Copy-Item -LiteralPath $fluentIconsLicense `
        -Destination (Join-Path $licensesDestination `
            "FluentSystemIcons-LICENSE.txt") -Force

    Copy-Directory `
        -Source (Join-Path $repositoryRoot "widgets") `
        -Destination (Join-Path $Destination "widgets")
    Copy-Directory `
        -Source (Join-Path $repositoryRoot "lang") `
        -Destination (Join-Path $Destination "lang")

}

function Write-Logo {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height
    )

    Add-Type -AssemblyName System.Drawing
    $sourceImage = [System.Drawing.Image]::FromFile($Source)
    $bitmap = New-Object System.Drawing.Bitmap(
        $Width, $Height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingMode =
            [System.Drawing.Drawing2D.CompositingMode]::SourceOver
        $graphics.CompositingQuality =
            [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode =
            [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode =
            [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.SmoothingMode =
            [System.Drawing.Drawing2D.SmoothingMode]::HighQuality

        $scale = [Math]::Min(
            $Width / $sourceImage.Width,
            $Height / $sourceImage.Height)
        $drawWidth = [Math]::Max(
            1, [int][Math]::Round($sourceImage.Width * $scale))
        $drawHeight = [Math]::Max(
            1, [int][Math]::Round($sourceImage.Height * $scale))
        $x = [int][Math]::Floor(($Width - $drawWidth) / 2)
        $y = [int][Math]::Floor(($Height - $drawHeight) / 2)
        $graphics.DrawImage(
            $sourceImage, $x, $y, $drawWidth, $drawHeight)
        $bitmap.Save(
            $Destination, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $graphics.Dispose()
        $bitmap.Dispose()
        $sourceImage.Dispose()
    }
}

function Get-MakeAppxPath {
    $kitsRoot = (Get-ItemProperty `
        -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" `
        -Name KitsRoot10).KitsRoot10
    $binRoot = Join-Path $kitsRoot "bin"
    $versions = Get-ChildItem -LiteralPath $binRoot -Directory |
        Where-Object { $_.Name -match "^\d+\.\d+\.\d+\.\d+$" } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($versionDirectory in $versions) {
        $candidate = Join-Path $versionDirectory.FullName "x64\makeappx.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "makeappx.exe was not found in the Windows SDK."
}

function Escape-XmlAttribute {
    param([Parameter(Mandatory = $true)][string]$Value)
    return [System.Security.SecurityElement]::Escape($Value)
}

function Get-PackageLanguages {
    $languageDirectory = Join-Path $repositoryRoot "lang"
    $languageFiles = @(Get-ChildItem `
        -LiteralPath $languageDirectory `
        -Filter "*.json" `
        -File)
    if ($languageFiles.Count -eq 0) {
        throw "No language files were found in $languageDirectory."
    }

    $languages = foreach ($languageFile in $languageFiles) {
        $language = $languageFile.BaseName
        try {
            [void][System.Globalization.CultureInfo]::GetCultureInfo($language)
        }
        catch {
            throw "Language file '$($languageFile.Name)' does not use a valid BCP-47 language tag."
        }
        $language
    }
    $languages = @($languages | Sort-Object -Unique)
    if ($languages -contains "en-US") {
        $languages = @("en-US") + @(
            $languages | Where-Object { $_ -ne "en-US" })
    }
    return $languages
}

$version = (Get-Content `
    -LiteralPath (Join-Path $repositoryRoot "version.json") `
    -Encoding UTF8 `
    -Raw | ConvertFrom-Json).version
$versionParts = $version -split "\."
if ($versionParts.Count -lt 3 -or $versionParts.Count -gt 4 -or
    $versionParts[0] -eq "0" -or
    ($versionParts.Count -eq 4 -and $versionParts[3] -ne "0")) {
    throw "version.json must use A.B.C or Store-compatible A.B.C.0 format."
}
foreach ($part in $versionParts) {
    if ($part -notmatch "^(0|[1-9][0-9]*)$" -or
        [uint64]$part -gt 65535) {
        throw "Invalid version component '$part' in $version."
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $artifactsRoot "v$version"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)

if (-not $SkipBuild) {
    Write-Host "Running the repository-standard Release build."
    Write-Host "The build will not stop SnowDesktop or restart Explorer."
    & cmd.exe /d /c "call `"$repositoryRoot\scripts\build.bat`""
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/build.bat failed with exit code $LASTEXITCODE."
    }
}

if ($Development) {
    if ([string]::IsNullOrWhiteSpace($IdentityName)) {
        $IdentityName = "SnowDesktop.Dev"
    }
    if ([string]::IsNullOrWhiteSpace($Publisher)) {
        $Publisher = "CN=FreeFallingSnow"
    }
    if ([string]::IsNullOrWhiteSpace($PublisherDisplayName)) {
        $PublisherDisplayName = "FreeFallingSnow"
    }
}
else {
    $identityPath = Join-Path $packagingDirectory "store-identity.json"
    if (([string]::IsNullOrWhiteSpace($IdentityName) -or
        [string]::IsNullOrWhiteSpace($Publisher) -or
        [string]::IsNullOrWhiteSpace($PublisherDisplayName) -or
        [string]::IsNullOrWhiteSpace($PackageFamilyName) -or
        [string]::IsNullOrWhiteSpace($PackageSid) -or
        [string]::IsNullOrWhiteSpace($StoreId)) -and
        (Test-Path -LiteralPath $identityPath -PathType Leaf)) {
        $identity = Get-Content -LiteralPath $identityPath -Encoding UTF8 -Raw |
            ConvertFrom-Json
        if ([string]::IsNullOrWhiteSpace($IdentityName)) {
            $IdentityName = $identity.identityName
        }
        if ([string]::IsNullOrWhiteSpace($Publisher)) {
            $Publisher = $identity.publisher
        }
        if ([string]::IsNullOrWhiteSpace($PublisherDisplayName)) {
            $PublisherDisplayName = $identity.publisherDisplayName
        }
        if ([string]::IsNullOrWhiteSpace($PackageFamilyName)) {
            $PackageFamilyName = $identity.packageFamilyName
        }
        if ([string]::IsNullOrWhiteSpace($PackageSid)) {
            $PackageSid = $identity.packageSid
        }
        if ([string]::IsNullOrWhiteSpace($StoreId)) {
            $StoreId = $identity.storeId
        }
    }

    if ([string]::IsNullOrWhiteSpace($IdentityName) -or
        [string]::IsNullOrWhiteSpace($Publisher) -or
        [string]::IsNullOrWhiteSpace($PublisherDisplayName) -or
        [string]::IsNullOrWhiteSpace($PackageFamilyName) -or
        [string]::IsNullOrWhiteSpace($PackageSid) -or
        [string]::IsNullOrWhiteSpace($StoreId)) {
        throw "Store identity is incomplete. Update packaging\store-identity.json from Partner Center, or use -Development."
    }
}

if ($IdentityName -match "REPLACE_WITH_" -or
    $Publisher -match "REPLACE_WITH_" -or
    $PackageFamilyName -match "REPLACE_WITH_" -or
    $PackageSid -match "REPLACE_WITH_" -or
    $StoreId -match "REPLACE_WITH_") {
    throw "Replace the sample Store identity values with values from Partner Center."
}
if ($IdentityName -notmatch "^[A-Za-z0-9.-]{3,50}$" -or
    $IdentityName.EndsWith(".")) {
    throw "Invalid MSIX package identity name: $IdentityName"
}
if ($Publisher -notmatch "^[A-Za-z][A-Za-z0-9.]*=") {
    throw "Publisher must be a certificate distinguished name: $Publisher"
}
if (-not $Development) {
    if ($PackageFamilyName -notmatch "^[A-Za-z0-9.-]+_[a-z0-9]{13}$") {
        throw "Invalid package family name: $PackageFamilyName"
    }
    if ($PackageSid -notmatch "^S-1-15-2(?:-[0-9]+)+$") {
        throw "Invalid package SID: $PackageSid"
    }
    if ($StoreId -notmatch "^[A-Z0-9]{12}$") {
        throw "Invalid Microsoft Store ID: $StoreId"
    }
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stagingRoot = Join-Path $OutputDirectory "staging"
$portableStage = Join-Path $stagingRoot "portable"
$msixStage = Join-Path $stagingRoot "msix"
$symbolsStage = Join-Path $stagingRoot "symbols"
$uploadStage = Join-Path $stagingRoot "upload"
Reset-Directory -Path $stagingRoot
New-Item -ItemType Directory -Path $portableStage -Force | Out-Null
New-Item -ItemType Directory -Path $msixStage -Force | Out-Null
New-Item -ItemType Directory -Path $symbolsStage -Force | Out-Null
New-Item -ItemType Directory -Path $uploadStage -Force | Out-Null

Copy-Payload -Destination $portableStage
$portablePath = Join-Path $OutputDirectory `
    "SparkDesktop-portable-x64-$version.zip"
Remove-OutputFile -Path $portablePath
Compress-Archive `
    -Path (Join-Path $portableStage "*") `
    -DestinationPath $portablePath `
    -CompressionLevel Optimal

Copy-Payload -Destination $msixStage
$assetsDirectory = Join-Path $msixStage "Assets"
New-Item -ItemType Directory -Path $assetsDirectory -Force | Out-Null
$sourceLogo = Join-Path $repositoryRoot "assets\icon\icon.png"
$sourceSmallLogo = Join-Path $repositoryRoot "assets\icon\icon_small.png"
Write-Logo -Source $sourceLogo `
    -Destination (Join-Path $assetsDirectory "StoreLogo.png") `
    -Width 50 -Height 50
Write-Logo -Source $sourceSmallLogo `
    -Destination (Join-Path $assetsDirectory "Square44x44Logo.png") `
    -Width 44 -Height 44
Write-Logo -Source $sourceLogo `
    -Destination (Join-Path $assetsDirectory "Square150x150Logo.png") `
    -Width 150 -Height 150

# Windows uses target-size assets for the taskbar, Start, search, Settings,
# task view, Alt+Tab, and snap assist. Windows 11 expects separate default,
# dark-theme unplated, and light-theme unplated variants. If the light variant
# is missing, Settings can fall back to an accent-color backplate even when
# the dark-theme taskbar icon is already correct.
$appListTargetSizes = @(
    16, 20, 24, 30, 32, 36, 40, 44,
    48, 60, 64, 72, 80, 96, 256
)
foreach ($targetSize in $appListTargetSizes) {
    $targetSource = if ($targetSize -le 48) {
        $sourceSmallLogo
    }
    else {
        $sourceLogo
    }
    $variants = @(
        "Square44x44Logo.targetsize-$($targetSize).png",
        "Square44x44Logo.targetsize-$($targetSize)_altform-unplated.png",
        "Square44x44Logo.targetsize-$($targetSize)_altform-lightunplated.png"
    )
    foreach ($variant in $variants) {
        Write-Logo -Source $targetSource `
            -Destination (Join-Path $assetsDirectory $variant) `
            -Width $targetSize -Height $targetSize
    }
}

$displayName = if ($Development) {
    "SnowDesktop (Development)"
}
else {
    "SnowDesktop"
}
$packageLanguages = @(Get-PackageLanguages)
$resourceLanguageElements = @($packageLanguages | ForEach-Object {
    "    <Resource Language=`"$(Escape-XmlAttribute $_)`" />"
}) -join [Environment]::NewLine
$manifestTemplate = Get-Content `
    -LiteralPath (Join-Path $packagingDirectory "AppxManifest.xml.in") `
    -Encoding UTF8 `
    -Raw
$manifest = $manifestTemplate.Replace(
    "@IDENTITY_NAME@", (Escape-XmlAttribute $IdentityName))
$manifest = $manifest.Replace(
    "@PUBLISHER@", (Escape-XmlAttribute $Publisher))
$manifest = $manifest.Replace(
    "@VERSION@", (Escape-XmlAttribute $version))
$manifest = $manifest.Replace(
    "@DISPLAY_NAME@", (Escape-XmlAttribute $displayName))
$manifest = $manifest.Replace(
    "@PUBLISHER_DISPLAY_NAME@",
    (Escape-XmlAttribute $PublisherDisplayName))
$manifest = $manifest.Replace(
    "@RESOURCE_LANGUAGES@",
    $resourceLanguageElements)
if ($manifest -match "@[A-Z_]+@") {
    throw "The generated AppxManifest.xml still contains placeholders."
}
[System.IO.File]::WriteAllText(
    (Join-Path $msixStage "AppxManifest.xml"),
    $manifest,
    [System.Text.UTF8Encoding]::new($false))

$makeAppx = Get-MakeAppxPath
$makePri = Join-Path (Split-Path -Parent $makeAppx) "makepri.exe"
if (-not (Test-Path -LiteralPath $makePri -PathType Leaf)) {
    throw "makepri.exe was not found next to makeappx.exe."
}

# Qualified target-size assets and their light/dark unplated variants are
# resolved through the package resource index. Without resources.pri Windows
# falls back to the plain Square44x44Logo and adds an accent-color plate.
$priConfig = Join-Path $stagingRoot "priconfig.xml"
& $makePri createconfig /cf $priConfig /dq en-US /o
if ($LASTEXITCODE -ne 0) {
    throw "MakePri createconfig failed with exit code $LASTEXITCODE."
}
& $makePri new `
    /pr $msixStage `
    /cf $priConfig `
    /mn (Join-Path $msixStage "AppxManifest.xml") `
    /of (Join-Path $msixStage "resources.pri") `
    /o /v
if ($LASTEXITCODE -ne 0) {
    throw "MakePri new failed with exit code $LASTEXITCODE."
}

$msixPath = Join-Path $OutputDirectory `
    "SparkDesktop-Store-x64-$version.msix"
Remove-OutputFile -Path $msixPath
& $makeAppx pack /d $msixStage /p $msixPath /o /v
if ($LASTEXITCODE -ne 0) {
    throw "MakeAppx failed with exit code $LASTEXITCODE."
}

$signed = $false
$signArguments = @()
if (-not [string]::IsNullOrWhiteSpace($CertificatePath) -and
    -not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    throw "Use either CertificatePath or CertificateThumbprint, not both."
}
if (-not [string]::IsNullOrWhiteSpace($CertificatePath) -or
    -not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $signTool = Join-Path (Split-Path -Parent $makeAppx) "signtool.exe"
    $signArguments = @("sign", "/fd", "SHA256")
}
if (-not [string]::IsNullOrWhiteSpace($CertificatePath)) {
    $CertificatePath = [System.IO.Path]::GetFullPath($CertificatePath)
    if (-not (Test-Path -LiteralPath $CertificatePath -PathType Leaf)) {
        throw "Signing certificate was not found: $CertificatePath"
    }
    $signArguments += @("/f", $CertificatePath)
}
elseif (-not [string]::IsNullOrWhiteSpace($CertificateThumbprint)) {
    $CertificateThumbprint =
        ($CertificateThumbprint -replace "\s", "").ToUpperInvariant()
    if ($CertificateThumbprint -notmatch "^[0-9A-F]{40}$") {
        throw "CertificateThumbprint must be a 40-digit SHA-1 thumbprint."
    }
    $storeCertificatePath =
        "Cert:\$CertificateStoreLocation\My\$CertificateThumbprint"
    if (-not (Test-Path -LiteralPath $storeCertificatePath)) {
        throw "Signing certificate was not found in $CertificateStoreLocation\\My: $CertificateThumbprint"
    }
    $signArguments += @("/sha1", $CertificateThumbprint, "/s", "My")
    if ($CertificateStoreLocation -eq "LocalMachine") {
        $signArguments += "/sm"
    }
}
if ($signArguments.Count -gt 0) {
    $signArguments += $msixPath
    & $signTool @signArguments
    if ($LASTEXITCODE -ne 0) {
        throw "SignTool failed with exit code $LASTEXITCODE."
    }
    $signed = $true
}

$pdbPath = Join-Path $buildOutput "SnowDesktop.pdb"
if (-not (Test-Path -LiteralPath $pdbPath -PathType Leaf)) {
    throw "Release symbols were not found: $pdbPath"
}
Copy-Item -LiteralPath $pdbPath -Destination $symbolsStage -Force

$appxSymPath = Join-Path $OutputDirectory `
    "SparkDesktop-Store-x64-$version.appxsym"
$appxSymZipPath = Join-Path $OutputDirectory `
    "SparkDesktop-Store-x64-$version.appxsym.zip"
Remove-OutputFile -Path $appxSymPath
Remove-OutputFile -Path $appxSymZipPath
Compress-Archive `
    -Path (Join-Path $symbolsStage "*") `
    -DestinationPath $appxSymZipPath `
    -CompressionLevel Optimal
Move-Item -LiteralPath $appxSymZipPath -Destination $appxSymPath

Copy-Item -LiteralPath $msixPath -Destination $uploadStage -Force
Copy-Item -LiteralPath $appxSymPath -Destination $uploadStage -Force
$msixUploadPath = Join-Path $OutputDirectory `
    "SparkDesktop-Store-x64-$version.msixupload"
$msixUploadZipPath = Join-Path $OutputDirectory `
    "SparkDesktop-Store-x64-$version.msixupload.zip"
Remove-OutputFile -Path $msixUploadPath
Remove-OutputFile -Path $msixUploadZipPath
Compress-Archive `
    -Path (Join-Path $uploadStage "*") `
    -DestinationPath $msixUploadZipPath `
    -CompressionLevel Optimal
Move-Item -LiteralPath $msixUploadZipPath -Destination $msixUploadPath

$packageInfo = [ordered]@{
    version = $version
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    architecture = "x64"
    supportedLanguages = $packageLanguages
    developmentIdentity = [bool]$Development
    identityName = $IdentityName
    publisher = $Publisher
    publisherDisplayName = $PublisherDisplayName
    packageFamilyName = $PackageFamilyName
    packageSid = $PackageSid
    storeId = $StoreId
    portable = [ordered]@{
        path = [System.IO.Path]::GetFileName($portablePath)
        sha256 = Get-Sha256 -Path $portablePath
    }
    msix = [ordered]@{
        path = [System.IO.Path]::GetFileName($msixPath)
        sha256 = Get-Sha256 -Path $msixPath
        signed = $signed
    }
    appxSymbols = [ordered]@{
        path = [System.IO.Path]::GetFileName($appxSymPath)
        sha256 = Get-Sha256 -Path $appxSymPath
    }
    storeUpload = [ordered]@{
        path = [System.IO.Path]::GetFileName($msixUploadPath)
        sha256 = Get-Sha256 -Path $msixUploadPath
    }
}
$packageInfoPath = Join-Path $OutputDirectory "package-info.json"
$packageInfo | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $packageInfoPath -Encoding utf8

$packageFiles = @(
    $portablePath,
    $msixPath,
    $appxSymPath,
    $msixUploadPath
)
$checksumPath = Join-Path $OutputDirectory "SHA256SUMS.txt"
$checksumLines = foreach ($packageFile in $packageFiles) {
    "{0}  {1}" -f (Get-Sha256 -Path $packageFile),
        [System.IO.Path]::GetFileName($packageFile)
}
$checksumLines |
    Set-Content -LiteralPath $checksumPath -Encoding ascii

$gitBranch = (& git -C $repositoryRoot branch --show-current 2>$null |
    Select-Object -First 1)
$gitCommit = (& git -C $repositoryRoot rev-parse HEAD 2>$null |
    Select-Object -First 1)
$gitDirty = [bool](& git -C $repositoryRoot status --porcelain 2>$null |
    Select-Object -First 1)
$summaryPath = Join-Path $OutputDirectory "release-summary.md"
$summaryLines = @(
    "# SnowDesktop v$version 发布摘要",
    "",
    "- 生成时间（UTC）：$((Get-Date).ToUniversalTime().ToString("yyyy-MM-dd HH:mm:ss"))",
    "- 源码分支：$gitBranch",
    "- 源码提交：$gitCommit",
    "- 工作区包含未提交修改：$gitDirty",
    "- 架构：x64",
    "- 支持语言：$($packageLanguages -join ', ')",
    "- MSIX 已签名：$signed",
    "",
    "## 发行文件",
    "",
    "| 文件 | SHA-256 |",
    "| --- | --- |"
)
foreach ($packageFile in $packageFiles) {
    $summaryLines += "| $([System.IO.Path]::GetFileName($packageFile)) | ``$(Get-Sha256 -Path $packageFile)`` |"
}
$summaryLines += @(
    "",
    "详细机器可读信息见 ``package-info.json``，校验清单见 ``SHA256SUMS.txt``。",
    "通过统一发布界面执行时，过程日志保存在 ``logs`` 目录。"
)
$summaryLines |
    Set-Content -LiteralPath $summaryPath -Encoding utf8

$notesPath = Join-Path $OutputDirectory "release-notes.md"
if (-not (Test-Path -LiteralPath $notesPath -PathType Leaf)) {
    @(
        "# SnowDesktop v$version 发布说明",
        "",
        "## 更新内容",
        "",
        "- 请在发布前填写本版本更新内容。",
        "",
        "## 发布检查",
        "",
        "- [ ] 携带版启动与数据目录验证",
        "- [ ] MSIX 本地安装与升级验证",
        "- [ ] 开机自启状态验证",
        "- [ ] 多版本防重复启动验证",
        "- [ ] Microsoft Store 提交材料复核"
    ) | Set-Content -LiteralPath $notesPath -Encoding utf8
}

if (Test-Path -LiteralPath $stagingRoot -PathType Container) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}

Write-Host ""
Write-Host "Package preparation complete:"
Write-Host "  Portable: $portablePath"
Write-Host "  MSIX:     $msixPath"
Write-Host "  Upload:   $msixUploadPath"
Write-Host "  Metadata: $packageInfoPath"
Write-Host "  Summary:  $summaryPath"
Write-Host "  Checksums:$checksumPath"
if (-not $signed) {
    Write-Warning "The MSIX is unsigned. Sign it before sideloading; Partner Center will perform its own validation during upload."
}
