#include "../build_tool_test_support.h"
#include "product_deployment_fixture.h"
#include <iostream>

using namespace build_test;
namespace {
void write_activation_script(const fs::path& path, std::string script) {
    replace(script,"$artifact=",product_deployment_fixture()+R"PS(
New-TestProductDeployment $payload 'new'
$files=@(Get-ChildItem -LiteralPath $payload -Recurse -File|ForEach-Object {
    [pscustomobject]@{path=$_.FullName.Substring($payload.Length+1).Replace('\','/');sha256=(Get-FileDigest $_.FullName)}
})
$artifact=)PS");
    write(path,script,true);
}
std::wstring hostMarker;
LRESULT CALLBACK host_window(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CLOSE||message==WM_TIMER){
        write(fs::path(hostMarker+L".closed"),"graceful");DestroyWindow(window);return 0;
    }
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
int host(const fs::path& marker) {
    hostMarker=marker.wstring();
    WNDCLASSW cls{};cls.lpfnWndProc=host_window;cls.hInstance=GetModuleHandleW(nullptr);
    cls.lpszClassName=L"SnowDesktopControlWindow";
    if(!RegisterClassW(&cls))return 80;
    const auto window=CreateWindowW(cls.lpszClassName,L"SnowDesktopControl",WS_POPUP,0,0,0,0,nullptr,nullptr,cls.hInstance,nullptr);
    if(!window)return 81;
    SetTimer(window,1,55000,nullptr);
    atomic(marker,Json::object({{"pid",static_cast<int>(GetCurrentProcessId())},
        {"generation",read(executable().parent_path()/L"generation.txt")},
        {"credential",utf8(environment(L"SNOWDESKTOP_EXECUTION_TOKEN"))}}).dump());
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
    return 0;
}
void activation(const fs::path& repo) {
    const auto root=temporary("build-activation"),runtime=root/L"payload",target=root/L".build/Release",state=root/L".build/collaboration";
    copy(repo,root,{"scripts/build_activate.ps1","scripts/build_candidate.ps1","scripts/build_inputs.ps1","scripts/build_entry.ps1","scripts/build_protocol.ps1","scripts/deployment_payload.psm1"});
    fs::create_directories(runtime);fs::create_directories(state);fs::create_directories(target/L"data");fs::create_directories(target/L"data.debug");
    const auto old=target/L"SnowDesktop.exe",marker=root/L"launch.json";
    fs::copy_file(executable(),old);fs::copy_file(executable(),runtime/L"SnowDesktop.exe");
    write(runtime/L"SnowDesktop.exe",read(runtime/L"SnowDesktop.exe")+"new-build-marker");
    write(target/L"generation.txt","old");write(runtime/L"generation.txt","new");
    write(target/L"data/layout.json","preserved layout");write(target/L"data.debug/profile.json","preserved debug profile");
    write(state/L"state.json","{\"current\":{\"phase\":\"editing\",\"id\":\"new-editor\"}}");
    write_activation_script(root/L"activate.ps1",R"PS(param([string]$Captured='2026-10-22T12:00:00Z',[string]$Run='11111111111111111111111111111111')
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_protocol.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_inputs.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_candidate.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_activate.ps1')
$payload=Join-Path $PSScriptRoot 'payload'
$files=@('SnowDesktop.exe','generation.txt'|ForEach-Object{[pscustomobject]@{path=$_;sha256=(Get-FileDigest (Join-Path $payload $_))}})
$artifact=[pscustomobject]@{digest=('a'*64);runtime=$payload;manifest=[pscustomobject]@{productRuntime=$true;configuration='Release';
 runId=$Run;candidate=('b'*32);capturedUtc=$Captured;createdUtc='2026-10-22T13:00:00Z';files=$files}}
$env:SNOWDESKTOP_EXECUTION_TOKEN='fixture-credential-must-not-reach-desktop'
Invoke-BuiltRuntimeActivation $PSScriptRoot $artifact|ConvertTo-Json -Depth 8
)PS");
    Child original({old.wstring(),L"--activation-host",marker.wstring()},root);
    until([&]{return fs::exists(marker);});
    const auto switched=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"activate.ps1").wstring()},root,30);
    require(switched.code==0,"Activation command failed: "+switched.out+switched.err);
    const auto result=Json::parse(switched.out);
    require(result.at("status").str()=="activated","Successful build was not activated: "+switched.out);
    require(original.wait().code==0&&read(marker.wstring()+L".closed")=="graceful","Previous application was not closed gracefully");
    const auto launch=Json::parse(read(marker));
    require(launch.at("generation").str()=="new"&&launch.at("credential").str().empty()&&
        launch.at("pid").integer()==result.at("newPid").integer(),"New runtime, arguments or desktop credential isolation changed");
    require(read(target/L"data/layout.json")=="preserved layout"&&read(target/L"data.debug/profile.json")=="preserved debug profile",
        "Activation replaced user data or debug profile");
    require(Json::parse(read(state/L"state.json")).at("current").at("id").str()=="new-editor","Activation changed the live editing window");
    const auto stale=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"activate.ps1").wstring(),L"-Captured",L"2026-10-22T11:00:00Z",L"-Run",L"22222222222222222222222222222222"},root,20);
    require(stale.code==0&&Json::parse(stale.out).at("status").str()=="superseded"&&
        Json::parse(read(marker)).at("pid").integer()==launch.at("pid").integer(),"Older build replaced the current newer runtime");
    const auto again=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"activate.ps1").wstring()},root,20);
    require(again.code==0&&Json::parse(again.out).at("status").str()=="already-current","Identical runtime restarted unnecessarily");
    // Request the fixture's own window only; the real desktop host can coexist.
    const auto launchedOwner=owner(static_cast<DWORD>(launch.at("pid").integer()));
    HWND window=nullptr;while((window=FindWindowExW(nullptr,window,L"SnowDesktopControlWindow",L"SnowDesktopControl"))!=nullptr){
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        if(pid==static_cast<DWORD>(launch.at("pid").integer())){PostMessageW(window,WM_CLOSE,0,0);break;}
    }
    until([&]{return !owner_alive(launchedOwner);});
    std::cout<<"PASS immediate runtime replacement, graceful exit, retained arguments/data, credential isolation, editing overlap and stale-build rejection\n";
}
void closed_activation(const fs::path& repo,bool oldClosed=false) {
    const auto root=temporary("closed-build-activation"),runtime=root/L"payload",target=root/L".build/Release",state=root/L".build/collaboration";
    copy(repo,root,{"scripts/build_activate.ps1","scripts/build_candidate.ps1","scripts/build_inputs.ps1","scripts/build_entry.ps1","scripts/build_protocol.ps1","scripts/deployment_payload.psm1"});
    if(oldClosed){auto script=read(root/L"scripts/build_activate.ps1");replace(script,"$arguments=''","if(-not $process){$result.status='not-running';return $result};$arguments=''");write(root/L"scripts/build_activate.ps1",script,true);}
    fs::create_directories(runtime);fs::create_directories(state);fs::create_directories(target/L"data");fs::create_directories(target/L"data.debug");
    fs::copy_file(executable(),target/L"SnowDesktop.exe");fs::copy_file(executable(),runtime/L"SnowDesktop.exe");
    write(runtime/L"SnowDesktop.exe",read(runtime/L"SnowDesktop.exe")+"new-closed-build");
    write(target/L"generation.txt","old");write(runtime/L"generation.txt","new");
    write(target/L"data/layout.json","closed layout");write(target/L"data.debug/profile.json","closed debug profile");
    write_activation_script(root/L"activate.ps1",R"PS(param([string]$Captured='2026-10-22T12:00:00Z',[switch]$FailInstall,[switch]$MissingSaved)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_protocol.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_inputs.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_candidate.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_activate.ps1')
function Start-ActivatedApplication($Executable,$Arguments){
 [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'unexpected-launch'),'attempted');throw 'Closed application must stay closed.'
}
$payload=Join-Path $PSScriptRoot 'payload'
$files=@('SnowDesktop.exe','generation.txt'|ForEach-Object{[pscustomobject]@{path=$_;sha256=(Get-FileDigest (Join-Path $payload $_))}})
$artifact=[pscustomobject]@{digest=('a'*64);runtime=$payload;manifest=[pscustomobject]@{productRuntime=$true;configuration='Release';
 runId=('c'*32);candidate=('b'*32);capturedUtc=$Captured;createdUtc='2026-10-22T13:00:00Z';files=$files}}
if($FailInstall){[IO.File]::Delete((Join-Path $payload 'generation.txt'))}
if($MissingSaved){[IO.File]::Delete((Join-Path $payload 'App.xbf'))}
Invoke-BuiltRuntimeActivation $PSScriptRoot $artifact|ConvertTo-Json -Depth 8
)PS");
    const auto invoke=[&](const std::vector<std::wstring>& extra={}){
        std::vector<std::wstring> args{powershell().wstring(),L"-NoProfile",L"-File",(root/L"activate.ps1").wstring()};
        args.insert(args.end(),extra.begin(),extra.end());const auto result=run(args,root,20);
        require(result.code==0,"Closed activation command failed: "+result.out+result.err);return Json::parse(result.out);
    };
    const auto installed=invoke();
    require(installed.at("status").str()=="installed"&&installed.at("oldPid").kind==Json::Kind::Null&&installed.at("newPid").kind==Json::Kind::Null&&
        installed.at("replacedFiles").integer()>2&&read(target/L"generation.txt")=="new"&&read(target/L"SnowDesktop.exe")==read(runtime/L"SnowDesktop.exe")&&
        read(target/L"SnowDesktop.winmd")=="new:SnowDesktop.winmd",
        "Closed application did not receive successful build: "+installed.dump());
    require(Json::parse(read(state/L"activation-current.json")).at("status").str()=="installed","Closed installation was not retained as the current runtime");
    require(invoke().at("status").str()=="already-current","Unchanged closed runtime was installed again");
    const auto missing=invoke({L"-MissingSaved"});
    require(missing.at("status").str()=="failed"&&read(target/L"SnowDesktop.winmd")=="new:SnowDesktop.winmd",
        "Activation accepted an incomplete deployment or changed the installed runtime");
    require(invoke({L"-Captured",L"2026-10-22T11:00:00Z"}).at("status").str()=="superseded","Older build replaced a newer closed installation");
    const auto saved=read(target/L"SnowDesktop.exe");write(runtime/L"SnowDesktop.exe",saved+"failed-new-build");write(runtime/L"generation.txt","failed-new");
    const auto failed=invoke({L"-Captured",L"2026-10-22T14:00:00Z",L"-FailInstall"});
    require(failed.at("status").str()=="restored-after-failure"&&failed.at("newPid").kind==Json::Kind::Null&&
        read(target/L"SnowDesktop.exe")==saved&&read(target/L"generation.txt")=="new","Closed installation failure did not restore the previous files");
    require(!fs::exists(root/L"unexpected-launch")&&read(target/L"data/layout.json")=="closed layout"&&read(target/L"data.debug/profile.json")=="closed debug profile",
        "Closed installation started the application or modified user data");
    if(!oldClosed){const auto control=run({executable().wstring(),repo.wstring(),L"--closed-old"},root,30,{},true);
        require(control.code==1&&control.err.find("Closed application did not receive successful build")!=std::string::npos,
            "Former closed-state behavior did not fail the delivery assertion: "+control.out+control.err);}
    std::cout<<"PASS closed runtime installation, no launch, unchanged/stale checks, rollback and old-behavior delivery failure\n";
}
void ordering(const fs::path& repo) {
    const auto root=temporary("activation-order"),scripts=root/L"scripts",state=root/L".build/collaboration";
    fs::create_directories(state);fs::create_directories(root/L"output/collaboration");
    copy(repo,root,{"scripts/build_candidate_batch.ps1"});
    const std::string batch(32,'b');
    write(state/wide(batch+".plan.json"),"{\"schemaVersion\":1,\"batchId\":\""+batch+"\",\"configuration\":\"Release\",\"buildRequired\":true}");
    write(scripts/L"build_candidate_artifacts.ps1",R"PS(function Publish-CandidateRuntime($Context,$Action,[scriptblock]$Check){
 [IO.File]::WriteAllText((Join-Path $Context.source 'published'),'saved before tests');[pscustomobject]@{id=('c'*32)}
})PS",true);
    write(scripts/L"build_activate.ps1",R"PS(function Invoke-CandidateRuntimeActivation($Root,$Run,[scriptblock]$Check){
 [IO.File]::WriteAllText((Join-Path $Root 'activated'),'switched before tests');[pscustomobject]@{status='activated'}
})PS",true);
    write(root/L"ordering.ps1",R"PS(param([int]$BuildCode=0)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/build_candidate_batch.ps1')
function Read-CandidateBatchBinding($Root,$Request){return @{} }
function Get-FileDigest($Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
function Read-EntryJson($Path){if([IO.File]::Exists($Path)){Get-Content -LiteralPath $Path -Raw|ConvertFrom-Json}}
function Write-CandidateJson($Value,$Path,[switch]$Immutable){$Value|ConvertTo-Json|Set-Content -LiteralPath $Path}
function Set-EntryRunControl($Root){}
function Invoke-EntryPreflight($Root,$Reload,$Close,$Configuration,$Log){return 0}
function Get-BuildInputIdentity($Root){[pscustomobject]@{digest='stable'}}
function Set-Field($Object,$Name,$Value){$Object|Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force}
function Write-RunRecord($Record){$Record|ConvertTo-Json -Depth 12|Set-Content (Join-Path $PSScriptRoot 'record.json')}
Add-Type @'
using System;using System.IO;
namespace SnowDesktop.Build {public static class Job {
 public static bool RunWasCancelled=false;public static int BuildCode;
 public static int RunLeasedCommand(string directory,string log,string command){File.WriteAllText(Path.Combine(directory,"built"),"attempt");return BuildCode;}
 public static int RunBatch(string directory,string log,string batch,bool build,bool reload,bool close){
  File.WriteAllText(Path.Combine(directory,"test-order"),File.Exists(Path.Combine(directory,"activated"))?"activated-before-tests":"not-activated");return 7;
 }
}}
'@
[SnowDesktop.Build.Job]::BuildCode=$BuildCode
$batch=('b'*32);$plan=Join-Path $PSScriptRoot ('.build/collaboration/'+$batch+'.plan.json')
$request=[pscustomobject]@{batch=$batch;id=('c'*32);planHash=(Get-FileDigest $plan)}
$context=[pscustomobject]@{source=$PSScriptRoot;output=(Join-Path $PSScriptRoot 'output')}
$record=[pscustomobject]@{inputStart=[pscustomobject]@{digest='stable'}}
$code=Invoke-CandidateBatchPipeline $PSScriptRoot $request $context $record
@{exitCode=$code;record=$record}|ConvertTo-Json -Depth 12
)PS",true);
    const auto failedBuild=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"ordering.ps1").wstring(),L"-BuildCode",L"9"},root,15);
    require(failedBuild.code==0&&Json::parse(failedBuild.out).at("exitCode").integer()==9&&
        !fs::exists(root/L"published")&&!fs::exists(root/L"activated")&&!fs::exists(root/L"test-order"),"Failed build published or switched the runtime");
    const auto failedTests=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"ordering.ps1").wstring()},root,15);
    require(failedTests.code==0,"Build/test boundary fixture failed: "+failedTests.out+failedTests.err);
    const auto result=Json::parse(failedTests.out);
    require(result.at("exitCode").integer()==7&&fs::exists(root/L"published")&&
        read(root/L"test-order")=="activated-before-tests"&&result.at("record").has("artifact")&&
        result.at("record").at("productBuild").at("exitCode").integer()==0,
        "Later test failure withheld or erased the successful build");
    std::cout<<"PASS build failure retains runtime; successful product publishes and activates before failing tests\n";
}
void retention(const fs::path& repo) {
    const auto root=temporary("runtime-retention"),state=root/L".build/collaboration";
    copy(repo,root,{"scripts/build_entry.ps1","scripts/build_protocol.ps1","scripts/build_inputs.ps1",
        "scripts/build_candidate.ps1","scripts/build_candidate_artifacts.ps1"});
    const std::string old(32,'a'),active(32,'b'),current(32,'c');
    const auto artifacts=root/L".build/candidate-artifacts",backups=root/L".build/activation-backups";
    const auto oldBackup=backups/wide(old+"-"+old),currentBackup=backups/wide(current+"-"+current);
    for(const auto& id:{old,active,current}){
        const auto directory=artifacts/wide(id),runtime=directory/L"runtime";
        write(runtime/L"SnowDesktop.exe",id);
        atomic(directory/L"artifact.json",Json::object({{"id",id},{"runId",id},{"publicationComplete",true},
            {"runtime",utf8(runtime.wstring())},{"productRuntime",true},
            {"createdUtc",id==current?"2026-10-31T03:00:00Z":"2026-10-31T02:00:00Z"}}).dump());
        atomic(state/L"runs"/wide(id+".json"),id==active?"{\"status\":\"running\",\"completedUtc\":null}":
            "{\"status\":\"passed\",\"completedUtc\":\"2026-10-31T03:00:00Z\"}");
    }
    write(oldBackup/L"old.dll","retired backup");write(currentBackup/L"old.dll","current rollback");
    atomic(state/L"activation-current.json",Json::object({{"runId",current},{"backup",utf8(currentBackup.wstring())}}).dump());
    write(root/L".build/Release/data/layout.json","personal layout");
    write(root/L".build/Release/data.debug/state.json","personal debug profile");
    write(root/L".build/candidate-slots/warm/output.obj","warm compiler output");
    write(root/L"outside/sentinel","outside cleanup scope");
    const auto activeLease=CreateFileW((state/L"runs"/wide(active+".owner.lock")).c_str(),GENERIC_READ|GENERIC_WRITE,
        0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(activeLease!=INVALID_HANDLE_VALUE,"Cannot hold active Run fixture lease");
    write(root/L"retention.ps1",R"PS(param([switch]$Preview,[switch]$Hold,[switch]$Link,[switch]$Unlink)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/build_entry.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_protocol.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_inputs.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_candidate.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_candidate_artifacts.ps1')
$linkPath=Join-Path $PSScriptRoot ('.build/candidate-artifacts/'+('a'*32)+'/runtime/escape')
if($Link){$null=New-Item -ItemType Junction -Path $linkPath -Target (Join-Path $PSScriptRoot 'outside')}
if($Unlink){Remove-Item -LiteralPath $linkPath -Force}
$reader=if($Hold){Open-CandidateRuntimeLease $PSScriptRoot}else{$null}
try {Invoke-CandidateRuntimeRetention $PSScriptRoot -Preview:$Preview|ConvertTo-Json -Depth 8}
finally {if($reader){$reader.Dispose()}}
)PS",true);
    const auto invoke=[&](const std::wstring& option=L""){
        std::vector<std::wstring> args{powershell().wstring(),L"-NoProfile",L"-File",(root/L"retention.ps1").wstring()};
        if(!option.empty())args.push_back(option);
        const auto result=run(args,root,20);require(result.code==0,"Retention failed: "+result.out+result.err);return Json::parse(result.out);
    };
    require(invoke(L"-Preview").at("status").str()=="preview"&&fs::exists(oldBackup/L"old.dll")&&
        fs::exists(artifacts/wide(old)/L"runtime/SnowDesktop.exe"),"Retention preview deleted files");
    require(invoke(L"-Hold").at("status").str()=="busy"&&fs::exists(oldBackup/L"old.dll"),
        "Retention ignored an active packaging/activation reader");
    const auto linked=invoke(L"-Link");
    require(linked.at("status").str()=="deferred"&&fs::exists(root/L"outside/sentinel")&&
        fs::exists(artifacts/wide(old)/L"runtime/SnowDesktop.exe"),"Retention traversed a junction or erased its evidence");
    require(invoke(L"-Unlink").at("status").str()=="completed"&&
        !fs::exists(artifacts/wide(old)/L"runtime")&&!fs::exists(oldBackup)&&
        fs::exists(artifacts/wide(old)/L"artifact.json")&&fs::exists(artifacts/wide(old)/L"retention.json")&&
        fs::exists(artifacts/wide(active)/L"runtime/SnowDesktop.exe")&&fs::exists(currentBackup/L"old.dll"),
        "Retention did not retire old bytes while preserving manifests, active Run and current rollback");
    atomic(state/L"runs"/wide(active+".json"),"{\"status\":\"passed\",\"completedUtc\":\"2026-10-31T04:00:00Z\"}");
    CloseHandle(activeLease);
    require(invoke().at("status").str()=="completed"&&!fs::exists(artifacts/wide(active)/L"runtime")&&
        fs::exists(artifacts/wide(current)/L"runtime/SnowDesktop.exe"),"Completed superseded Run was not pruned to one runtime");
    require(read(root/L".build/Release/data/layout.json")=="personal layout"&&
        read(root/L".build/Release/data.debug/state.json")=="personal debug profile"&&
        read(root/L".build/candidate-slots/warm/output.obj")=="warm compiler output"&&
        read(root/L"outside/sentinel")=="outside cleanup scope","Retention changed data, compiler workspace or outside files");
    std::cout<<"PASS single-runtime/rollback retention, preview, busy readers, active Run, junction rejection, manifests and user data\n";
}
}
int main(int argc,char** argv){
    if(argc==3&&std::string(argv[1])=="--activation-host"){try{return host(fs::path(wide(argv[2])));}catch(...){return 82;}}
    if(argc==3&&std::string(argv[2])=="--closed-old"){try{closed_activation(fs::path(wide(argv[1])),true);return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
    return test_main(argc,argv,[&](const fs::path& repo){activation(repo);closed_activation(repo);ordering(repo);retention(repo);});
}
