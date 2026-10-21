#include "../build_tool_test_support.h"
#include <algorithm>
#include <cmath>
#include <iostream>
using namespace build_test;

namespace {
const Json& named(const Json& values,const std::string& name){
    const auto it=std::find_if(values.items.begin(),values.items.end(),[&](const Json& item){return item.at("test").str()==name;});
    require(it!=values.items.end(),"Missing per-test evidence: "+name);return *it;
}
int task_revision(Coordinator& c,const std::string& id){
    const auto tasks=c.call({"task-list"});
    for(const auto& task:tasks.at("tasks").items)if(task.at("id").str()==id)return task.at("revision").integer();
    throw std::runtime_error("Visible fixture task missing");
}
void claim_files(Coordinator& c,const std::string& task,const std::string& batch,int revision,const std::string& files){
    auto args=c.bound("claim",task,batch,revision);args.insert(args.end(),{"-Files",files});c.call(args);
}
int edit_revision(Coordinator& c,const std::string& task){
    const auto state=c.state();for(const auto& member:state.at("current").at("participants").items)if(member.at("id").str()==task)return member.at("editRevision").integer();
    throw std::runtime_error("Editing fixture member missing");
}
Json acceptance_start(Coordinator& c,const std::string& batch,int revision,const std::string& files=".build/Release/tests/ObservedRuntime.exe"){
    return c.call({"acceptance-start","a","-Batch",batch,"-Revision",std::to_string(revision),"-TaskRevision",std::to_string(task_revision(c,"a")),"-AcceptanceCriterion","Original sample produces one visible item","-Files",files}).at("acceptance");
}
Json acceptance_complete(Coordinator& c,const std::string& id,const std::string& outcome="passed",int code=0){
    return c.call({"acceptance-complete","a","-TaskRevision",std::to_string(task_revision(c,"a")),"-AcceptanceId",id,"-AcceptanceOutcome",outcome,"-Reporter","user","-Reference","fixture:user-observation","-Notes","Observed original sample; this fixture does not claim a real desktop result"},code);
}
}

