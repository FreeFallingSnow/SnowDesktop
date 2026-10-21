#include "build_tool_test_support.h"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace build_test;
namespace {
class ParentJob {
    HANDLE job_=nullptr,process_=nullptr;
public:
    ParentJob(const fs::path& script,const fs::path& root,bool allow,const std::string& label="native-launcher") {
        HANDLE input=INVALID_HANDLE_VALUE,output=INVALID_HANDLE_VALUE,error=INVALID_HANDLE_VALUE;
        LPPROC_THREAD_ATTRIBUTE_LIST attributes=nullptr;bool initialized=false;PROCESS_INFORMATION info{};
        const auto cleanup=[&]{
            for(HANDLE handle:{input,output,error,info.hThread})if(handle&&handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
            if(initialized)DeleteProcThreadAttributeList(attributes);
        };
        std::vector<unsigned char> storage;
        try {
            job_=CreateJobObjectW(nullptr,nullptr);require(job_!=nullptr,"Parent job creation failed");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|(allow?JOB_OBJECT_LIMIT_BREAKAWAY_OK:0);
            require(SetInformationJobObject(job_,JobObjectExtendedLimitInformation,&limits,sizeof(limits))!=FALSE,"Parent job limits failed");
            auto engine=powershell();std::wstring command=quote(engine.wstring())+L" -NoProfile -NonInteractive -File "+quote(script.wstring());
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
            input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
            output=CreateFileW((root/wide(label+".out")).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,0,nullptr);
            error=CreateFileW((root/wide(label+".err")).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,0,nullptr);
            require(input!=INVALID_HANDLE_VALUE&&output!=INVALID_HANDLE_VALUE&&error!=INVALID_HANDLE_VALUE,"Controlled launcher streams failed");
            SIZE_T size=0;InitializeProcThreadAttributeList(nullptr,1,0,&size);storage.resize(size);attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            require(InitializeProcThreadAttributeList(attributes,1,0,&size)!=FALSE,"Controlled launcher attribute allocation failed");initialized=true;
            HANDLE handles[]={input,output,error};require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr)!=FALSE,"Controlled launcher stream whitelist failed");
            STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput=input;startup.StartupInfo.hStdOutput=output;startup.StartupInfo.hStdError=error;startup.lpAttributeList=attributes;
            require(CreateProcessW(engine.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,root.c_str(),&startup.StartupInfo,&info)!=FALSE,"Controlled launcher creation failed");
            process_=info.hProcess;
            if(!AssignProcessToJobObject(job_,process_)){TerminateProcess(process_,99);throw std::runtime_error("Controlled launcher job assignment failed");}
            require(ResumeThread(info.hThread)!=static_cast<DWORD>(-1),"Controlled launcher resume failed");
            cleanup();
        } catch(...) {cleanup();if(process_){TerminateProcess(process_,99);CloseHandle(process_);}if(job_)CloseHandle(job_);throw;}
    }
    ~ParentJob(){close();if(process_)CloseHandle(process_);}
    void close(){if(job_){CloseHandle(job_);job_=nullptr;}}
    bool running() const {return WaitForSingleObject(process_,0)==WAIT_TIMEOUT;}
    int wait() const {until([&]{return WaitForSingleObject(process_,0)==WAIT_OBJECT_0;},20);DWORD code=0;require(GetExitCodeProcess(process_,&code)!=FALSE,"Launcher exit missing");return static_cast<int>(code);}
};

