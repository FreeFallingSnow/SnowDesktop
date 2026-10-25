#pragma once
#include <string>

// Fixture construction only. The production deployment reader, inventory,
// exporter, activation and packager perform all validation.
inline std::string product_deployment_fixture() {
    return R"PS(
function New-TestProductDeployment($Output,$Generation) {
    $files=@()
    foreach($name in @('SnowDesktop.winmd','App.xbf','SettingsShell.xbf','Assets/Fonts/Example.ttf',
        'Assets/Settings/Icons/example.svg','SnowDesktop.Runtime/Microsoft.ui.xaml.dll')) {
        $path=Join-Path $Output $name
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        [IO.File]::WriteAllText($path,$Generation+':'+$name)
        if($name.StartsWith('SnowDesktop.Runtime/')) {
            $files+=@([pscustomobject]@{path='Microsoft.ui.xaml.dll';buildPath=$name;
                source='Microsoft.WindowsAppSDK';sha256=(Get-FileHash $path -Algorithm SHA256).Hash})
        } else {
            $files+=@([pscustomobject]@{path=$name;source='SnowDesktop';sha256=(Get-FileHash $path -Algorithm SHA256).Hash})
        }
    }
    $fragments=@();$notices=@()
    $fragment='.deployment/appxfragments/framework.appxfragment'
    $path=Join-Path $Output $fragment
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
    [IO.File]::WriteAllText($path,'<Fragment xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"/>')
    $fragments+=@([pscustomobject]@{path=$fragment;sha256=(Get-FileHash $path -Algorithm SHA256).Hash})
    foreach($name in @('CppWinRT-LICENSE.txt','WindowsAppSDK-LICENSE.txt','WindowsAppSDK-NOTICE.txt',
        'WindowsML-LICENSE.txt','WindowsML-NOTICE.txt','WebView2-LICENSE.txt','WebView2-NOTICE.txt',
        'YASB-LICENSE.txt','MiSans-LICENSE.pdf','HarmonyOS-Sans-LICENSE.txt')) {
        $relative='.deployment/licenses/'+$name;$path=Join-Path $Output $relative
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
        [IO.File]::WriteAllText($path,$Generation+':'+$name)
        $notices+=@([pscustomobject]@{path=$relative;destination=('licenses/'+$name);sha256=(Get-FileHash $path -Algorithm SHA256).Hash})
    }
    [pscustomobject]@{schemaVersion=1;architecture='x64';windowsAppSdkPackageVersion='2.4.0';
        cppWinRtPackageVersion='3.0.260818.1';files=$files;appxFragments=$fragments;notices=$notices} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $Output 'SnowDesktop.deployment.json') -Encoding UTF8
}
)PS";
}