void evidence_records_tests(const fs::path& repo,bool omitEvidence){
    Coordinator c(repo,omitEvidence?"former-evidence":"evidence",true);
    copy(repo,c.root,{"scripts/test_manager.ps1","scripts/test_output.ps1"});
    write(c.scripts/L"build_batch_tests.ps1","param([string]$Batch)\n& (Join-Path $PSScriptRoot 'test_manager.ps1') -Mode plan -PlanBatch $Batch;exit $LASTEXITCODE\n",true);
    if(omitEvidence){auto source=read(c.scripts/L"build_manager.ps1");replace(source,"Set-Field $result 'evidence' (New-BatchTestEvidence $repositoryRoot $result)","# Former result publication omitted per-test evidence");write(c.scripts/L"build_manager.ps1",source,true);}
    fs::create_directories(c.root/L"fake-bin");for(const auto& name:{L"cmake.exe",L"ctest.exe"})fs::copy_file(standin(),c.root/L"fake-bin"/name);
    Env env={{L"PATH",(c.root/L"fake-bin").wstring()+L";"+environment(L"PATH")}};
    write(c.root/L"behavior","failed");
    auto a=c.begin("a"),b=c.begin("b");auto batch=a.at("batchId").str();
    claim_files(c,"a",batch,0,"src/a.txt");claim_files(c,"b",batch,0,"src/b.txt");
    c.plan("a",batch,0,"selected","Alpha","tool","src/a.txt");c.plan("b",batch,0,"selected","Alpha,Beta","tool","src/b.txt");
    const auto waiting=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","a","-Batch",batch,"-Revision","0","-WaitSeconds","1"},env,15);
    require(waiting.code!=0&&c.state().at("current").at("phase").str()=="editing","First requester unexpectedly ran before its peer was ready");
    const auto executed=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","b","-Batch",batch,"-Revision","0","-WaitSeconds","60"},env,75);
    require(executed.code!=0,"Synthetic Beta failure became a successful batch");
    const auto failed=c.result(batch);const auto original=read(c.stateRoot/wide(batch+".json"));
    require(failed.at("outcome").str()=="failed"&&failed.has("evidence"),"Immutable result omitted per-test evidence");
    const auto& entries=failed.at("evidence").at("tests");
    require(entries.items.size()==2&&named(entries,"Alpha").at("status").str()=="passed"&&named(entries,"Beta").at("status").str()=="failed","Per-test evidence lost actual outcomes");
    require(std::abs(named(entries,"Alpha").at("seconds").number-0.1)<0.000001&&named(entries,"Alpha").at("attempts").items[0].at("exitCode").kind==Json::Kind::Null&&named(entries,"Alpha").at("attempts").items[0].at("runnerExitCode").integer()!=0,"Per-test report duration or runner/individual exit distinction was lost");
    require(named(entries,"Alpha").at("requesters").items.size()==2&&named(entries,"Beta").at("requesters").items.size()==1&&read(c.root/L"build.count")=="build\n","Merged requests repeated a target build or lost subscribers");
    for(const auto& entry:entries.items){
        require(entry.at("bindingDigest").str().size()==64&&entry.at("binding").at("assertionBinary").str().size()==64&&entry.at("binding").at("invocationDigest").str().size()==64&&entry.at("binding").at("environmentDigest").str().size()==64,"Evidence did not bind actual command/binary/environment identity");
        require(!entry.at("reuseAllowed").boolean&&entry.at("runId").str()==failed.at("runId").str(),"Unknown dependencies were promoted to reusable evidence");
    }
    const auto filtered=c.call({"evidence-list","a","-Batch",batch});
    require(filtered.at("tests").items.size()==1&&filtered.at("tests").items[0].at("test").str()=="Alpha","Evidence query lost task-specific requests");
    if(omitEvidence)return;
    auto repair=c.call({"repair","b","-Batch",batch,"-Revision","0","-Reason","Synthetic Beta dependency changed; preserve original evidence"});
    const auto child=repair.at("batchId").str();
    const auto reopenedA=c.begin("a");require(reopenedA.at("batchId").str()==child,"Supplement reopened a different candidate");
    const auto aRevision=edit_revision(c,"a"),bRevision=edit_revision(c,"b");
    claim_files(c,"b",child,bRevision,"src/b.txt");write(c.root/L"src/b.txt","corrected fixture input\n");
    c.plan("a",child,aRevision,"none","","tool","src/a.txt");c.plan("b",child,bRevision,"selected","Beta","tool","src/b.txt");
    write(c.root/L"behavior","pass");
    const auto waitingAgain=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","a","-Batch",child,"-Revision",std::to_string(aRevision),"-WaitSeconds","1"},env,15);
    require(waitingAgain.code!=0&&c.state().at("current").at("phase").str()=="editing","Supplement escaped the remaining editor");
    const auto supplemented=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","b","-Batch",child,"-Revision",std::to_string(bRevision),"-WaitSeconds","60"},env,75);
    require(supplemented.code==0,"Targeted evidence supplement failed: "+supplemented.out+supplemented.err);
    const auto fixed=c.result(child);const auto& latest=fixed.at("evidence").at("tests");
    require(fixed.at("repairOf").str()==batch&&latest.items.size()==1&&latest.items[0].at("test").str()=="Beta"&&latest.items[0].at("status").str()=="passed"&&original==read(c.stateRoot/wide(batch+".json")),"Supplement overwrote the original failure or pretended to rerun passed tests");
    require(latest.items[0].at("bindingDigest").str()!=named(entries,"Beta").at("bindingDigest").str(),"Changed candidate retained the old evidence identity");
    const auto control=run({executable().wstring(),repo.wstring(),L"--without-evidence"},c.root,80);
    require(control.code==1&&control.err.find("Immutable result omitted per-test evidence")!=std::string::npos,"Former result publication escaped the evidence business assertion: "+control.out+control.err);
    std::cout<<"PASS production candidate/test evidence, shared request provenance, failed attempt preservation and targeted supplement; former result control fails\n";
}

