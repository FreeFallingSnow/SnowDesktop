#include "build_tool_test_support.h"
#include <iostream>
#include <stdexcept>

using namespace build_test;
namespace {
class FileLease {
    HANDLE handle_=INVALID_HANDLE_VALUE;
public:
    explicit FileLease(const fs::path& path) {
        handle_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr);
        require(handle_!=INVALID_HANDLE_VALUE,"Controlled state lock acquisition failed");
    }
    ~FileLease(){release();}
    void release(){if(handle_!=INVALID_HANDLE_VALUE){CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;}}
};
void classification(const fs::path& repo,bool legacy){
    const auto root=temporary(legacy?"state-lock-types-ps5":"state-lock-types-ps7");
    copy(repo,root,{"scripts/build_entry.ps1"});
    fs::create_directories(root/L".build/collaboration");
    write(root/L"probe.ps1",R"PS($ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')
$timer=[Diagnostics.Stopwatch]::StartNew();$busy='';$admission=''
try {$lease=Open-EntryStateTransaction $PSScriptRoot -TimeoutMilliseconds 150;$lease.Dispose()}catch{$busy=$_.Exception.Message}
try {$entry=Enter-BuildEntry $PSScriptRoot;Exit-BuildEntry $entry}catch{$admission=$_.Exception.Message}
$other=New-Object IO.IOException('controlled non-sharing I/O failure',-2147024784)
$wrapped=New-Object Exception('wrapper',$other)
[pscustomobject]@{busy=$busy;admission=$admission;elapsed=$timer.Elapsed.TotalSeconds;otherBusy=(Test-EntrySharingViolation $wrapped)}|ConvertTo-Json -Compress
)PS",true);
    FileLease lock(root/L".build/collaboration/state.lock");
    Bridge bridge(repo,root,legacy);
    const auto result=bridge.call(root/L"probe.ps1",{});
    require(result.at("busy").str().find("state reservation timed out")!=std::string::npos&&
            result.at("admission").str().find("Shared output lease/transaction is busy;")==0,
            "Wrapped sharing failure was not classified as bounded state contention: "+result.dump());
    require(result.at("elapsed").number>=0.15&&result.at("elapsed").number<3&&!result.at("otherBusy").boolean,
            "State wait was unbounded or unrelated I/O was classified as contention: "+result.dump());
    lock.release();
    write(root/L"released.ps1",". (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')\n$lease=Open-EntryStateTransaction $PSScriptRoot -TimeoutMilliseconds 150\n$lease.Dispose()\n@{acquired=$true}|ConvertTo-Json\n",true);
    require(bridge.call(root/L"released.ps1",{}).at("acquired").boolean,"Released transaction remained unavailable");
}
void batch_contention(const fs::path& repo,bool reservation,bool former=false,bool cancel=false,bool formerCancellation=false){
    Coordinator c(repo,reservation?"state-lock-reservation":"state-lock-admission",true);
    const auto batch=c.begin("runner").at("batchId").str();c.plan("runner",batch,0,"selected","Alpha","tool");
    auto source=read(c.scripts/L"build_run.ps1");
    if(formerCancellation)replace(source,"$queuedCancellation=$true;break","$record.status='cancelled';$queuedCancellation=$true;break");
    const std::string gate=R"PS([IO.File]::WriteAllText((Join-Path $root 'transaction-paused'),'paused')
        $pause=[Diagnostics.Stopwatch]::StartNew()
        while([IO.File]::Exists((Join-Path $root 'hold-transaction'))){
            if($pause.Elapsed.TotalSeconds -gt 10){throw 'Controlled transaction gate was not released'}
            Start-Sleep -Milliseconds 20
        }
        )PS";
    if(reservation){
        replace(source,"$batchTransaction=Open-EntryStateTransaction $root",gate+"$batchTransaction=Open-EntryStateTransaction $root");
        auto entry=read(c.scripts/L"build_entry.ps1");
        replace(entry,"            Start-Sleep -Milliseconds 50","            [IO.File]::WriteAllText((Join-Path $Root 'retry-observed'),'sharing')\n            Start-Sleep -Milliseconds 50");
        write(c.scripts/L"build_entry.ps1",entry,true);
    }else{
        replace(source,"$metadata=[IO.File]::Open((Join-Path $root '.build/collaboration/state.lock'),'OpenOrCreate','ReadWrite','None')",gate+"$metadata=[IO.File]::Open((Join-Path $root '.build/collaboration/state.lock'),'OpenOrCreate','ReadWrite','None')");
        if(former)replace(source,"if(Test-EntrySharingViolation $_.Exception){throw 'Shared output lease/transaction is busy; the frozen Run is queued.'}","if($_.Exception -is [IO.IOException]){throw 'Shared output lease/transaction is busy; the frozen Run is queued.'}");
        replace(source,"$record.status='failed';$record.exitCode=2;$record.error=$_.Exception.ToString()","[IO.File]::WriteAllText((Join-Path $root 'transaction-failed'),'failed')\n        $record.status='failed';$record.exitCode=2;$record.error=$_.Exception.ToString()");
    }
    write(c.scripts/L"build_run.ps1",source,true);c.gate("hold-transaction");
    write(c.root/L"finish.ps1","& "+ps_literal(c.scripts/L"build_manager.ps1")+" ready-and-wait runner -Batch "+batch+" -Revision 0\nexit $LASTEXITCODE\n",true);
    Child caller({powershell().wstring(),L"-NoProfile",L"-File",(c.root/L"finish.ps1").wstring()},c.root,{},false,false,true);
    until([&]{return fs::exists(c.root/L"transaction-paused");});
    FileLease lock(c.stateRoot/L"state.lock");c.gate("hold-transaction",false);
    Json view;std::string id;
    if(reservation){until([&]{return fs::exists(c.root/L"retry-observed");});}
    else{
        id=c.state().at("current").at("runId").str();
        until([&]{view=Json::parse(read(c.stateRoot/L"runs"/wide(id+".json")));return !view.at("waitIntervals").items.empty()||fs::exists(c.root/L"transaction-failed");});
        // Register the detached owner for cleanup even when a business assertion fails.
        if(view.at("owner").kind!=Json::Kind::Null)c.workers.push_back(view.at("owner"));
        const bool queued=view.at("status").str()=="queued"&&!view.at("waitIntervals").items.empty();
        if(!queued){lock.release();throw std::runtime_error("Transient state read failed instead of queueing: "+view.dump());}
        if(cancel){
            const auto cancelled=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"cancel",L"-Task",L"runner",L"-RunId",wide(id)},c.root);
            require(cancelled.code==0,"Queued state contention did not accept cancellation: "+cancelled.err);
        }
    }
    require(c.calls()==0&&caller.running(),"Contended state transaction started work or abandoned its caller");
    lock.release();
    if(cancel){
        until([&]{view=Json::parse(read(c.stateRoot/L"runs"/wide(id+".json")));return view.at("status").str()=="cancelled"||view.at("status").str()=="failed";},15);
        require(view.at("status").str()=="cancelled"&&fs::exists(c.stateRoot/wide(batch+".json"))&&c.state().at("current").kind==Json::Kind::Null,
                "Queued cancellation failed to publish and retire its batch: "+view.dump());
    }
    const auto completed=caller.wait(15);
    require(completed.code==(cancel?4:0),"State contention did not finish with its expected result: "+completed.out+completed.err);
    const auto result=c.result(batch);
    require(result.at("outcome").str()==(cancel?"cancelled":"passed")&&c.calls()==(cancel?0:1)&&c.state().at("current").kind==Json::Kind::Null,
            "Released state contention duplicated work, lost cancellation or failed to retire: "+result.dump());
}
void test(const fs::path& repo){
    classification(repo,false);classification(repo,true);
    batch_contention(repo,true);batch_contention(repo,false);batch_contention(repo,false,false,true);
    const auto root=temporary("former-state-lock-control");
    const auto control=run({executable().wstring(),repo.wstring(),L"--former-admission"},root,25);
    require(control.code==1&&control.err.find("Transient state read failed instead of queueing")!=std::string::npos,
            "Former wrapped-exception handling did not reach its business failure: "+control.out+control.err);
    const auto cancelRoot=temporary("former-queued-cancellation-control");
    const auto cancelControl=run({executable().wstring(),repo.wstring(),L"--former-cancellation"},cancelRoot,25);
    require(cancelControl.code==1&&cancelControl.err.find("Queued cancellation failed to publish and retire its batch")!=std::string::npos,
            "Former early cancellation publication did not reach its business failure: "+cancelControl.out+cancelControl.err);
    std::cout<<"PASS PS7/PS5 bounded transaction classification, real batch reservation/admission, queued cancellation and former-behavior failure\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){if(argc>2&&std::string(argv[2])=="--former-admission"){batch_contention(repo,false,true);return;}if(argc>2&&std::string(argv[2])=="--former-cancellation"){batch_contention(repo,false,false,true,true);return;}test(repo);});}
