#include "../build_tool_test_support.h"
#include <iostream>
using namespace build_test;
namespace {
int collectors(const Json& view){int count=0;for(const auto& event:view.at("stageEvents").items)if(event.at("kind").str()=="collector")++count;return count;}
double build_seconds(const Json& view){double seconds=0;for(const auto& event:view.at("stageEvents").items)if(event.at("kind").str()=="end"&&event.has("name")&&event.at("name").str()=="test-target-build")seconds+=event.at("elapsedSeconds").number;return seconds;}
}
void test_build_roots_tests(const fs::path& repo){
    Coordinator c(repo,"test-build-roots");
    copy(repo,c.root,{"scripts/test_manager.ps1","scripts/test_output.ps1","cmake/SnowDesktop.SharedBuildGuard.cmake","cmake/SnowDesktop.PowerShell.cmake"});
    c.call({"task-open","roots","-TaskName","selected native build roots"});
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(SelectedRoots LANGUAGES CXX)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.SharedBuildGuard.cmake")
snowdesktop_check_shared_output()
enable_testing()
add_library(RootValue STATIC value.cpp)
foreach(name Alpha Beta Gamma)
 add_executable(SnowDesktopRoot${name}Tests main.cpp)
 target_link_libraries(SnowDesktopRoot${name}Tests PRIVATE RootValue)
 set_target_properties(SnowDesktopRoot${name}Tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
endforeach()
add_dependencies(SnowDesktopRootGammaTests SnowDesktopRootAlphaTests SnowDesktopRootBetaTests)
file(READ "${CMAKE_SOURCE_DIR}/expected.txt" expected)
add_test(NAME alpha COMMAND SnowDesktopRootAlphaTests "${expected}")
add_test(NAME beta COMMAND SnowDesktopRootBetaTests "${expected}")
add_test(NAME gamma COMMAND SnowDesktopRootGammaTests "${expected}")
)CMAKE");
    write(c.root/L"CMakePresets.json",R"JSON({"version":3,"configurePresets":[{"name":"release","generator":"Visual Studio 18 2026","architecture":"x64","binaryDir":"${sourceDir}/.build"},{"name":"tests","inherits":"release"}],"buildPresets":[{"name":"tests","configurePreset":"tests","configuration":"Release","jobs":1}],"testPresets":[{"name":"tests","configurePreset":"tests","configuration":"Release","output":{"outputOnFailure":true},"execution":{"jobs":1}},{"name":"all-tests","inherits":"tests"}]})JSON");
    write(c.root/L"main.cpp","#include <cstdlib>\nint value(); int main(int n,char** a){return n==2&&value()==std::atoi(a[1])?0:7;}\n");
    write(c.root/L"value.cpp","int value(){return 13;}\n");write(c.root/L"expected.txt","13");
    const auto original=read(c.scripts/L"test_manager.ps1");
    auto unpruned=original;
    replace(unpruned,"$buildRoots=Get-TestBuildRoots $repositoryRoot $selection.Targets","$buildRoots=[pscustomobject]@{targets=$selection.Targets;omitted=@();reason='original-targets-control'}");
    const auto execute=[&](const std::string& label){
        const auto submit=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"start",L"-Task",L"roots",L"-Action",L"tests",L"-Mode",L"name",L"-Filter",L"^(alpha|beta|gamma)$"},c.root,15,{},true);
        require(submit.code==0,"Root build submission failed: "+submit.out+submit.err);
        const auto id=Json::parse(submit.out).at("id").str();
        const auto receipt=c.stateRoot/L"runs"/wide(id+".launch.json");until([&]{return fs::exists(receipt);},12);c.workers.push_back(Json::parse(read(receipt)));
        const auto waited=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"wait",L"-RunId",wide(id),L"-Seconds",L"60"},c.root,70);
        const auto view=Json::parse(waited.out);std::string diagnostics;
        if(waited.code!=0||view.at("status").str()!="passed"){
            const auto record=Json::parse(read(c.stateRoot/L"runs"/wide(id+".json")));
            for(const auto* name:{"logPath","errorPath"})if(record.has(name)&&record.at(name).kind==Json::Kind::String){const auto path=fs::path(wide(record.at(name).str()));if(fs::is_regular_file(path)){auto text=read(path);diagnostics+=text.substr(text.size()>10000?text.size()-10000:0);}}
            for(const auto& event:view.at("stageEvents").items)if(event.has("logPath")&&!event.at("logPath").str().empty()){const auto path=fs::path(wide(event.at("logPath").str()));if(fs::is_regular_file(path)){auto text=read(path);diagnostics+=text.substr(text.size()>10000?text.size()-10000:0);}}
        }
        require(waited.code==0&&view.at("status").str()=="passed","Selected-root execution failed: "+label+" "+waited.out+waited.err+diagnostics);
        require(view.at("stageError").str().empty(),"Selected-root event stream was incomplete");
        int executed=0;
        for(const auto& event:view.at("stageEvents").items)if(event.at("kind").str()=="end"&&event.has("name")&&event.at("name").str()=="test-execute")++executed;
        require(executed==1,"Root pruning changed the test execution count");
        std::cout<<"CASE "<<label<<" MSBuild invocations="<<collectors(view)<<" target-build-seconds="<<build_seconds(view)<<'\n';
        write(c.root/wide(label+".json"),view.dump());return view;
    };
    write(c.scripts/L"test_manager.ps1",unpruned,true);require(collectors(execute("cold-original"))==3,"Baseline did not build all three selected targets");
    Json samples=Json::array({});
    for(int sample=0;sample<3;++sample){
        write(c.scripts/L"test_manager.ps1",unpruned,true);const auto before=execute("original-"+std::to_string(sample));
        write(c.scripts/L"test_manager.ps1",original,true);const auto after=execute("roots-"+std::to_string(sample));
        require(collectors(before)==3&&collectors(after)==1,"Selected dependencies were not built through one confirmed root invocation");
        samples.items.push_back(Json::object({{"sample",sample},{"originalSeconds",build_seconds(before)},{"rootsSeconds",build_seconds(after)}}));
    }
    write(c.root/L"value.cpp","int value(){return 29;}\n");write(c.root/L"expected.txt","29");
    require(collectors(execute("changed-dependency"))==1,"Changed shared input prevented a single-root build");
    // Remove only the diagnostic Build confirmation in an isolated logger.
    // The real build still runs; omitted explicit calls must safely return.
    auto logger=read(c.scripts/L"build_msbuild_logger.cs");replace(logger,"if(value.TargetName==\"Build\"){","if(value.TargetName==\"NoBuildObservationControl\"){");write(c.scripts/L"build_msbuild_logger.cs",logger,true);
    const auto absent=execute("without-confirmation");require(collectors(absent)==3,"Missing Build confirmation silently omitted explicit selected targets");
    write(c.root/L"root-comparison.json",samples.dump());
    std::cout<<"PASS real selected-root builds, same selected assertions, shared-library invalidation, three warm paired samples and missing-confirmation fallback\n";
}