void setup_run(Coordinator& c,const fs::path& repo) {
    copy(repo,c.root,{"scripts/build_run.ps1"});
    c.call({"task-open","runner","-TaskName","durable run fixture"});
    write(c.root/L"leaf.ps1",R"PS(param([string]$Marker='grandchild.json')
$record=@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()}
[IO.File]::WriteAllText((Join-Path $PSScriptRoot $Marker),($record|ConvertTo-Json -Compress))
while($true){Start-Sleep -Milliseconds 200}
)PS");
    auto fake=read(c.scripts/L"fake.ps1");
    replace(fake,"if($Phase -eq 'build'){",R"PS(if($Phase -eq 'build'){
 $childInfo=New-Object Diagnostics.ProcessStartInfo
 $childInfo.FileName=$env:SNOWDESKTOP_ENTRY_POWERSHELL;$childInfo.Arguments='-NoProfile -File "'+(Join-Path $root 'leaf.ps1')+'"'
 $childInfo.UseShellExecute=$false;$childInfo.CreateNoWindow=$true
 $child=New-Object Diagnostics.Process;$child.StartInfo=$childInfo;[void]$child.Start();$child.Dispose()
)PS");
    write(c.scripts/L"fake.ps1",fake,true);c.gate("hold-build");
    write(c.root/L"launch-run.ps1","$ErrorActionPreference='Stop'\n& "+ps_literal(c.scripts/L"build_run.ps1")+
          " -Command start -Task runner -Action release 1> "+ps_literal(c.root/L"submitted.json")+" 2> "+ps_literal(c.root/L"launcher.err")+"\nexit $LASTEXITCODE\n",true);
}

