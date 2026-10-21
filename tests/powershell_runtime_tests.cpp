#include "build_tool_test_support.h"
#include "tools/development_launcher_tests.h"
#include <iostream>
#include <sstream>
#include <thread>
using namespace build_test;
namespace {
void fallback_module_test(const fs::path& repo,const fs::path& entryOverride={}) {
    const auto core=powershell(),legacy=powershell(true);
    if(core==legacy){std::cout<<"Core-to-Desktop module-path case requires installed PowerShell7\n";return;}
    const auto root=temporary("fallback-module-path");
    write(root/L"broken pwsh.exe","not a PE executable");write(root/L"hash-input.txt","abc");
    fs::create_directory(root/L"custom modules");
    write(root/L"hash-target.ps1",R"PS($ErrorActionPreference='Stop'
[IO.File]::AppendAllText(([IO.Path]::Combine($PSScriptRoot,'executed.txt')),"once`n")
$failure='';$hash='';try{$hash=(Get-FileHash -LiteralPath ([IO.Path]::Combine($PSScriptRoot,'hash-input.txt')) -Algorithm SHA256).Hash}catch{$failure=$_.Exception.Message}
$module=Get-Module Microsoft.PowerShell.Utility
$observation=@{major=$PSVersionTable.PSVersion.Major;hash=$hash;failure=$failure;utility=$module.Path;modulePath=$env:PSModulePath;sentinel=$env:SNOWDESKTOP_CHILD_SENTINEL}
[IO.File]::WriteAllText(([IO.Path]::Combine($PSScriptRoot,'observed.json')),($observation|ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false))
if($failure){exit 1}
)PS",true);
    write(root/L"launch.ps1",R"PS(param([string]$Entry,[string]$Runtime)
. $Entry
$env:PSModulePath=(Join-Path $PSScriptRoot 'custom modules')+';'+$env:PSModulePath
$before=$env:PSModulePath;$env:SNOWDESKTOP_CHILD_SENTINEL='路径-child-marker'
$env:SNOWDESKTOP_ENTRY_POWERSHELL=$Runtime
$p=Start-BuildPowerShellScript -Script (Join-Path $PSScriptRoot 'hash-target.ps1') -WorkingDirectory $PSScriptRoot -OutputPath (Join-Path $PSScriptRoot 'child.out') -ErrorPath (Join-Path $PSScriptRoot 'child.err')
$p.WaitForExit();$code=$p.ExitCode;$p.Dispose()
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'parent.json'),(@{before=$before;after=$env:PSModulePath}|ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false))
exit $code
)PS",true);
    const auto entry=entryOverride.empty()?repo/L"scripts/build_entry.ps1":entryOverride;
    for(const bool fallback:{true,false}) {
        fs::remove(root/L"executed.txt");fs::remove(root/L"observed.json");
        const auto result=run({core.wstring(),L"-NoProfile",L"-File",(root/L"launch.ps1").wstring(),L"-Entry",entry.wstring(),L"-Runtime",(fallback?root/L"broken pwsh.exe":core).wstring()},root,20);
        const auto observation=fs::exists(root/L"observed.json")?Json::parse(read(root/L"observed.json")):Json::object({});
        require(result.code==0&&observation.has("hash")&&observation.at("hash").str()=="BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD",std::string(fallback?"Fallback Windows PowerShell could not hash with its own Utility module: ":"Core child lost its hashing module: ")+result.out+result.err+observation.dump());
        const auto parent=Json::parse(read(root/L"parent.json"));
        require(parent.at("before").str()==parent.at("after").str()&&read(root/L"executed.txt")=="once\n","Child module-path repair changed the parent environment or replayed business execution");
        require(observation.at("major").integer()==(fallback?5:7)&&observation.at("sentinel").str()=="路径-child-marker","Child environment lost runtime selection or a Unicode value");
        const auto utility=(fallback?legacy:core).parent_path()/L"Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1";
        require(fs::equivalent(fs::path(wide(observation.at("utility").str())),utility),"Child loaded a different runtime's Utility module");
        require((observation.at("modulePath").str().find(utf8((root/L"custom modules").wstring()))!=std::string::npos)==!fallback,"Core child lost its custom path or Desktop child retained process-only Core paths");
    }
    std::cout<<"PASS actual Core-to-Desktop fallback hashing, own Utility module, parent/custom Core paths, Unicode environment and exactly one execution\n";
}
void test(const fs::path& repo) {
    fallback_module_test(repo);
    development_launcher_tests::Run(repo);
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
    if(auto fixture=development_launcher_tests::Fixture(argc,argv))return *fixture;
    if(executable().filename().wstring().find(L"delayed pwsh")!=std::wstring::npos){std::this_thread::sleep_for(std::chrono::seconds(9));return 99;}
    return test_main(argc,argv,[&](const fs::path& repo){if(argc>3&&std::string(argv[2])=="--fallback-module-control")fallback_module_test(repo,fs::path(wide(argv[3])));else test(repo);});
}
