#include "../build_tool_test_support.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
using namespace build_test;
namespace {
void checked(const std::vector<std::wstring>& command,const fs::path& root){const auto result=run(command,root,70);require(result.code==0,result.out+result.err);}
struct Report{fs::path path;std::string source;};
std::vector<Report> reports(const fs::path& root,const std::wstring& config){
    std::vector<Report> result;
    for(const auto& entry:fs::recursive_directory_iterator(root/L".build")){
        const auto name=entry.path().filename().wstring();
        if(!entry.is_regular_file()||(!name.starts_with(L"sd-")&&!name.ends_with(L".cpp.json")&&!name.ends_with(L".cxx.json")&&!name.ends_with(L".c.json"))||!name.ends_with(L".json")||entry.path().wstring().find(L"\\"+config+L"\\")==std::wstring::npos)continue;
        auto text=read(entry.path());if(text.starts_with("\xef\xbb\xbf"))text.erase(0,3);
        const auto data=Json::parse(text);result.push_back({entry.path(),data.at("Data").at("Source").str()});
    }
    return result;
}
using Times=std::map<fs::path,fs::file_time_type>;
Times outputs(const fs::path& root,const std::wstring& config){
    Times result;
    for(const auto& entry:fs::recursive_directory_iterator(root/L".build"))if(entry.is_regular_file()&&entry.path().wstring().find(L"\\"+config+L"\\")!=std::wstring::npos){
        const auto extension=entry.path().extension();if(extension==L".obj"||extension==L".pch"||extension==L".json"||extension==L".exe"||extension==L".lib")result.emplace(entry.path(),entry.last_write_time());
    }
    return result;
}
}
void source_report_tests(const fs::path& repo,bool disableReports,bool serializedReports){
    // FileTracker intentionally filters Temp/AppData. Keep the earlier Temp
    // reporting regression; use ordinary build output for incremental checks.
    const auto git=run({L"git",L"rev-parse",L"--path-format=absolute",L"--git-common-dir"},repo);
    auto commonText=git.out;while(!commonText.empty()&&(commonText.back()=='\r'||commonText.back()=='\n'))commonText.pop_back();
    const auto common=fs::path(wide(commonText));
    require(git.code==0&&common.filename()==L".git","Cannot bind the repository's short isolated test output root");
    const auto root=temporary("sr space",common.parent_path()/L".build/Release/tests/sr");
    copy(repo,root,{"cmake/SnowDesktop.DependencyEvidence.cmake","scripts/build_test_dependencies.ps1"});
    if(serializedReports){auto text=read(root/L"cmake/SnowDesktop.DependencyEvidence.cmake");replace(text,"SourceDependenciesFile=$([System.IO.Path]::GetDirectoryName('%(ClCompile.ObjectFileName)'))","SourceDependenciesFile=%(ClCompile.ObjectFileName)%(ClCompile.Filename)%(ClCompile.Extension).json");write(root/L"cmake/SnowDesktop.DependencyEvidence.cmake",text);}
    write(root/L"CMakeLists.txt",R"CM(cmake_minimum_required(VERSION 3.24)
project(StandardSourceReports LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SnowDesktop.DependencyEvidence.cmake")
add_library(CPart STATIC cpart.c)
add_subdirectory(child)
add_executable(ReportHost main.view.cpp helper.cpp left/same.cpp right/same.cpp)
target_include_directories(ReportHost PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ReportHost PRIVATE CPart ChildPart)
target_precompile_headers(ReportHost PRIVATE pch.hpp)
target_compile_options(ReportHost PRIVATE /MP2)
set_property(SOURCE main.view.cpp PROPERTY VS_SETTINGS "SnowDesktopExistingMetadata=keep")
add_executable(ReportReuse reuse.cpp)
target_include_directories(ReportReuse PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_precompile_headers(ReportReuse REUSE_FROM ReportHost)
enable_testing()
add_test(NAME StandardReport COMMAND ReportHost 24)
set_tests_properties(StandardReport PROPERTIES REQUIRED_FILES "$<TARGET_FILE:ReportHost>")
add_test(NAME ReusedPchReport COMMAND ReportReuse)
set_tests_properties(ReusedPchReport PROPERTIES REQUIRED_FILES "$<TARGET_FILE:ReportReuse>")
)CM");
    write(root/L"child/CMakeLists.txt","add_library(ChildPart STATIC child.cpp)\n");write(root/L"child/child.cpp","int childValue(){return 9;}\n");
    write(root/L"cpart.c","int cValue(void){return 7;}\n");
    write(root/L"pch.hpp","#pragma once\n#include <cstdint>\n#include <cstdlib>\n");
    write(root/L"left/value.hpp","constexpr int leftValue=3;\n");write(root/L"right/value.hpp","constexpr int rightValue=5;\n");
    write(root/L"left/same.cpp","#include \"left/value.hpp\"\nint left(){return leftValue;}\n");write(root/L"right/same.cpp","#include \"right/value.hpp\"\nint right(){return rightValue;}\n");
    write(root/L"main.view.cpp","#include \"pch.hpp\"\nextern \"C\" int cValue(void);int left();int right();int childValue();int mainSupport();int main(int n,char**v){return n==2&&left()+right()+cValue()+childValue()+mainSupport()==std::atoi(v[1])?0:9;}\n");
    write(root/L"helper.cpp","int mainSupport(){return 0;}\n");
    write(root/L"reuse.cpp","#include \"pch.hpp\"\nint main(){return sizeof(std::uint32_t)==4?0:1;}\n");
    checked({L"cmake",L"-S",root.wstring(),L"-B",(root/L".build").wstring(),L"-A",L"x64",disableReports?L"-DSNOWDESKTOP_SOURCE_DEPENDENCIES=OFF":L"-DSNOWDESKTOP_SOURCE_DEPENDENCIES=ON"},root);
    auto build=[&](const std::wstring& config,bool multiTool=false){std::vector<std::wstring> args={L"cmake",L"--build",(root/L".build").wstring(),L"--config",config,L"--parallel",L"2"};if(multiTool)args.insert(args.end(),{L"--",L"/p:UseMultiToolTask=true",L"/p:MultiProcMaxCount=2"});checked(args,root);};
    const auto initial=run({L"cmake",L"--build",(root/L".build").wstring(),L"--config",L"Release",L"--parallel",L"2",L"--",L"/v:n"},root,70);require(initial.code==0,initial.out+initial.err);
    const auto releaseReports=reports(root,L"Release");std::set<std::string> sources;for(const auto& report:releaseReports)sources.insert(report.source);
    require(releaseReports.size()==8&&sources.size()==8,"Standard build omitted declared compiler source dependency reports (C/C++, same basename, nested target, PCH and PCH reuse)");
    bool batched=false;std::istringstream lines(initial.out);for(std::string line;std::getline(lines,line);)if(line.find("/c ")!=std::string::npos&&line.find("main.view.cpp")!=std::string::npos&&line.find("helper.cpp")!=std::string::npos)batched=true;
    require(batched,"Dependency reporting serialized previously batchable compilation units");
    int same=0,pch=0;for(const auto& report:releaseReports){const auto name=fs::path(wide(report.source)).filename();if(name==L"same.cpp")++same;if(name==L"cmake_pch.cxx")++pch;}
    require(same==2&&pch==1,"Parallel same-name sources overwrote reports or the generated PCH was omitted");
    require(read(root/L".build/ReportHost.vcxproj").find("<SnowDesktopExistingMetadata>keep</SnowDesktopExistingMetadata>")!=std::string::npos,"Dependency metadata replaced existing XAML/source metadata");
    checked({L"ctest",L"--test-dir",(root/L".build").wstring(),L"-C",L"Release",L"--output-on-failure"},root);
    const auto releaseTimes=outputs(root,L"Release");build(L"Release");require(releaseTimes==outputs(root,L"Release"),"No-op source reporting recompiled a source or rewrote dependency evidence");
    build(L"Debug",true);require(reports(root,L"Debug").size()==8&&releaseTimes==outputs(root,L"Release"),"Debug/MultiToolTask source reports overwrote Release outputs or lost a compile source");
    checked({L"ctest",L"--test-dir",(root/L".build").wstring(),L"-C",L"Debug",L"--output-on-failure"},root);
    // Ordinary compiler output tracking must recover a missing JSON, rather
    // than requiring a clean rebuild or silently presenting an old report.
    const auto lost=std::find_if(releaseReports.begin(),releaseReports.end(),[](const Report& value){return fs::path(wide(value.source)).parent_path().filename()==L"right";});
    require(lost!=releaseReports.end(),"Right-source report not observed");const auto beforeRecovery=outputs(root,L"Release");fs::rename(lost->path,lost->path.wstring()+L".saved");build(L"Release");require(fs::exists(lost->path),"Incremental build did not recover its missing compiler source report");
    const auto afterRecovery=outputs(root,L"Release");int recompiled=0;for(const auto& [path,time]:beforeRecovery)if(path.extension()==L".obj"&&afterRecovery.at(path)!=time)++recompiled;require(recompiled==1,"Missing report recovery recompiled unrelated translation units");
    const auto recoveredTimes=outputs(root,L"Release");build(L"Release");require(recoveredTimes==outputs(root,L"Release"),"Recovered source report kept forcing recompilation on the next no-op build");
    write(root/L"left/value.hpp","constexpr int leftValue=11;\n");build(L"Release");
    const auto fresh=run({(root/L".build/Release/ReportHost.exe").wstring(),L"32"},root);
    const auto old=run({(root/L".build/Release/ReportHost.exe").wstring(),L"24"},root);
    require(fresh.code==0&&old.code==9,"Indirect-header incremental build retained the old runtime behavior");
    write(root/L"observe.ps1",R"PS(param([string]$Config)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_test_dependencies.ps1')
$raw=& ctest --test-dir (Join-Path $PSScriptRoot '.build') -C $Config --show-only=json-v1
if($LASTEXITCODE -ne 0){throw 'Unable to observe actual fixture CTest declarations'}
$tests=@((($raw -join "`n")|ConvertFrom-Json).tests)
$m=Get-TestDependencyManifest -Root $PSScriptRoot -Tests $tests -Configuration $Config
@{tests=@($m.tests|Select-Object test,status,sourceReports,issues);reuseAllowed=$m.reuseAllowed}|ConvertTo-Json -Depth 12
)PS",true);
    Bridge bridge(repo,root);for(const auto& config:{"Release","Debug"}){
        const auto observed=bridge.call(root/L"observe.ps1",{"-Config",config});require(!observed.at("reuseAllowed").boolean,"Compiler reports alone authorized result reuse");
        for(const auto& row:observed.at("tests").items)require(row.at("status").str()=="observed"&&row.at("sourceReports").integer()>=6,"Standard reporter and evidence consumer disagree: "+observed.dump());
    }
    std::cout<<"PASS standard source report metadata, C/C++, parallel same-name units, preserved source metadata, PCH/reuse, nested projects, Release/Debug/MultiToolTask separation, no-op timestamps, missing-report recovery and observed indirect-header runtime change\n";
}
