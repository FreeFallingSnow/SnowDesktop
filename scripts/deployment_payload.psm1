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
    }
    foreach ($entry in @($manifest.files)) {
        $buildPath = if ($null -ne $entry.PSObject.Properties["buildPath"] -and
            -not [string]::IsNullOrWhiteSpace([string]$entry.buildPath)) {
            [string]$entry.buildPath
        }
        else {
            [string]$entry.path
        }
        $path = Resolve-SnowDesktopDeploymentPath `
            -Root $BuildOutput -RelativePath $buildPath
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Deployment manifest file is missing: $path"
        }
        if ((Get-SnowDesktopFileHash -Path $path) -cne $entry.sha256) {
            throw "Deployment manifest hash mismatch: $path"
        }
    }
    foreach ($entry in @($manifest.appxFragments) + @($manifest.notices)) {
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
        "licenses/WindowsML-NOTICE.txt",
        "licenses/WebView2-LICENSE.txt",
        "licenses/WebView2-NOTICE.txt"
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

function Get-SnowDesktopRuntimeRelativePath {
    param(
        [Parameter(Mandatory = $true)]$Entry,
        [string]$RuntimeDirectory = ""
    )

    $relativePath = [string]$Entry.path
    if ([string]::IsNullOrWhiteSpace($RuntimeDirectory) -or
        [string]$Entry.source -ceq "SnowDesktop") {
        return $relativePath
    }
    if ([System.IO.Path]::GetFileName($RuntimeDirectory) -cne
            $RuntimeDirectory -or
        $RuntimeDirectory -in @(".", "..")) {
        throw "RuntimeDirectory must be a single safe directory name: $RuntimeDirectory"
    }
    return "$RuntimeDirectory/$relativePath"
}

function Copy-SnowDesktopDeploymentPayload {
    param(
        [Parameter(Mandatory = $true)][string]$BuildOutput,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string]$RuntimeDirectory = ""
    )

    $manifest = Read-SnowDesktopDeploymentManifest -BuildOutput $BuildOutput
    foreach ($entry in @($manifest.files)) {
        $buildPath = if ($null -ne $entry.PSObject.Properties["buildPath"] -and
            -not [string]::IsNullOrWhiteSpace([string]$entry.buildPath)) {
            [string]$entry.buildPath
        }
        else {
            [string]$entry.path
        }
        $source = Resolve-SnowDesktopDeploymentPath `
            -Root $BuildOutput -RelativePath $buildPath
        $relativeTarget = Get-SnowDesktopRuntimeRelativePath `
            -Entry $entry -RuntimeDirectory $RuntimeDirectory
        $target = Resolve-SnowDesktopDeploymentPath `
            -Root $Destination -RelativePath $relativeTarget
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

function Find-SnowDesktopWindowsSdkTool {
    param([Parameter(Mandatory = $true)][string]$Name)

    $kitsRoot = (Get-ItemProperty `
        -LiteralPath "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" `
        -Name KitsRoot10).KitsRoot10
    $binRoot = Join-Path $kitsRoot "bin"
    $versionDirectories = @(
        [System.IO.Directory]::EnumerateDirectories($binRoot) |
            Where-Object {
                [System.IO.Path]::GetFileName($_) -match
                    "^\d+\.\d+\.\d+\.\d+$"
            } |
            Sort-Object {
                [version][System.IO.Path]::GetFileName($_)
            } -Descending
    )
    foreach ($versionDirectory in $versionDirectories) {
        $candidate = Join-Path $versionDirectory "x64\$Name"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "$Name was not found in the Windows SDK."
}

function Save-SnowDesktopXmlDocument {
    param(
        [Parameter(Mandatory = $true)]
        [System.Xml.XmlDocument]$Document,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $settings = [System.Xml.XmlWriterSettings]::new()
    $settings.Encoding = [System.Text.UTF8Encoding]::new($false)
    $settings.Indent = $true
    $settings.NewLineChars = [Environment]::NewLine
    $settings.NewLineHandling = [System.Xml.NewLineHandling]::Replace
    $writer = [System.Xml.XmlWriter]::Create($Path, $settings)
    try {
        $Document.Save($writer)
    }
    finally {
        $writer.Dispose()
    }
}

function Add-SnowDesktopPrivateAssemblyDependency {
    param(
        [Parameter(Mandatory = $true)]
        [System.Xml.XmlDocument]$Document,
        [Parameter(Mandatory = $true)]
        [System.Xml.XmlNamespaceManager]$Namespaces,
        [Parameter(Mandatory = $true)][string]$RuntimeDirectory,
        [Parameter(Mandatory = $true)][string]$Version
    )

    $applicationRoot = $Document.SelectSingleNode(
        "/asmv1:assembly", $Namespaces)
    if ($null -eq $applicationRoot) {
        throw "Executable manifest has an unexpected root element."
    }
    $existing = $applicationRoot.SelectSingleNode(
        "asmv1:dependency/asmv1:dependentAssembly/asmv1:assemblyIdentity[@name='$RuntimeDirectory']",
        $Namespaces)
    if ($null -ne $existing) {
        return
    }

    $dependency = $Document.CreateElement(
        "dependency", "urn:schemas-microsoft-com:asm.v1")
    $dependentAssembly = $Document.CreateElement(
        "dependentAssembly", "urn:schemas-microsoft-com:asm.v1")
    $dependencyIdentity = $Document.CreateElement(
        "assemblyIdentity", "urn:schemas-microsoft-com:asm.v1")
    $dependencyIdentity.SetAttribute("type", "win32")
    $dependencyIdentity.SetAttribute("name", $RuntimeDirectory)
    $dependencyIdentity.SetAttribute("version", $Version)
    $dependencyIdentity.SetAttribute("processorArchitecture", "amd64")
    $dependencyIdentity.SetAttribute("language", "*")
    [void]$dependentAssembly.AppendChild($dependencyIdentity)
    [void]$dependency.AppendChild($dependentAssembly)
    $trustInfo = $applicationRoot.SelectSingleNode(
        "asmv3:trustInfo", $Namespaces)
    if ($null -eq $trustInfo) {
        [void]$applicationRoot.AppendChild($dependency)
    }
    else {
        [void]$applicationRoot.InsertBefore($dependency, $trustInfo)
    }
}

function Enable-SnowDesktopPrivateRuntimeAssembly {
    param(
        [Parameter(Mandatory = $true)][string]$BuildOutput,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$Version,
        [string]$RuntimeDirectory = "SnowDesktop.Runtime",
        [string[]]$AdditionalRuntimeDlls = @(),
        [string[]]$AdditionalExecutables = @()
    )

    if ($Version -notmatch "^[1-9][0-9]*\.[0-9]+\.[0-9]+\.0$") {
        throw "Private runtime assembly version must use A.B.C.0 format: $Version"
    }
    if ([System.IO.Path]::GetFileName($RuntimeDirectory) -cne
            $RuntimeDirectory -or
        $RuntimeDirectory -in @(".", "..")) {
        throw "RuntimeDirectory must be a single safe directory name: $RuntimeDirectory"
    }

    $deployment = Read-SnowDesktopDeploymentManifest -BuildOutput $BuildOutput
    $runtimeRoot = Resolve-SnowDesktopDeploymentPath `
        -Root $PackageRoot -RelativePath $RuntimeDirectory
    if (-not (Test-Path -LiteralPath $runtimeRoot -PathType Container)) {
        throw "Private runtime directory was not found: $runtimeRoot"
    }
    $executable = Join-Path $PackageRoot "SnowDesktop.exe"
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "SnowDesktop.exe was not found in the package root: $PackageRoot"
    }

    $runtimeDlls = [System.Collections.Generic.HashSet[string]]::new(
        [System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in @($deployment.files)) {
        if ([string]$entry.source -ceq "SnowDesktop") {
            continue
        }
        $relativePath = ([string]$entry.path).Replace("/", "\")
        $runtimePath = Resolve-SnowDesktopDeploymentPath `
            -Root $runtimeRoot -RelativePath $relativePath
        if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
            throw "Private runtime payload file was not found: $runtimePath"
        }
        if ($relativePath.EndsWith(
                ".dll", [System.StringComparison]::OrdinalIgnoreCase)) {
            if ([System.IO.Path]::GetFileName($relativePath) -cne
                    $relativePath) {
                throw "Private runtime DLLs must be stored directly in $RuntimeDirectory`: $relativePath"
            }
            [void]$runtimeDlls.Add($relativePath)
        }
    }
    foreach ($dll in $AdditionalRuntimeDlls) {
        if ([System.IO.Path]::GetFileName($dll) -cne $dll -or
            -not $dll.EndsWith(
                ".dll", [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Additional runtime DLL must be a safe DLL filename: $dll"
        }
        $runtimePath = Join-Path $runtimeRoot $dll
        if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
            throw "Additional private runtime DLL was not found: $runtimePath"
        }
        [void]$runtimeDlls.Add($dll)
    }
    if ($runtimeDlls.Count -eq 0) {
        throw "The deployment manifest does not contain private runtime DLLs."
    }

    $mt = Find-SnowDesktopWindowsSdkTool -Name "mt.exe"
    $applicationManifestPath = [System.IO.Path]::GetTempFileName()
    try {
        & $mt "-inputresource:$executable;#1" `
            "-out:$applicationManifestPath"
        if ($LASTEXITCODE -ne 0) {
            throw "mt.exe could not extract the SnowDesktop application manifest (exit $LASTEXITCODE)."
        }

        $applicationDocument = [System.Xml.XmlDocument]::new()
        $applicationDocument.PreserveWhitespace = $false
        $applicationDocument.Load($applicationManifestPath)
        $applicationNamespaces = [System.Xml.XmlNamespaceManager]::new(
            $applicationDocument.NameTable)
        $applicationNamespaces.AddNamespace(
            "asmv1", "urn:schemas-microsoft-com:asm.v1")
        $applicationNamespaces.AddNamespace(
            "asmv3", "urn:schemas-microsoft-com:asm.v3")
        $activationFiles = @($applicationDocument.SelectNodes(
            "/asmv1:assembly/asmv3:file", $applicationNamespaces))
        $activationFilesComeFromApplication = $true
        if ($activationFiles.Count -eq 0) {
            # Build output is already runnable through the private assembly.
            # Reuse its activation declarations when producing a portable,
            # MSIX, or Steam copy with a potentially different DLL set.
            $existingPrivateManifest = Join-Path `
                (Join-Path $BuildOutput $RuntimeDirectory) `
                "$RuntimeDirectory.manifest"
            if (-not (Test-Path -LiteralPath $existingPrivateManifest `
                    -PathType Leaf)) {
                throw "The SnowDesktop application manifest contains no WinRT activation files, and its build private assembly manifest is missing: $existingPrivateManifest"
            }
            $existingPrivateDocument = [System.Xml.XmlDocument]::new()
            $existingPrivateDocument.PreserveWhitespace = $false
            $existingPrivateDocument.Load($existingPrivateManifest)
            $existingPrivateNamespaces = `
                [System.Xml.XmlNamespaceManager]::new(
                    $existingPrivateDocument.NameTable)
            $existingPrivateNamespaces.AddNamespace(
                "asmv1", "urn:schemas-microsoft-com:asm.v1")
            $existingPrivateNamespaces.AddNamespace(
                "asmv3", "urn:schemas-microsoft-com:asm.v3")
            $activationFiles = @($existingPrivateDocument.SelectNodes(
                "/asmv1:assembly/asmv3:file[*]", `
                $existingPrivateNamespaces))
            $activationFilesComeFromApplication = $false
            if ($activationFiles.Count -eq 0) {
                throw "The existing SnowDesktop private assembly manifest contains no WinRT activation files: $existingPrivateManifest"
            }
        }

        $privateDocument = [System.Xml.XmlDocument]::new()
        $privateDocument.PreserveWhitespace = $false
        [void]$privateDocument.AppendChild(
            $privateDocument.CreateXmlDeclaration("1.0", "UTF-8", $null))
        $privateAssembly = $privateDocument.CreateElement(
            "assembly", "urn:schemas-microsoft-com:asm.v1")
        $privateAssembly.SetAttribute("manifestVersion", "1.0")
        [void]$privateDocument.AppendChild($privateAssembly)
        $privateIdentity = $privateDocument.CreateElement(
            "assemblyIdentity", "urn:schemas-microsoft-com:asm.v1")
        $privateIdentity.SetAttribute("type", "win32")
        $privateIdentity.SetAttribute("name", $RuntimeDirectory)
        $privateIdentity.SetAttribute("version", $Version)
        $privateIdentity.SetAttribute("processorArchitecture", "amd64")
        [void]$privateAssembly.AppendChild($privateIdentity)

        $listedDlls = [System.Collections.Generic.HashSet[string]]::new(
            [System.StringComparer]::OrdinalIgnoreCase)
        foreach ($activationFile in $activationFiles) {
            $dll = $activationFile.GetAttribute("name")
            if (-not $runtimeDlls.Contains($dll)) {
                throw "The application manifest references a DLL outside the private runtime payload: $dll"
            }
            [void]$listedDlls.Add($dll)
            [void]$privateAssembly.AppendChild(
                $privateDocument.ImportNode($activationFile, $true))
            if ($activationFilesComeFromApplication) {
                [void]$activationFile.ParentNode.RemoveChild($activationFile)
            }
        }
        foreach ($dll in $runtimeDlls) {
            if ($listedDlls.Add($dll)) {
                $file = $privateDocument.CreateElement(
                    "file", "urn:schemas-microsoft-com:asm.v3")
                $file.SetAttribute("name", $dll)
                [void]$privateAssembly.AppendChild($file)
            }
        }

        Add-SnowDesktopPrivateAssemblyDependency `
            -Document $applicationDocument `
            -Namespaces $applicationNamespaces `
            -RuntimeDirectory $RuntimeDirectory `
            -Version $Version

        $privateManifestPath = Join-Path $runtimeRoot `
            "$RuntimeDirectory.manifest"
        Save-SnowDesktopXmlDocument `
            -Document $privateDocument -Path $privateManifestPath
        Save-SnowDesktopXmlDocument `
            -Document $applicationDocument -Path $applicationManifestPath
        & $mt "-manifest" $privateManifestPath "-validate_manifest"
        if ($LASTEXITCODE -ne 0) {
            throw "The private runtime assembly manifest is invalid (exit $LASTEXITCODE)."
        }
        & $mt "-manifest" $applicationManifestPath `
            "-outputresource:$executable;#1"
        if ($LASTEXITCODE -ne 0) {
            throw "mt.exe could not embed the private runtime dependency (exit $LASTEXITCODE)."
        }

        foreach ($name in $AdditionalExecutables) {
            if ([System.IO.Path]::GetFileName($name) -cne $name -or
                -not $name.EndsWith(
                    ".exe", [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "Additional executable must be a safe EXE filename: $name"
            }
            $dependentExecutable = Join-Path $PackageRoot $name
            if (-not (Test-Path -LiteralPath $dependentExecutable -PathType Leaf)) {
                throw "Additional private runtime executable was not found: $dependentExecutable"
            }
            $dependentManifestPath = [System.IO.Path]::GetTempFileName()
            try {
                & $mt "-inputresource:$dependentExecutable;#1" `
                    "-out:$dependentManifestPath"
                if ($LASTEXITCODE -ne 0) {
                    throw "mt.exe could not extract the $name manifest (exit $LASTEXITCODE)."
                }
                $dependentDocument = [System.Xml.XmlDocument]::new()
                $dependentDocument.PreserveWhitespace = $false
                $dependentDocument.Load($dependentManifestPath)
                $dependentNamespaces = [System.Xml.XmlNamespaceManager]::new(
                    $dependentDocument.NameTable)
                $dependentNamespaces.AddNamespace(
                    "asmv1", "urn:schemas-microsoft-com:asm.v1")
                $dependentNamespaces.AddNamespace(
                    "asmv3", "urn:schemas-microsoft-com:asm.v3")
                Add-SnowDesktopPrivateAssemblyDependency `
                    -Document $dependentDocument `
                    -Namespaces $dependentNamespaces `
                    -RuntimeDirectory $RuntimeDirectory `
                    -Version $Version
                Save-SnowDesktopXmlDocument `
                    -Document $dependentDocument -Path $dependentManifestPath
                & $mt "-manifest" $dependentManifestPath `
                    "-outputresource:$dependentExecutable;#1"
                if ($LASTEXITCODE -ne 0) {
                    throw "mt.exe could not embed the private runtime dependency in $name (exit $LASTEXITCODE)."
                }
            }
            finally {
                if (Test-Path -LiteralPath $dependentManifestPath -PathType Leaf) {
                    Remove-Item -LiteralPath $dependentManifestPath -Force
                }
            }
        }
    }
    finally {
        if (Test-Path -LiteralPath $applicationManifestPath -PathType Leaf) {
            Remove-Item -LiteralPath $applicationManifestPath -Force
        }
    }
}

function Merge-SnowDesktopAppxFragments {
    param(
        [Parameter(Mandatory = $true)][string]$BuildOutput,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [string]$RuntimeDirectory = ""
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
                if ([System.IO.Path]::GetFileName($dll) -cne $dll) {
                    throw "Package fragment references a missing or unsafe runtime file: $dll"
                }
                $relativePath = if ([string]::IsNullOrWhiteSpace(
                        $RuntimeDirectory)) {
                    $dll
                }
                else {
                    "$RuntimeDirectory\$dll"
                }
                if (-not (Test-Path -LiteralPath `
                        (Join-Path $PackageRoot $relativePath) -PathType Leaf)) {
                    throw "Package fragment references a missing or unsafe runtime file: $relativePath"
                }
                $pathNode.InnerText = $relativePath
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
    Enable-SnowDesktopPrivateRuntimeAssembly, `
    Merge-SnowDesktopAppxFragments
