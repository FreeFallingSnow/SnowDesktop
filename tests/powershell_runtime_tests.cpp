#include "build_tool_test_support.h"
#include <iostream>
#include <sstream>
#include <thread>
using namespace build_test;
namespace {
void test(const fs::path& repo) {
    auto root=temporary("powershell runtime");
    write(root/L"target.ps1",R"PS(param([string]$Value,[int]$Code=0,[int]$Delay=0)
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
[IO.File]::AppendAllText((Join-Path $PSScriptRoot 'executed.txt'),"executed`n")
@{value=$Value;runtime=$env:SNOWDESKTOP_ENTRY_POWERSHELL;major=$PSVersionTable.PSVersion.Major}|ConvertTo-Json -Compress
if($Delay){Start-Sleep -Seconds $Delay}
exit $Code
)PS",true);
    write(root/L"selector.cmd","@echo off\r\ncall \""+utf8((repo/L"scripts/powershell_runtime.bat").wstring())+"\"\r\nif errorlevel 1 exit /b 2\r\necho %SNOWDESKTOP_ENTRY_POWERSHELL%\r\n");
    auto legacy=powershell(true),preferred=powershell();
    std::vector<fs::path> engines={legacy};if(preferred!=legacy)engines.push_back(preferred);
    const std::string value="路径 with space and 'quote'";
    for(const auto& engine:engines) {
        fs::remove(root/L"executed.txt");
        Env env={{L"SNOWDESKTOP_ENTRY_POWERSHELL",engine.wstring()}};
        auto result=run({engine.wstring(),L"-NoProfile",L"-NonInteractive",L"-File",(root/L"target.ps1").wstring(),L"-Value",wide(value),L"-Code",L"39"},root,15,env);
        require(result.code==39&&read(root/L"executed.txt")=="executed\n","Real runtime argument/exit failed: "+result.err);
        auto payload=Json::parse(result.out);require(payload.at("value").str()==value&&wide(payload.at("runtime").str())==engine.wstring(),"Unicode/pinned runtime mismatch");
        require(payload.at("major").integer()==(engine==legacy?5:7),"Wrong runtime major");
        auto selected=run({L"cmd.exe",L"/d",L"/c",(root/L"selector.cmd").wstring()},root,5,env);
        require(selected.code==0&&selected.out.find(utf8(engine.wstring()))!=std::string::npos,"Batch selector ignored pin");
        std::istringstream input(read(repo/L"scripts/squash_release_to_main.bat"));std::string line,versionLine;
        while(std::getline(input,line))if(line.rfind("for /f \"usebackq",0)==0)versionLine=line;
        require(!versionLine.empty(),"Real version read statement missing");
        copy(repo,root,{"version.json"});write(root/L"read-version.cmd","@echo off\r\nsetlocal\r\n"+versionLine+"\r\necho %VERSION%\r\n");
        auto version=run({L"cmd.exe",L"/d",L"/c",(root/L"read-version.cmd").wstring()},root,6,env);
        require(version.code==0&&version.out.find(Json::parse(read(repo/L"version.json")).at("version").str())!=std::string::npos,"Real version statement failed: "+version.err);
        std::cout<<"PASS real arguments, Unicode, exit 39, pinned runtime and version read: "<<utf8(engine.wstring())<<std::endl;
    }
    Env missing={{L"SNOWDESKTOP_ENTRY_POWERSHELL",L""},{L"PROGRAMFILES",(root/L"absent").wstring()},{L"PROGRAMW6432",(root/L"absent").wstring()},{L"PATH",L""}};
    auto selected=run({L"cmd.exe",L"/d",L"/c",(root/L"selector.cmd").wstring()},root,5,missing);
    require(selected.code==0&&selected.out.find(utf8(legacy.wstring()))!=std::string::npos,"Missing 7 must select 5.1");
    auto broken=root/L"broken pwsh.exe";write(broken,"not an executable");
    auto delayed=root/L"delayed pwsh.exe";fs::copy_file(executable(),delayed);
    write(root/L"guard.ps1",R"PS(param([string]$Entry,[string]$Target,[string]$Runtime,[int]$Code=41,[int]$Delay=0)
. $Entry
$env:SNOWDESKTOP_ENTRY_POWERSHELL=$Runtime
$p=Start-BuildPowerShellScript -Script $Target -Arguments ('-Value "async guard,with comma" -Code '+$Code+' -Delay '+$Delay) -WorkingDirectory $PSScriptRoot -OutputPath (Join-Path $PSScriptRoot 'guard.out') -ErrorPath (Join-Path $PSScriptRoot 'guard.err')
@{pid=$p.Id;startTicks=$p.StartTime.ToUniversalTime().Ticks.ToString()}|ConvertTo-Json -Compress|Set-Content (Join-Path $PSScriptRoot 'guard-child.json') -Encoding UTF8
$p.WaitForExit();$code=$p.ExitCode;$p.Dispose();exit $code
)PS",true);
    for(bool old:{true,false}) {
        if(!old&&preferred==legacy)continue;
        Bridge bridge(repo,root,old);
        for(const auto& candidate:{broken,delayed}) {
            fs::remove(root/L"executed.txt");
            auto result=bridge.invoke(root/L"guard.ps1",{"-Entry",utf8((repo/L"scripts/build_entry.ps1").wstring()),"-Target",utf8((root/L"target.ps1").wstring()),"-Runtime",utf8(candidate.wstring())});
            require(result.code==41&&read(root/L"executed.txt")=="executed\n","Startup recovery did not execute target once: "+result.err);
            auto first=Json::parse(read(root/L"guard.out.startup-1.json"));require(!first.at("scriptStarted").boolean&&first.at("runtime").str()==utf8(candidate.wstring()),"First startup evidence missing");
            auto payload=Json::parse(read(root/L"guard.out.retry"));require(payload.at("major").integer()==5&&payload.at("value").str()=="async guard,with comma","Recovery did not use 5.1 or corrupted CSV/space arguments");
        }
    }
    std::cout<<"PASS missing 7 and corrupt/delayed native executable fallback to 5.1, one target execution, immutable first-start evidence"<<std::endl;
    write(root/L"detached.ps1",R"PS(param([string]$Entry,[string]$Target)
. $Entry
$p=Start-BuildPowerShellScript -Script $Target -Arguments '-Value detached -Delay 12' -WorkingDirectory $PSScriptRoot -OutputPath (Join-Path $PSScriptRoot 'detached.out') -ErrorPath (Join-Path $PSScriptRoot 'detached.err')
@{pid=$p.Id;startTicks=$p.StartTime.ToUniversalTime().Ticks.ToString()}|ConvertTo-Json -Compress|Set-Content (Join-Path $PSScriptRoot 'detached.json') -Encoding UTF8
$p.Dispose();Write-Output 'parent exited';exit 0
)PS",true);
    for(const auto& engine:engines) {
        try {
            Child parent({engine.wstring(),L"-NoProfile",L"-File",(root/L"detached.ps1").wstring(),L"-Entry",(repo/L"scripts/build_entry.ps1").wstring(),L"-Target",(root/L"target.ps1").wstring()},root,{},false,true);
            auto result=parent.wait(6);
            require(result.code==0&&result.out.find("parent exited")!=std::string::npos,"Detached parent failed: "+result.err);
            require(owner_alive(Json::parse(read(root/L"detached.json"))),"Detached child did not survive parent");
        } catch(...) {if(fs::exists(root/L"detached.json"))stop_owner(Json::parse(read(root/L"detached.json")));throw;}
        stop_owner(Json::parse(read(root/L"detached.json")));fs::remove(root/L"detached.json");
    }
    std::cout<<"PASS detached child survives actual parent completion without inheriting redirected pipes, both runtimes"<<std::endl;
    fs::remove(root/L"executed.txt");
    Child active({preferred.wstring(),L"-NoProfile",L"-File",(root/L"guard.ps1").wstring(),L"-Entry",(repo/L"scripts/build_entry.ps1").wstring(),L"-Target",(root/L"target.ps1").wstring(),L"-Runtime",preferred.wstring(),L"-Delay",L"8",L"-Code",L"0"},root);
    until([&]{return fs::exists(root/L"executed.txt");},5);
    std::this_thread::sleep_for(std::chrono::seconds(1));active.stop();
    auto child=Json::parse(read(root/L"guard-child.json"));stop_owner(child);
    require(read(root/L"executed.txt")=="executed\n","Interrupted business target was replayed");
    std::cout<<"PASS interruption after execution starts preserves exactly one side effect"<<std::endl;
}
}
int main(int argc,char** argv) {
    if(executable().filename().wstring().find(L"delayed pwsh")!=std::wstring::npos){std::this_thread::sleep_for(std::chrono::seconds(9));return 99;}
    return test_main(argc,argv,test);
}
