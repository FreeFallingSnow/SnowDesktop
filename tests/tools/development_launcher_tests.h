#pragma once
#include "build_tool_test_support.h"
#include <iostream>
#include <optional>

namespace development_launcher_tests {
using namespace build_test;
inline LRESULT CALLBACK FixtureWindow(HWND window,UINT message,WPARAM wParam,LPARAM lParam)
{
    if(message==WM_CLOSE){DestroyWindow(window);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,wParam,lParam);
}
inline std::optional<int> Fixture(int argc,char** argv)
{
    if(environment(L"SNOWDESKTOP_DEVELOPMENT_FIXTURE").empty())return {};
    const auto name=executable().stem().wstring();
    if(name!=L"SnowDesktop"&&name!=L"snowwidget"&&name!=L"SnowDesktopSteamBridge"&&name!=L"SnowDesktopWorkshopManager")return {};
    if(name==L"SnowDesktopSteamBridge"&&argc>1&&std::string(argv[1])=="configuration"){
        std::cout<<"{\"ok\":true,\"steamworksCompiled\":true,\"expectedAppId\":12345}"<<std::endl;return 0;
    }
    auto record=Json::object({{"pid",static_cast<int>(GetCurrentProcessId())},
        {"path",utf8(executable().wstring())},{"cwd",utf8(fs::current_path().wstring())},
        {"arguments",Json::array({})}});
    for(int i=1;i<argc;++i)record["arguments"].items.push_back(argv[i]);
    write(executable().parent_path()/wide("launch-"+std::to_string(GetCurrentProcessId())+".json"),record.dump());
    if(name!=L"SnowDesktop"||environment(L"SNOWDESKTOP_DEVELOPMENT_FIXTURE")!=L"host")return 0;
    WNDCLASSW klass{};klass.lpfnWndProc=FixtureWindow;klass.hInstance=GetModuleHandleW(nullptr);klass.lpszClassName=L"SnowDesktopDevelopmentFixture";
    if(!RegisterClassW(&klass))return 81;
    const auto window=CreateWindowW(klass.lpszClassName,L"Owned development fixture",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        -32000,-32000,1,1,nullptr,nullptr,klass.hInstance,nullptr);
    if(!window)return 82;
    write(executable().parent_path()/wide("ready-"+std::to_string(GetCurrentProcessId())),"ready");
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return 0;
}
inline void CopyExecutable(const fs::path& root,const std::wstring& name)
{
    fs::create_directories(root);fs::copy_file(executable(),root/name,fs::copy_options::overwrite_existing);
}
inline std::vector<Json> Launches(const fs::path& root)
{
    std::vector<Json> result;
    for(const auto& item:fs::directory_iterator(root))if(item.path().filename().wstring().starts_with(L"launch-"))result.push_back(Json::parse(read(item.path())));
    return result;
}
inline void Run(const fs::path& repo)
{
    int failures=0;
    const auto check=[&](const char* name,const auto& action){try{action();std::cout<<"PASS "<<name<<std::endl;}
        catch(const std::exception& error){++failures;std::cerr<<"FAIL "<<name<<": "<<error.what()<<std::endl;}};
    const Env record{{L"SNOWDESKTOP_DEVELOPMENT_FIXTURE",L"record"}};
    const auto setup=[&](const char* label){auto root=temporary(label);copy(repo,root,{"scripts/widget_dev.ps1","scripts/steam_dev.ps1","scripts/development_output.psm1","CMakePresets.json"});
        write(root/L"component/widget.json",R"({"slug":"fixture","entry":"main.lua"})");write(root/L"component/main.lua","return {}\n");return root;};
    check("R27-03 actual Debug widget launch and sync",[&]{
        auto root=setup("debug development output");auto output=root/L".build_debug/Debug";
        CopyExecutable(output,L"SnowDesktop.exe");CopyExecutable(output,L"snowwidget.exe");
        auto result=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"scripts/widget_dev.ps1").wstring(),L"component",L"-Configuration",L"Debug",L"-Once"},root,15,record);
        require(result.code==0,"Debug launcher rejected real preset output: "+result.err);
        until([&]{return Launches(output).size()==2;},5);
        require(read(output/L"data/widgets/dev/fixture/main.lua")=="return {}\n","Debug sync missed selected output");
    });
    check("R27-02 all actual Steam development launch targets",[&]{
        auto root=setup("arranged steam development output");auto output=root/L".build/Release";
        for(const auto* name:{L"SnowDesktop.exe",L"SnowDesktopWorkshopManager.exe",L"SnowDesktopSteamBridge.exe"})CopyExecutable(output,name);
        write(root/L"packaging/steam-identity.json",R"({"appId":12345})");
        write(output/L"SnowDesktop.Runtime/steam_api64.dll","fixture SDK runtime");
        write(output/L"SnowDesktop.Runtime/SnowDesktop.Runtime.manifest",R"(<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"><assemblyIdentity name="SnowDesktop.Runtime"/><file xmlns="urn:schemas-microsoft-com:asm.v3" name="steam_api64.dll"/></assembly>)");
        for(const auto* target:{L"manager",L"app",L"bridge"}){
            auto result=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"scripts/steam_dev.ps1").wstring(),L"-Target",target,L"argument with space"},root,15,record);
            require(result.code==0,"arranged runtime launcher failed: "+result.err);
            require(!fs::exists(output/L"steam_appid.txt"),"temporary App ID was not cleaned");
        }
        require(Launches(output).size()==3,"every target must actually execute");
        for(const auto& launch:Launches(output))require(fs::equivalent(fs::path(wide(launch.at("cwd").str())),output)&&
            launch.at("arguments").items.at(0).str()=="argument with space","actual launch arguments or working directory differ");
    });
    check("R27-04 real restart preserves another deployment",[&]{
        auto root=setup("owned development restart");auto output=root/L".build/Release";auto other=root/L"other deployment";
        CopyExecutable(output,L"SnowDesktop.exe");CopyExecutable(output,L"snowwidget.exe");CopyExecutable(other,L"SnowDesktop.exe");
        Env host{{L"SNOWDESKTOP_DEVELOPMENT_FIXTURE",L"host"}};
        Child selected({(output/L"SnowDesktop.exe").wstring()},output,host);
        Child unrelated({(other/L"SnowDesktop.exe").wstring()},other,host);
        const auto otherOwner=owner(unrelated.pid());
        until([&]{return fs::exists(output/wide("ready-"+std::to_string(selected.pid())))&&fs::exists(other/wide("ready-"+std::to_string(unrelated.pid())));},5);
        write(root/L"restart.ps1",R"PS(param([string]$Script,[string]$Ids)
$env:SNOWDESKTOP_FIXTURE_PROCESS_IDS=$Ids
function global:Get-Process { param([string]$Name,$ErrorAction) Microsoft.PowerShell.Management\Get-Process -Id @($env:SNOWDESKTOP_FIXTURE_PROCESS_IDS.Split(',') | ForEach-Object { [int]$_ }) -ErrorAction SilentlyContinue }
& $Script component -Once -RestartHost
)PS",true);
        host[L"SNOWDESKTOP_DEVELOPMENT_FIXTURE"]=L"record"; // Restart child exits after recording; no detached fixture leaks.
        auto result=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"restart.ps1").wstring(),L"-Script",(root/L"scripts/widget_dev.ps1").wstring(),L"-Ids",wide(std::to_string(selected.pid())+","+std::to_string(unrelated.pid()))},root,20,host);
        require(result.code==0,"actual restart script failed: "+result.err);
        require(owner_alive(otherOwner),"restart terminated another deployment");
        require(!selected.running(),"selected deployment did not stop");
        until([&]{return Launches(output).size()>=3;},5);
    });
    check("R27-01 actual isolated local squash survives branch switch",[&]{
        auto root=temporary("local squash snapshot");
        copy(repo,root,{"scripts/release_manager.ps1","scripts/build_entry.ps1","scripts/squash_release_to_main.bat","scripts/powershell_runtime.bat"});
        write(root/L".gitignore","/artifacts/\n/.build/\n");write(root/L".gitattributes","* -text\n");write(root/L"version.json",R"({"version":"1.2.3.0"})");init_git(root);
        const auto git=[&](std::vector<std::wstring> args){std::vector<std::wstring> command{L"git"};command.insert(command.end(),args.begin(),args.end());auto result=run(command,root);require(result.code==0,result.err);return result.out;};
        git({L"config",L"user.name",L"Fixture"});git({L"config",L"user.email",L"fixture@example.invalid"});git({L"branch",L"-M",L"main"});
        git({L"remote",L"add",L"origin",L"https://github.com/FreeFallingSnow/SnowDesktop.git"});git({L"switch",L"-c",L"release/v1.2.3.0"});
        write(root/L"release-content.txt","release candidate\n");git({L"add",L"release-content.txt"});git({L"commit",L"--quiet",L"-m",L"release fixture"});
        auto result=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"scripts/release_manager.ps1").wstring(),L"squash",L"-Yes",L"-ConfirmVersion",L"1.2.3.0",L"-Message",L"v1.2.3.0 - fixture"},root,25);
        require(result.code==0,"actual local squash failed: "+result.err+result.out);
        require(git({L"branch",L"--show-current"}).find("main")!=std::string::npos &&
            git({L"cat-file",L"-t",L"refs/tags/v1.2.3.0"}).find("tag")!=std::string::npos &&
            read(root/L"release-content.txt")=="release candidate\n","local squash output/tag incomplete");
        require(git({L"rev-list",L"--count",L"main"}).find('2')!=std::string::npos,"local squash must add exactly one version commit");
    });
    require(failures==0,std::to_string(failures)+" actual development/release launcher cases failed");
}
}
