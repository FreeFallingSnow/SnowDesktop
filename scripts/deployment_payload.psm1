Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-SnowDesktopFileHash {
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

function Resolve-SnowDesktopDeploymentPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [System.IO.Path]::IsPathRooted($RelativePath)) {
        throw "Deployment manifest paths must be non-empty and relative: $RelativePath"
    }
    $normalized = $RelativePath.Replace("/", "\")
    $parts = @($normalized.Split("\", [System.StringSplitOptions]::RemoveEmptyEntries))
    if ($parts.Count -eq 0 -or $parts -contains "." -or $parts -contains "..") {
        throw "Deployment manifest path contains traversal: $RelativePath"
    }
    $fullRoot = [System.IO.Path]::GetFullPath($Root).TrimEnd("\") + "\"
    $fullPath = [System.IO.Path]::GetFullPath((Join-Path $fullRoot ($parts -join "\")))
    if (-not $fullPath.StartsWith(
            $fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Deployment manifest path escapes its root: $RelativePath"
    }
    return $fullPath
}

function Read-SnowDesktopDeploymentManifest {
    param([Parameter(Mandatory = $true)][string]$BuildOutput)

    $manifestPath = Join-Path $BuildOutput "SnowDesktop.deployment.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "Self-contained deployment manifest was not found: $manifestPath. Run scripts\build.bat first."
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Encoding UTF8 -Raw |
        ConvertFrom-Json
    if ([int]$manifest.schemaVersion -ne 1 -or
        $manifest.architecture -cne "x64" -or
        $manifest.windowsAppSdkPackageVersion -cne "2.4.0" -or
        $manifest.cppWinRtPackageVersion -cne "3.0.260818.1") {
        throw "Unsupported or stale SnowDesktop deployment manifest: $manifestPath"
    }

    $seen = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in @($manifest.files) + @($manifest.appxFragments) +
            @($manifest.notices)) {
        if ($null -eq $entry -or
            $entry.sha256 -cnotmatch "^[0-9A-F]{64}$" -or
            -not $seen.Add([string]$entry.path)) {
            throw "Deployment manifest contains an invalid or duplicate entry."
        }
        $path = Resolve-SnowDesktopDeploymentPath `
            -Root $BuildOutput -RelativePath ([string]$entry.path)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Deployment manifest file is missing: $path"
        }
        if ((Get-SnowDesktopFileHash -Path $path) -cne $entry.sha256) {
            throw "Deployment manifest hash mismatch: $path"
        }
    }
    $requiredNoticeDestinations = @(
        "licenses/CppWinRT-LICENSE.txt",
        "licenses/WindowsAppSDK-LICENSE.txt",
        "licenses/WindowsAppSDK-NOTICE.txt",
        "licenses/WindowsML-LICENSE.txt",
        "licenses/WindowsML-NOTICE.txt"
    )
    $noticeDestinations = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in @($manifest.notices)) {
        $destination = [string]$entry.destination
        if (-not $noticeDestinations.Add($destination)) {
            throw "Deployment manifest contains an invalid or duplicate notice destination."
        }
        [void](Resolve-SnowDesktopDeploymentPath `
            -Root $BuildOutput -RelativePath $destination)
    }
    if (@($manifest.files).Count -eq 0 -or
        @($manifest.appxFragments).Count -eq 0 -or
        @($manifest.notices).Count -ne $requiredNoticeDestinations.Count -or
        @($requiredNoticeDestinations | Where-Object {
            -not $noticeDestinations.Contains($_)
        }).Count -ne 0) {
        throw "Deployment manifest must include runtime files, package fragments, and notices."
    }
    return $manifest
}

function Copy-SnowDesktopDeploymentPayload {
    param(
        [Parameter(Mandatory = $true)][string]$BuildOutput,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $manifest = Read-SnowDesktopDeploymentManifest -BuildOutput $BuildOutput
    foreach ($entry in @($manifest.files)) {
        $source = Resolve-SnowDesktopDeploymentPath `
            -Root $BuildOutput -RelativePath ([string]$entry.path)
        $target = Resolve-SnowDesktopDeploymentPath `
            -Root $Destination -RelativePath ([string]$entry.path)
        $parent = Split-Path -Parent $target
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    foreach ($entry in @($manifest.notices)) {
        $source = Resolve-SnowDesktopDeploymentPath `
            -Root $BuildOutput -RelativePath ([string]$entry.path)
        $target = Resolve-SnowDesktopDeploymentPath `
            -Root $Destination -RelativePath ([string]$entry.destination)
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) `
            -Force | Out-Null
        Copy-Item -LiteralPath $source -Destination $target -Force
    }
    return $manifest
}

function Merge-SnowDesktopAppxFragments {
    param(
        [Parameter(Mandatory = $true)][string]$BuildOutput,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath
    )

    $deployment = Read-SnowDesktopDeploymentManifest -BuildOutput $BuildOutput
    $namespace = "http://schemas.microsoft.com/appx/manifest/foundation/windows10"
    $document = [System.Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $false
    $document.Load($ManifestPath)
    if ($document.DocumentElement.LocalName -cne "Package" -or
        $document.DocumentElement.NamespaceURI -cne $namespace) {
        throw "The generated MSIX manifest has an unexpected root element."
    }

    $manager = [System.Xml.XmlNamespaceManager]::new($document.NameTable)
    $manager.AddNamespace("m", $namespace)
    $extensions = $document.SelectSingleNode("/m:Package/m:Extensions", $manager)
    if ($null -eq $extensions) {
        $extensions = $document.CreateElement("Extensions", $namespace)
        $capabilities = $document.SelectSingleNode("/m:Package/m:Capabilities", $manager)
        if ($null -eq $capabilities) {
            [void]$document.DocumentElement.AppendChild($extensions)
        }
        else {
            [void]$document.DocumentElement.InsertBefore($extensions, $capabilities)
        }
    }

    $merged = 0
    foreach ($entry in @($deployment.appxFragments)) {
        $fragmentPath = Resolve-SnowDesktopDeploymentPath `
            -Root $BuildOutput -RelativePath ([string]$entry.path)
        $fragment = [System.Xml.XmlDocument]::new()
        $fragment.Load($fragmentPath)
        $fragmentManager = [System.Xml.XmlNamespaceManager]::new($fragment.NameTable)
        $fragmentManager.AddNamespace("m", $namespace)
        $nodes = @($fragment.SelectNodes(
            "/m:Fragment/m:Extensions/m:Extension", $fragmentManager))
        foreach ($node in $nodes) {
            $category = $node.Attributes["Category"]
            if ($null -eq $category -or
                -not $category.Value.StartsWith(
                    "windows.activatableClass.",
                    [System.StringComparison]::Ordinal)) {
                throw "Unsupported extension in official package fragment: $fragmentPath"
            }
            foreach ($pathNode in @($node.SelectNodes(".//m:Path", $fragmentManager))) {
                $dll = [string]$pathNode.InnerText
                if ([System.IO.Path]::GetFileName($dll) -cne $dll -or
                    -not (Test-Path -LiteralPath (Join-Path $PackageRoot $dll) -PathType Leaf)) {
                    throw "Package fragment references a missing or unsafe runtime file: $dll"
                }
            }
            [void]$extensions.AppendChild($document.ImportNode($node, $true))
            ++$merged
        }
    }
    if ($merged -eq 0) {
        throw "No activatable-class registrations were merged into the MSIX manifest."
    }

    $settings = [System.Xml.XmlWriterSettings]::new()
    $settings.Encoding = [System.Text.UTF8Encoding]::new($false)
    $settings.Indent = $true
    $settings.NewLineChars = [Environment]::NewLine
    $settings.NewLineHandling = [System.Xml.NewLineHandling]::Replace
    $writer = [System.Xml.XmlWriter]::Create($ManifestPath, $settings)
    try {
        $document.Save($writer)
    }
    finally {
        $writer.Dispose()
    }
}

Export-ModuleMember -Function `
    Read-SnowDesktopDeploymentManifest, `
    Copy-SnowDesktopDeploymentPayload, `
    Merge-SnowDesktopAppxFragments