Json status(Coordinator& c,const std::string& id) {
    const auto value=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"status",L"-RunId",wide(id)},c.root,12);
    require(value.code==0,"Run status failed: "+value.err);return Json::parse(value.out);
}
Result command(Coordinator& c,const std::string& id,const std::vector<std::wstring>& extra) {
    std::vector<std::wstring> args={powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-RunId",wide(id)};
    args.insert(args.end(),extra.begin(),extra.end());return run(args,c.root,15);
}
Json await_running(Coordinator& c,const std::string& id) {
    until([&]{return fs::exists(c.root/L"build-started")&&fs::exists(c.root/L"child.json")&&fs::exists(c.root/L"grandchild.json");},20);
    auto view=status(c,id);require(view.at("status").str()=="running"&&view.at("ownerState").str()=="alive","Run executor not alive");
    c.workers.push_back(view.at("record").at("owner"));return view;
}
void request_publication(const fs::path& repo,bool oldPublication=false){
    for(const bool legacy:{false,true}){
        Coordinator c(repo,legacy?"request-publication-ps5":"request-publication-core");setup_run(c,repo);
        auto source=read(c.scripts/L"build_run.ps1");
        if(oldPublication)replace(source,"$temporary=$path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'","$temporary=$path");
        replace(source,"$stream=[IO.File]::Open($temporary,'CreateNew','Write','None')",R"PS($stream=[IO.File]::Open($temporary,'CreateNew','Write','None')
        [IO.File]::WriteAllText((Join-Path $root 'request-held'),'opened')
        while([IO.File]::Exists((Join-Path $root 'hold-publication'))){[Threading.Thread]::Sleep(20)}
)PS");
        write(c.scripts/L"build_run.ps1",source,true);c.gate("hold-publication");
        // Capture JSON with an explicit UTF8 writer in either real engine.
        write(c.root/L"publish.ps1","$ErrorActionPreference='Stop'\n$value=& "+ps_literal(c.scripts/L"build_run.ps1")+" -Command start -Task runner -Action release\n$code=$LASTEXITCODE\n[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'submitted.json'),($value -join [Environment]::NewLine),[Text.UTF8Encoding]::new($false))\nexit $code\n",true);
        auto launcher=c.root/L"publish.ps1";
        if(legacy){
            launcher=c.root/L"publish-ps5.ps1";
            write(launcher,"$env:SNOWDESKTOP_ENTRY_POWERSHELL="+ps_literal(powershell(true))+"\n& "+ps_literal(powershell(true))+" -NoProfile -File "+ps_literal(c.root/L"publish.ps1")+"\nexit $LASTEXITCODE\n",true);
        }
        ParentJob parent(launcher,c.root,true);
        until([&]{return fs::exists(c.root/L"request-held")||!parent.running();},20);
        require(fs::exists(c.root/L"request-held"),"Publication producer failed before held window: "+read(c.root/L"native-launcher.err"));
        const auto requests=[&]{std::vector<fs::path> files;for(const auto& file:fs::directory_iterator(c.stateRoot/L"runs"))if(file.path().filename().wstring().ends_with(L".request.json"))files.push_back(file.path());return files;};
        require(requests().empty(),"Final Run request was visible before its bytes were complete");
        c.gate("hold-publication",false);
        require(parent.wait()==0,"Publication launcher failed: "+read(c.root/L"native-launcher.err"));
        const auto submitted=Json::parse(read(c.root/L"submitted.json"));
        const auto id=submitted.at("id").str();const auto visible=requests();
        require(visible.size()==1&&visible[0].filename()==wide(id+".request.json"),"Request publication lost its unique final identity");
        const auto bytes=read(visible[0]);const auto request=Json::parse(bytes);
        require(request.at("schemaVersion").integer()==1&&request.at("id").str()==id&&request.at("task").str()=="runner"&&request.at("nonce").str().size()==32,"Published request was incomplete or changed its existing wire contract");
        auto view=await_running(c,id);parent.close();
        require(owner_alive(view.at("record").at("owner")),"Publishing parent lost the admitted durable executor");
        require(command(c,id,{L"-Command",L"cancel",L"-Task",L"runner"}).code==0,"Published fixture cancellation was rejected");
        require(command(c,id,{L"-Command",L"wait",L"-Seconds",L"10"}).code==4&&read(visible[0])==bytes,"Cancellation changed immutable published request bytes");
        std::cout<<"PASS held request publication and immutable admitted execution "<<(legacy?"PS5":"Core")<<'\n';
    }
    {
        Coordinator c(repo,"request-publication-abort");setup_run(c,repo);c.gate("hold-publication");
        auto source=read(c.scripts/L"build_run.ps1");
        replace(source,"$stream=[IO.File]::Open($temporary,'CreateNew','Write','None')",R"PS($stream=[IO.File]::Open($temporary,'CreateNew','Write','None')
        [IO.File]::WriteAllText((Join-Path $root 'request-held'),'opened')
        while([IO.File]::Exists((Join-Path $root 'hold-publication'))){[Threading.Thread]::Sleep(20)}
)PS");
        write(c.scripts/L"build_run.ps1",source,true);ParentJob parent(c.root/L"launch-run.ps1",c.root,true);
        until([&]{return fs::exists(c.root/L"request-held")||!parent.running();},20);
        require(fs::exists(c.root/L"request-held"),"Aborted producer did not enter the held publication window");
        parent.close();require(parent.wait()!=0,"Closing the controlled pre-admission Job did not stop its producer");
        for(const auto& file:fs::directory_iterator(c.stateRoot/L"runs"))require(!file.path().filename().wstring().ends_with(L".request.json"),"Stopped producer left a discoverable partial request");
        require(c.calls()==0&&!fs::exists(c.root/L"build-started"),"Pre-publication termination started execution");
    }
    {
        Coordinator c(repo,"request-publication-collision");setup_run(c,repo);
        const auto id=std::string(32,'c');const auto request=c.stateRoot/L"runs"/wide(id+".request.json");
        const auto existing=Json::object({{"id",id},{"marker","preexisting immutable request"}}).dump();write(request,existing);
        auto source=read(c.scripts/L"build_run.ps1");replace(source,"$id=[Guid]::NewGuid().ToString('N')","$id='"+id+"'");write(c.scripts/L"build_run.ps1",source,true);
        ParentJob parent(c.root/L"launch-run.ps1",c.root,true);
        require(parent.wait()!=0&&read(request)==existing&&c.calls()==0&&!fs::exists(c.root/L"build-started"),"Publication overwrote an existing request or admitted execution after collision");
    }
    std::cout<<"PASS request producer termination and create-only collision\n";
}
void await_dead(const Json& value) {until([&]{return !owner_alive(value);},10);}
void old_entry_edit_control(const fs::path& repo){
    Coordinator c(repo,"old-entry-edit-control");setup_run(c,repo);
    auto manager=read(c.scripts/L"build_manager.ps1");
    replace(manager,"if(-not $current -and $Command -in 'begin','repair') {","if($false) { # Controlled removal of the existing standalone Run edit gate");
    write(c.scripts/L"build_manager.ps1",manager,true);
    ParentJob parent(c.root/L"launch-run.ps1",c.root,true);require(parent.wait()==0,"Entry/edit control could not launch");
    const auto id=Json::parse(read(c.root/L"submitted.json")).at("id").str();await_running(c,id);
    const auto attempted=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"begin","pending-editor"});
    require(attempted.code==2,"Active ordinary Run allowed a source-edit grant; exit="+std::to_string(attempted.code)+" "+attempted.out+attempted.err);
}
void tool_plan(const fs::path& repo,bool oldBehavior=false){
    Coordinator c(repo,oldBehavior?"old-tool-plan":"tool-plan-no-host",true);
    if(oldBehavior){
        auto source=read(c.scripts/L"build_protocol.ps1");
        replace(source,"if((Get-Field $plan 'scope' 'unknown') -notin 'docs','component','tool'){$build=$true}","$build=$true");
        write(c.scripts/L"build_protocol.ps1",source,true);
    }
    const auto batch=c.begin("tool").at("batchId").str();c.plan("tool",batch,0,"selected","Alpha","tool");
    write(c.root/L"finish.ps1","& "+ps_literal(c.scripts/L"build_manager.ps1")+" ready-and-wait tool -Batch "+batch+" -Revision 0\nexit $LASTEXITCODE\n",true);
    ParentJob parent(c.root/L"finish.ps1",c.root,true);
    require(parent.wait()==0,"Tool-only durable batch failed: "+read(c.root/L"native-launcher.err"));
    require(c.calls()==1&&read(c.root/L"calls.txt")=="test\n"&&!fs::exists(c.root/L"build-started"),"Tool plan invoked unnecessary host compilation");
    const auto result=c.result(batch);
    require(result.at("outcome").str()=="passed"&&!result.at("testingPlan").at("buildRequired").boolean&&result.at("testingPlan").at("mode").str()=="selected","Tool tests were skipped or escalated to a host build");
}

