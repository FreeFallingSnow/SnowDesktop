#include "../build_tool_test_support.h"
#include <iostream>
using namespace build_test;
void configuration_cache_tests(const fs::path& repo,bool omitHeaderIdentity);
void resource_publication_tests(const fs::path& repo,bool formerBehavior);
namespace {
void test(const fs::path& repo,bool withoutLogger=false) {
    const auto label="allocation-collision-"+std::to_string(GetTickCount64());
    const auto retained=fs::temp_directory_path()/wide("SnowDesktop-native-"+label+"-"+std::to_string(GetCurrentProcessId())+"-0");
    require(fs::create_directory(retained),"Cannot prepare isolated allocation collision");write(retained/L"evidence","preserved");
    const auto allocated=temporary(label);
    require(!fs::equivalent(allocated,retained)&&read(retained/L"evidence")=="preserved","Fixture allocation replaced retained collision evidence");
    Coordinator c(repo,"observed-native-build");
    if(withoutLogger){auto source=read(c.scripts/L"build_telemetry.ps1");replace(source,"if($logger){","if($false -and $logger){");write(c.scripts/L"build_telemetry.ps1",source,true);}
    copy(repo,c.root,{"scripts/build_command.ps1","scripts/build_msbuild_logger.cs","cmake/SnowDesktop.SharedBuildGuard.cmake","cmake/SnowDesktop.PowerShell.cmake"});
    c.call({"task-open","observations","-TaskName","native phase observations"});
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(ObservedNative LANGUAGES CXX)
file(WRITE "${CMAKE_BINARY_DIR}/vs-version.txt" "${CMAKE_VS_VERSION_BUILD_NUMBER}")
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
        std::string diagnostics;
        if(waited.code!=0||view.at("status").str()!="passed")
            for(const auto& event:view.at("stageEvents").items)
                if(event.has("logPath")&&!event.at("logPath").str().empty()){
                    const auto path=fs::path(wide(event.at("logPath").str()));
                    if(fs::exists(path)){const auto content=read(path);diagnostics+=content.substr(content.size()>12000?content.size()-12000:0);}
                }
        require(waited.code==0&&view.at("status").str()=="passed","Real observed build failed: "+waited.out+waited.err+diagnostics);
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
    // Configure a fresh output with the actual failing instance specification.
    // An already initialized CMake generator cannot change its instance value.
    const auto cachePath=c.root/L".build/cxx/CMakeCache.txt";
    auto cache=read(cachePath);
    const std::string prefix="CMAKE_GENERATOR_INSTANCE:INTERNAL=";
    const auto instanceStart=cache.find(prefix);
    require(instanceStart!=std::string::npos,"Configured VS instance is missing");
    const auto valueStart=instanceStart+prefix.size();
    const auto valueEnd=cache.find_first_of("\r\n",valueStart);
    const auto instance=cache.substr(valueStart,valueEnd-valueStart);
    const auto location=instance.substr(0,instance.find(",version="));
    // Use CMake's selected installation version, including Build Tools installs
    // that have no devenv.exe.
    const auto version=read(c.root/L".build/cxx/vs-version.txt");
    require(!version.empty(),"CMake did not report the selected VS installation version");
    auto presets=Json::parse(read(c.root/L"CMakePresets.json"));
    auto& configure=presets["configurePresets"].items.front();
    configure["binaryDir"]="${sourceDir}/.build/versioned-cxx";
    configure["cacheVariables"]=Json::object({{"CMAKE_GENERATOR_INSTANCE",location+",version="+version}});
    write(c.root/L"CMakePresets.json",presets.dump());
    counts(execute(),2);
    counts(execute(),0);
    write(c.root/L"worker.cpp","int worker(){return 3;}\n");
    counts(execute(),1);
    const auto controlRoot=temporary("omitted-native-collector");
    const auto control=run({executable().wstring(),repo.wstring(),L"--without-logger"},controlRoot,60,{},true);
    require(control.code==1&&control.err.find("Real CMake/MSBuild did not publish configuration and logger observations")!=std::string::npos,"Omitted collector escaped the actual native observation assertion: "+control.out+control.err);
    std::cout<<"PASS actual leased CMake/MSBuild phases, two translation units, no-op build and one-unit increment\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){
    if(argc>2){
        const std::string mode=argv[2];
        if(mode=="--configuration-cache"){configuration_cache_tests(repo,false);return;}
        if(mode=="--configuration-header-control"){configuration_cache_tests(repo,true);return;}
        if(mode=="--resource-publication"||mode=="--resource-single-attempt"){resource_publication_tests(repo,mode=="--resource-single-attempt");return;}
    }
    test(repo,argc>2&&std::string(argv[2])=="--without-logger");
});}
