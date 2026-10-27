#include "../build_tool_test_support.h"
#include <algorithm>
#include <iostream>
#include <sstream>
using namespace build_test;
namespace {
std::string generic(const fs::path& path){return utf8(path.generic_wstring());}
std::string xml(std::string value){for(const auto& pair:std::vector<std::pair<std::string,std::string>>{{"&","&amp;"},{"\"","&quot;"},{"<","&lt;"},{">","&gt;"}}){size_t at=0;while((at=value.find(pair.first,at))!=std::string::npos){value.replace(at,pair.first.size(),pair.second);at+=pair.second.size();}}return value;}
struct Build {
    Coordinator* coordinator;
    std::string id;
};
Build start(Coordinator& c,const fs::path& probe,const std::string& side){
    const Env env={{L"SNOWDESKTOP_COMPILER_PROBE",probe.wstring()},{L"SNOWDESKTOP_COMPILER_PROJECT",wide(side)},{L"SNOWDESKTOP_REAL_CL_FILE",(probe/wide("compiler-"+side+".txt")).wstring()}};
    const auto result=c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","start","-Task","budget","-Action","ide","-Targets","BudgetProgram"},0,env);
    const auto id=result.at("id").str();
    const auto receipt=c.stateRoot/L"runs"/wide(id+".launch.json");
    until([&]{return fs::exists(receipt);},12);c.workers.push_back(Json::parse(read(receipt)));
    return {&c,id};
}
Json finish(const Build& build){
    auto& c=*build.coordinator;
    const auto result=c.bridge->invoke(c.scripts/L"build_run.ps1",{"-Command","wait","-Task","budget","-RunId",build.id,"-Seconds","60"},{},70);
    const auto view=Json::parse(result.out);
    std::string log;
    for(const auto& event:view.at("stageEvents").items)if(event.has("logPath")&&!event.at("logPath").str().empty()){
        const auto path=fs::path(wide(event.at("logPath").str()));if(fs::exists(path)){const auto text=read(path);log+=text.substr(text.size()>8000?text.size()-8000:0);}
    }
    require(result.code==0&&view.at("status").str()=="passed","Budget build failed: "+view.dump()+result.err+log);
    require(view.at("stageError").str().empty(),"Budget event stream error: "+view.at("stageError").str());
    require(log.find("warning C")==std::string::npos&&log.find("warning D")==std::string::npos&&log.find("warning LNK")==std::string::npos,"Actual compiler/linker warning in budget fixture: "+log);
    return view;
}
void units(const Json& view,int expected){
    int actual=0;bool mtt=false;
    for(const auto& event:view.at("stageEvents").items){
        if(event.at("kind").str()=="start"&&event.has("task")&&event.at("task").str()=="MultiToolTask"&&event.at("name").str()=="compile")mtt=true;
        if(event.at("kind").str()=="end"&&event.has("name")&&event.at("name").str()=="compile"){
            require(event.at("unitCountKnown").boolean,"MultiToolTask compiler observations became unknown: "+event.dump());actual+=event.at("attemptedUnits").integer();
        }
    }
    require(actual==expected&&(expected==0||mtt),"Compiler telemetry disagrees with real compiler executions: expected "+std::to_string(expected)+", observed "+std::to_string(actual));
}
void prepare(Coordinator& c,const fs::path& repo,const std::string& control){
    copy(repo,c.root,{"cmake/SnowDesktop.CompileParallelism.cmake"});
    if(control=="limit"){
        auto source=read(c.root/L"cmake/SnowDesktop.CompileParallelism.cmake");replace(source,"EnforceProcessCountAcrossBuilds=true","EnforceProcessCountAcrossBuilds=false");write(c.root/L"cmake/SnowDesktop.CompileParallelism.cmake",source);
    }
    if(control=="observation"){
        auto source=read(c.scripts/L"build_msbuild_logger.cs");replace(source,"if (task == \"MultiToolTask\" && target == \"ClCompile\") return \"compile\";","if (task == \"MultiToolTask\") return null;");write(c.scripts/L"build_msbuild_logger.cs",source);
    }
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+"\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='Compiler budget fixture has no optional Python dashboard'}}\n",true);
    write(c.root/L".gitignore",read(c.root/L".gitignore")+"!/cmake/\n!/CMakeLists.txt\n!/Directory.Build.targets\n");
    fs::create_directories(c.root/L"proxy");fs::copy_file(standin(),c.root/L"proxy/cl-budget.exe");
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(CompilerBudget LANGUAGES CXX)
# This workload uses ordinary C++20 headers/PCH, not C++ modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.CompileParallelism.cmake")
if(DEFINED ENV{SNOWDESKTOP_COMPILER_PROBE} AND DEFINED ENV{SNOWDESKTOP_COMPILER_PROJECT})
 file(WRITE "$ENV{SNOWDESKTOP_COMPILER_PROBE}/compiler-$ENV{SNOWDESKTOP_COMPILER_PROJECT}.txt" "${CMAKE_CXX_COMPILER}")
endif()
list(APPEND CMAKE_VS_GLOBALS "CLToolPath=${CMAKE_SOURCE_DIR}/proxy/" "CLToolExe=cl-budget.exe")
add_executable(BudgetProgram src/main.cpp src/unit0.cpp src/unit1.cpp src/unit2.cpp)
target_include_directories(BudgetProgram PRIVATE src)
target_precompile_headers(BudgetProgram PRIVATE src/common.h)
target_compile_features(BudgetProgram PRIVATE cxx_std_20)
target_compile_options(BudgetProgram PRIVATE /W4 /WX /MP${SNOWDESKTOP_COMPILE_JOBS} /Z7)
get_target_property(observed BudgetProgram VS_GLOBAL_UseMultiToolTask)
file(WRITE "${CMAKE_BINARY_DIR}/observed-mode.txt" "${observed}")
)CMAKE");
    write(c.root/L"CMakePresets.json",R"JSON({"version":3,"configurePresets":[{"name":"release","generator":"Visual Studio 18 2026","architecture":"x64","binaryDir":"${sourceDir}/.build/cxx","cacheVariables":{"SNOWDESKTOP_BUILD_TOOL_JOBS":"2"}}],"buildPresets":[{"name":"release","configurePreset":"release","configuration":"Release","jobs":2,"targets":["BudgetProgram"]}]})JSON");
    write(c.root/L"Directory.Build.targets","<Project><Target Name=\"CompilerBudgetProbeGate\" BeforeTargets=\"ClCompile\" Condition=\"'$(MSBuildProjectName)' == 'BudgetProgram'\"><Exec Command=\"&quot;"+xml(generic(c.root/L"proxy/cl-budget.exe"))+"&quot; --budget-build-gate\" /></Target></Project>\n");
    write(c.root/L"src/common.h","#pragma once\n#include <array>\n#include <string>\n#include <vector>\ninline constexpr int value=11;\n");
    for(int i=0;i<3;++i)write(c.root/L"src"/wide("unit"+std::to_string(i)+".cpp"),"#include \"common.h\"\nint unit"+std::to_string(i)+"(){return value+"+std::to_string(i)+";}\n");
    write(c.root/L"src/main.cpp","#include \"common.h\"\nint unit0();int unit1();int unit2();int main(){return unit0()+unit1()+unit2();}\n");
    c.call({"task-open","budget","-TaskName","Actual cross-build compiler budget"});
}
Json counter(const fs::path& probe){return Json::parse(read(probe/L"counter.json"));}
void program(Coordinator& c,int code){const auto result=run({(c.root/L".build/cxx/Release/BudgetProgram.exe").wstring()},c.root);require(result.code==code,"PCH/source dependency produced stale program output");}
void scenario(const fs::path& repo,const std::string& control){
    // AppData/TEMP are excluded from MSBuild tracking: source/PCH fixtures live
    // under the workspace; observational counters stay in the excluded temp.
    // Do not redefine TEMP to the source fixture, which would exclude it again.
    const auto fixtureParent=repo/L".codex-probes/compile-budget-fixtures";
    Coordinator left(repo,"compiler-budget-left",false,fixtureParent),right(repo,"compiler-budget-right",false,fixtureParent);
    const auto probe=temporary("compiler-budget-counter");
    prepare(left,repo,control);prepare(right,repo,control);
    auto first=start(left,probe,"left"),second=start(right,probe,"right");
    const auto a=finish(first),b=finish(second);
    const auto cold=counter(probe);
    require(cold.at("starts").integer()==10&&cold.at("active").integer()==0,"Actual compiler proxy did not produce both four-unit/PCH programs");
    require(cold.at("maximum").integer()<=2,"Cross-build compiler budget exceeded: expected <=2, observed "+std::to_string(cold.at("maximum").integer()));
    units(a,5);units(b,5);program(left,36);program(right,36);
    if(!control.empty())return;
    units(finish(start(left,probe,"left")),0);
    require(counter(probe).at("starts").integer()==10,"No-op budget build recompiled unchanged sources");
    write(left.root/L"src/unit0.cpp","#include \"common.h\"\nint unit0(){return value+10;}\n");
    units(finish(start(left,probe,"left")),1);program(left,46);
    require(counter(probe).at("starts").integer()==11,"One-unit budget increment recompiled unrelated units");
    write(left.root/L"src/common.h","#pragma once\n#include <array>\n#include <string>\n#include <vector>\ninline constexpr int value=12;\n");
    units(finish(start(left,probe,"left")),5);program(left,49);
    require(counter(probe).at("starts").integer()==16,"Changed PCH header did not invalidate the actual dependent objects");
    for(const auto& specification:std::vector<std::pair<std::wstring,std::string>>{{L"-DSNOWDESKTOP_BUILD_TOOL_JOBS=bad","SNOWDESKTOP_BUILD_TOOL_JOBS must be an integer"},{L"-DSNOWDESKTOP_BUILD_TOOL_JOBS=65537","SNOWDESKTOP_BUILD_TOOL_JOBS must be an integer"},{L"-DCMAKE_VS_GLOBALS=EnforceProcessCountAcrossBuilds=false","Conflicting CMAKE_VS_GLOBALS"}}){
        const auto result=run({L"cmake",L"-S",left.root.wstring(),L"-B",(left.root/L".build/diagnostic").wstring(),L"-G",L"Visual Studio 18 2026",L"-A",L"x64",L"-DSNOWDESKTOP_BUILD_TOOL_JOBS=2",specification.first},left.root,30);
        require(result.code!=0&&(result.out+result.err).find(specification.second)!=std::string::npos,"Invalid/conflicting process budget configured silently: "+result.out+result.err);
    }
    const auto disabled=run({L"cmake",L"-S",left.root.wstring(),L"-B",(left.root/L".build/disabled").wstring(),L"-G",L"Visual Studio 18 2026",L"-A",L"x64",L"-DSNOWDESKTOP_BUILD_TOOL_JOBS=0"},left.root,30);
    require(disabled.code==0&&read(left.root/L".build/disabled/observed-mode.txt").find("NOTFOUND")!=std::string::npos,"Disabled budget unexpectedly enabled MultiToolTask");
    for(const auto& mutation:std::vector<std::pair<std::string,std::string>>{{"limit","Cross-build compiler budget exceeded"},{"observation","Compiler telemetry disagrees with real compiler executions"}}){
        const auto result=run({executable().wstring(),repo.wstring(),wide("--compile-budget-"+mutation.first)},left.root,90,{},true);
        require(result.code==1&&result.err.find(mutation.second)!=std::string::npos,"Cross-build budget negative control missed its business assertion: "+result.out+result.err);
    }
    std::cout<<"PASS production CMake budget across two durable MSBuild Runs, actual compiler peak="<<cold.at("maximum").integer()<<", PCH/source increments, MultiToolTask provenance, disabled/conflicting configuration; removed limit/observation controls fail\n";
}
}
void compile_budget_tests(const fs::path& repo,const std::string& control){scenario(repo,control);}
