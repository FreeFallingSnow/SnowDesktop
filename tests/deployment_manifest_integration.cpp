#include "build_tool_test_support.h"
#include <iostream>
using namespace build_test;
namespace {
void test(const fs::path& repo){
    std::vector<fs::path> performanceFiles{L"scripts/profile.bat", L"scripts/profile.ps1", L"scripts/profile_analyze.ps1"};
    for (const auto& entry : fs::recursive_directory_iterator(repo / L"tools/performance"))
        if (entry.is_regular_file() && (entry.path().extension() == L".ps1" || entry.path().extension() == L".sql"))
            performanceFiles.push_back(fs::relative(entry.path(), repo));
    auto root=temporary("deployment"),target=root/L"target",intermediate=root/L"intermediate",payload=root/L"payload",relocated=root/L"relocated-payload";std::vector<std::pair<std::string,std::string>> required={{"App.xbf","SnowDesktop"},{"SettingsShell.xbf","SnowDesktop"},{"SnowDesktop.pri","SnowDesktop"},{"SnowDesktop.winmd","SnowDesktop"},{"Assets/Fonts/MiSans/MiSans-Regular.otf","SnowDesktop"},{"Assets/Fonts/HarmonyOS-Sans/HarmonyOS_Sans_SC_Regular.ttf","SnowDesktop"},{"Microsoft.WindowsAppRuntime.dll","Microsoft.WindowsAppSDK"},{"Microsoft.ui.xaml.dll","Microsoft.WindowsAppSDK"},{"Microsoft.UI.Xaml.winmd","Microsoft.WindowsAppSDK"},{"Microsoft.UI.Xaml/Assets/map.html","Microsoft.WindowsAppSDK"},{"Microsoft.UI.Xaml/Assets/NoiseAsset_256X256_PNG.png","Microsoft.WindowsAppSDK"}};std::string files;for(const auto& [name,source]:required){write(target/name,"fixture:"+name);files+=name+"\t"+source+"\tGenerated\n";}write(intermediate/L"files.txt",files);auto fragment=root/L"nuget/microsoft.windowsappsdk.test/1.0.0/runtimes-framework/package.appxfragment";write(fragment,"<?xml version=\"1.0\" encoding=\"utf-8\"?><Fragment xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\"><Extensions><Extension Category=\"windows.activatableClass.inProcessServer\"><InProcessServer><Path>Microsoft.ui.xaml.dll</Path><ActivatableClass ActivatableClassId=\"SnowDesktop.Test\" ThreadingModel=\"both\"/></InProcessServer></Extension></Extensions></Fragment>");write(intermediate/L"fragments.txt",utf8(fragment.wstring()));for(const auto& name:{"WindowsML-LICENSE.txt","WindowsML-NOTICE.txt","WebView2-LICENSE.txt","WebView2-NOTICE.txt"})write(intermediate/name,"fixture:"+std::string(name));auto nuget=fs::path(environment(L"USERPROFILE"))/L".nuget/packages";Bridge bridge(repo,root);auto path=[](const fs::path& value){return utf8(value.wstring());};std::vector<std::string> writerArgs={"-TargetDirectory",path(target),"-FileList",path(intermediate/L"files.txt"),"-FragmentList",path(intermediate/L"fragments.txt"),"-WindowsAppSdkVersion","2.4.0","-CppWinRtVersion","3.0.260818.1","-WindowsAppSdkLicensePath",path(nuget/L"microsoft.windowsappsdk/2.4.0/license.txt"),"-WindowsAppSdkNoticePath",path(nuget/L"microsoft.windowsappsdk/2.4.0/NOTICE.txt"),"-CppWinRtLicensePath",path(nuget/L"microsoft.windows.cppwinrt/3.0.260818.1/LICENSE"),"-WindowsMlLicensePath",path(intermediate/L"WindowsML-LICENSE.txt"),"-WindowsMlNoticePath",path(intermediate/L"WindowsML-NOTICE.txt"),"-WebView2LicensePath",path(intermediate/L"WebView2-LICENSE.txt"),"-WebView2NoticePath",path(intermediate/L"WebView2-NOTICE.txt")};auto result=bridge.invoke(repo/L"scripts/write_deployment_manifest.ps1",writerArgs);require(result.code==0,result.out+result.err);auto deployment=Json::parse(read(target/L"SnowDesktop.deployment.json"));require(deployment.at("files").items.size()==required.size()+performanceFiles.size()&&deployment.at("notices").items.size()==11,"Generated manifest inventory incorrect");auto probe=root/L"payload.ps1";write(probe,"param($Operation,$Destination)\nImport-Module "+ps_literal(repo/L"scripts/deployment_payload.psm1")+" -Force\nif($Operation -eq 'read'){Read-SnowDesktopDeploymentManifest -BuildOutput "+ps_literal(target)+"|ConvertTo-Json -Depth 20}\nelseif($Operation -eq 'copy'){Copy-SnowDesktopDeploymentPayload -BuildOutput "+ps_literal(target)+" -Destination $Destination -RuntimeDirectory SnowDesktop.Runtime|Out-Null}\nelse{Merge-SnowDesktopAppxFragments -BuildOutput "+ps_literal(target)+" -PackageRoot "+ps_literal(payload)+" -ManifestPath "+ps_literal(payload/L"AppxManifest.xml")+" -RuntimeDirectory SnowDesktop.Runtime}\n",true);bridge.call(probe,{"read"});write(target/L"App.xbf","tampered");result=bridge.invoke(probe,{"read"});require(result.code!=0&&result.err.find("hash mismatch")!=std::string::npos,"Tampered deployment file accepted");write(target/L"App.xbf","fixture:App.xbf");fs::create_directories(payload);result=bridge.invoke(probe,{"copy",path(payload)});require(result.code==0,result.out+result.err);
    for(auto& file:deployment["files"].items)if(file.at("source").str()!="SnowDesktop"){auto relative=file.at("path").str(),buildPath="SnowDesktop.Runtime/"+relative;fs::create_directories((target/buildPath).parent_path());fs::rename(target/relative,target/buildPath);file["buildPath"]=buildPath;}write(target/L"SnowDesktop.deployment.json",deployment.dump());fs::create_directories(relocated);result=bridge.invoke(probe,{"copy",path(relocated)});require(result.code==0,result.out+result.err);write(payload/L"AppxManifest.xml","<?xml version=\"1.0\" encoding=\"utf-8\"?><Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\"><Capabilities/></Package>");result=bridge.invoke(probe,{"merge","unused"});require(result.code==0,result.out+result.err);auto xml=read(payload/L"AppxManifest.xml");require(xml.find("SnowDesktop.Runtime\\Microsoft.ui.xaml.dll")!=std::string::npos&&xml.find("SnowDesktop.Test")!=std::string::npos,"Merged activatable class path incorrect");
    for(const auto& destination:{payload,relocated}){for(const auto& relative:performanceFiles) require(read(destination/L"performance"/relative)==read(repo/relative),"Performance payload bytes changed");for(const auto& name:{"SnowDesktop.Runtime/Microsoft.ui.xaml.dll","Microsoft.UI.Xaml/Assets/map.html","SnowDesktop.Runtime/Microsoft.UI.Xaml/Assets/map.html","Microsoft.UI.Xaml/Assets/NoiseAsset_256X256_PNG.png","SnowDesktop.Runtime/Microsoft.UI.Xaml/Assets/NoiseAsset_256X256_PNG.png","SnowDesktop.pri","licenses/WindowsAppSDK-LICENSE.txt","licenses/WindowsML-NOTICE.txt","licenses/WebView2-NOTICE.txt"})require(fs::is_regular_file(destination/name),"Payload file missing "+std::string(name));require(!fs::exists(destination/L"Microsoft.ui.xaml.dll"),"Private runtime escaped to package root");for(const auto& [name,source]:required)if(name.starts_with("Assets/Fonts/")){require(source=="SnowDesktop"&&read(destination/name)=="fixture:"+name&&!fs::exists(destination/L"SnowDesktop.Runtime"/name),"Application font URI path moved into private runtime");}for(const auto& [notice,original]:std::vector<std::pair<std::string,std::string>>{{"licenses/Perfetto-LICENSE.txt","third_party/perfetto/LICENSE"},{"licenses/YASB-LICENSE.txt","third_party/yasb/LICENSE"},{"licenses/MiSans-LICENSE.pdf","third_party/misans/LICENSE.pdf"},{"licenses/HarmonyOS-Sans-LICENSE.txt","third_party/harmonyos-sans/LICENSE.txt"}})require(read(destination/notice)==read(repo/original),"License bytes changed: "+notice);}
    // A native host integration executable has its own generated PRI name.
    // Keep the shipping default mandatory, accept an explicit target PRI, and
    // refuse a declared but missing resource without replacing the manifest.
    const auto customTarget = root / L"custom-target";
    fs::create_directories(customTarget);
    // The original target has already relocated its runtime files. Recreate
    // the original fixture inputs so this check exercises the writer directly.
    for (const auto& [name, source] : required)
    {
        (void)source;
        write(customTarget / name, "fixture:" + name);
    }
    fs::rename(customTarget / L"SnowDesktop.pri", customTarget / L"NativeHost.pri");
    auto customFiles = files;
    replace(customFiles, "SnowDesktop.pri", "NativeHost.pri");
    write(intermediate / L"custom-files.txt", customFiles);
    auto customArgs = writerArgs;
    customArgs[1] = path(customTarget);
    customArgs[3] = path(intermediate / L"custom-files.txt");
    result = bridge.invoke(repo / L"scripts/write_deployment_manifest.ps1", customArgs);
    require(result.code != 0 && result.err.find("missing required file: SnowDesktop.pri") != std::string::npos,
        "shipping default PRI must remain required when no target override is provided");
    customArgs.insert(customArgs.end(), {"-ApplicationPriFileName", "NativeHost.pri"});
    result = bridge.invoke(repo / L"scripts/write_deployment_manifest.ps1", customArgs);
    require(result.code == 0, "explicit native target PRI must generate a real deployment inventory: " + result.out + result.err);
    const auto customManifest = read(customTarget / L"SnowDesktop.deployment.json");
    const auto customDocument = Json::parse(customManifest);
    bool foundTargetPri = false;
    for (const auto& entry : customDocument.at("files").items)
    {
        require(entry.at("path").str() != "SnowDesktop.pri", "custom inventory must not publish an absent shipping PRI");
        if (entry.at("path").str() == "NativeHost.pri")
            // SHA256 of the literal fixture bytes, preserved by the rename.
            foundTargetPri = entry.at("sha256").str() ==
                "E633AD7111A1FF289D50FD4D38D7D463D96A4D4CB8801CA9CAAD63C966A2BCBB";
    }
    require(foundTargetPri, "custom inventory must retain the actual target PRI and its content fingerprint");
    fs::remove(customTarget / L"NativeHost.pri");
    result = bridge.invoke(repo / L"scripts/write_deployment_manifest.ps1", customArgs);
    require(result.code != 0 && result.err.find("NativeHost.pri") != std::string::npos &&
        read(customTarget / L"SnowDesktop.deployment.json") == customManifest,
        "missing target PRI must be rejected without overwriting the valid inventory");
    std::cout<<"PASS generated deployment inventory, tamper rejection, runtime relocation, XAML assets, fonts and licenses\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,test);}
