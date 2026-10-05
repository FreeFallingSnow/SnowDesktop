#include "build_tool_test_support.h"
#include <iostream>
using namespace build_test;
namespace {
void test(const fs::path& repo) {
    auto root=temporary("plan-execution");
    copy(repo,root,{"scripts/build_job.cs","scripts/build_entry.ps1","scripts/test_manager.ps1","scripts/test_output.ps1","scripts/build_protocol.ps1"});
    auto source=read(root/L"scripts/test_manager.ps1");
    auto start=source.find("function Get-HostRuntimeLocks {");auto end=source.find("function Assert-HostRuntimeAvailable {",start);
    require(start!=std::string::npos&&end!=std::string::npos,"Host occupancy fixture hook missing");
    source.replace(start,end-start,"function Get-HostRuntimeLocks {if([IO.File]::Exists((Join-Path $repositoryRoot 'occupied-host'))){'controlled fixture host owner'}}\n\n");
    write(root/L"scripts/test_manager.ps1",source,true);
    write(root/L"scripts/arrange_build_output.ps1",R"PS(param($BuildOutput,[switch]$AllowMissingFirstPartyRuntime)
[IO.File]::AppendAllText((Join-Path $PSScriptRoot '../arrange.count'),"arrange`n")
exit 0
)PS",true);
    auto definitions=read(repo/L"CMakeLists.txt");
    for(const std::string name:{"SnowDesktopThemeWorkflowTests","SnowDesktopWidgetAuthorPreviewCliTests"}) {
        auto position=definitions.find("\n        "+name+"\n");require(position!=std::string::npos,"Host test declaration missing: "+name);
        require(definitions.substr(position,definitions.find(')',position)-position).find("host-runtime")!=std::string::npos,"Missing dynamic host-runtime contract: "+name);
    }
    fs::create_directories(root/L"fake-bin");for(const auto& name:{L"cmake.exe",L"ctest.exe"})fs::copy_file(standin(),root/L"fake-bin"/name);
    Env env={{L"PATH",(root/L"fake-bin").wstring()+L";"+environment(L"PATH")}};
    auto folder=root/L".build/collaboration";fs::create_directories(folder);std::string batch(32,'c');
    auto request=Json::object({{"suites",Json::array({"selected"})},{"tests",Json::array({"Alpha"})},{"requiredFull",false},{"reason","independent fixture"}});
    auto second=request;second["tests"]=Json::array({"Beta"});
    auto plan=Json::object({{"schemaVersion",1},{"batchId",batch},{"configuration","Release"},{"mode","selected"},{"suites",Json::array({"selected"})},{"tests",Json::array({"Alpha","Beta"})},{"tasks",Json::array({Json::object({{"participant","a"},{"requirement",request}}),Json::object({{"participant","b"},{"requirement",second}})})}});
    Bridge bridge(repo,root);
    auto invoke=[&](const std::string& mode,bool success) {
        write(root/L"behavior",mode);write(folder/wide(batch+".plan.json"),plan.dump());
        auto result=bridge.invoke(root/L"scripts/test_manager.ps1",{"-Mode","plan","-PlanBatch",batch},env,20);
        require((result.code==0)==success,"Test manager outcome: "+mode+" code="+std::to_string(result.code)+"\n"+result.out+"\n"+result.err);
        return Json::parse(read(folder/wide(batch+".coverage.json")));
    };
    auto coverage=invoke("pass",true);require(coverage.at("status").str()=="passed"&&coverage.at("tasks").items.size()==2,"Coverage tasks not passed");
    for(const auto& item:coverage.at("tasks").items)require(item.at("status").str()=="passed","Per-task pass missing");
    require(coverage.at("testExecutablesBeforeRun").items.size()==2,"Fingerprint inventory incomplete");for(const auto& item:coverage.at("testExecutablesBeforeRun").items)require(item.at("sha256").str().size()==64,"Fingerprint missing");
    coverage=invoke("failed",false);require(coverage.at("tasks").items[1].at("failed").items[0].str()=="Beta"&&coverage.at("tasks").items[0].at("status").str()=="passed","Per-task failure attribution incorrect");
    coverage=invoke("skipped",false);for(const auto& item:coverage.at("tasks").items)require(item.at("status").str()=="not-run","Skipped treated as pass");
    auto before=read(root/L"build.count");coverage=invoke("environment",false);
    require(coverage.at("status").str()=="environment-blocked"&&coverage.at("blocked").items[0].str()=="Beta"&&coverage.at("completed").items.empty()&&before==read(root/L"build.count"),"Required unavailable test did not block before compilation");
    plan["tests"]=Json::array({"Missing"});coverage=invoke("pass",false);require(coverage.at("status").str()=="not-run"&&coverage.at("selectionStatus").str()=="pending"&&coverage.at("selected").items.empty(),"Zero selection became pass");
    for(const std::string name:{"theme_workflow","widget_author_preview_cli"}) {
        plan["tests"]=Json::array({name});auto requirement=request;requirement["tests"]=Json::array({name});plan["tasks"]=Json::array({Json::object({{"participant","host"},{"requirement",requirement}})});
        for(const std::string mode:{"cold","legacy"}) {
            write(root/L"occupied-host","controlled occupancy");before=read(root/L"build.count");auto arranged=fs::exists(root/L"arrange.count")?read(root/L"arrange.count"):"";
            coverage=invoke(mode,false);require(coverage.at("error").str().find("Host runtime is in use")!=std::string::npos&&before==read(root/L"build.count"),"Occupied host was compiled or error lost");
            require(arranged==(fs::exists(root/L"arrange.count")?read(root/L"arrange.count"):""),"Occupied output was arranged");
            fs::remove(root/L"occupied-host");coverage=invoke(mode,true);require(coverage.at("selected").items[0].str()==name&&read(root/L"arrange.count")==arranged+"arrange\n","Host arrangement/selection failed");
            require(read(root/L"build-args.txt").find(name=="theme_workflow"?"SnowDesktopThemeWorkflowTests":"SnowDesktopWidgetAuthorPreviewCliTests")!=std::string::npos,"Dynamic host test target missing");
        }
    }
    plan["tests"]=Json::array({"Alpha"});plan["tasks"]=Json::array({Json::object({{"participant","no-host"},{"requirement",request}})});write(root/L"occupied-host","irrelevant host owner");auto arranged=read(root/L"arrange.count");invoke("cold",true);require(read(root/L"arrange.count")==arranged,"Nonhost test used host output");fs::remove(root/L"occupied-host");
    plan["mode"]="full";plan["suites"]=Json::array({"full"});plan["tests"]=Json::array({});request["suites"]=Json::array({"full"});request["tests"]=Json::array({});request["requiredFull"]=true;plan["tasks"]=Json::array({Json::object({{"participant","full"},{"requirement",request}})});
    coverage=invoke("pass",true);require(coverage.at("postChecks").at("outputIsolation").at("status").str()=="passed","Full output checkpoint missing");
    coverage=invoke("failed",false);require(coverage.at("postChecks").at("outputIsolation").at("status").str()=="not-run","Failed tests ran pass checkpoint");
    write(root/L".build/Release/escaped.dll","controlled output violation");coverage=invoke("pass",false);require(coverage.at("postChecks").at("outputIsolation").at("status").str()=="failed"&&coverage.at("tasks").items[0].at("status").str()=="failed","Output escape passed full suite");
    std::cout<<"PASS real test manager selection, fingerprints, task coverage, explicit unavailable/zero/skipped failure, cold/legacy host guards, arrangement and full output checkpoints"<<std::endl;
}
}
int main(int argc,char** argv) {return test_main(argc,argv,test);}
