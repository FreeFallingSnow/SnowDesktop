#include "../build_tool_test_support.h"
#include <iostream>
using namespace build_test;
namespace {
int collectors(const Json& view){int count=0;for(const auto& event:view.at("stageEvents").items)if(event.at("kind").str()=="collector")++count;return count;}
double build_seconds(const Json& view){double seconds=0;for(const auto& event:view.at("stageEvents").items)if(event.at("kind").str()=="end"&&event.has("name")&&event.at("name").str()=="test-target-build")seconds+=event.at("elapsedSeconds").number;return seconds;}
int compiled_units(const Json& view){int count=0;for(const auto& event:view.at("stageEvents").items)if(event.at("kind").str()=="end"&&event.has("name")&&event.at("name").str()=="compile"){require(event.at("unitCountKnown").boolean,"Root comparison has unobserved compiler units");count+=event.at("attemptedUnits").integer();}return count;}
double pipeline_seconds(const Json& view){for(const auto& stage:view.at("record").at("stages").items)if(stage.at("name").str()=="pipeline")return stage.at("elapsedSeconds").number;throw std::runtime_error("Missing root pipeline interval");}
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
if(EXISTS "${CMAKE_SOURCE_DIR}/disable-reference-builds")
 set_target_properties(SnowDesktopRootGammaTests PROPERTIES VS_GLOBAL_BuildProjectReferences false)
endif()
file(READ "${CMAKE_SOURCE_DIR}/expected.txt" expected)
add_test(NAME alpha COMMAND SnowDesktopRootAlphaTests "${expected}")
add_test(NAME beta COMMAND SnowDesktopRootBetaTests "${expected}")
add_test(NAME gamma COMMAND SnowDesktopRootGammaTests "${expected}")
set_tests_properties(alpha PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopRootAlphaTests>")
set_tests_properties(beta PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopRootBetaTests>")
set_tests_properties(gamma PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopRootGammaTests>")
)CMAKE");
    write(c.root/L"CMakePresets.json",R"JSON({"version":3,"configurePresets":[{"name":"release","generator":"Visual Studio 18 2026","architecture":"x64","binaryDir":"${sourceDir}/.build"},{"name":"tests","inherits":"release"}],"buildPresets":[{"name":"tests","configurePreset":"tests","configuration":"Release","jobs":1}],"testPresets":[{"name":"tests","configurePreset":"tests","configuration":"Release","output":{"outputOnFailure":true},"execution":{"jobs":1}},{"name":"all-tests","inherits":"tests"}]})JSON");
    write(c.root/L"main.cpp","#include <cstdlib>\n#include <cstdio>\nint value(); int main(int n,char** a){if(n!=2)return 8;int expected=std::atoi(a[1]);if(value()!=expected){std::fprintf(stderr,\"FAILED: compiled shared dependency value differs: expected %d observed %d\\n\",expected,value());return 7;}return 0;}\n");
    write(c.root/L"value.cpp","int value(){return 13;}\n");write(c.root/L"expected.txt","13");
    write(c.root/L".gitignore",read(c.root/L".gitignore")+"\n!/CMakeLists.txt\n!/main.cpp\n!/value.cpp\n!/expected.txt\n!/disable-reference-builds\n");
    const auto original=read(c.scripts/L"test_manager.ps1");
    auto unpruned=original;
    replace(unpruned,"$buildRoots=Get-TestBuildRoots $repositoryRoot $selection.Targets","$buildRoots=[pscustomobject]@{targets=$selection.Targets;omitted=@();reason='original-targets-control'}");
    const auto execute=[&](const std::string& label,bool expectStaleParent=false){
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
        if(expectStaleParent)require(waited.code!=0&&view.at("status").str()=="failed"&&diagnostics.find("FAILED: compiled shared dependency value differs")!=std::string::npos,"Omitting the parent recheck did not reach the stale native executable assertion: "+diagnostics);
        else require(waited.code==0&&view.at("status").str()=="passed","Selected-root execution failed: "+label+" "+waited.out+waited.err+diagnostics);
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
        require(compiled_units(before)==0&&compiled_units(after)==0,"Warm root comparison unexpectedly recompiled sources");
        samples.items.push_back(Json::object({{"sample",sample},{"originalSeconds",build_seconds(before)},{"rootsSeconds",build_seconds(after)},
            {"originalPipelineSeconds",pipeline_seconds(before)},{"rootsPipelineSeconds",pipeline_seconds(after)},
            {"originalCpuSeconds",before.at("resources").at("cpuSeconds")},{"rootsCpuSeconds",after.at("resources").at("cpuSeconds")},
            {"originalPeakJobMemoryBytes",before.at("resources").at("peakJobMemoryBytes")},{"rootsPeakJobMemoryBytes",after.at("resources").at("peakJobMemoryBytes")}}));
    }
    write(c.root/L"value.cpp","int value(){return 29;}\n");write(c.root/L"expected.txt","29");
    const auto changed=execute("changed-dependency");require(collectors(changed)==1&&compiled_units(changed)==1,"Changed shared input did not build its one source through the selected root");
    // Remove only the diagnostic Build confirmation in an isolated logger.
    // The real build still runs; omitted explicit calls must safely return.
    auto logger=read(c.scripts/L"build_msbuild_logger.cs");replace(logger,"if(value.TargetName==\"Build\"){","if(value.TargetName==\"NoBuildObservationControl\"){");write(c.scripts/L"build_msbuild_logger.cs",logger,true);
    const auto absent=execute("without-confirmation");require(collectors(absent)==4,"Missing Build confirmation silently omitted original selected target calls");
    copy(repo,c.root,{"scripts/build_msbuild_logger.cs"});
    write(c.root/L"disable-reference-builds","true");write(c.root/L"value.cpp","int value(){return 41;}\n");write(c.root/L"expected.txt","41");
    require(collectors(execute("disabled-reference-builds"))==4,"Disabled reference builds did not restore the complete original order");
    auto noParent=original;replace(noParent,"(@('--build','--preset','tests','--target')+$selection.Targets)","(@('--build','--preset','tests','--target')+$remaining)");write(c.scripts/L"test_manager.ps1",noParent,true);
    write(c.root/L"value.cpp","int value(){return 53;}\n");write(c.root/L"expected.txt","53");execute("without-parent-recheck",true);
    write(c.root/L"root-comparison.json",samples.dump());
    std::cout<<"PASS real selected-root builds, same selected assertions, shared-library invalidation, three warm paired samples, absent-confirmation/disabled-reference fallback and stale-parent negative control\n";
}
