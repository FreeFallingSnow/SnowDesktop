#include "../build_tool_test_support.h"
#include <iostream>
using namespace build_test;
namespace {
void test(const fs::path& repo) {
    Coordinator c(repo,"observed-native-build");
    copy(repo,c.root,{"scripts/build_command.ps1","scripts/build_msbuild_logger.cs","cmake/SnowDesktop.SharedBuildGuard.cmake","cmake/SnowDesktop.PowerShell.cmake"});
    c.call({"task-open","observations","-TaskName","native phase observations"});
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(ObservedNative LANGUAGES CXX)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.SharedBuildGuard.cmake")
add_executable(ObservedNative main.cpp worker.cpp)
target_compile_options(ObservedNative PRIVATE /W4 /WX)
)CMAKE");
    write(c.root/L"CMakePresets.json",R"JSON({"version":3,"configurePresets":[{"name":"release","generator":"Visual Studio 18 2026","architecture":"x64","binaryDir":"${sourceDir}/.build/cxx"}],"buildPresets":[{"name":"release","configurePreset":"release","configuration":"Release","jobs":2,"targets":["ObservedNative"]}]})JSON");
    write(c.root/L"main.cpp","int worker(); int main(){return worker()==2?0:1;}\n");
    write(c.root/L"worker.cpp","int worker(){return 2;}\n");
    auto execute=[&]{
        const auto submitted=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"start",L"-Task",L"observations",L"-Action",L"ide",L"-Targets",L"ObservedNative"},c.root,15,{},true);
        require(submitted.code==0,"Observed build submission failed: "+submitted.out+submitted.err);
        const auto id=Json::parse(submitted.out).at("id").str();
        const auto waited=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"wait",L"-RunId",wide(id),L"-Seconds",L"60"},c.root,70);
        const auto view=Json::parse(waited.out);
        require(waited.code==0&&view.at("status").str()=="passed","Real observed build failed: "+waited.out+waited.err);
        require(view.at("stageError").str().empty(),"Stage stream failed to read: "+view.at("stageError").str());
        return view;
    };
    auto counts=[&](const Json& view,int expected){
        bool configured=false,linked=false,collected=false;int units=0,compiles=0;
        for(const auto& event:view.at("stageEvents").items){
            if(event.at("kind").str()=="collector")collected=true;
            if(!event.has("name"))continue;
            const auto name=event.at("name").str();
            if(event.at("kind").str()=="end"){
                if(name=="configure")configured=true;
                if(name=="link")linked=true;
                if(name=="compile"){
                    require(event.at("unitCountKnown").boolean,"Actual compiler source count was not captured: "+event.dump());
                    if(event.at("compilerCommandObserved").boolean)++compiles;
                    units+=event.at("attemptedUnits").integer();
                    require(event.at("elapsedSeconds").number>=0&&event.at("sourceElapsedSeconds").number>=0,"Native task duration is invalid");
                }
            }
        }
        require(configured&&collected,"Real CMake/MSBuild did not publish configuration and logger observations");
        require(units==expected&&(expected==0?compiles==0:linked),"Actual compiled-unit or incremental/link observation mismatch: expected "+std::to_string(expected)+", observed "+std::to_string(units)+" / "+view.at("stageEvents").dump());
    };
    counts(execute(),2);
    counts(execute(),0);
    write(c.root/L"worker.cpp","int worker(){return 3;}\n");
    counts(execute(),1);
    std::cout<<"PASS actual leased CMake/MSBuild phases, two translation units, no-op build and one-unit increment\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,test);}