void durable_cancel(const fs::path& repo,bool oldObservation=false,bool oldPublication=false) {
        Coordinator c(repo,"durable-batch",true);setup_run(c,repo);
        if(oldPublication){
            auto source=read(c.scripts/L"build_run.ps1");
            replace(source,"$publicationCode=$LASTEXITCODE","if($LASTEXITCODE -ne 0){throw 'Batch result publication failed; preserve state and inspect status/recover.'}\n                $publicationCode=$LASTEXITCODE");
            write(c.scripts/L"build_run.ps1",source,true);
        }
        // The result must be durable before retiring metadata, so existence is
        // not the Run's completion signal. Hold that real publication window.
        auto manager=read(c.scripts/L"build_manager.ps1");
        replace(manager,"    $State.current = $null",R"PS(    [IO.File]::WriteAllText((Join-Path $stateRoot 'publication-paused'),'paused')
    $pause=[Diagnostics.Stopwatch]::StartNew()
    while([IO.File]::Exists((Join-Path $stateRoot 'hold-publication'))){
        if($pause.Elapsed.TotalSeconds -gt 10){throw 'Publication fixture gate not released'}
        Start-Sleep -Milliseconds 20
    }
    $State.current = $null)PS");
        write(c.scripts/L"build_manager.ps1",manager,true);
        write(c.stateRoot/L"hold-publication","hold");
        const auto batch=c.begin("runner").at("batchId").str();c.begin("peer");
        require(c.state().at("current").at("protocolVersion").integer()==3,"New production batch did not choose durable execution");
        for(const std::string task:{"runner","peer"})write(c.root/wide(task+"-finish.ps1"),"& "+ps_literal(c.scripts/L"build_manager.ps1")+" ready-and-wait "+task+" -Batch "+batch+" -Revision 0\nexit $LASTEXITCODE\n",true);
        ParentJob first(c.root/L"runner-finish.ps1",c.root,true,"runner");ParentJob second(c.root/L"peer-finish.ps1",c.root,true,"peer");
        std::string id;
        until([&]{const auto current=c.state().at("current");if(current.has("runId")){id=current.at("runId").str();return true;}return false;});
        auto view=await_running(c,id);first.close();second.close();
        c.call({"execute",view.at("task").str(),"-Batch",batch,"-Revision","0","-ExecutionRun",id},2);
        require(owner_alive(view.at("record").at("owner"))&&c.calls()==1,"Closing all batch observers killed or duplicated execution");
        size_t requests=0;for(const auto& file:fs::directory_iterator(c.stateRoot/L"runs"))if(file.path().filename().wstring().ends_with(L".request.json"))++requests;
        require(requests==1,"Concurrent finishers reserved more than one Run");
        c.call({"event-post","runner","-Target",view.at("task").str(),"-EventKind","requirements","-Importance","urgent","-Text","review before continuing","-EventKey","durable-test"});
        require(command(c,id,{L"-Command",L"wait",L"-Seconds",L"1"}).code==6&&owner_alive(view.at("record").at("owner")),"Urgent observation cancelled or hid the active batch");
        require(command(c,id,{L"-Command",L"cancel",L"-Task",wide(view.at("task").str())}).code==0,"Explicit batch cancellation failed");
        until([&]{return fs::exists(c.stateRoot/L"publication-paused");});
        require(fs::exists(c.stateRoot/wide(batch+".json"))&&c.state().at("current").at("id").str()==batch&&!status(c,id).at("terminal").boolean,"Publication fixture did not expose a durable result before retirement");
        const bool oldCompleted=c.state().at("current").kind==Json::Kind::Null;
        fs::remove(c.stateRoot/L"hold-publication");
        // Observe the existing executor's terminal record, never restart it or
        // infer completion from the earlier immutable batch-result file.
        Json terminal;
        until([&]{terminal=Json::parse(read(c.stateRoot/L"runs"/wide(id+".json")));const auto state=terminal.at("status").str();return state=="cancelled"||state=="failed"||state=="passed"||state=="invalidated"||state=="interrupted";},20);
        require(terminal.at("status").str()=="cancelled","Cancelled batch disagrees with terminal Run: "+terminal.dump());
        if(oldObservation)require(oldCompleted,"Early result-file observation preceded batch retirement");
        const auto result=c.result(batch);
        require(result.at("outcome").str()=="cancelled"&&result.at("runId").str()==id&&c.state().at("current").kind==Json::Kind::Null,"Batch did not publish one shared cancellation after lease release");
        const auto firstResult=c.call({"wait","runner","-Batch",batch,"-Revision","0"},4);
        const auto secondResult=c.call({"wait","peer","-Batch",batch,"-Revision","0"},4);
        require(equivalent_results(result,firstResult)&&equivalent_results(firstResult,secondResult),"Cancelled participants received different shared results");
        HANDLE lease=CreateFileW((c.stateRoot/L"build.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        require(lease!=INVALID_HANDLE_VALUE,"Shared batch result preceded output lease release");CloseHandle(lease);
        await_dead(Json::parse(read(c.root/L"child.json")));await_dead(Json::parse(read(c.root/L"grandchild.json")));
        std::cout<<"PASS concurrent durable finishers, observer loss, urgent return, explicit cancellation and shared terminal release\n";
}
void test(const fs::path& repo) {
    durable_cancel(repo);
    {
        const auto controlRoot=temporary("early-result-control");
        const auto control=run({executable().wstring(),repo.wstring(),L"--early-result-control"},controlRoot,35);
        require(control.code==1&&control.err.find("Early result-file observation preceded batch retirement")!=std::string::npos,"Controlled publication window did not reject the former observation: "+control.out+control.err);
        std::cout<<"PASS controlled publication window rejects premature retirement observation\n";
        const auto publicationRoot=temporary("old-cancel-publication-control");
        const auto publication=run({executable().wstring(),repo.wstring(),L"--old-cancel-publication"},publicationRoot,35);
        require(publication.code==1&&publication.err.find("Cancelled batch disagrees with terminal Run")!=std::string::npos&&publication.err.find("Batch result publication failed")!=std::string::npos,"Restored nonzero publication rejection was not caught: "+publication.out+publication.err);
        std::cout<<"PASS restored cancellation publication rejection fails the Run/batch consistency assertion\n";
    }
    {
        tool_plan(repo);
        const auto controlRoot=temporary("old-host-plan-control");
        const auto control=run({executable().wstring(),repo.wstring(),L"--old-host-plan"},controlRoot,30);
        require(control.code==1&&control.err.find("Tool plan invoked unnecessary host compilation")!=std::string::npos,"Restored host-build coupling was not caught by its actual invocation assertion: "+control.out+control.err);
        std::cout<<"PASS tool-only batch avoids host compilation and rejects restored old behavior\n";
    }
    {
        Coordinator c(repo,"batch-refusal",true);
        const auto batch=c.begin("tool").at("batchId").str();c.plan("tool",batch,0,"selected","Alpha","tool");
        write(c.root/L"finish.ps1","& "+ps_literal(c.scripts/L"build_manager.ps1")+" ready-and-wait tool -Batch "+batch+" -Revision 0\nexit $LASTEXITCODE\n",true);
        ParentJob parent(c.root/L"finish.ps1",c.root,false);
        require(parent.wait()==2,"Restrictive batch parent silently used foreground execution");
        const auto result=c.result(batch);require(result.at("outcome").str()=="failed"&&result.at("runId").kind==Json::Kind::String&&c.calls()==0,"Refused batch lost shared failure or started work");
        const auto view=status(c,result.at("runId").str());require(view.at("status").str()=="start-failed","Refused batch did not retain its immutable Run request");
        std::cout<<"PASS refused persistent batch launch publishes failure and preserves its request without executing\n";
    }
    {
        Coordinator c(repo,"durable-batch-crash",true);setup_run(c,repo);
        const auto batch=c.begin("runner").at("batchId").str();
        write(c.root/L"finish.ps1","& "+ps_literal(c.scripts/L"build_manager.ps1")+" ready-and-wait runner -Batch "+batch+" -Revision 0\nexit $LASTEXITCODE\n",true);
        ParentJob parent(c.root/L"finish.ps1",c.root,true);
        std::string id;until([&]{const auto current=c.state().at("current");if(current.has("runId")){id=current.at("runId").str();return true;}return false;});
        auto view=await_running(c,id);parent.close();stop_owner(view.at("record").at("owner"));
        await_dead(Json::parse(read(c.root/L"child.json")));await_dead(Json::parse(read(c.root/L"grandchild.json")));
        require(status(c,id).at("status").str()=="interrupted"&&!fs::exists(c.stateRoot/wide(batch+".json")),"Lost executor silently published success or replayed the batch");
        c.call({"recover","-Batch",batch,"-ConfirmStopped","-Reason","verified fixture executor exited"});
        require(c.result(batch).at("outcome").str()=="interrupted"&&c.result(batch).at("runId").str()==id&&c.calls()==1,"Explicit recovery lost the Run link or repeated work");
        std::cout<<"PASS batch executor loss cleans children and requires explicit shared recovery\n";
    }
    {
        Coordinator c(repo,"default-entry");setup_run(c,repo);
        write(c.root/L"launch-entry.ps1","[Environment]::SetEnvironmentVariable('SNOWDESKTOP_EXECUTION_TOKEN',$null,'Process')\n[Environment]::SetEnvironmentVariable('SNOWDESKTOP_RUN_ID',$null,'Process')\n& "+ps_literal(c.scripts/L"build_entry.ps1")+" -Action release\nexit $LASTEXITCODE\n",true);
        ParentJob parent(c.root/L"launch-entry.ps1",c.root,true);
        std::string id;
        until([&]{if(!fs::exists(c.stateRoot/L"runs"))return false;for(const auto& file:fs::directory_iterator(c.stateRoot/L"runs"))if(file.path().filename().wstring().ends_with(L".request.json")){id=Json::parse(read(file.path())).at("id").str();return true;}return false;});
        auto view=await_running(c,id);const auto task=view.at("task").str();
        require(task.starts_with("entry-")&&view.at("request").at("automaticTask").boolean,"Default entry did not register its actual execution task");
        c.call({"task-open","pending-editor","-TaskName","investigation during ordinary Run"});
        const auto refused=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"begin","pending-editor"});
        require(refused.code==2&&c.state().at("current").kind==Json::Kind::Null&&owner_alive(view.at("record").at("owner")),"Active ordinary Run allowed a source-edit grant");
        parent.close();require(owner_alive(view.at("record").at("owner"))&&c.calls()==1,"Closing default entry observer killed or duplicated execution");
        c.gate("hold-build",false);
        const auto result=command(c,id,{L"-Command",L"wait",L"-Seconds",L"10"});
        view=Json::parse(result.out);
        require(result.code==0&&view.at("status").str()=="passed"&&c.calls()==1,"Default entry did not publish successful durable execution");
        const auto editing=c.call({"begin","pending-editor"});
        c.call({"recover","pending-editor","-Batch",editing.at("batchId").str(),"-ConfirmStopped","-Reason","fixture editor completed without writing"});
        HANDLE lease=CreateFileW((c.stateRoot/L"build.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        require(lease!=INVALID_HANDLE_VALUE,"A terminal default Run still owns its output lease");CloseHandle(lease);
        require(view.at("stageError").str().empty(),"Actual stage observations were unreadable");
        bool preflight=false,pipeline=false,completed=false;
        for(const auto& event:view.at("stageEvents").items){
            preflight=preflight||event.at("name").str()=="preflight";
            pipeline=pipeline||event.at("name").str()=="release-pipeline";
            if(event.at("kind").str()=="end")completed=completed||(event.at("exitCode").integer()==0&&event.at("elapsedSeconds").number>=0);
        }
        require(preflight&&pipeline&&completed,"Default production entry did not retain actual preflight/pipeline intervals");
        const auto taskState=c.state();bool closed=false;for(const auto& entry:taskState.at("taskLedger").at("tasks").items)if(entry.at("id").str()==task)closed=entry.at("closedUtc").kind==Json::Kind::String;
        require(closed,"A terminal default Run has not completed its automatic task transaction");
        std::cout<<"PASS default entry survives observer Job close, registers/finishes its task and records actual stages\n";
        const auto controlRoot=temporary("entry-edit-control-parent");
        const auto control=run({executable().wstring(),repo.wstring(),L"--old-entry-edit"},controlRoot,40,{},true);
        require(control.code==1&&control.err.find("Active ordinary Run allowed a source-edit grant")!=std::string::npos,"Former ordinary-Run edit overlap escaped its actual grant assertion: "+control.out+control.err);
    }
    {
        Coordinator c(repo,"run-cancel");setup_run(c,repo);
        ParentJob parent(c.root/L"launch-run.ps1",c.root,true);
        const int launchCode=parent.wait();
        require(launchCode==0,"Breakaway-capable parent could not submit, exit "+std::to_string(launchCode)+": "+read(c.root/L"native-launcher.err"));
        const auto id=Json::parse(read(c.root/L"submitted.json")).at("id").str();
        auto view=await_running(c,id);const auto executor=view.at("record").at("owner");
        parent.close();require(owner_alive(executor),"Closing parent Job killed the admitted executor");
        const auto requestPath=c.stateRoot/L"runs"/wide(id+".request.json");const auto request=read(requestPath);
        const auto waiting=command(c,id,{L"-Command",L"wait",L"-Seconds",L"1"});
        require(waiting.code==10&&!Json::parse(waiting.out).at("terminal").boolean,"Bounded observation falsely completed a held Run");
        Child observer({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"wait",L"-RunId",wide(id),L"-Seconds",L"60"},c.root);
        observer.stop();require(owner_alive(executor)&&c.calls()==1,"Observer cancellation stopped or duplicated execution");
        const auto denied=command(c,id,{L"-Command",L"cancel",L"-Task",L"other"});require(denied.code==2,"Another task cancelled a Run");
        write(c.stateRoot/L"runs"/wide(id+".cancel"),"wrong-generation");
        Child sentinel({powershell().wstring(),L"-NoProfile",L"-File",(c.root/L"leaf.ps1").wstring(),L"-Marker",L"unrelated.json"},c.root);
        until([&]{return fs::exists(c.root/L"unrelated.json");});
        require(owner_alive(executor),"Stale cancellation nonce stopped execution");
        const auto cancelled=command(c,id,{L"-Command",L"cancel",L"-Task",L"runner"});require(cancelled.code==0,"Explicit cancellation was rejected: "+cancelled.err);
        const auto result=command(c,id,{L"-Command",L"wait",L"-Seconds",L"10"});
        require(result.code==4,"Cancelled Run did not publish exit 4: "+result.out+result.err);
        view=Json::parse(result.out);
        require(view.at("status").str()=="cancelled"&&view.at("resources").at("cancelled").boolean,"Cancellation request was mistaken for a completed cancellation");
        await_dead(Json::parse(read(c.root/L"child.json")));await_dead(Json::parse(read(c.root/L"grandchild.json")));
        require(sentinel.running()&&read(requestPath)==request&&c.calls()==1,"Cancellation changed immutable input, duplicated work or killed an unrelated process");
        require(view.at("resources").at("accountingAvailable").boolean&&view.at("resources").at("cpuSeconds").number>=0&&view.at("resources").at("peakJobMemoryBytes").number>0,"Job resource accounting was missing or fabricated");
        require(view.at("record").at("stages").items.size()==2&&view.at("record").at("stages").items[0].at("completedUtc").kind==Json::Kind::String,"Actual queue/pipeline intervals were not retained");
        std::cout<<"PASS parent Job close, observer exit, fenced cancellation, resource accounting and unrelated-process survival\n";
    }
    {
        Coordinator c(repo,"run-success");setup_run(c,repo);
        ParentJob parent(c.root/L"launch-run.ps1",c.root,true);require(parent.wait()==0,"Success Run launch failed");
        const auto id=Json::parse(read(c.root/L"submitted.json")).at("id").str();await_running(c,id);parent.close();c.gate("hold-build",false);
        const auto result=command(c,id,{L"-Command",L"wait",L"-Seconds",L"10"});
        require(result.code==0&&Json::parse(result.out).at("status").str()=="passed"&&c.calls()==1,"Successful persistent execution did not publish its result");
        await_dead(Json::parse(read(c.root/L"grandchild.json")));
        std::cout<<"PASS successful Run, immutable input check and descendant cleanup\n";
    }
    {
        Coordinator c(repo,"run-refusal");setup_run(c,repo);
        ParentJob parent(c.root/L"launch-run.ps1",c.root,false);
        require(parent.wait()==2,"A restrictive parent Job silently launched a non-persistent worker");
        const auto receipt=Json::parse(read(c.root/L"submitted.json"));const auto id=receipt.at("id").str();
        require(receipt.at("status").str()=="start-failed"&&!fs::exists(c.root/L"build-started")&&fs::exists(c.stateRoot/L"runs"/wide(id+".request.json")),"Refused launch lost the request or started build work");
        std::cout<<"PASS restrictive parent Job refuses execution and retains its request\n";
    }
    {
        Coordinator c(repo,"run-crash");setup_run(c,repo);
        ParentJob parent(c.root/L"launch-run.ps1",c.root,true);require(parent.wait()==0,"Crash fixture launch failed");
        const auto id=Json::parse(read(c.root/L"submitted.json")).at("id").str();auto view=await_running(c,id);parent.close();
        const auto requestPath=c.stateRoot/L"runs"/wide(id+".request.json");const auto request=read(requestPath);
        stop_owner(view.at("record").at("owner"));
        await_dead(Json::parse(read(c.root/L"child.json")));await_dead(Json::parse(read(c.root/L"grandchild.json")));
        view=status(c,id);require(view.at("status").str()=="interrupted"&&view.at("terminal").boolean&&read(requestPath)==request,"Executor loss hid the interrupted outcome or lost the plan");
        const auto duplicate=command(c,id,{L"-Command",L"worker"});require(duplicate.code==2&&c.calls()==1,"An interrupted Run silently acquired another worker");
        std::cout<<"PASS worker crash cleans its Job and preserves an interrupted request without automatic takeover\n";
    }
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){if(argc>2&&std::string(argv[2])=="--request-publication"){request_publication(repo);return;}if(argc>2&&std::string(argv[2])=="--old-request-publication"){request_publication(repo,true);return;}if(argc>2&&std::string(argv[2])=="--old-entry-edit"){old_entry_edit_control(repo);return;}if(argc>2&&std::string(argv[2])=="--old-host-plan"){tool_plan(repo,true);return;}if(argc>2&&std::string(argv[2])=="--early-result-control"){durable_cancel(repo,true);return;}if(argc>2&&std::string(argv[2])=="--old-cancel-publication"){durable_cancel(repo,false,true);return;}test(repo);});}
