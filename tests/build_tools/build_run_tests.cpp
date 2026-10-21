#include "build_tool_test_support.h"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace build_test;
namespace {
class ParentJob {
    HANDLE job_=nullptr,process_=nullptr;
public:
    ParentJob(const fs::path& script,const fs::path& root,bool allow) {
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
            output=CreateFileW((root/L"native-launcher.out").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,0,nullptr);
            error=CreateFileW((root/L"native-launcher.err").c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,CREATE_ALWAYS,0,nullptr);
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
void await_dead(const Json& value) {until([&]{return !owner_alive(value);},10);}

void test(const fs::path& repo) {
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
int main(int argc,char** argv){return test_main(argc,argv,test);}
