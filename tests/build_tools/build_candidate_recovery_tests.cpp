#include "../build_tool_test_support.h"
#include <iostream>

using namespace build_test;
namespace {
void recovery(const fs::path& repo){
    Coordinator c(repo,"candidate-source-recovery",true);
    copy(repo,c.root,{"scripts/build_candidate.ps1","scripts/build_candidate_recovery.ps1"});
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+
        "\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='Recovery fixture has no optional dashboard'}}\n",true);
    c.call({"task-open","recovery","-TaskName","Single source recovery fixture"});
    const auto started=c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","start","-Task","recovery","-Action","snapshot"});
    const auto captured=c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","wait","-Task","recovery","-RunId",started.at("id").str(),"-Seconds","60"});
    require(captured.at("status").str()=="passed","Recovery fixture snapshot failed");
    const auto snapshot=captured.at("record").at("candidate").at("id").str();
    const auto initial=c.bridge->call(c.scripts/L"build_candidate.ps1",{"-Command","materialize","-Task","recovery","-Snapshot",snapshot});
    const auto source=fs::path(wide(initial.at("source").str())),output=fs::path(wide(initial.at("output").str()));
    const auto workspace=source.parent_path(),sentinel=output/L"kept-object.obj";
    write(sentinel,"existing compiler object\n");const auto objectTime=fs::last_write_time(sentinel),unchangedTime=fs::last_write_time(source/L"src/b.txt");
    // Real partial materialization plus an untracked human file. Ordinary
    // materialization must refuse to overwrite it; explicit recovery archives it.
    write(source/L"src/a.txt","partially copied next snapshot\n");write(source/L"manual-note.txt","human notes to retain\n");
    const auto beforeRepair=c.bridge->invoke(c.scripts/L"build_candidate.ps1",{"-Command","materialize","-Task","recovery","-Snapshot",snapshot});
    require(beforeRepair.code==2&&beforeRepair.err.find("modified outside its lease")!=std::string::npos,"Partial source did not reproduce the materialization blockage");
    write(c.root/L"repair-invoke.ps1",R"PS(param([string]$Id,[string]$Mode='normal')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_protocol.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_inputs.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_candidate.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_candidate_recovery.ps1')
$snapshot=Read-CandidateManifest $PSScriptRoot $Id -VerifyObjects
$slot=Enter-CandidateSlot $snapshot 'ignored-recovery-name'
try {
 if($Mode -eq 'no-lease'){$slot.lease.Dispose();$slot.lease=$null}
 if($Mode -eq 'shared-lease'){$slot.lease.Dispose();$slot.lease=[IO.File]::Open((Join-Path $slot.directory 'slot.lock'),'Open','ReadWrite',([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))}
 if($Mode -eq 'outside'){$slot.source=Join-Path $PSScriptRoot 'outside'}
 if($Mode -eq 'interrupt'){$result=Repair-CandidateWorkspace $snapshot $slot {param($Phase)if($Phase -eq 'after-archive'){throw [OperationCanceledException]::new('controlled recovery interruption')}}}
 else{$result=Repair-CandidateWorkspace $snapshot $slot}
 $result|ConvertTo-Json -Depth 8
}catch{[Console]::Error.WriteLine($_.Exception.Message);exit 2}finally{if($slot.lease){$slot.lease.Dispose()}}
)PS",true);
    auto invoke=[&](const std::string& mode="normal"){return c.bridge->invoke(c.root/L"repair-invoke.ps1",{snapshot,mode});};
    for(const auto& mode:{"no-lease","shared-lease","outside"}){
        const auto rejected=invoke(mode);
        require(rejected.code==2&&(rejected.err.find("lease is required")!=std::string::npos||rejected.err.find("workspace path changed")!=std::string::npos),"Recovery accepted a foreign/shared output lease or escaped source: "+rejected.out+rejected.err);
        require(read(source/L"manual-note.txt")=="human notes to retain\n"&&read(sentinel)=="existing compiler object\n","Rejected recovery changed source or compiler output");
    }
    const auto repaired=invoke();require(repaired.code==0,"Source recovery failed: "+repaired.out+repaired.err);
    const auto receipt=Json::parse(repaired.out);const auto archive=fs::path(wide(receipt.at("archive").str()));
    require(receipt.at("recovered").boolean&&receipt.at("source").str()==initial.at("source").str()&&receipt.at("output").str()==initial.at("output").str(),"Recovery created another working source/output directory");
    require(read(source/L"src/a.txt")=="original\n"&&!fs::exists(source/L"manual-note.txt")&&read(archive/L"source/src/a.txt")=="partially copied next snapshot\n"&&read(archive/L"source/manual-note.txt")=="human notes to retain\n","Recovery lost the original snapshot or the untracked human file");
    require(fs::is_regular_file(archive/L"materialization.json")&&Json::parse(read(archive/L"recovery.json")).at("status").str()=="complete","Recovery archive lost its source manifest or completion record");
    require(read(sentinel)=="existing compiler object\n"&&fs::last_write_time(sentinel)==objectTime&&fs::last_write_time(source/L"src/b.txt")==unchangedTime,"Recovery changed compiler output or the timestamp of an unchanged source");
    // Unknown junctions are rejected before anything is moved. Only the two
    // explicitly checked output aliases may be present in the source archive.
    const auto outside=c.root/L"outside";fs::create_directories(outside);write(outside/L"private.txt","external data\n");
    const auto linked=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("New-Item -ItemType Junction -Path "+ps_literal(source/L"escape")+" -Target "+ps_literal(outside)+"|Out-Null")},c.root,10);
    require(linked.code==0,"Recovery junction fixture failed: "+linked.err);
    const auto unsafe=invoke();require(unsafe.code==2&&unsafe.err.find("unsafe reparse point")!=std::string::npos&&read(outside/L"private.txt")=="external data\n"&&fs::exists(source/L"src/a.txt"),"Recovery followed an unknown source junction or mutated its target");fs::remove(source/L"escape");
    write(source/L"second-note.txt","retain across interruption\n");
    const auto interrupted=invoke("interrupt");require(interrupted.code==2&&interrupted.err.find("controlled recovery interruption")!=std::string::npos,"Recovery interruption did not reach the archive boundary");
    require(!fs::exists(source)&&read(sentinel)=="existing compiler object\n","Interrupted recovery wrote output or failed outside the archived-source boundary");
    const auto retried=invoke();require(retried.code==0,"Explicit retry could not restore an interrupted recovery: "+retried.out+retried.err);
    bool retained=false;for(const auto& item:fs::directory_iterator(workspace/L"recovery"))if(fs::is_regular_file(item.path()/L"source/second-note.txt"))retained=read(item.path()/L"source/second-note.txt")=="retain across interruption\n";
    require(retained&&read(source/L"src/a.txt")=="original\n"&&read(sentinel)=="existing compiler object\n","Recovery retry lost the earlier archive, frozen source or compiler output");
    // A stale configuration reset belongs to the old compiler context. Keep
    // its evidence, then bind the pending reset to the restored source context.
    const auto reset=output/L"collaboration/configuration-reset.json";write(reset,"{\"schemaVersion\":1,\"compilerContextDigest\":\"wrong\",\"configured\":[\"old\"]}");
    const auto configRepair=invoke();require(configRepair.code==0,"Recovery of a stale configure marker failed: "+configRepair.err);
    const auto configReceipt=Json::parse(configRepair.out),material=Json::parse(read(workspace/L"materialization.json")),fixed=Json::parse(read(reset));
    require(fixed.at("compilerContextDigest").str()==material.at("compilerContextDigest").str()&&fixed.at("configured").items.empty()&&fs::is_regular_file(fs::path(wide(configReceipt.at("archive").str()))/L"configuration-reset.json"),"Recovery retained a stale configure identity or lost its original marker");
    require(read(sentinel)=="existing compiler object\n"&&fs::last_write_time(sentinel)==objectTime,"Configuration-marker recovery changed compiled output");
    std::cout<<"PASS explicit source recovery preserves untracked files and output, keeps unchanged timestamps, rejects unsafe leases/paths and retries after interruption\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,recovery);}