void acceptance_records_tests(const fs::path& repo,bool omitRevisionGuard){
    Coordinator c(repo,omitRevisionGuard?"former-acceptance":"acceptance");
    if(omitRevisionGuard){auto source=read(c.scripts/L"build_acceptance.ps1");replace(source,"if($entry.requirementsRevision -ne $Task.requirementsRevision)","if($false)");write(c.scripts/L"build_acceptance.ps1",source);}
    const auto runtime=c.root/L".build/Release/tests/ObservedRuntime.exe";
    write(runtime,"original fixture runtime");
    // Only the external build/test boundary is synthetic. The coordinator,
    // candidate result, file hashing and acceptance transitions are production.
    write(c.scripts/L"build_batch_tests.ps1",R"PS(param([string]$Batch)
& (Join-Path $PSScriptRoot 'fake.ps1') -Phase test
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
$artifact='.build/Release/tests/ObservedRuntime.exe'
$coverage=@{schemaVersion=1;batchId=$Batch;selected=@('Alpha');completed=@(@{name='Alpha';status='passed';seconds=0.1});testExecutablesBeforeRun=@(@{test='Alpha';executable=$artifact;sha256=(Get-FileHash (Join-Path (Split-Path $PSScriptRoot -Parent) $artifact)).Hash.ToLowerInvariant()})}
$coverage|ConvertTo-Json -Depth 10|Set-Content (Join-Path $PSScriptRoot ('../.build/collaboration/'+$Batch+'.coverage.json')) -Encoding UTF8
)PS",true);
    auto opened=c.begin("a");const auto batch=opened.at("batchId").str();claim_files(c,"a",batch,0,"src/a.txt");c.plan("a",batch,0,"selected","Alpha","infrastructure","src/a.txt");
    write(c.root/L"preflight-blocked","controlled owner");
    const auto blocked=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","a","-Batch",batch,"-Revision","0","-WaitSeconds","1"},{},15);
    require(blocked.code!=0&&c.state().at("current").at("planStatus").str()=="waiting-output-owner","Occupancy fixture did not reach the real waiting state");
    opened=c.begin("a");const auto candidateRevision=opened.at("editRevision").integer();
    c.plan("a",batch,candidateRevision,"selected","Alpha","tool","src/a.txt");c.gate("hold-test");
    auto executor=c.start({"ready-and-wait","a","-Batch",batch,"-Revision",std::to_string(candidateRevision),"-WaitSeconds","60"});
    until([&]{return fs::exists(c.root/L"test-started");},25);
    require(c.state().at("current").at("planStatus").str()=="executing","Active execution retained an obsolete output-owner blocker");
    c.gate("hold-test",false);const auto executed=executor->wait(30);require(executed.code==0,"Acceptance candidate failed: "+executed.out+executed.err);
    const auto resultBytes=read(c.stateRoot/wide(batch+".json"));
    write(c.root/L".build/Release/UnverifiedRuntime.exe","unassociated existing runtime");
    c.call({"acceptance-start","a","-Batch",batch,"-Revision",std::to_string(candidateRevision),"-TaskRevision",std::to_string(task_revision(c,"a")),"-AcceptanceCriterion","Do not accept an unrelated existing output","-Files",".build/Release/UnverifiedRuntime.exe"},2);
    auto first=acceptance_start(c,batch,candidateRevision);const auto id=first.at("id").str();
    c.call({"task-close","a","-TaskRevision",std::to_string(task_revision(c,"a")),"-TaskPhase","completed"},2);
    auto clockJump=c.state();clockJump["taskLedger"]["acceptances"].items[0]["startedUtc"]="2099-01-01T00:00:00Z";atomic(c.stateRoot/L"state.json",clockJump.dump());
    auto active=acceptance_complete(c,id).at("acceptance");
    require(active.at("status").str()=="passed"&&active.at("reporter").str()=="user"&&active.at("durationStatus").str()=="monotonic"&&std::isfinite(active.at("elapsedSeconds").number)&&active.at("elapsedSeconds").number>=0&&active.at("elapsedSeconds").number<60,"Acceptance lost its actual independent monotonic interval");
    acceptance_complete(c,id,"failed",2);
    auto artifact=acceptance_start(c,batch,candidateRevision);write(runtime,"changed runtime");acceptance_complete(c,artifact.at("id").str(),"passed",2);acceptance_complete(c,artifact.at("id").str(),"failed");write(runtime,"original fixture runtime");
    auto changed=acceptance_start(c,batch,candidateRevision);write(c.root/L"src/a.txt","changed source during acceptance\n");acceptance_complete(c,changed.at("id").str(),"passed",2);acceptance_complete(c,changed.at("id").str(),"failed");write(c.root/L"src/a.txt","original\n");
    auto reboot=acceptance_start(c,batch,candidateRevision);auto rebootState=c.state();
    for(auto& record:rebootState["taskLedger"]["acceptances"].items)if(record.at("id").str()==reboot.at("id").str())record["clock"]["bootUtc"]="2001-01-01T00:00:00Z";
    atomic(c.stateRoot/L"state.json",rebootState.dump());const auto noDuration=acceptance_complete(c,reboot.at("id").str()).at("acceptance");
    require(noDuration.at("status").str()=="passed"&&noDuration.at("durationStatus").str()=="unavailable"&&noDuration.at("elapsedSeconds").kind==Json::Kind::Null,"A different boot fabricated an acceptance interval");
    auto revision=acceptance_start(c,batch,candidateRevision);
    c.call({"task-stage","a","-TaskRevision",std::to_string(task_revision(c,"a")),"-TaskPhase","validating","-RequirementsRevision","1","-Progress","A new requirement supersedes the old criterion"});
    const auto rejected=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"acceptance-complete","a","-TaskRevision",std::to_string(task_revision(c,"a")),"-AcceptanceId",revision.at("id").str(),"-AcceptanceOutcome","passed","-Reporter","user","-Reference","fixture:user","-Notes","Old observation does not prove the new requirement"});
    require(rejected.code==2,"Changed requirements were accepted using an old candidate");
    if(omitRevisionGuard)return;
    acceptance_complete(c,revision.at("id").str(),"cancelled");
    const auto records=c.call({"acceptance-list","a","-Batch",batch});
    require(records.at("acceptances").items.size()==5&&c.state().at("current").kind==Json::Kind::Null&&resultBytes==read(c.stateRoot/wide(batch+".json")),"Manual acceptance changed a build result or granted editing authority");
    const auto control=run({executable().wstring(),repo.wstring(),L"--without-acceptance-revision"},c.root,65);
    require(control.code==1&&control.err.find("Changed requirements were accepted using an old candidate")!=std::string::npos,"Missing requirement guard escaped the acceptance business assertion: "+control.out+control.err);
    std::cout<<"PASS independent acceptance timing, task/requirement/artifact binding, immutable result separation and superseded-requirement control\n";
}
