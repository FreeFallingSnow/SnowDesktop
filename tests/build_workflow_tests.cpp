#include "build_tool_test_support.h"
#include <iostream>
#include <thread>
using namespace build_test;
namespace {
Json foreground(Coordinator& c,const std::string& task,const std::string& batch,int revision=0,int code=0){auto args=c.bound("ready-and-wait",task,batch,revision);args.insert(args.end(),{"-WaitSeconds","30"});return c.call(args,code);}
bool checked(Coordinator& c,size_t index,const std::string& status){auto state=c.state();auto& task=state.at("current").at("participants").items.at(index);return task.has("check")&&task.at("check").at("status").str()==status;}
void git_wait_tests(const fs::path& repo){
    Coordinator c(repo,"git-wait",true);
    const auto grant=c.begin("waiter");const auto batch=grant.at("batchId").str();
    c.plan("waiter",batch);
    struct Lock {
        HANDLE handle;
        explicit Lock(const fs::path& path):handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr)){
            require(handle!=INVALID_HANDLE_VALUE,"Controlled Git transaction lease unavailable");
        }
        void release(){if(handle!=INVALID_HANDLE_VALUE){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;}}
        ~Lock(){release();}
    } git(c.stateRoot/L"git.lock");
    const auto before=read(c.stateRoot/L"state.json");
    const auto start=[&]{
        std::vector<std::wstring> command={powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_manager.ps1").wstring()};
        auto args=c.bound("ready-and-wait","waiter",batch);args.insert(args.end(),{"-WaitSeconds","30"});
        for(const auto& arg:args)command.push_back(wide(arg));
        return std::make_unique<Child>(command,c.root,Env{},false,false,true);
    };
    auto first=start();
    until([&]{return !first->running()||first->err().find("Git transaction")!=std::string::npos;},10);
    require(first->running(),"Transient Git transaction ended the ready observer instead of retaining its process: "+first->err());
    require(read(c.stateRoot/L"state.json")==before&&c.calls()==0,"Waiting for Git changed the registration or started execution");
    // Posting requires the real state transaction. This would deadlock if the
    // observer slept while holding it; no model or automatic acknowledgement participates.
    const auto event=c.call({"event-post","waiter","-Target","waiter","-Importance","urgent","-EventKind","requirements","-EventKey","during-git","-Text","recheck before finishing"});
    const auto attention=first->wait(10);
    require(attention.code==6&&Json::parse(attention.out).at("registrationRetained").boolean,"Git wait failed to deliver urgent attention without abandoning registration");
    require(c.calls()==0&&!fs::exists(c.stateRoot/wide(batch+".json")),"Urgent Git wait crossed the execution barrier");
    c.call({"event-ack","waiter","-Sequence",std::to_string(event.at("sequence").integer()),"-AckLevel","agent"});
    auto shortArgs=c.bound("ready-and-wait","waiter",batch);shortArgs.insert(shortArgs.end(),{"-WaitSeconds","1"});
    const auto timeout=c.bridge->invoke(c.scripts/L"build_manager.ps1",shortArgs);
    require(timeout.code==2&&timeout.err.find("Wait timed out")!=std::string::npos&&timeout.err.find("Git transaction")!=std::string::npos,"Git wait lost its bounded deadline or actual blocking reason");
    require(c.state().at("current").at("participants").items[0].at("state").str()=="editing"&&c.calls()==0,"Timed-out Git wait mutated editing authority");
    auto resumed=start();until([&]{return !resumed->running()||resumed->err().find("Git transaction")!=std::string::npos;},10);
    require(resumed->running(),"Explicitly resumed observer did not retain the same edit revision");
    git.release();
    until([&]{
        const auto state=c.state();
        if(state.at("current").kind!=Json::Kind::Null&&state.at("current").has("runId")){
            const auto path=c.stateRoot/L"runs"/wide(state.at("current").at("runId").str()+".json");
            if(fs::exists(path)){const auto run=Json::parse(read(path));if(run.has("owner")&&run.at("owner").kind!=Json::Kind::Null)c.workers.push_back(run.at("owner"));}
        }
        return !resumed->running();
    },35);
    const auto completed=resumed->wait();const auto result=Json::parse(completed.out);
    require(completed.code==0&&result.at("outcome").str()=="passed"&&result.has("runId"),"Git release did not complete the original durable batch: "+completed.err);
    require(c.calls()==2&&c.state().at("current").kind==Json::Kind::Null,"Git contention duplicated execution or failed to retire the batch");
    std::cout<<"PASS Git contention retains observer, releases state, delivers urgent messages, respects deadline and executes one durable batch\n";
}
void input_identity_tests(const fs::path& repo){
    // steam_bridge consumes SDK_ROOT. A different SDK location must invalidate
    // candidate identity even when repository bytes and the legacy PATH agree.
    Coordinator c(repo,"input-context");
    write(c.root/L"identity.ps1",". $PSScriptRoot/scripts/build_inputs.ps1;Get-BuildInputIdentity $PSScriptRoot|ConvertTo-Json\n",true);
    const auto identity=[&](const std::wstring& sdk,const std::wstring& legacy){
        return c.bridge->call(c.root/L"identity.ps1",{},0,{{L"SNOWDESKTOP_STEAMWORKS_SDK_ROOT",sdk},{L"SNOWDESKTOP_STEAMWORKS_SDK_PATH",legacy}});
    };
    const auto first=identity(L"C:/sdk/first",L"C:/legacy"),second=identity(L"C:/sdk/second",L"C:/legacy");
    require(first.at("digest").str()!=second.at("digest").str(),"Actual Steamworks SDK_ROOT change reused the repository input identity");
    require(first.at("digest").str()==identity(L"C:/sdk/first",L"C:/legacy").at("digest").str(),"Unchanged actual SDK context did not retain its identity");
    require(first.at("digest").str()!=identity(L"C:/sdk/first",L"C:/other-legacy").at("digest").str(),"Legacy SDK_PATH binding was silently removed");
    require(first.at("digest").str()!=identity(L"",L"C:/legacy").at("digest").str(),"Removing actual SDK_ROOT retained its identity");
    require(first.at("algorithm").str()=="sha256-repository-inputs-v3","Changed environment input policy retained its old identity version");
    std::cout<<"PASS actual Steamworks SDK_ROOT invalidation, stable context, absent root and retained legacy binding; external file closure is not claimed\n";
}
void commit_lifecycle_tests(const fs::path& repo,const std::string& mode){
    for(const bool legacy:{false,true}){
        Coordinator c(repo,legacy?"checkpoint-ps5":"checkpoint-ps7");
        if(legacy)c.bridge=std::make_unique<Bridge>(repo,c.root,true);
        if(mode=="--legacy-directory-control"){
            auto script=read(c.scripts/L"build_ownership.ps1");
            replace(script,"if(Test-Path -LiteralPath (Join-Path $repositoryRoot $path))","if([IO.File]::Exists((Join-Path $repositoryRoot $path)))");
            write(c.scripts/L"build_ownership.ps1",script,true);
        }
        if(mode=="--legacy-close-control"){
            auto script=read(c.scripts/L"build_manager.ps1");
            replace(script,"if($TaskPhase -in 'completed','awaiting_input','cancelled')","if($false)");
            write(c.scripts/L"build_manager.ps1",script,true);
        }
        const auto revision=[&](const std::string& id){const auto state=c.state();for(const auto& task:state.at("taskLedger").at("tasks").items)if(task.at("id").str()==id)return std::to_string(task.at("revision").integer());throw std::runtime_error("Missing visible task");};
        const auto batch=c.begin("author").at("batchId").str();
        auto claim=c.bound("claim","author",batch);claim.insert(claim.end(),{"-Files","src/new widget"});c.call(claim);
        write(c.root/L"src/new widget/main.txt","first file\n");write(c.root/L"src/new widget/nested/second.txt","second file\n");
        write(c.root/L"src/peer.txt","peer staged bytes\n");c.git({L"add",L"src/peer.txt"});
        const auto peerIndex=c.git({L"ls-files",L"--stage",L"src/peer.txt"}).out;
        const auto taskRevision=revision("author");
        const auto denied=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"task-stage","author","-TaskRevision",taskRevision,"-TaskPhase","awaiting_input"});
        require(denied.code==2&&denied.err.find("Uncommitted task paths")!=std::string::npos,"Owned untracked files silently left in awaiting_input: "+denied.err);
        require(revision("author")==taskRevision,"Rejected source closure consumed task revision");
        const auto audit=c.call({"task-list"}).at("sourceChanges").at("changes");
        int sourceFiles=0,owned=0,unassigned=0;
        for(const auto& change:audit.items){
            if(!change.at("path").str().starts_with("src/"))continue; // Legacy controls modify a copied script.
            ++sourceFiles;
            if(change.at("task").kind==Json::Kind::Null)++unassigned;else if(change.at("task").str()=="author")++owned;
        }
        require(sourceFiles==3,"Source audit did not enumerate owned and unassigned new files");
        require(owned==2&&unassigned==1,"Source audit invented ownership for an unclaimed peer path");
        write(c.root/L"message.txt","fixture source checkpoint\n");
        auto commit=c.bound("commit","author",batch);commit.insert(commit.end(),{"-Files","src/new widget","-MessageFile",utf8((c.root/L"message.txt").wstring())});
        auto saved=c.call(commit);
        require(saved.at("otherIndexEntriesUnchanged").boolean&&peerIndex==c.git({L"ls-files",L"--stage",L"src/peer.txt"}).out,"Directory checkpoint changed peer staging");
        require(c.git({L"diff-tree",L"--no-commit-id",L"--name-only",L"-r",L"HEAD"}).out=="src/new widget/main.txt\nsrc/new widget/nested/second.txt\n","New directory checkpoint omitted nested files or included peer bytes");
        require(read(c.root/L"src/new widget/main.txt")=="first file\n","Checkpoint rewrote source bytes");
        write(c.root/L"src/new widget/main.txt","validated next attempt\n");fs::remove(c.root/L"src/new widget/nested/second.txt");write(c.root/L"src/new widget/third.txt","third file\n");
        c.plan("author",batch,0,"none","","tool","src/new widget");c.ready("author",batch);c.finish("author",batch);
        require(c.state().at("current").kind==Json::Kind::Null,"Empty fixture plan failed to retire registration");
        const auto resultBytes=read(c.stateRoot/wide(batch+".json"));const auto beforeCalls=c.calls();
        c.call({"task-close","author","-TaskRevision",taskRevision,"-TaskPhase","completed"},2);
        const auto head=c.git({L"rev-parse",L"HEAD"}).out;
        write(c.root/L"src/new widget/main.txt","changed after ready\n");c.call(commit,2);
        require(head==c.git({L"rev-parse",L"HEAD"}).out,"Changed frozen source was committed as validated input");
        write(c.root/L"src/new widget/main.txt","validated next attempt\n");saved=c.call(commit);
        require(saved.at("otherIndexEntriesUnchanged").boolean&&peerIndex==c.git({L"ls-files",L"--stage",L"src/peer.txt"}).out,"Retired checkpoint changed peer staging");
        require(resultBytes==read(c.stateRoot/wide(batch+".json"))&&c.calls()==beforeCalls&&c.state().at("current").kind==Json::Kind::Null,"Git checkpoint rewrote immutable result, reopened edits or repeated execution");
        require(c.git({L"ls-tree",L"-r",L"--name-only",L"HEAD",L"src/new widget"}).out=="src/new widget/main.txt\nsrc/new widget/third.txt\n","Retired checkpoint lost deletion/new-file changes");
        // A reviewed successor owns new edits; the old task can close without
        // taking credit for or committing that successor's work.
        const auto next=c.begin("successor").at("batchId").str();
        auto handoff=c.bound("claim","successor",next);handoff.insert(handoff.end(),{"-Files","src/new widget"});c.call(handoff);
        write(c.root/L"src/new widget/main.txt","successor bytes\n");
        c.call({"task-close","author","-TaskRevision",taskRevision,"-TaskPhase","completed"});
        write(c.root/L"src/new widget/main.txt","validated next attempt\n");c.call(commit,2);
        write(c.root/L"src/new widget/main.txt","successor bytes\n");
        c.call({"recover","successor","-Batch",next,"-ConfirmStopped","-Reason","fixture editor stopped"});
        const auto cancelled=c.call({"task-close","successor","-TaskRevision",revision("successor"),"-TaskPhase","cancelled"});
        require(cancelled.at("task").at("sourceChanges").at("pending").items.size()==1,"Cancellation discarded outstanding source responsibility");
        c.call({"task-open","reader"});c.call({"task-close","reader","-TaskRevision","0","-TaskPhase","completed"});
        require(peerIndex==c.git({L"ls-files",L"--stage",L"src/peer.txt"}).out,"Lifecycle closure altered unrelated staging");
        std::cout<<"PASS "<<(legacy?"PowerShell 5.1":"PowerShell 7")<<" new-directory checkpoint, dirty completion gate, retired Git-only save, immutable results, source freshness, handoff, cancellation and peer index preservation\n";
    }
}
void wait_progress_tests(const fs::path& repo){
    // Preserve the durable Run and private Job; only the external build/test
    // boundary is gated. Observers must report that actual execution phase.
    Coordinator c(repo,"wait-progress",true);
    auto fake=read(c.scripts/L"fake.ps1");
    replace(fake,"$timer=[Diagnostics.Stopwatch]::StartNew()",R"PS(. (Join-Path $PSScriptRoot 'build_entry.ps1')
. (Join-Path $PSScriptRoot 'build_telemetry.ps1')
$phaseName=if($Phase -eq 'test'){'test-execute'}else{'build-targets'}
$stage=Start-BuildStage $root $phaseName
$timer=[Diagnostics.Stopwatch]::StartNew())PS");
    write(c.scripts/L"fake.ps1",fake,true);
    c.gate("hold-build");c.gate("hold-test");
    const auto batch=c.begin("worker").at("batchId").str();
    std::vector<std::wstring> command={powershell().wstring(),L"-NoProfile",L"-NonInteractive",L"-File",(c.scripts/L"build_manager.ps1").wstring()};
    for(const auto& argument:c.bound("ready-and-wait","worker",batch))command.push_back(wide(argument));
    auto first=std::make_unique<Child>(command,c.root,Env{},false,false,true);
    until([&]{const bool started=fs::exists(c.root/L"build-started");require(started||first->running(),"Durable fixture exited before build: "+first->err());return started;},18);
    const auto state=c.state();const auto id=state.at("current").at("runId").str();
    const auto ownerBefore=Json::parse(read(c.stateRoot/L"runs"/wide(id+".launch.json")));
    c.workers.push_back(ownerBefore);
    auto observe=[&](const std::string& stage){
        auto args=c.bound("wait","worker",batch);args.insert(args.end(),{"-WaitSeconds","30"});auto p=c.start(args);
        until([&]{const auto text=p->err();return text.find("Waiting for this batch")!=std::string::npos||text.find("Waiting for batch ")!=std::string::npos;},8);
        const auto text=p->err();
        require(text.find("executing Run "+id)!=std::string::npos&&text.find("editors must finish")==std::string::npos,"Executing durable Run was reported as waiting for editors: "+text);
        require(text.find(stage)!=std::string::npos,"Wait progress omitted the observed execution stage: "+text);
        p->stop();
    };
    observe("build-targets");c.gate("hold-build",false);
    until([&]{return fs::exists(c.root/L"test-started");},10);
    observe("test-execute");
    require(first->running()&&owner_alive(ownerBefore)&&c.calls()==2,"Observing progress changed Run ownership or repeated the pipeline");
    c.gate("hold-test",false);const auto result=first->wait(18);
    require(result.code==0&&c.result(batch).at("runId").str()==id&&c.result(batch).at("outcome").str()=="passed","Observed Run did not retain its common successful result");
    // Adjacent lifecycle states are controlled projection inputs, not evidence
    // of resource admission. Exercise the real wait entry without a new build.
    const auto projection=c.begin("projection").at("batchId").str();
    const std::string projectedRun(32,'d'),nonce(32,'e');
    auto model=c.state();model["current"]["phase"]="building";model["current"]["owner"]=owner(GetCurrentProcessId());model["current"]["runId"]=projectedRun;
    atomic(c.stateRoot/L"state.json",model.dump());
    const auto request=Json::object({{"id",projectedRun},{"nonce",nonce},{"repositoryRoot",utf8(c.root.wstring())},{"batch",projection}});
    atomic(c.stateRoot/L"runs"/wide(projectedRun+".request.json"),request.dump());
    auto projected=Json::object({{"id",projectedRun},{"nonce",nonce},{"repositoryRoot",utf8(c.root.wstring())},{"status","queued"},{"waitingFor","shared-output"}});
    const auto show=[&](const std::string& status,const std::string& expected){
        projected["status"]=status;atomic(c.stateRoot/L"runs"/wide(projectedRun+".json"),projected.dump());
        auto p=c.start(c.bound("wait","projection",projection));until([&]{return p->err().find("Waiting for batch ")!=std::string::npos;},8);
        const auto text=p->err();p->stop();require(text.find(expected)!=std::string::npos,"Wait projection misreported "+status+": "+text);
        if(status!="running")require(text.find("executing Run")==std::string::npos,"Non-running lifecycle state was reported as execution: "+text);
    };
    show("queued","queued; waiting for shared-output");show("starting","starting");show("passed","passed; common result pending");
    show("running","stage unavailable (no observation yet)");
    write(c.stateRoot/L"runs"/wide(projectedRun+".stages.control.jsonl"),"invalid observation\n");
    show("running","stage unavailable (observation failed)");
    projected["nonce"]=std::string(32,'f');show("queued","state unavailable (observation failed)");
    require(c.calls()==2,"Lifecycle projection observations started another pipeline");model["current"]=Json{};atomic(c.stateRoot/L"state.json",model.dump());
    std::cout<<"PASS real durable build/test wait messages, stage provenance, observer exit, unchanged owner and one pipeline\n";
}
void run_tests(const fs::path& repo){
    entry_tests(repo);shell_recovery_tests(repo);
    Coordinator c(repo,"workflow");
    auto a=c.begin("a"),b=c.begin("b");auto bid=a.at("batchId").str();require(b.at("batchId").str()==bid,"Overlapping editors split batches");
    c.plan("a",bid);c.plan("b",bid,0,"none","","docs","src/b.txt");
    auto started=std::chrono::steady_clock::now();c.ready("a",bid);require(std::chrono::steady_clock::now()-started<std::chrono::seconds(5),"ready blocked on peer");
    until([&]{return checked(c,0,"passed");});require(c.calls()==0,"Editing peer did not prevent build");
    require(c.begin("a").at("editRevision").integer()==1,"Reopen revision missing");c.finish("a",bid,0,2);c.call({"finish","a","-Batch",bid},2);c.ready("a",bid,1,2);c.plan("a",bid,1,"selected","Beta");
    auto args=c.bound("ready-and-wait","a",bid,1);args.insert(args.end(),{"-WaitSeconds","30"});auto active=c.start(args);auto pid=active->pid();
    until([&]{return checked(c,0,"passed");});std::this_thread::sleep_for(std::chrono::milliseconds(200));require(active->running()&&active->pid()==pid&&c.calls()==0,"Foreground waiter did not retain its process/barrier");
    c.ready("b",bid);auto done=active->wait(30);require(done.code==0,done.err);auto result=c.result(bid);require(equivalent_results(Json::parse(done.out),result)&&result.at("outcome").str()=="passed"&&c.calls()==2,"One pipeline/common result broken");
    require(result.at("testingPlan").at("tests").dump()==Json::array({"Beta"}).dump(),"Frozen selection lost");require(equivalent_results(c.call(c.bound("wait","a",bid,1)),c.call(c.bound("wait","b",bid))),"Waiters saw different results");
    write(c.root/L"src/a.txt","changed input\n");write(c.root/L"unit.ps1",". $PSScriptRoot/scripts/build_inputs.ps1;Get-BuildInputIdentity $PSScriptRoot|ConvertTo-Json\n",true);
    auto identity=[&]{return c.bridge->call(c.root/L"unit.ps1",{}).at("digest").str();};auto before=identity();c.git({L"add",L"src/a.txt"});c.git({L"commit",L"--quiet",L"-m",L"content already hashed"});require(before==identity(),"HEAD-only commit changed content identity");
    auto commitBatch=c.begin("commit-a").at("batchId").str();auto claim=c.bound("claim","commit-a",commitBatch);claim.insert(claim.end(),{"-Files","src/a.txt"});c.call(claim);c.begin("commit-b");claim[1]="commit-b";c.call(claim,2);
    write(c.root/L"src/a.txt","owned commit\n");write(c.root/L"src/other.txt","other staged\n");c.git({L"add",L"src/other.txt"});auto index=c.git({L"ls-files",L"--stage",L"src/other.txt"}).out;write(c.root/L"message.txt","fixture owned commit\n");auto commit=c.bound("commit","commit-a",commitBatch);commit.insert(commit.end(),{"-Files","src/a.txt","-MessageFile",utf8((c.root/L"message.txt").wstring())});auto receipt=c.call(commit);require(receipt.at("otherIndexEntriesUnchanged").boolean&&index==c.git({L"ls-files",L"--stage",L"src/other.txt"}).out,"Owned commit changed peer index bytes");require(c.git({L"diff-tree",L"--no-commit-id",L"--name-only",L"-r",L"HEAD"}).out=="src/a.txt\n","Owned commit included another path");
    c.call({"recover","commit-a","-Batch",commitBatch,"-ConfirmStopped","-Reason","fixture editors stopped"});c.call({"recover","commit-b","-Batch",commitBatch,"-ConfirmStopped","-Reason","fixture editors stopped"});
    auto original=read(c.scripts/L"build_manager.ps1"),denied=original;replace(denied,"$worker=Start-BuildPowerShellScript -Script $waiterScript -Arguments $arguments -WorkingDirectory $repositoryRoot -OutputPath ($output+'.out') -ErrorPath ($output+'.err')","throw 'fixture hidden launch denied'");write(c.scripts/L"build_manager.ps1",denied,true);
    auto noHidden=c.begin("no-hidden").at("batchId").str();c.plan("no-hidden",noHidden);auto unavailable=c.ready("no-hidden",noHidden,0,2);require(unavailable.at("waiter").at("status").str()=="unavailable"&&checked(c,0,"pending"),"Hidden launch failure crossed barrier");require(foreground(c,"no-hidden",noHidden).at("outcome").str()=="passed","Foreground takeover failed");write(c.scripts/L"build_manager.ps1",original,true);
    auto local=c.begin("repair-local").at("batchId").str();c.begin("repair-peer");c.plan("repair-local",local);c.plan("repair-peer",local,0,"selected","ManualPreview","module","src/b.txt");write(c.root/L"fail-test","17");c.ready("repair-peer",local);auto failed=foreground(c,"repair-local",local,0,17);require(failed.at("exitCode").integer()==17,"Original exit lost");auto failureBytes=read(c.stateRoot/wide(local+".json"));fs::remove(c.root/L"fail-test");auto repair=c.bound("repair","repair-local",local);repair.insert(repair.end(),{"-Reason","reviewed local failure"});auto child=c.call(repair);auto childId=child.at("batchId").str();auto revision=child.at("editRevision").integer();c.plan("repair-local",childId,revision,"selected","Beta");auto repaired=foreground(c,"repair-local",childId,revision);require(repaired.at("testingPlan").at("tests").dump()==Json::array({"Beta","ManualPreview"}).dump()&&repaired.at("attempt").integer()==2&&repaired.at("logicalBatchId").str()==local&&repaired.at("repairOf").str()==local,"Repair lost peer requirements/logical attempt");for(const auto& task:repaired.at("participants").items)require(task.at("check").at("status").str()=="passed","Repair reused stale peer check");require(failureBytes==read(c.stateRoot/wide(local+".json")),"Repair overwrote first failure");
    auto stale=c.begin("stale").at("batchId").str();c.plan("stale",stale);c.begin("peer");auto state=c.state();auto& task=state["current"]["participants"].items[0];task["state"]="finished";task["check"]=Json::object({{"status","running"},{"owner",Json::object({{"pid",2147483647},{"startTicks","0"}})},{"editRevision",0}});task["registeredUtc"]="2000-01-01T00:00:00Z";atomic(c.stateRoot/L"state.json",state.dump());auto shortWait=c.bound("finish","peer",stale);shortWait.insert(shortWait.end(),{"-WaitSeconds","1"});c.call(shortWait,2);require(c.state().at("current").at("phase").str()=="editing","Stale/crashed check enabled freeze");c.call(c.bound("check","stale",stale));c.finish("peer",stale);
    // Return observations from the real protocol; C++ owns every assertion.
    write(c.root/L"unit.ps1",R"PS(. $PSScriptRoot/scripts/build_protocol.ps1
$e=[pscustomobject]@{editRevision=0};$errors=@()
foreach($name in @('Alpha;whoami','Alpha|Beta','$(whoami)')){try{New-TaskPlan $e module selected $name src/a.txt mapping|Out-Null;$errors+= $false}catch{$errors+= $true}}
$inv=@([pscustomobject]@{name='Alpha';properties=@([pscustomobject]@{name='LABELS';value=@('core')})},[pscustomobject]@{name='Beta';properties=@([pscustomobject]@{name='LABELS';value=@('integration')})},[pscustomobject]@{name='Manual';properties=@([pscustomobject]@{name='LABELS';value=@('manual')})})
$p=[pscustomobject]@{tests=@('Manual','Alpha');suites=@('full');mode='full';tasks=@()};$names=@(Resolve-PlanTests $p $inv);$p.tests=@('Missing');$missing=$false
try{Resolve-PlanTests $p $inv|Out-Null}catch{$missing=$true}
@{rejected=$errors;names=$names;missing=$missing}|ConvertTo-Json -Depth 8
)PS",true);auto literal=c.bridge->call(c.root/L"unit.ps1",{});for(const auto& rejected:literal.at("rejected").items)require(rejected.boolean,"Literal plan accepted shell syntax");require(literal.at("names").dump()==Json::array({"Alpha","Beta","Manual"}).dump()&&literal.at("missing").boolean,"Plan resolution accepted unknown test/lost manual union");
    auto legacy=c.begin("legacy").at("batchId").str();state=c.state();state["current"].fields.erase("protocolVersion");atomic(c.stateRoot/L"state.json",state.dump());c.ready("legacy",legacy,0,2);c.call(c.bound("plan","legacy",legacy),2);require(c.state().at("current").at("participants").items[0].at("state").str()=="editing","Legacy caller mutated registration");c.call({"finish","legacy","-Batch",legacy});
    write(c.root/L"src/a.txt","trailing whitespace   \n");auto checkBatch=c.begin("check-failure").at("batchId").str();c.begin("checking-peer");c.plan("check-failure",checkBatch);auto callCount=c.calls();c.ready("check-failure",checkBatch);until([&]{return checked(c,0,"failed");});require(c.calls()==callCount&&c.state().at("current").at("phase").str()=="editing","Failed check authorized build");require(c.begin("check-failure").at("editRevision").integer()==1,"Failed check not reopened");write(c.root/L"src/a.txt","repaired input\n");c.plan("check-failure",checkBatch,1);c.ready("check-failure",checkBatch,1);c.finish("checking-peer",checkBatch);require(c.result(checkBatch).at("outcome").str()=="passed","Repaired check did not complete");auto issue=c.call({"issue","check-failure","-Batch",checkBatch,"-Reason","explicit fixture handoff","-Assignee","checking-peer"});c.call({"issue","checking-peer","-Batch",checkBatch,"-IssueId",issue.at("id").str(),"-IssueState","resolved","-Reason","reviewed fixture resolution","-Assignee","checking-peer"});
    auto fresh=c.begin("freshness").at("batchId").str();c.begin("freshness-peer");c.plan("freshness",fresh);c.ready("freshness",fresh);until([&]{return checked(c,0,"passed");});write(c.root/L"src/a.txt","unexpected change after ready\n");c.finish("freshness-peer",fresh,0,2);require(c.state().at("current").at("planStatus").str()=="blocked","Changed declared input did not block freeze");revision=c.begin("freshness").at("editRevision").integer();c.plan("freshness",fresh,revision);c.ready("freshness",fresh,revision);c.finish("freshness-peer",fresh);
    std::cout<<"PASS barriers, revisions, foreground recovery, ownership, repair, literal plans, legacy and input freshness\n";
}
}
int main(int argc,char** argv){
    const std::string mode=argc>2?argv[2]:"";
    if(mode=="--commit-lifecycle"||mode=="--legacy-directory-control"||mode=="--legacy-close-control")return test_main(argc,argv,[&](const fs::path& repo){commit_lifecycle_tests(repo,mode);});
    return test_main(argc,argv,mode=="--input-identity"?input_identity_tests:mode=="--wait-progress"?wait_progress_tests:mode=="--git-wait"?git_wait_tests:run_tests);
}
