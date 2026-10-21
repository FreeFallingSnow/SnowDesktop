#include "build_tool_test_support.h"
#include <algorithm>
#include <future>
#include <iostream>
#include <syncstream>
using namespace build_test;
namespace {
Json complete(Child& child,int code=0){auto result=child.wait(40);require(result.code==code,result.out+result.err);return result.out.empty()?Json{}:Json::parse(result.out);}
void equal(const Json& a,const Json& b){require(equivalent_results(a,b),"Waiters did not receive equivalent persisted results\n"+a.dump()+"\n"+b.dump());}
Json registration_semantics(Json value){
    for(auto& [name,item]:value.fields){
        if(name.ends_with("Utc")&&item.kind==Json::Kind::String){
            auto& text=item.text;
            if(text.size()>21&&text[10]=='T'&&text[19]=='.'&&text.back()=='Z'&&
                std::all_of(text.begin()+20,text.end()-1,[](char c){return c>='0'&&c<='9';})){
                // Date coercion may omit zero fractional digits. Retain every
                // nonzero digit and every other registration/execution field.
                while(text.size()>21&&text[text.size()-2]=='0')text.erase(text.size()-2,1);
                if(text.size()==21)text.erase(19,1);
            }
        }
        item=registration_semantics(std::move(item));
    }
    for(auto& item:value.items)item=registration_semantics(std::move(item));
    return value;
}
void run_presence(const fs::path& repo,bool legacy){
        Coordinator presence(repo,legacy?"presence-ps5":"presence-ps7");
        if(legacy)presence.bridge=std::make_unique<Bridge>(repo,presence.root,true);
        auto empty=presence.call({"task-list"});
        require(empty.at("tasks").items.empty()&&!fs::exists(presence.stateRoot/L"state.json"),"Read-only task list wrote state");
        auto opened=presence.call({"task-open","reader","-TaskName","Investigate deadlock","-Adapter","dsh"});
        require(opened.at("task").at("phase").str()=="investigating"&&!opened.at("editingAuthority").boolean&&presence.state().at("current").kind==Json::Kind::Null,"Investigation joined editing barrier");
        presence.call({"edit-verify","reader","-Files","src/a.txt"},2);
        auto batch=presence.begin("editor").at("batchId").str();
        auto state=presence.state();
        write(presence.scripts/L"clock.ps1", ". $PSScriptRoot/build_protocol.ps1\n$now=[DateTime]::UtcNow\n$invalid=$false;try{ConvertTo-UtcTimestamp ([DateTime]::SpecifyKind($now,[DateTimeKind]::Unspecified))|Out-Null}catch{$invalid=$true}\n[pscustomobject]@{utc=(Get-TimestampAge $now);local=(Get-TimestampAge $now.ToLocalTime());offset=(Get-TimestampAge ([DateTimeOffset]$now));text=(Get-TimestampAge $now.ToString('o'));invalid=$invalid}|ConvertTo-Json\n");
        const auto ages=presence.bridge->call(presence.scripts/L"clock.ps1",{});
        for(const auto& field:{"utc","local","offset","text"})require(ages.at(field).number>=0&&ages.at(field).number<5,"UTC type conversion introduced timezone offset");
        require(ages.at("invalid").boolean,"Ambiguous timestamp silently used local zone");
        auto status=presence.call({"status"});
        require(status.at("current").at("registrationAgesSeconds").items[0].at("ageSeconds").number<15,"JSON date coercion inflated registration age");
        auto claim=presence.bound("claim","editor",batch);claim.insert(claim.end(),{"-Files","src/a.txt"});presence.call(claim);
        auto verify=presence.bound("edit-verify","editor",batch);verify.insert(verify.end(),{"-Files","src/a.txt"});
        require(presence.call(verify).at("allowed").boolean,"Valid path grant rejected");verify.back()="src/not-owned.txt";presence.call(verify,2);
        presence.gate("hold-build");auto worker=presence.start(presence.bound("finish","editor",batch));
        until([&]{return fs::exists(presence.root/L"build-started");});
        presence.call({"task-open","reader-while-building"});
        require(presence.state().at("current").at("participants").items.size()==1,"Task open altered frozen participants");
        presence.gate("hold-build",false);complete(*worker);
        require(presence.calls()==2&&presence.state().at("taskLedger").at("tasks").items.size()==3,"Investigation delayed build or was lost at retirement");
        auto updated=presence.call({"task-stage","reader","-TaskRevision","0","-TaskPhase","blocked","-Progress","Located delay","-Blocker","Waiting for dependency"});
        require(updated.at("task").at("revision").integer()==1,"Task progress revision missing");
        presence.call({"task-stage","reader","-TaskRevision","0"},2);
        auto waiting=presence.begin("waiter").at("batchId").str();presence.begin("peer");
        auto waiter=presence.start(presence.bound("finish","waiter",waiting));
        until([&]{return presence.state().at("current").at("participants").items[0].at("state").str()=="finished";});
        HANDLE timestampLease=INVALID_HANDLE_VALUE;
        until([&]{timestampLease=CreateFileW((presence.stateRoot/L"state.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);return timestampLease!=INVALID_HANDLE_VALUE;},5);
        try{
            auto controlled=presence.state();controlled["current"]["participants"].items[0]["finishedUtc"]="2026-10-21T11:43:44.5345720Z";
            atomic(presence.stateRoot/L"state.json",controlled.dump());
        }catch(...){CloseHandle(timestampLease);throw;}
        CloseHandle(timestampLease);
        const auto beforeState=presence.state().at("current");const auto before=beforeState.dump();
        const std::vector<std::string> post={"event-post","reader","-Target","waiter","-EventKey","requirement-1","-EventKind","requirements","-Importance","urgent","-Text","Review changed requirement","-RequirementsRevision","2"};
        auto event=presence.call(post);require(event.at("delivery").str()=="persisted"&&event.at("receipts").items.empty()&&event.at("recipients").items.size()==1&&event.at("recipients").items[0].str()=="waiter","Persistence claimed delivery or singleton recipient lost array identity");
        require(presence.call(post).at("sequence").integer()==event.at("sequence").integer(),"Retried event was duplicated");
        require(complete(*waiter,6).at("registrationRetained").boolean,"Urgent wait did not retain registration");
        const auto afterState=presence.state().at("current");const auto after=afterState.dump();
        write(presence.stateRoot/L"urgent-before.json",before);write(presence.stateRoot/L"urgent-after.json",after);
        require(registration_semantics(afterState).dump()==registration_semantics(beforeState).dump(),"Urgent wait return changed registration or execution\nBefore: "+before+"\nAfter: "+after);
        auto changedTime=afterState;changedTime["participants"].items[0]["finishedUtc"]="2026-10-21T11:43:44.5345721Z";
        require(registration_semantics(changedTime).dump()!=registration_semantics(beforeState).dump(),"Registration comparison lost a real UTC instant change");
        presence.call({"event-ack","reader-while-building","-Sequence","1"},2);
        auto received=presence.call({"event-ack","waiter","-Sequence","1","-AckLevel","adapter"});
        require(received.at("receipts").items.size()==1&&received.at("receipts").items[0].at("level").str()=="adapter","Adapter receipt became agent acknowledgement");
        presence.call(presence.bound("wait","waiter",waiting),6);
        presence.call({"event-ack","waiter","-Sequence","1","-AckLevel","agent"});
        auto timed=presence.call({"event-wait","waiter","-AfterSequence","1","-WaitSeconds","1"});
        require(timed.at("timedOut").boolean&&timed.at("events").items.empty(),"Bounded idle wait returned false messages");
        auto messageWait=presence.start({"event-wait","waiter","-AfterSequence","1","-WaitSeconds","5"});
        auto broadcast=presence.call({"event-post","reader","-EventKey","broadcast-1","-Text","Project progress"});
        require(complete(*messageWait).at("events").items[0].at("sequence").integer()==broadcast.at("sequence").integer(),"Message wait failed to release metadata for broadcast writer");
        presence.call({"task-open","late-reader"});
        require(presence.call({"event-read","late-reader"}).at("events").items.empty(),"Broadcast silently changed recipient scope");
        presence.call({"task-close","waiter","-TaskRevision","1","-TaskPhase","completed"},2);
        auto shared=presence.finish("peer",waiting);require(shared.at("outcome").str()=="passed","Acknowledged event blocked resumed build");
        presence.call({"task-close","reader","-TaskRevision","1","-TaskPhase","completed"});
        require(!presence.call({"task-list"}).at("tasks").items[0].at("capabilities").at("idleWake").boolean,"CLI falsely advertised host wake");
}
void run_contracts(const fs::path& repo){
    {
        Coordinator unplanned(repo,"explicit-selection");
        const auto registration=unplanned.call({"begin","missing-plan"});
        const auto id=registration.at("batchId").str();
        unplanned.call(unplanned.bound("ready","missing-plan",id),2);
        unplanned.call(unplanned.bound("finish","missing-plan",id),2);
        require(unplanned.calls()==0&&unplanned.state().at("current").at("participants").items[0].at("state").str()=="editing","Missing plan started work or finalized editor");
        auto omitted=unplanned.bound("plan","missing-plan",id);
        omitted.insert(omitted.end(),{"-Scope","unknown","-Inputs","src/a.txt","-Reason","reviewed mapping"});
        unplanned.call(omitted,2);
        auto unjustified=unplanned.bound("plan","missing-plan",id);
        unjustified.insert(unjustified.end(),{"-Scope","unknown","-Suites","full"});
        unplanned.call(unjustified,2);
        require(unplanned.calls()==0,"Omitted selection or unjustified full started work");
        unplanned.plan("missing-plan",id,0,"selected","Alpha,Beta","infrastructure");
        auto state=unplanned.state();
        state["current"]["participants"].items[0]["testPlan"]["requiredFull"]=true;
        atomic(unplanned.stateRoot/L"state.json",state.dump());
        const auto result=unplanned.finish("missing-plan",id);
        require(result.at("testingPlan").at("mode").str()=="selected"&&result.at("testingPlan").at("tests").dump()==Json::array({"Alpha","Beta"}).dump(),"Scope or historical escalation overrode explicit related tests");
    }
    Coordinator c(repo,"collaboration");auto initial=c.call({"status"});require(initial.at("current").kind==Json::Kind::Null&&!fs::exists(c.stateRoot/L"state.json"),"status registered a batch");
    auto bid=c.begin("a").at("batchId").str();require(c.begin("b").at("batchId").str()==bid&&c.begin("a").at("batchId").str()==bid,"Overlapping/repeated begin split batch");
    auto scoped=c.plan("a",bid,0,"selected","Alpha","module","src/winui/presenter.cpp,src/core/desktop.cpp,scripts/build_protocol.ps1");require(!scoped.at("testPlan").at("requiredFull").boolean,"Module paths silently escalated coverage");auto empty=c.bound("plan","a",bid);empty.insert(empty.end(),{"-Scope","module","-Suites","selected"});c.call(empty,2);require(c.state().at("current").at("participants").items[0].at("testPlan").at("tests").dump()==Json::array({"Alpha"}).dump(),"Invalid plan replaced valid plan");
    for(const auto& risk:{"public","infrastructure","unknown"})require(!c.plan("a",bid,0,"selected","Alpha",risk).at("testPlan").at("requiredFull").boolean,"Scope silently escalated related tests to full coverage");require(c.plan("a",bid,0,"full","","unknown").at("testPlan").at("requiredFull").boolean,"Explicit justified full request was lost");c.finish("a",std::string(32,'0'),0,2);
    c.gate("hold-build");c.gate("hold-test");auto first=c.start(c.bound("finish","a",bid));until([&]{return c.state().at("current").at("participants").items[0].at("state").str()=="finished";});require(first->running()&&c.calls()==0,"First finisher built while peer edits");auto timeout=c.bound("finish","a",bid);timeout.insert(timeout.end(),{"-WaitSeconds","1"});c.call(timeout,2);require(c.state().at("current").at("participants").items.size()==2,"Timeout cleared peer registration");auto duplicate=c.start(c.bound("finish","a",bid));until([&]{return duplicate->err().find("Waiting")!=std::string::npos;});auto registration=c.state().at("current").at("participants").items[0].at("registeredUtc").str();auto reopened=c.begin("a");require(reopened.at("editRevision").integer()==1&&registration==c.state().at("current").at("participants").items[0].at("registeredUtc").str(),"Reopen changed membership/registration identity");complete(*first,2);complete(*duplicate,2);require(c.calls()==0&&c.begin("a").at("editRevision").integer()==1,"Superseded finish finalized reopened editor");c.plan("a",bid,1,"full","","unknown");auto args=c.bound("finish","a",bid,1);args.push_back("-ReloadShell");first=c.start(args);until([&]{return c.state().at("current").at("participants").items[0].at("state").str()=="finished";});require(c.calls()==0&&!fs::exists(c.root/L"preflight-arguments.txt"),"Reload request caused early process action");duplicate=c.start(c.bound("finish","a",bid,1));auto last=c.start(c.bound("finish","b",bid));until([&]{return fs::exists(c.root/L"build-started");});require(c.calls()==1&&read(c.root/L"preflight-arguments.txt")=="--reload-shell"&&read(c.root/L"build-arguments.txt").empty(),"Outer owner reload/single-build boundary broken");auto pending=c.start({"begin","c"});c.call({"begin","d","-WaitSeconds","1"},2);require(pending->running()&&c.state().at("current").at("participants").items.size()==2,"New begin entered frozen batch");c.gate("hold-build",false);until([&]{return fs::exists(c.root/L"test-started");});require(pending->running()&&c.calls()==2,"New editor entered during testing");c.gate("hold-test",false);auto resultA=complete(*first);equal(resultA,complete(*last));equal(resultA,complete(*duplicate));equal(resultA,c.finish("a",bid,1));require(resultA.at("inputCheck").str()=="stable"&&resultA.at("inputStart").at("digest").str()==resultA.at("inputEnd").at("digest").str(),"Result did not bind stable input identity");auto next=complete(*pending).at("batchId").str();require(next!=bid,"Queued begin reused frozen batch");equal(resultA,c.call({"status","-Batch",bid}));require(c.state().at("current").at("participants").items.size()==1,"Historical read disturbed next registration");c.plan("c",next,0,"full","","module","src,scripts,CMakePresets.json,CMakeUserPresets.json");c.finish("c",next);require(c.calls()==4&&read(c.root/L"build-arguments.txt").empty(),"Next batch inherited reload/pipeline");
    auto race=c.begin("race-a").at("batchId").str();c.begin("race-b");first=c.start(c.bound("finish","race-a",race));last=c.start(c.bound("finish","race-b",race));equal(complete(*first),complete(*last));require(c.calls()==6,"Simultaneous last finishes repeated pipeline");
    for(const std::string phase:{"build","test"}){auto before=c.calls();write(c.root/("fail-"+phase),"7");auto task="failure-"+phase;auto failedId=c.begin(task).at("batchId").str();auto failure=c.finish(task,failedId,0,7);require(failure.at("outcome").str()=="failed"&&failure.at("error").str().find("code 7")!=std::string::npos&&read(wide(failure.at("logPath").str())).find("controlled "+phase+" failure")!=std::string::npos,"Failure receipt/log lost original code");equal(failure,c.finish(task,failedId,0,7));require(c.calls()-before==(phase=="build"?1:2),"Failure repeated/skipped wrong pipeline step");c.gate("fail-"+phase,false);task="after-"+phase;c.finish(task,c.begin(task).at("batchId").str());}
    auto stale=c.begin("stale").at("batchId").str();c.begin("live");auto state=c.state();state["current"]["participants"].items[0]["registeredUtc"]="2000-01-01T00:00:00Z";atomic(c.stateRoot/L"state.json",state.dump());auto before=c.calls();last=c.start(c.bound("finish","live",stale));until([&]{return c.state().at("current").at("participants").items[1].at("state").str()=="finished";});require(c.calls()==before,"Registration age bypassed barrier");c.call({"recover","stale","-Batch",stale,"-Reason","inspected"},2);c.call({"recover","live","-Batch",stale,"-ConfirmStopped","-Reason","inspected"},2);c.call({"recover","stale","-Batch",stale,"-ConfirmStopped","-Reason","inspected"});auto staleResult=complete(*last);require(staleResult.at("participants").items[0].at("state").str()=="withdrawn"&&staleResult.at("participants").items[1].at("state").str()=="finished","Targeted withdrawal lost live registration");auto cancelled=c.begin("abandoned").at("batchId").str();c.call({"recover","abandoned","-Batch",cancelled,"-ConfirmStopped","-Reason","inspected"});require(c.call({"status","-Batch",cancelled}).at("outcome").str()=="cancelled","All-withdrawn batch became success");
    c.gate("build-started",false);c.gate("hold-build");auto crash=c.begin("crash").at("batchId").str();auto crashed=c.start(c.bound("finish","crash",crash));until([&]{return fs::exists(c.root/L"build-started");});auto ownedChild=Json::parse(read(c.root/L"child.json"));before=c.calls();c.call({"recover","-Batch",crash,"-ConfirmStopped","-Reason","inspected"},2);crashed->stop();until([&]{return !owner_alive(ownedChild);},5);require(c.call({"status"}).at("current").at("ownerState").str()=="exited","Orphan owner not identified");c.call({"begin","blocked","-WaitSeconds","1"},2);require(c.calls()==before,"Orphan automatically rebuilt");c.gate("hold-build",false);c.call({"recover","-Batch",crash,"-ConfirmStopped","-Reason","inspected"});auto interrupted=c.finish("crash",crash,0,4);require(interrupted.at("outcome").str()=="interrupted"&&interrupted.at("error").str()=="inspected","Explicit interruption lost result");auto after=c.begin("after-crash").at("batchId").str();c.call({"recover","nobody","-Batch",crash,"-ConfirmStopped","-Reason","inspected"});require(c.state().at("current").at("id").str()==after,"Old recovery cleared next batch");c.finish("after-crash",after);
    state=c.state();state["current"]=Json::object({{"id",bid},{"phase","building"},{"participants",resultA.at("participants")},{"createdUtc",resultA.at("createdUtc")},{"buildStartedUtc",resultA.at("buildStartedUtc")},{"logPath",resultA.at("logPath")},{"owner",Json::object({{"pid",static_cast<double>(GetCurrentProcessId())},{"startTicks","0"}})}});atomic(c.stateRoot/L"state.json",state.dump());before=c.calls();c.call({"recover","-Batch",bid,"-ConfirmStopped","-Reason","published"});require(c.state().at("current").kind==Json::Kind::Null&&c.calls()==before,"Published-result recovery reran pipeline");
    before=c.calls();c.gate("preflight-unknown");auto guarded=c.begin("output-owner").at("batchId").str();first=c.start(c.bound("finish","output-owner",guarded));auto observed=[&](const std::string& status){auto current=c.state().at("current");return current.has("preflight")&&current.at("preflight").at("status").str()==status;};until([&]{return observed("unknown");});require(first->running()&&c.calls()==before,"Unknown output owner allowed build");c.gate("preflight-blocked");c.gate("preflight-unknown",false);until([&]{return observed("blocked");});require(first->running()&&c.calls()==before,"Blocked output owner allowed build");c.gate("preflight-blocked",false);require(complete(*first).at("outcome").str()=="passed"&&c.calls()==before+2,"Clear ownership did not resume one pipeline");
    c.gate("preflight-throw");auto preflightId=c.begin("preflight-failure").at("batchId").str();args=c.bound("finish","preflight-failure",preflightId);args.push_back("-ReloadShell");before=c.calls();auto failure=c.call(args,3);auto checkpoint=Json::parse(read(c.stateRoot/wide(preflightId+".preflight.json")));require(c.calls()==before&&checkpoint.at("exitCode").integer()==3&&checkpoint.at("error").str().find("controlled owner preflight exception")!=std::string::npos&&read(wide(failure.at("logPath").str())).find("controlled owner preflight exception")!=std::string::npos,"Pre-Job exception lost evidence or ran native child");c.gate("preflight-throw",false);
    for(const std::string change:{"content","addition","deletion","rename","configuration","local-configuration"}){
        if(change=="deletion")c.git({L"add",L"src/added.h"});c.gate("build-started",false);c.gate("hold-build");auto task="input-"+change;auto changed=c.begin(task).at("batchId").str();first=c.start(c.bound("finish",task,changed));until([&]{return fs::exists(c.root/L"build-started");});
        if(change=="content")write(c.root/L"src/fixture.cpp","modified source");if(change=="addition")write(c.root/L"src/added.h","new input");if(change=="deletion")fs::remove(c.root/L"src/added.h");if(change=="rename")fs::rename(c.root/L"src/fixture.cpp",c.root/L"src/renamed.cpp");if(change=="configuration")write(c.root/L"CMakePresets.json","{\"version\":6}");if(change=="local-configuration")write(c.root/L"CMakeUserPresets.json","{\"version\":6}");c.gate("hold-build",false);auto invalid=complete(*first,5);require(invalid.at("outcome").str()=="invalidated"&&invalid.at("pipelineExitCode").integer()==0&&invalid.at("inputStart").at("digest").str()!=invalid.at("inputEnd").at("digest").str(),"Input change did not invalidate success: "+change);equal(invalid,c.finish(task,changed,0,5));
    }
    c.finish("a",bid,1,2);auto newA=c.begin("a").at("batchId").str();c.finish("a",bid,1,2);require(c.state().at("current").at("id").str()==newA,"Old passed result finished new editor");auto fresh=c.finish("a",newA);require(fresh.at("inputCheck").str()=="stable"&&fresh.at("inputStart").at("digest").str()!=resultA.at("inputStart").at("digest").str(),"New result reused old input identity");equal(resultA,c.call({"status","-Batch",bid}));
    std::cout<<"PASS coordinator overlap, race, failure, crash, preflight and six input invalidations\n";
}
void run_tests(const fs::path& repo,bool sequential){
    const auto measured=[&](const char* name,const auto& action){
        const auto started=std::chrono::steady_clock::now();
        const auto report=[&](const char* outcome){
            const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
            std::osyncstream(std::cout)<<"CASE "<<Json::object({{"name",name},{"mode",sequential?"sequential":"parallel"},{"outcome",outcome},{"elapsedSeconds",elapsed}}).dump()<<'\n';
        };
        try{action();report("passed");}catch(...){report("failed");throw;}
    };
    const auto ps7=[&]{measured("presence-ps7",[&]{run_presence(repo,false);});};
    const auto ps5=[&]{measured("presence-ps5",[&]{run_presence(repo,true);});};
    const auto contracts=[&]{measured("coordinator-contracts",[&]{run_contracts(repo);});};
    if(sequential){ps7();ps5();contracts();return;}
    // Each group owns its repository, metadata, processes and streams. Preserve
    // every assertion and await all cleanup even when one group fails.
    auto first=std::async(std::launch::async,ps7);
    auto second=std::async(std::launch::async,ps5);
    auto third=std::async(std::launch::async,contracts);
    std::exception_ptr failure;
    for(auto* group:{&first,&second,&third}){try{group->get();}catch(...){if(!failure)failure=std::current_exception();}}
    if(failure)std::rethrow_exception(failure);
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){run_tests(repo,argc>2&&std::string(argv[2])=="--sequential");});}
