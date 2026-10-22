#include "../build_tool_test_support.h"
#include <algorithm>
#include <iostream>

using namespace build_test;
namespace {
fs::path fixture_parent(const fs::path& repo) {
    const auto git=run({L"git",L"rev-parse",L"--path-format=absolute",L"--git-common-dir"},repo);
    auto text=git.out;while(!text.empty()&&(text.back()=='\r'||text.back()=='\n'))text.pop_back();
    const auto common=fs::path(wide(text));
    return (git.code==0&&common.filename()==L".git"?common.parent_path():repo)/L".codex-probes/nop";
}
void prepare(Coordinator& c,const fs::path& repo,const std::string& control) {
    copy(repo,c.root,{"cmake/SnowDesktop.ProductBuild.cmake","cmake/SnowDesktop.SharedBuildGuard.cmake",
        "cmake/SnowDesktop.PowerShell.cmake","scripts/arrange_build_output.ps1","scripts/deployment_payload.psm1","version.json"});
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+
        "\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='Native output fixture has no optional dashboard'}}\n",true);
    write(c.root/L".gitignore",read(c.root/L".gitignore")+"!/cmake/\n!/CMakeLists.txt\n!/version.json\n!/fixture.ps1\n!/fixture.manifest\n!/fixture.rc\n");
    auto presets=Json::parse(read(repo/L"CMakePresets.json"));
    for(auto& preset:presets["configurePresets"].items)if(preset.at("name").str()=="release") {
        preset["generator"]="Visual Studio 18 2026";preset["architecture"]="x64";
    }
    if(control=="targets")presets["buildPresets"].items[0]["targets"]=Json::array(
        {"FixtureArrange","SnowDesktopLauncher","SnowDesktopSteamBridge","SnowDesktopWorkshopManager","snowwidget"});
    write(c.root/L"CMakePresets.json",presets.dump());
    if(control=="layout") {
        auto module=read(c.root/L"cmake/SnowDesktop.ProductBuild.cmake");
        while(module.find("${output_root}/SnowDesktop.Runtime")!=std::string::npos)
            replace(module,"${output_root}/SnowDesktop.Runtime","${output_root}");
        write(c.root/L"cmake/SnowDesktop.ProductBuild.cmake",module);
    }
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.SharedBuildGuard.cmake")
snowdesktop_check_shared_output()
project(ProductOutputFixture LANGUAGES CXX)
option(BUILD_TESTING "Fixture test discovery is owned by the native driver" OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.ProductBuild.cmake")
include(ExternalProject)
snowdesktop_observed_build_command(child_command "${CMAKE_BINARY_DIR}/child" "$<CONFIG>")
ExternalProject_Add(NestedBuild SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/child"
 BINARY_DIR "${CMAKE_BINARY_DIR}/child" CMAKE_GENERATOR "${CMAKE_GENERATOR}"
 CMAKE_GENERATOR_PLATFORM x64 BUILD_COMMAND ${child_command} BUILD_ALWAYS TRUE INSTALL_COMMAND "")
add_library(SnowDesktopHookEntry SHARED src/dll.cpp)
snowdesktop_private_runtime_target(SnowDesktopHookEntry "${CMAKE_BINARY_DIR}/$<CONFIG>")
add_executable(SnowDesktop src/main.cpp fixture.rc)
target_link_options(SnowDesktop PRIVATE /MANIFEST:NO)
add_dependencies(SnowDesktop SnowDesktopHookEntry NestedBuild)
foreach(target SnowDesktopLauncher SnowDesktopSteamBridge SnowDesktopWorkshopManager snowwidget)
 add_executable(${target} src/tool.cpp)
endforeach()
add_dependencies(SnowDesktop SnowDesktopSteamBridge snowwidget)
foreach(target SnowDesktop SnowDesktopHookEntry SnowDesktopLauncher SnowDesktopSteamBridge SnowDesktopWorkshopManager snowwidget)
 target_compile_features(${target} PRIVATE cxx_std_20)
 target_compile_options(${target} PRIVATE /W4 /WX /Z7)
endforeach()
add_custom_target(FixtureArrange
 COMMAND "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile -File "${CMAKE_SOURCE_DIR}/fixture.ps1"
  -Output "${CMAKE_BINARY_DIR}/$<CONFIG>"
 COMMAND "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/arrange_build_output.ps1"
  -BuildOutput "${CMAKE_BINARY_DIR}/$<CONFIG>" -AllowMissingFirstPartyRuntime
 DEPENDS SnowDesktop SnowDesktopLauncher SnowDesktopSteamBridge SnowDesktopWorkshopManager snowwidget VERBATIM)
snowdesktop_add_product_build(SnowDesktopReleaseBuild FixtureArrange)
snowdesktop_add_product_build(SnowDesktopDebugBuild FixtureArrange)
snowdesktop_install_shared_guard()
)CMAKE");
    write(c.root/L"src/child/CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(NestedObservedBuild LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_library(ChildLibrary STATIC ../child.cpp)
target_compile_options(ChildLibrary PRIVATE /W4 /WX)
)CMAKE");
    write(c.root/L"src/child.cpp","int child_value(){return 1;}\n");
    write(c.root/L"src/common.h","#pragma once\ninline constexpr int value=7;\n");
    write(c.root/L"src/dll.cpp","#include \"common.h\"\nextern \"C\" __declspec(dllexport) int fixture_value(){return value;}\n");
    write(c.root/L"src/tool.cpp","#include \"common.h\"\nint main(){return value;}\n");
    write(c.root/L"src/main.cpp",R"CPP(#include <windows.h>
#include <cstring>
int main(){
 const auto dll=LoadLibraryW(L"SnowDesktop.Runtime\\SnowDesktopHookEntry.dll");
 if(!dll)return 100;
 const auto address=GetProcAddress(dll,"fixture_value");
 int (*value)()=nullptr;static_assert(sizeof(value)==sizeof(address));
 std::memcpy(&value,&address,sizeof(value));
 const auto result=value?value():101;FreeLibrary(dll);return result;
}
)CPP");
    write(c.root/L"fixture.manifest",R"XML(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
 <file xmlns="urn:schemas-microsoft-com:asm.v3" name="SnowDesktopHookEntry.dll">
  <activatableClass xmlns="urn:schemas-microsoft-com:winrt.v1" name="Noop.Fixture" threadingModel="both"/>
 </file>
</assembly>)XML");
    write(c.root/L"fixture.rc","1 24 \"fixture.manifest\"\n");
    // Data materialization only: the native test owns all result assertions.
    // The fixture does not exercise WinAppSDK activation or actual Hook logic.
    write(c.root/L"fixture.ps1",R"PS(param([string]$Output)
$ErrorActionPreference='Stop'
$relative='SnowDesktopHookEntry.dll'
if(-not (Test-Path -LiteralPath (Join-Path $Output $relative))){$relative='SnowDesktop.Runtime/SnowDesktopHookEntry.dll'}
$entries=@()
$names=@('CppWinRT-LICENSE.txt','WindowsAppSDK-LICENSE.txt','WindowsAppSDK-NOTICE.txt','WindowsML-LICENSE.txt','WindowsML-NOTICE.txt','WebView2-LICENSE.txt','WebView2-NOTICE.txt','YASB-LICENSE.txt','MiSans-LICENSE.pdf','HarmonyOS-Sans-LICENSE.txt')
$meta=Join-Path $Output '.deployment/fixture'
New-Item -ItemType Directory -Path $meta -Force|Out-Null
foreach($name in $names){
 $path=Join-Path $meta $name;[IO.File]::WriteAllText($path,'Output-layout fixture only')
 $entries+=@{path=".deployment/fixture/$name";sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;destination="licenses/$name"}
}
$fragment=Join-Path $meta 'fragment.xml';[IO.File]::WriteAllText($fragment,'<Fragment/>')
@{schemaVersion=1;architecture='x64';windowsAppSdkPackageVersion='2.4.0';cppWinRtPackageVersion='3.0.260818.1'
 files=@(@{path='SnowDesktopHookEntry.dll';buildPath=$relative;source='FixtureRuntime';sha256=(Get-FileHash -LiteralPath (Join-Path $Output $relative) -Algorithm SHA256).Hash})
 appxFragments=@(@{path='.deployment/fixture/fragment.xml';sha256=(Get-FileHash -LiteralPath $fragment -Algorithm SHA256).Hash});notices=$entries
}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $Output 'SnowDesktop.deployment.json') -Encoding UTF8
)PS",true);
    c.call({"task-open","noop","-TaskName","Actual output and graph no-op checks"});
}
Json build(Coordinator& c,const std::string& configuration="Release") {
    const auto launch=c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","start","-Task","noop","-Action","ide","-Configuration",configuration});
    const auto id=launch.at("id").str();const auto receipt=c.stateRoot/L"runs"/wide(id+".launch.json");
    until([&]{return fs::exists(receipt);},12);c.workers.push_back(Json::parse(read(receipt)));
    Json view;Result result;
    for(int attempt=0;attempt<2;++attempt){
        result=c.bridge->invoke(c.scripts/L"build_run.ps1",{"-Command","wait","-Task","noop","-RunId",id,"-Seconds","60"},{},70);
        view=Json::parse(result.out);if(result.code!=10)break;
    }
    std::string logs;
    for(const auto& event:view.at("stageEvents").items)if(event.has("logPath")&&!event.at("logPath").str().empty()){
        const auto path=fs::path(wide(event.at("logPath").str()));if(fs::exists(path))logs+=read(path);
    }
    require(result.code==0&&view.at("status").str()=="passed","Output fixture build failed: "+view.dump()+result.err+logs);
    require(view.at("stageError").str().empty(),"Output fixture stage error: "+view.dump());
    require(logs.find("warning C")==std::string::npos&&logs.find("warning D")==std::string::npos&&logs.find("warning LNK")==std::string::npos,"Output fixture compiler/linker warning: "+logs);
    return view;
}
void observations(const Json& view,int units) {
    int collectors=0,ends=0,guards=0,actual=0;
    for(const auto& event:view.at("stageEvents").items) {
        const auto kind=event.at("kind").str();
        if(kind=="collector")++collectors;
        if(kind=="collector-end"){++ends;require(event.at("unfinishedTasks").integer()==0,"Unfinished MSBuild observations");}
        if(kind=="start"&&event.has("project")&&event.at("project").str()=="SnowDesktopSharedBuildGuard")++guards;
        if(kind=="end"&&event.has("name")&&event.at("name").str()=="compile"){
            require(event.at("unitCountKnown").boolean,"Unknown fixture compiler count");actual+=event.at("attemptedUnits").integer();
        }
    }
    require(guards==1,"Product graph repeated the shared guard: "+std::to_string(guards));
    require(collectors==2&&ends==2,"Nested absolute-directory build lost observations: "+std::to_string(collectors));
    require(actual==units,"Incorrect actual translation units: "+std::to_string(actual)+", expected "+std::to_string(units));
}
using FileState=std::map<fs::path,std::pair<std::uintmax_t,fs::file_time_type>>;
FileState outputs(const fs::path& binary,const fs::path& output,bool debug) {
    FileState state;
    for(const auto& name:{"SnowDesktop.Runtime/SnowDesktopHookEntry.dll","SnowDesktopHookEntry.lib"}) {
        const auto path=output/name;require(fs::exists(path),"Missing final output/import library: "+utf8(path.wstring()));
        state[path]={fs::file_size(path),fs::last_write_time(path)};
    }
    if(debug) {
        const auto pdb=output/L"SnowDesktopHookEntry.pdb";
        require(fs::exists(pdb),"Linker PDB was discarded by arrangement");
        state[pdb]={fs::file_size(pdb),fs::last_write_time(pdb)};
        for(const auto& file:fs::recursive_directory_iterator(binary))if(file.is_regular_file()&&file.path().extension()==L".ilk")
            state[file.path()]={fs::file_size(file.path()),fs::last_write_time(file.path())};
        require(state.size()>3,"Debug fixture did not produce incremental link state");
    }
    return state;
}
void unchanged(const FileState& state) {
    for(const auto& [path,before]:state)require(fs::exists(path)&&fs::file_size(path)==before.first&&fs::last_write_time(path)==before.second,
        "No-op build relinked or lost output: "+utf8(path.wstring()));
}
void program(const fs::path& output,int expected) {
    const auto result=run({(output/L"SnowDesktop.exe").wstring()},output);
    require(result.code==expected,"Arranged runtime returned stale/invalid code: "+std::to_string(result.code));
}
void scenario(const fs::path& repo,const std::string& control) {
    Coordinator c(repo,"n",false,fixture_parent(repo));prepare(c,repo,control);
    const auto output=c.root/L".build/Release";
    observations(build(c),7);program(output,7);
    const auto before=outputs(c.root/L".build",output,false);
    observations(build(c),0);unchanged(before);program(output,7);
    if(!control.empty())return;
    observations(build(c,"Debug"),7);
    const auto debug=c.root/L".build_debug/Debug";program(debug,7);
    const auto debugState=outputs(c.root/L".build_debug",debug,true);
    observations(build(c,"Debug"),0);unchanged(debugState);program(debug,7);
    write(c.root/L"src/dll.cpp","#include \"common.h\"\nextern \"C\" __declspec(dllexport) int fixture_value(){return value+1;}\n");
    observations(build(c),1);program(output,8);
    write(c.root/L"src/common.h","#pragma once\ninline constexpr int value=10;\n");
    observations(build(c),5);program(output,11);
    require(run({(output/L"snowwidget.exe").wstring()},output).code==10,"Changed header did not reach other product targets");
    fs::remove(output/L"SnowDesktop.Runtime/SnowDesktopHookEntry.dll");
    observations(build(c),0);program(output,11);
    const auto rejected=run({L"cmake",L"--build",(c.root/L".build").wstring(),L"--config",L"Release",L"--target",L"SnowDesktopReleaseBuild"},c.root,30);
    require(rejected.code!=0,"Aggregate target bypassed expired shared-output authority");
    for(const auto& mutation:std::vector<std::pair<std::string,std::string>>{{"layout","No-op build relinked or lost output"},{"targets","Product graph repeated the shared guard"}}){
        const auto result=run({executable().wstring(),repo.wstring(),wide("--"+mutation.first)},c.root,80,{},true);
        require(result.code==1&&result.err.find(mutation.second)!=std::string::npos,"Negative control missed its business assertion: "+result.out+result.err);
    }
    std::cout<<"PASS arranged Release/Debug no-op output preservation, linker state, source/header invalidation, missing-DLL repair, grouped graph and nested telemetry; both legacy controls fail\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){scenario(repo,argc>2?std::string(argv[2]).substr(2):"");});}
