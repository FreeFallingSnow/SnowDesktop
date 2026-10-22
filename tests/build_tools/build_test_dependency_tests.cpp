#include "../build_tool_test_support.h"
#include <algorithm>
#include <iostream>
using namespace build_test;
void source_report_tests(const fs::path& repo,bool disableReports,bool serializedReports=false);
namespace {
const Json& named(const Json& rows,const std::string& name){
    for(const auto& row:rows.items)if(row.at("test").str()==name)return row;
    throw std::runtime_error("Missing dependency row: "+name);
}
void checked(const std::vector<std::wstring>& args,const fs::path& root){
    const auto result=run(args,root,55);require(result.code==0,result.out+result.err);
}
void tests(const fs::path& repo,bool omitReads){
    const auto root=temporary("dependency graph");
    copy(repo,root,{"scripts/build_test_dependencies.ps1"});
    if(omitReads){auto text=read(root/L"scripts/build_test_dependencies.ps1");replace(text,"$node.files+=Add-TestDependencyFile $Context $inputPath","# Legacy project-only graph omits actual compiler/linker reads");replace(text,"$node.files+=Add-TestDependencyFile $Context ([string]$reportInput)","# Legacy graph omits compiler source-level dependencies");write(root/L"scripts/build_test_dependencies.ps1",text,true);}
    write(root/L"CMakeLists.txt",R"CM(cmake_minimum_required(VERSION 3.25)
project(DependencyFixture LANGUAGES CXX)
add_library(DependencyLeaf STATIC shared.cpp)
target_include_directories(DependencyLeaf PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(SnowDesktopDependencyATests main_a.cpp)
target_link_libraries(SnowDesktopDependencyATests PRIVATE DependencyLeaf)
add_executable(SnowDesktopDependencyBTests main_b.cpp)
foreach(target IN ITEMS DependencyLeaf SnowDesktopDependencyATests SnowDesktopDependencyBTests)
  target_compile_options(${target} PRIVATE /sourceDependencies "$(IntDir).")
endforeach()
enable_testing()
add_test(NAME Alpha COMMAND SnowDesktopDependencyATests "${CMAKE_CURRENT_SOURCE_DIR}/runtime.txt")
set_tests_properties(Alpha PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopDependencyATests>;${CMAKE_CURRENT_SOURCE_DIR}/runtime.txt")
add_test(NAME Beta COMMAND SnowDesktopDependencyBTests)
set_tests_properties(Beta PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopDependencyBTests>")
)CM");
    write(root/L"shared.cpp","#include \"include/indirect.hpp\"\nint answer(){return dependencyAnswer;}\n");
    write(root/L"include/indirect.hpp","constexpr int dependencyAnswer=7;\n");
    write(root/L"main_a.cpp","#include <fstream>\nint answer();int main(int n,char**v){if(n!=2)return 2;int x=0;std::ifstream(v[1])>>x;return x==answer()?0:3;}\n");
    write(root/L"main_b.cpp","int main(){return 0;}\n");write(root/L"runtime.txt","7\n");
    checked({L"cmake",L"-S",root.wstring(),L"-B",(root/L".build").wstring(),L"-A",L"x64"},root);
    checked({L"cmake",L"--build",(root/L".build").wstring(),L"--config",L"Release",L"--target",L"SnowDesktopDependencyATests",L"SnowDesktopDependencyBTests",L"--parallel",L"2"},root);
    checked({L"ctest",L"--test-dir",(root/L".build").wstring(),L"-C",L"Release",L"--output-on-failure"},root);
    auto inventory=run({L"ctest",L"--test-dir",(root/L".build").wstring(),L"-C",L"Release",L"--show-only=json-v1"},root);
    require(inventory.code==0,inventory.err);write(root/L"inventory.json",inventory.out);
    write(root/L"inspect.ps1",R"PS(param([string]$Changed)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_test_dependencies.ps1')
$tests=@((Get-Content (Join-Path $PSScriptRoot 'inventory.json') -Raw|ConvertFrom-Json).tests)
$manifest=Get-TestDependencyManifest -Root $PSScriptRoot -Tests $tests
$record=Save-TestDependencyManifest $PSScriptRoot $tests
$advice=Get-TestDependencyAdvice $manifest @($Changed)
$header=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'include/indirect.hpp')).ToLowerInvariant()
@{tests=$manifest.tests;advice=$advice;record=$record;indirectObserved=(@($manifest.files|Where-Object id -eq $header).Count -eq 1);
uniqueFiles=$manifest.files.Count;pathCount=@($manifest.files.id|Sort-Object -Unique).Count}|ConvertTo-Json -Depth 15
)PS",true);
    Bridge bridge(repo,root);
    auto probe=[&](const std::string& path){return bridge.call(root/L"inspect.ps1",{"-Changed",path});};
    const auto first=probe("include/indirect.hpp");
    require(first.at("indirectObserved").boolean,"Indirect header read through a transitive static library was absent from observed dependencies");
    const auto& alpha=named(first.at("tests"),"Alpha");const auto& beta=named(first.at("tests"),"Beta");
    require(alpha.at("compilerReadLogs").integer()>0&&alpha.at("linkerReadLogs").integer()>0&&alpha.at("sourceReports").integer()==2&&alpha.at("projects").items.size()>beta.at("projects").items.size(),"Actual compiler source reports/read logs or transitive project edge missing");
    require(alpha.at("status").str()=="observed"&&beta.at("status").str()=="observed","Complete fixture graph was not observed: "+first.dump());
    require(named(first.at("advice").at("tests"),"Alpha").at("recommendRun").boolean&&!named(first.at("advice").at("tests"),"Beta").at("recommendRun").boolean,"Indirect header impact incorrectly invalidated independent target");
    require(first.at("uniqueFiles").integer()==first.at("pathCount").integer(),"Shared dependency identity table repeated a path");
    for(const auto& row:first.at("tests").items)require(!row.at("reuseAllowed").boolean&&row.at("runtimeClosure").str()=="unclosed","Observed read graph falsely authorized completed-result reuse");
    require(first.at("record").at("uniqueFiles").integer()==first.at("uniqueFiles").integer()&&first.at("record").at("sha256").str().size()==64,"Dependency sidecar omitted shared identities or its hash");
    const auto stable=probe("include/indirect.hpp");require(named(stable.at("tests"),"Alpha").at("digest").str()==alpha.at("digest").str(),"Unchanged dependency graph identity was unstable");
    write(root/L"include/indirect.hpp","constexpr int dependencyAnswer=11;\n");
    const auto changed=probe("include/indirect.hpp");
    require(named(changed.at("tests"),"Alpha").at("digest").str()!=alpha.at("digest").str()&&named(changed.at("tests"),"Beta").at("digest").str()==beta.at("digest").str(),"Related header change did not isolate dependency identity");
    const auto runtime=probe("runtime.txt");require(named(runtime.at("advice").at("tests"),"Alpha").at("recommendRun").boolean&&!named(runtime.at("advice").at("tests"),"Beta").at("recommendRun").boolean,"Explicit runtime file impact lost");
    const auto unknown=probe("new-unmapped.cpp");for(const auto& row:unknown.at("advice").at("tests").items)require(row.at("recommendRun").boolean&&row.at("reason").str()=="unmapped-change","Unmapped input silently removed a test");
    std::vector<fs::path> reports;for(const auto& item:fs::recursive_directory_iterator(root/L".build/DependencyLeaf.dir/Release"))if(item.is_regular_file()&&item.path().filename().wstring().ends_with(L".cpp.json"))reports.push_back(item.path());
    require(reports.size()==1,"Fixture compiler source report missing or ambiguous");
    fs::rename(reports[0],reports[0].wstring()+L".saved");
    const auto noReport=probe("include/indirect.hpp");require(named(noReport.at("tests"),"Alpha").at("status").str()=="incomplete"&&named(noReport.at("advice").at("tests"),"Alpha").at("recommendRun").boolean,"Excluded temporary input became complete without its compiler source report");
    fs::rename(reports[0].wstring()+L".saved",reports[0]);
    // Removing actual read tracking must preserve the reason, not pretend that
    // source/project declarations alone closed the compiler's include graph.
    std::vector<fs::path> logs;for(const auto& item:fs::recursive_directory_iterator(root/L".build/DependencyLeaf.dir/Release"))if(item.is_regular_file()&&item.path().filename().wstring().find(L"CL.read.")!=std::wstring::npos)logs.push_back(item.path());
    require(!logs.empty(),"Compiler tracking fixture missing");for(const auto& path:logs)fs::rename(path,path.wstring()+L".saved");
    const auto missing=probe("include/indirect.hpp");require(named(missing.at("tests"),"Alpha").at("status").str()=="incomplete"&&named(missing.at("advice").at("tests"),"Alpha").at("recommendRun").boolean,"Missing compiler read logs became a complete dependency graph");
    for(const auto& path:logs)fs::rename(path.wstring()+L".saved",path);
    Bridge ps5(repo,root,true);const auto legacy=ps5.call(root/L"inspect.ps1",{"-Changed","include/indirect.hpp"});require(legacy.at("indirectObserved").boolean&&named(legacy.at("tests"),"Alpha").at("digest").str()==named(changed.at("tests"),"Alpha").at("digest").str(),"PowerShell5.1 dependency graph differs from PowerShell7");
    write(root/L"capture.ps1",". (Join-Path $PSScriptRoot 'scripts/build_test_dependencies.ps1');$tests=@((Get-Content (Join-Path $PSScriptRoot 'inventory.json') -Raw|ConvertFrom-Json).tests);Save-TestDependencyManifest $PSScriptRoot $tests|ConvertTo-Json -Depth 12\n",true);
    const auto project=root/L".build/DependencyLeaf.vcxproj";const auto projectBytes=read(project);write(project,"<invalid-project");
    const auto unavailable=bridge.call(root/L"capture.ps1",{});require(unavailable.at("status").str()=="unavailable"&&!unavailable.at("reuseAllowed").boolean,"Auxiliary dependency failure prevented fail-closed observation");write(project,projectBytes);
    std::cout<<"PASS real CMake/MSBuild static-library/include graph, actual compiler/linker reads, stable per-test identities, isolated impact, explicit runtime input, unmapped/missing-log fallback, shared sidecar and PS7/5.1; semantic/runtime closure remains unclosed\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){
    const std::string mode=argc>2?argv[2]:"";
    if(mode=="--source-reports"||mode=="--without-standard-reports"||mode=="--serialized-source-reports")source_report_tests(repo,mode=="--without-standard-reports",mode=="--serialized-source-reports");
    else tests(repo,mode=="--without-compiler-reads");
});}
