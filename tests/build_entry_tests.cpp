#include "build_tool_test_support.h"
#include <algorithm>
#include <iostream>
#include <sstream>
namespace build_test {
namespace {
void preflight_tests(const fs::path& repo){
    auto root=temporary("preflight entry with spaces");copy(repo,root,{"scripts/build.bat","scripts/powershell_runtime.bat","scripts/build_entry.ps1","scripts/build_job.cs"});
    write(root/L"scripts/build_preflight.ps1",R"PS(param([switch]$ReloadShell,[switch]$CloseApplication)
[IO.File]::AppendAllText((Join-Path $PSScriptRoot '../preflight.json'),((ConvertTo-Json -Compress @{reload=[bool]$ReloadShell;closeApplication=[bool]$CloseApplication;directory=$PSScriptRoot})+"`n"))
if($env:SNOWDESKTOP_PREFLIGHT_FIXTURE_EXIT -eq 'throw'){throw 'controlled preflight exception'}
exit ([int]$env:SNOWDESKTOP_PREFLIGHT_FIXTURE_EXIT)
)PS",true);write(root/L"bin/cmake.cmd","@echo off\r\necho unexpected configure>\"%SNOWDESKTOP_PREFLIGHT_FIXTURE_ROOT%\\cmake.txt\"\r\nexit /b 19\r\n");
    struct Scenario{bool reload,close;const char* code;};
    for(const Scenario scenario:std::vector<Scenario>{{false,false,"7"},{true,false,"7"},{true,false,"-1"},{true,false,"throw"},{true,false,"0"},{false,true,"7"},{false,true,"throw"},{false,true,"0"}}){
        fs::remove(root/L"preflight.json");fs::remove(root/L"cmake.txt");std::string command="@echo off\r\ncall scripts\\build.bat";if(scenario.reload)command+=" --reload-shell";if(scenario.close)command+=" --close-application";command+="\r\nexit /b %ERRORLEVEL%\r\n";write(root/L"probe.cmd",command);Env env{{L"SNOWDESKTOP_PREFLIGHT_FIXTURE_EXIT",wide(scenario.code)},{L"SNOWDESKTOP_PREFLIGHT_FIXTURE_ROOT",root.wstring()},{L"PATH",(root/L"bin").wstring()+L";"+environment(L"PATH")}};auto result=run({environment(L"COMSPEC"),L"/d",L"/c",(root/L"probe.cmd").wstring()},root,25,env);bool success=std::string(scenario.code)=="0";require(result.code==(success?19:3),"Preflight exit/SHIFT scenario failed "+result.out+result.err);std::istringstream lines(read(root/L"preflight.json"));std::string line;size_t count=0;while(std::getline(lines,line)){auto value=Json::parse(line);require(value.at("reload").boolean==(count==0&&scenario.reload)&&value.at("closeApplication").boolean==(count==0&&scenario.close)&&value.at("directory").str()==utf8((root/L"scripts").wstring()),"Preflight path/options changed after SHIFT");++count;}require(count==(success&&(scenario.reload||scenario.close)?2U:1U)&&fs::exists(root/L"cmake.txt")==success,"Preflight did not prevent/permit configure once");
    }
    auto closeRoot=temporary("application-only-preflight");copy(repo,closeRoot,{"scripts/build_preflight.ps1","scripts/build_entry.ps1"});write(closeRoot/L".build/Release/SnowDesktopTaskbarHook.dll","fixture");write(closeRoot/L"probe.ps1",R"PS(param([switch]$InitiallyOccupied)
$global:Closed=$false;$global:InitiallyOccupied=[bool]$InitiallyOccupied;$global:ProbeRoot=$PSScriptRoot
function Get-Process {param($Name,$Id,$ErrorAction)
 if($Id -eq $PID){return Microsoft.PowerShell.Management\Get-Process -Id $PID}
 if($Name -contains 'explorer'){
  if($global:InitiallyOccupied -or $global:Closed){return [pscustomobject]@{Id=13579;ProcessName='explorer';StartTime=[datetime]::UtcNow;Modules=@([pscustomobject]@{FileName=(Join-Path $global:ProbeRoot '.build/Release/SnowDesktopTaskbarHook.dll')})}}
 }elseif(-not $global:InitiallyOccupied -and -not $global:Closed){$app=[pscustomobject]@{Id=24680;ProcessName='SnowDesktop';Path=(Join-Path $global:ProbeRoot '.build/Release/SnowDesktop.exe');StartTime=[datetime]::UtcNow};$app|Add-Member -MemberType ScriptMethod -Name CloseMainWindow -Value {$global:Closed=$true;return $true};return $app}
}
function Stop-Process {[IO.File]::WriteAllText((Join-Path $global:ProbeRoot 'shell-action'),'stop');throw 'Unexpected process termination'}
function Start-Process {[IO.File]::WriteAllText((Join-Path $global:ProbeRoot 'shell-action'),'start');throw 'Unexpected shell restart'}
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1');$entry=Enter-BuildEntry $PSScriptRoot
try{& (Join-Path $PSScriptRoot 'scripts/build_preflight.ps1') -CloseApplication}finally{Exit-BuildEntry $entry}
)PS",true);Bridge bridge(repo,closeRoot);for(bool occupied:{true,false}){auto result=bridge.invoke(closeRoot/L"probe.ps1",occupied?std::vector<std::string>{"-InitiallyOccupied"}:std::vector<std::string>{});require(result.code!=0&&result.err.find("Explorer hook")!=std::string::npos&&!fs::exists(closeRoot/L"shell-action"),"Application-only close controlled real Shell/ignored new hook");}
}
void cmake_entry_tests(const fs::path& repo,const fs::path& root,Bridge& bridge){
    copy(repo,root,{"cmake/SnowDesktop.SharedBuildGuard.cmake","cmake/SnowDesktop.PowerShell.cmake"});write(root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.SharedBuildGuard.cmake")
snowdesktop_check_shared_output()
project(Entry NONE)
add_custom_target(Native COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/native.marker")
snowdesktop_install_shared_guard()
)CMAKE");write(root/L"CMakePresets.json",R"JSON({"version":3,"configurePresets":[{"name":"release","binaryDir":"${sourceDir}/.build"}],"buildPresets":[{"name":"release","configurePreset":"release","configuration":"Release"}]})JSON");auto result=run({L"cmake",L"--preset",L"release"},root);require(result.code!=0&&result.err.find("live leased entry")!=std::string::npos&&!fs::exists(root/L".build/native.marker"),"Raw shared configure bypassed authority");result=bridge.invoke(root/L"scripts/build_entry.ps1",{"-Action","ide","-Targets","Native"});require(result.code==0&&fs::exists(root/L".build/native.marker"),result.out+result.err);auto stamp=fs::last_write_time(root/L".build/native.marker");result=run({L"cmake",L"--build",L"--preset",L"release",L"--target",L"Native"},root);require(result.code!=0&&fs::last_write_time(root/L".build/native.marker")==stamp,"Expired IDE lease executed native target");result=run({L"cmake",L"-S",root.wstring(),L"-B",(root/L"independent").wstring()},root);require(result.code==0,result.err);result=run({L"cmake",L"--build",(root/L"independent").wstring(),L"--target",L"Native"},root);require(result.code==0&&fs::exists(root/L"independent/native.marker"),"Independent diagnostic output was blocked");
}
}
void entry_tests(const fs::path& repo){
    preflight_tests(repo);
    auto root=temporary("entry");copy(repo,root,{"scripts/powershell_runtime.bat","scripts/build.bat","scripts/build_debug.bat","scripts/build_entry.ps1","scripts/build_runtime.ps1","scripts/build_job.cs","scripts/test_manager.ps1","scripts/test_output.ps1","scripts/build_protocol.ps1","scripts/build_manager.ps1","scripts/build_inputs.ps1","scripts/build_ownership.ps1","scripts/build_batch_tests.ps1"});
    write(root/L"scripts/build_preflight.ps1","param([switch]$ReloadShell,[switch]$CloseApplication)\nfunction Get-ReadOnlyPreflight($Root){@{status='clear';observedUtc=[DateTime]::UtcNow.ToString('o');owners=@()}}\nif($MyInvocation.InvocationName -ne '.'){[IO.File]::WriteAllText((Join-Path $PSScriptRoot '../preflight.marker'),'called');exit 3}\n",true);
    auto directory=root/L".build/collaboration";auto bid=std::string(32,'b');auto state=Json::object({{"schemaVersion",1},{"repositoryRoot",utf8(root.wstring())},{"current",Json::object({{"id",bid},{"phase","editing"},{"owner",Json{}},{"participants",Json::array({Json::object({{"id","independent-terminal"},{"state","editing"},{"registeredUtc","2026-10-02T00:00:00Z"}})})}})}});atomic(directory/L"state.json",state.dump());
    auto commandFile=root/L"entry.cmd";auto entry=[&](const std::string& name){write(commandFile,"@echo off\r\ncall scripts\\"+name+" --reload-shell\r\nexit /b %ERRORLEVEL%\r\n");return run({environment(L"COMSPEC"),L"/d",L"/c",commandFile.wstring()},root);};
    auto original=read(root/L"scripts/build.bat"),unguarded=original;auto start=unguarded.find("rem Acquire execution authority before any preflight/process action or output write.");auto end=unguarded.find("if defined RELOAD_SHELL (",start);require(start!=std::string::npos&&end!=std::string::npos,"Entry negative control anchor missing");unguarded.erase(start,end-start);write(root/L"scripts/build.bat",unguarded);require(entry("build.bat").code==3&&fs::exists(root/L"preflight.marker"),"Unguarded negative control did not reach preflight");fs::remove(root/L"preflight.marker");write(root/L"scripts/build.bat",original);
    Bridge bridge(repo,root);
    auto probe=[&](int code,const Env& env=Env{}){
        for(const auto& name:{"build.bat","build_debug.bat"}){write(commandFile,"@echo off\r\ncall scripts\\"+std::string(name)+" --reload-shell\r\nexit /b %ERRORLEVEL%\r\n");auto result=run({environment(L"COMSPEC"),L"/d",L"/c",commandFile.wstring()},root,25,env);require(result.code==code,result.out+result.err);}
        for(const auto& pair:std::vector<std::pair<std::string,std::vector<std::string>>>{{"test_manager.ps1",{"-Mode","name","-Filter","Alpha"}},{"build_entry.ps1",{"-Action","ide"}}}){auto result=bridge.invoke(root/L"scripts"/pair.first,pair.second,env);require(result.code==code,result.out+result.err);}
    };
    for(const auto& phase:{"editing","building"}){state["current"]["phase"]=phase;atomic(directory/L"state.json",state.dump());probe(2);require(!fs::exists(root/L"preflight.marker"),"Lease refusal occurred after preflight");}
    state["current"]=Json{};atomic(directory/L"state.json",state.dump());HANDLE lease=CreateFileW((directory/L"build.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr);require(lease!=INVALID_HANDLE_VALUE,"Fixture lock unavailable");try{probe(2);auto busy=bridge.invoke(root/L"scripts/build_manager.ps1",{"begin","new-terminal"});require(busy.code==2,"begin ignored active output lease");}catch(...){CloseHandle(lease);throw;}CloseHandle(lease);
    probe(2,{{L"SNOWDESKTOP_EXECUTION_TOKEN",L"forged"}});require(!fs::exists(root/L"preflight.marker"),"Forged authority reached preflight");require(entry("build.bat").code==3&&read(root/L"preflight.marker")=="called","Standalone preflight exit lost");
    auto fresh=temporary("readonly-entry");copy(repo,fresh,{"scripts/build_manager.ps1","scripts/build_entry.ps1","scripts/build_inputs.ps1","scripts/build_protocol.ps1","scripts/build_ownership.ps1","scripts/build_preflight.ps1"});auto observed=run({powershell().wstring(),L"-NoProfile",L"-File",(fresh/L"scripts/build_manager.ps1").wstring(),L"status"},fresh);require(observed.code==0&&Json::parse(observed.out).at("current").kind==Json::Kind::Null&&!fs::exists(fresh/L".build/collaboration"),"status created coordination state");
    auto before=read(directory/L"state.json");bridge.call(root/L"scripts/build_manager.ps1",{"status"});require(before==read(directory/L"state.json"),"status changed state");
    cmake_entry_tests(repo,root,bridge);
    // Optional production-tool probes use native stand-ins. The tests themselves
    // remain native even when no Python interpreter can be found.
    auto runtime=temporary("optional-runtime");copy(repo,runtime,{"scripts/build_runtime.ps1","scripts/build_batch_tests.ps1","scripts/build_entry.ps1","scripts/build_job.cs"});fs::create_directories(runtime/L"bin");write(runtime/L".build/collaboration"/wide(bid+".plan.json"),Json::object({{"schemaVersion",1},{"batchId",bid},{"configuration","Release"},{"mode","selected"},{"buildRequired",false}}).dump());
    write(runtime/L"scripts/test_manager.ps1","param($Mode,$PlanBatch)\n[IO.File]::AppendAllText((Join-Path $PSScriptRoot '../fallback.count'),'once');exit 17\n",true);write(runtime/L"scripts/build_test_retry.py","external production tool stand-in path\n");write(runtime/L"scripts/build_wait_tasks.py","external production tool stand-in path\n");
    Bridge runtimeBridge(repo,runtime);auto system=fs::path(environment(L"SystemRoot"))/L"System32";auto alias=runtime/L"bin/python.exe";Env local{{L"PATH",alias.parent_path().wstring()+L";"+system.wstring()}};
    auto invoke=[&](int code,const Env& env){auto result=runtimeBridge.invoke(runtime/L"scripts/build_batch_tests.ps1",{"-Batch",bid},env);require(result.code==code,result.out+result.err);};invoke(17,{{L"PATH",system.wstring()}});write(alias,"broken executable fixture");invoke(17,local);fs::remove(alias);fs::copy_file(standin(),alias);local[L"SNOWDESKTOP_STANDIN_MODE"]=L"old";invoke(17,local);require(read(runtime/L"fallback.count")=="onceonceonce"&&!fs::exists(runtime/L"enhancement.count"),"Invalid optional runtime started enhancement");local[L"SNOWDESKTOP_STANDIN_MODE"]=L"healthy";invoke(19,local);require(read(runtime/L"fallback.count")=="onceonceonce"&&read(runtime/L"enhancement.count")=="once\n","Started enhancement failure was replayed");local[L"SNOWDESKTOP_STANDIN_MODE"]=L"watch";auto forwarded=runtimeBridge.invoke(runtime/L"scripts/build_runtime.ps1",{"watch","watch","wait","terminal","--condition","window"},local);require(forwarded.code==23&&Json::parse(forwarded.out).dump()==Json::array({"watch","wait","terminal","--condition","window"}).dump(),"Optional watch changed CLI arguments/exit");
    require(read(repo/L"cmake/SnowDesktop.ToolTests.cmake").find("find_package(Python")==std::string::npos,"Native test inventory depends on Python discovery");
    std::cout<<"PASS native entry guards, forged authority, read-only status and optional production runtime fallback\n";
}
}
