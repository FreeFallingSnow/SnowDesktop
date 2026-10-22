#include "../build_tool_test_support.h"
#include <algorithm>
#include <iostream>
using namespace build_test;
namespace {
Json command(Coordinator& c,const Env& env,std::vector<std::string> args,int code=0){
    return c.bridge->call(c.scripts/L"build_run.ps1",args,code,env);
}
void track(Coordinator& c,const std::string& id){
    until([&]{return fs::exists(c.stateRoot/L"runs"/wide(id+".launch.json"));},12);
    c.workers.push_back(Json::parse(read(c.stateRoot/L"runs"/wide(id+".launch.json"))));
}
std::string count(const fs::path& path){return fs::exists(path)?read(path):"";}
Json wait_run(Coordinator& c,const Env& env,const std::string& id,int code=0){return command(c,env,{"-Command","wait","-RunId",id,"-Seconds","30"},code);}
void scenario(const fs::path& repo,const std::string& control){
    Coordinator c(repo,"verification"+control,true);
    copy(repo,c.root,{"scripts/test_manager.ps1","scripts/test_output.ps1"});
    write(c.scripts/L"build_batch_tests.ps1","param([string]$Batch)\n& (Join-Path $PSScriptRoot 'test_manager.ps1') -Mode plan -PlanBatch $Batch;exit $LASTEXITCODE\n",true);
    if(control=="configure"){
        auto source=read(c.scripts/L"test_manager.ps1");replace(source,"Write-Host '=== Existing artifacts verified; configure, build and arrangement are omitted ==='","Invoke-Checked -FilePath 'cmake' -Arguments @('--preset','tests')");
        write(c.scripts/L"test_manager.ps1",source,true);
    }
    if(control=="duplicate"){
        auto source=read(c.scripts/L"build_run.ps1");replace(source,"$ledger.entries|Where-Object key -eq $verification.key","$ledger.entries|Where-Object {$false}");
        write(c.scripts/L"build_run.ps1",source,true);
    }
    fs::create_directories(c.root/L"fake-bin");for(const auto& name:{L"cmake.exe",L"ctest.exe"})fs::copy_file(standin(),c.root/L"fake-bin"/name);
    Env env={{L"PATH",(c.root/L"fake-bin").wstring()+L";"+environment(L"PATH")}};
    write(c.root/L".build/known-runtime.txt","original explicit runtime resource");
    auto begun=c.begin("owner");const auto batch=begun.at("batchId").str();
    c.plan("owner",batch,0,"selected","Alpha,Beta","tool","src/a.txt");
    const auto produced=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","owner","-Batch",batch,"-Revision","0","-WaitSeconds","60"},env,75);
    require(produced.code==0,"Baseline fixture failed: "+produced.out+produced.err);
    const auto original=read(c.stateRoot/wide(batch+".json"));
    write(c.root/L".build/learned-cost","changed scheduling history is not a changed assertion");
    require(c.result(batch).at("inputCheck").str()=="stable","Baseline did not bind stable inputs");
    c.call({"task-open","subscriber","-TaskName","A second requester"});
    const auto builds=count(c.root/L"build.count"),configures=count(c.root/L".build/configure.count"),executions=count(c.root/L".build/verification-exec.count");
    auto start=[&](const std::string& task,const Env& context){
        auto value=command(c,context,{"-Command","start","-Action","tests","-Mode","verify","-Task",task,"-PlanBatch",batch,"-Filter","Beta,Alpha","-Reason","Supplement existing candidate without a build"});
        track(c,value.at("id").str());return value;
    };
    fs::remove(c.root/L".build/verification-started");c.gate(".build/hold-verification");
    const auto first=start("owner",env);const auto id=first.at("id").str();
    until([&]{return fs::exists(c.root/L".build/verification-started");},20);
    if(control=="configure"){
        c.gate(".build/hold-verification",false);wait_run(c,env,id);
        require(builds==count(c.root/L"build.count")&&configures==count(c.root/L".build/configure.count"),"Verification repeated configure/build instead of executing the existing candidate");return;
    }
    const auto second=start("subscriber",env);
    require(second.at("id").str()==id&&second.at("joined").boolean,"Equivalent verification requests launched duplicate Runs");
    require(start("subscriber",env).at("id").str()==id,"Repeated subscription was not idempotent");
    auto view=command(c,env,{"-Command","status","-RunId",id});
    require(view.at("subscriptions").items.size()==2,"Subscriber provenance was duplicated or lost");
    // The real Run/test-manager/coordinator and Job remain in use. Only cmake
    // and ctest are external stand-ins, with observable execution counters.
    Child observer({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"wait",L"-Task",L"subscriber",L"-RunId",wide(id),L"-Seconds",L"60"},c.root,env);
    require(observer.running(),"Observer did not start");observer.stop();
    require(owner_alive(view.at("record").at("owner")),"Closing an observer stopped the verification owner");
    c.call({"event-post","owner","-Target","subscriber","-Importance","urgent","-Text","Review a changed requirement while waiting","-EventKey","verification-observer"});
    const auto urgent=command(c,env,{"-Command","wait","-Task","subscriber","-RunId",id,"-Seconds","1"},6);
    require(urgent.at("runPreserved").boolean&&urgent.at("events").items.size()==1,"Subscriber wait did not receive its own urgent event");
    c.call({"event-ack","subscriber","-Sequence",std::to_string(urgent.at("events").items[0].at("sequence").integer()),"-AckLevel","agent"});
    command(c,env,{"-Command","cancel","-Task","subscriber","-RunId",id},2);
    command(c,env,{"-Command","cancel","-Task","owner","-RunId",id},2);
    command(c,env,{"-Command","unsubscribe","-Task","subscriber","-RunId",id});
    require(!fs::exists(c.stateRoot/L"runs"/wide(id+".cancel")),"Subscription withdrawal cancelled a shared Run");
    require(start("subscriber",env).at("id").str()==id,"Resubscription started another executor");
    auto changedEnv=env;changedEnv[L"SNOWDESKTOP_VERIFY_CASE"]=L"different test behavior";
    const auto different=start("subscriber",changedEnv);const auto other=different.at("id").str();
    require(other!=id,"Changed test environment was merged into the original verification");
    command(c,env,{"-Command","cancel","-Task","subscriber","-RunId",other});wait_run(c,env,other,4);
    c.gate(".build/hold-verification",false);const auto done=wait_run(c,env,id);
    require(done.at("status").str()=="passed"&&done.at("record").at("evidence").at("tests").items.size()==2,"Standalone verification lost actual test evidence");
    require(builds==count(c.root/L"build.count")&&configures==count(c.root/L".build/configure.count")&&count(c.root/L".build/verification-exec.count")==executions+"execute\n","Verification repeated configure/build or executed duplicate tests");
    require(original==read(c.stateRoot/wide(batch+".json")),"Supplement overwrote the immutable baseline result");
    const auto projection=Json::parse(read(c.root/L".build/verification"/wide(id+".json")));
    require(projection.at("schemaVersion").integer()==2&&projection.at("noBuild").boolean&&!projection.at("reuseAllowed").boolean&&projection.at("runId").str()==id,"Verification projection lost its authority or implied unclosed dependency reuse");
    for(const auto& evidence:done.at("record").at("evidence").at("tests").items)require(evidence.at("requesters").items.size()==2&&evidence.at("bindingDigest").str().size()==64,"Shared result omitted requester/binding provenance");
    // A changed binary must fail admission, with no execution or fallback build.
    const auto binary=c.root/L".build/Release/tests/SnowDesktopAlphaTests.exe";const auto bytes=read(binary);
    write(binary,"changed assertion binary");
    command(c,env,{"-Command","start","-Task","owner","-Mode","verify","-PlanBatch",batch,"-Filter","Alpha","-Reason","Reject stale binary"},2);write(binary,bytes);
    write(c.root/L".build/known-runtime.txt","changed runtime resource");
    command(c,env,{"-Command","start","-Task","owner","-Mode","verify","-PlanBatch",batch,"-Filter","Alpha","-Reason","Reject stale explicit runtime file"},2);
    write(c.root/L".build/known-runtime.txt","original explicit runtime resource");
    // Queued intent is checked again after editing releases the output lease.
    auto editing=c.begin("editor");const auto editBatch=editing.at("batchId").str();
    const auto queued=start("owner",env);const auto queuedId=queued.at("id").str();
    write(c.root/L"src/a.txt","changed while verification queued\n");
    c.plan("editor",editBatch,0,"none","","tool","src/a.txt");
    const auto retired=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","editor","-Batch",editBatch,"-Revision","0","-WaitSeconds","60"},env,75);
    require(retired.code==0,"Editing fixture did not release the verification resource: "+retired.out+retired.err);
    const auto invalid=wait_run(c,env,queuedId,5);
    require(invalid.at("status").str()=="invalidated"&&count(c.root/L".build/verification-exec.count")==executions+"execute\n","Queued source change ran tests or retained a passing result");
    write(c.root/L"src/a.txt","original\n");
    fs::remove(c.root/L".build/verification-started");c.gate(".build/hold-verification");const auto during=start("owner",env);const auto duringId=during.at("id").str();
    until([&]{return fs::exists(c.root/L".build/verification-started");},20);write(binary,"changed during execution");c.gate(".build/hold-verification",false);
    require(wait_run(c,env,duringId,5).at("status").str()=="invalidated","Changed assertion binary retained a passing result");write(binary,bytes);
    for(const std::string mutation:{"configure","duplicate"}){
        const auto negative=run({executable().wstring(),repo.wstring(),wide("--verification-"+mutation)},c.root,65);
        const auto signal=mutation=="configure"?"Verification repeated configure/build instead of executing the existing candidate":"Equivalent verification requests launched duplicate Runs";
        require(negative.code==1&&negative.err.find(signal)!=std::string::npos,"Negative control did not reach the verification business assertion: "+negative.out+negative.err);
    }
    std::cout<<"PASS durable existing-artifact verification, exact active-intent dedup, independent observer/subscriber lifetime, environment separation, immutable evidence and queued/during-run invalidation; forced build and duplicate controls fail\n";
}
}
void verification_records_tests(const fs::path& repo,const std::string& control){scenario(repo,control);}
