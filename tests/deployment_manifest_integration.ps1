[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$RepositoryRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepositoryRoot = [System.IO.Path]::GetFullPath($RepositoryRoot)
$probe = Join-Path ([System.IO.Path]::GetTempPath()) `
    ("SnowDesktop-deployment-" + [guid]::NewGuid().ToString("N"))
try {
    $target = Join-Path $probe "target"
    $intermediate = Join-Path $probe "intermediate"
    $fragmentDirectory = Join-Path $probe `
        "nuget\microsoft.windowsappsdk.test\1.0.0\runtimes-framework"
    $payload = Join-Path $probe "payload"
    New-Item -ItemType Directory `
        -Path $target, $intermediate, $fragmentDirectory, $payload `
        -Force | Out-Null

    $required = @(
        "App.xbf",
        "SettingsShell.xbf",
        "SnowDesktop.pri",
        "SnowDesktop.winmd",
        "Microsoft.WindowsAppRuntime.dll",
        "Microsoft.ui.xaml.dll",
        "Microsoft.UI.Xaml.winmd"
    )
    foreach ($name in $required) {
        [System.IO.File]::WriteAllText(
            (Join-Path $target $name), "fixture:$name")
    }
    $fileList = Join-Path $intermediate "files.txt"
    [System.IO.File]::WriteAllLines(
        $fileList,
        @($required | ForEach-Object {
            "$_`tSnowDesktop`tGenerated"
        }),
        [System.Text.UTF8Encoding]::new($false))

    $fragment = Join-Path $fragmentDirectory "package.appxfragment"
    [System.IO.File]::WriteAllText(
        $fragment,
        '<?xml version="1.0" encoding="utf-8"?><Fragment xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"><Extensions><Extension Category="windows.activatableClass.inProcessServer"><InProcessServer><Path>Microsoft.ui.xaml.dll</Path><ActivatableClass ActivatableClassId="SnowDesktop.Test" ThreadingModel="both" /></InProcessServer></Extension></Extensions></Fragment>',
        [System.Text.UTF8Encoding]::new($false))
    $fragmentList = Join-Path $intermediate "fragments.txt"
    [System.IO.File]::WriteAllText(
        $fragmentList, $fragment, [System.Text.UTF8Encoding]::new($false))
    $windowsMlLicense = Join-Path $intermediate "WindowsML-LICENSE.txt"
    $windowsMlNotice = Join-Path $intermediate "WindowsML-NOTICE.txt"
    [System.IO.File]::WriteAllText(
        $windowsMlLicense, "fixture:WindowsML-LICENSE")
    [System.IO.File]::WriteAllText(
        $windowsMlNotice, "fixture:WindowsML-NOTICE")
    $webView2License = Join-Path $intermediate "WebView2-LICENSE.txt"
    $webView2Notice = Join-Path $intermediate "WebView2-NOTICE.txt"
    [System.IO.File]::WriteAllText(
        $webView2License, "fixture:WebView2-LICENSE")
    [System.IO.File]::WriteAllText(
        $webView2Notice, "fixture:WebView2-NOTICE")

    $nugetRoot = Join-Path $env:USERPROFILE ".nuget\packages"
    & (Join-Path $RepositoryRoot "scripts\write_deployment_manifest.ps1") `
        -TargetDirectory $target `
        -FileList $fileList `
        -FragmentList $fragmentList `
        -WindowsAppSdkVersion "2.4.0" `
        -CppWinRtVersion "3.0.260818.1" `
        -WindowsAppSdkLicensePath (Join-Path $nugetRoot `
            "microsoft.windowsappsdk\2.4.0\license.txt") `
        -WindowsAppSdkNoticePath (Join-Path $nugetRoot `
            "microsoft.windowsappsdk\2.4.0\NOTICE.txt") `
        -CppWinRtLicensePath (Join-Path $nugetRoot `
            "microsoft.windows.cppwinrt\3.0.260818.1\LICENSE") `
        -WindowsMlLicensePath $windowsMlLicense `
        -WindowsMlNoticePath $windowsMlNotice `
        -WebView2LicensePath $webView2License `
        -WebView2NoticePath $webView2Notice

    Import-Module (Join-Path $RepositoryRoot `
        "scripts\deployment_payload.psm1") -Force
    $deployment = Read-SnowDesktopDeploymentManifest -BuildOutput $target
    [System.IO.File]::WriteAllText(
        (Join-Path $target "App.xbf"), "tampered")
    $tamperRejected = $false
    try {
        $null = Read-SnowDesktopDeploymentManifest -BuildOutput $target
    }
    catch {
        $tamperRejected = $_.Exception.Message -like "*hash mismatch*"
    }
    if (-not $tamperRejected) {
        throw "A tampered deployment file was not rejected."
    }
    [System.IO.File]::WriteAllText(
        (Join-Path $target "App.xbf"), "fixture:App.xbf")
    $null = Copy-SnowDesktopDeploymentPayload `
        -BuildOutput $target -Destination $payload

    $appxManifest = Join-Path $payload "AppxManifest.xml"
    [System.IO.File]::WriteAllText(
        $appxManifest,
        '<?xml version="1.0" encoding="utf-8"?><Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"><Capabilities /></Package>',
        [System.Text.UTF8Encoding]::new($false))
    Merge-SnowDesktopAppxFragments `
        -BuildOutput $target `
        -PackageRoot $payload `
        -ManifestPath $appxManifest

    [xml]$xml = Get-Content -LiteralPath $appxManifest -Encoding UTF8 -Raw
    $namespace = [System.Xml.XmlNamespaceManager]::new($xml.NameTable)
    $namespace.AddNamespace(
        "m", "http://schemas.microsoft.com/appx/manifest/foundation/windows10")
    if (@($deployment.files).Count -ne 7 -or
        @($deployment.notices).Count -ne 7 -or
        $xml.SelectNodes(
            "/m:Package/m:Extensions/m:Extension", $namespace).Count -ne 1 -or
        -not (Test-Path -LiteralPath (Join-Path $payload `
            "licenses\WindowsAppSDK-LICENSE.txt") -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $payload `
            "licenses\WindowsML-NOTICE.txt") -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $payload `
            "licenses\WebView2-NOTICE.txt") -PathType Leaf)) {
        throw "Deployment integration assertions failed."
    }
    Write-Host "Deployment manifest integration checks passed."
}
finally {
    $resolved = [System.IO.Path]::GetFullPath($probe)
    $temporaryRoot = [System.IO.Path]::GetFullPath(
        [System.IO.Path]::GetTempPath()).TrimEnd("\") + "\"
    if ($resolved.StartsWith(
            $temporaryRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
