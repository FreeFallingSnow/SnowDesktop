#include "../build_tool_test_support.h"
#include <iostream>
#include <iterator>

using namespace build_test;
namespace {
fs::path fixture_parent(const fs::path& repo) {
    const auto result=run({L"git",L"rev-parse",L"--path-format=absolute",L"--git-common-dir"},repo);
    auto text=result.out;while(!text.empty()&&(text.back()=='\r'||text.back()=='\n'))text.pop_back();
    const auto common=fs::path(wide(text));
    return (result.code==0&&common.filename()==L".git"?common.parent_path():repo)/L".codex-probes/ce";
}
Json view(Coordinator& c,const std::string& id) {
    return c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","status","-RunId",id});
}
void candidate_until(Coordinator& c,const std::string& id,const std::function<bool()>& predicate,int seconds,const std::string& phase) {
    try{until(predicate,seconds);}
    catch(const std::exception& error){throw std::runtime_error(phase+": "+error.what()+"; Run observations: "+view(c,id).dump());}
}
std::string submit(Coordinator& c,std::vector<std::string> args,const Env& env={}) {
    args.insert(args.begin(),{"-Command","start","-Task","candidate"});
    const auto result=c.bridge->call(c.scripts/L"build_run.ps1",args,0,env);
    const auto id=result.at("id").str();const auto receipt=c.stateRoot/L"runs"/wide(id+".launch.json");
    until([&]{return fs::exists(receipt);},12);c.workers.push_back(Json::parse(read(receipt)));return id;
}
Json wait(Coordinator& c,const std::string& id,int code=0) {
    const auto result=c.bridge->invoke(c.scripts/L"build_run.ps1",{"-Command","wait","-Task","candidate","-RunId",id,"-Seconds","60"},{},70);
    require(result.code==code,"Candidate Run returned unexpected code: "+result.out+result.err);
    const auto value=Json::parse(result.out);
    require(value.at("terminal").boolean&&value.at("stageError").str().empty(),"Candidate Run lost terminal observations");return value;
}
std::string capture(Coordinator& c) {
    const auto result=wait(c,submit(c,{"-Action","snapshot"}));
    return result.at("record").at("candidate").at("id").str();
}
std::vector<std::string> execution(const std::string& snapshot,const std::string& action="ide") {
    std::vector<std::string> args={"-Action","candidate","-Snapshot",snapshot,"-CandidateAction",action,"-CandidateSlot","warm"};
    if(action=="ide")args.insert(args.end(),{"-Targets","SnowDesktopCandidateFixtureTests"});
    if(action=="tests")args.insert(args.end(),{"-Mode","name","-Filter","^candidate_rule$"});
    return args;
}
void program(const fs::path& output,int expected) {
    const auto result=run({(output/L"Release/tests/SnowDesktopCandidateFixtureTests.exe").wstring()},output);
    require(result.code==expected,"Candidate program has incorrect frozen inputs or stale outputs: expected "+std::to_string(expected)+", got "+std::to_string(result.code));
}
void prepare(Coordinator& c,const fs::path& repo,const std::string& control) {
    for(const auto& file:fs::directory_iterator(repo/L"scripts")){
        if(file.is_regular_file()){
            const auto destination=c.scripts/file.path().filename();
            if(fs::exists(destination)&&read(destination)==read(file.path()))continue;
            fs::copy_file(file.path(),destination,fs::copy_options::overwrite_existing);
        }
    }
    copy(repo,c.root,{"cmake/SnowDesktop.SharedBuildGuard.cmake","cmake/SnowDesktop.PowerShell.cmake"});
    // Optional UI service is outside this execution fixture's resources.
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+
        "\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='Candidate fixture has no dashboard'}}\n",true);
    if(control.empty()||control=="poll-reparse"){
        // Observe the real readers and file-lock attempts without replacing them.
        auto script=read(c.scripts/L"build_candidate.ps1");
        replace(script,"    $store=if($StoreContext){$StoreContext}else{Get-CandidateStore $Root}",
            "    if($env:CANDIDATE_FIXTURE_PROBE){[IO.File]::AppendAllText((Join-Path $env:CANDIDATE_FIXTURE_PROBE ($PID.ToString()+'.reads')),\"manifest`n\")}\n"
            "    $store=if($StoreContext){$StoreContext}else{Get-CandidateStore $Root}");
        replace(script,"function Enter-CandidateSlot($Snapshot,[string]$Name) {",
            "function Enter-CandidateSlot($Snapshot,[string]$Name) {\n"
            "    if($env:CANDIDATE_FIXTURE_PROBE){[IO.File]::AppendAllText((Join-Path $env:CANDIDATE_FIXTURE_PROBE ($PID.ToString()+'.reads')),\"slot`n\")}");
        write(c.scripts/L"build_candidate.ps1",script,true);
    }
    if(control=="poll-reparse"){
        auto script=read(c.scripts/L"build_candidate_execution.ps1");
        replace(script,"$snapshot=if($SnapshotForSlot){$SnapshotForSlot}else{Get-CandidateExecutionSnapshot $Root $Request}",
            "$snapshot=Get-CandidateExecutionSnapshot $Root $Request");
        write(c.scripts/L"build_candidate_execution.ps1",script,true);
    }else if(control=="checkout-lock"){
        auto script=read(c.scripts/L"build_run.ps1");
        replace(script,"$candidateExecution=Enter-CandidateExecution $root $candidateRequest -RequireExisting:$candidateVerification",
            "$originalEntry=Enter-BuildEntry $root; $candidateExecution=Enter-CandidateExecution $root $candidateRequest -RequireExisting:$candidateVerification");
        replace(script,"$runnerLocks=@();$stage=$null","$runnerLocks=@();$stage=$null;$originalEntry=$null");
        replace(script,"foreach($stream in $runnerLocks){$stream.Dispose()}","foreach($stream in $runnerLocks){$stream.Dispose()}; if($originalEntry){Exit-BuildEntry $originalEntry}");
        write(c.scripts/L"build_run.ps1",script,true);
    }else if(control=="unlocked-source"){
        auto script=read(c.scripts/L"build_candidate_execution.ps1");
        replace(script,"if($file.state -eq 'present'){$inputLocks+=@([IO.File]::Open($path,'Open','Read',[IO.FileShare]::Read))}","# Controlled former executor holds no source handles.");
        write(c.scripts/L"build_candidate_execution.ps1",script,true);
    }else if(control=="artifacts-slot-reference"){
        auto script=read(c.scripts/L"build_candidate_artifacts.ps1");
        // Preserve the actual copy and manifest. Only reproduce returning the
        // mutable compiler slot to the caller as its acceptance program.
        replace(script,"runtime=$payload;profile=$selection.profile;files=$records.Count",
            "runtime=$selection.root;profile=$selection.profile;files=$records.Count");
        write(c.scripts/L"build_candidate_artifacts.ps1",script,true);
    }else if(control=="verification-default-preset"){
        auto script=read(c.scripts/L"test_manager.ps1");
        replace(script,"-TestPreset $(if($ExecutionOnly){'all-tests'}else{$TestPreset})","-TestPreset $TestPreset");
        write(c.scripts/L"test_manager.ps1",script,true);
    }else if(control=="batches-held-window"){
        auto script=read(c.scripts/L"build_candidate_batch.ps1");
        replace(script,"Set-Field $state 'activeCandidates' (@($active)+@($batch));$state.current=$null",
            "# Controlled old behavior retains the original frozen editing window during compilation.");
        write(c.scripts/L"build_candidate_batch.ps1",script,true);
    }else{require(control.empty(),"Unknown executor negative control");}
    write(c.root/L".gitignore",read(c.root/L".gitignore")+"!/CMakeLists.txt\n!/cmake/\n");
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(CandidateExecutorFixture LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
include(cmake/SnowDesktop.SharedBuildGuard.cmake)
snowdesktop_check_shared_output()
enable_testing()
option(BUILD_TESTING "Build fixture tests" ON)
add_executable(SnowDesktopCandidateFixtureTests src/main.cpp)
target_compile_features(SnowDesktopCandidateFixtureTests PRIVATE cxx_std_20)
target_compile_options(SnowDesktopCandidateFixtureTests PRIVATE /W4 /WX /Z7)
set_target_properties(SnowDesktopCandidateFixtureTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
add_custom_target(CandidateGate COMMAND "${SNOWDESKTOP_POWERSHELL_EXECUTABLE}" -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/candidate_gate.ps1" VERBATIM)
add_dependencies(SnowDesktopCandidateFixtureTests CandidateGate)
add_test(NAME candidate_rule COMMAND SnowDesktopCandidateFixtureTests check)
set_tests_properties(candidate_rule PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopCandidateFixtureTests>" LABELS "core")
snowdesktop_install_shared_guard()
)CMAKE");
    write(c.root/L"CMakePresets.json",R"JSON({"version":3,"configurePresets":[{"name":"release","generator":"Visual Studio 18 2026","architecture":"x64","binaryDir":"${sourceDir}/.build"},{"name":"tests","inherits":"release","cacheVariables":{"BUILD_TESTING":"ON"}}],"buildPresets":[{"name":"release","configurePreset":"release","configuration":"Release","jobs":2},{"name":"tests","configurePreset":"tests","configuration":"Release","jobs":2}],"testPresets":[{"name":"all-tests","configurePreset":"tests","configuration":"Release","execution":{"jobs":2,"noTestsAction":"error"}},{"name":"tests","inherits":"all-tests"}]})JSON");
    write(c.root/L"src/value.h","#pragma once\ninline constexpr int value=7;\n");
    write(c.root/L"src/main.cpp","#include \"value.h\"\nint main(int argc,char**){return argc>1 ? (value==7 ? 0 : 1) : value;}\n");
    write(c.scripts/L"candidate_gate.ps1",R"PS($ErrorActionPreference='Stop'
$gate=[Environment]::GetEnvironmentVariable('CANDIDATE_FIXTURE_GATE','Process')
if($gate -and [IO.File]::Exists($gate)){
 $marker=$gate+'.ready';$pending=$marker+'.'+[Guid]::NewGuid().ToString('N')+'.pending'
 try{
  [IO.File]::WriteAllText($pending,(@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()}|ConvertTo-Json))
  [IO.File]::Move($pending,$marker)
 }finally{if([IO.File]::Exists($pending)){[IO.File]::Delete($pending)}}
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while([IO.File]::Exists($gate)){if($clock.Elapsed.TotalSeconds -gt 50){throw 'Candidate fixture gate expired'};Start-Sleep -Milliseconds 40}
}
)PS",true);
    require(c.git({L"add",L"--all"}).code==0&&c.git({L"commit",L"-m",L"Candidate executor fixture baseline"}).code==0,"Cannot establish candidate executor baseline");
    c.call({"task-open","candidate","-TaskName","Candidate execution acceptance fixture"});
}
void private_preflight(const fs::path& repo,bool oldPath,bool modulePathOnly=false) {
    const auto root=temporary("candidate-preflight",fixture_parent(repo));
    copy(repo,root,{"scripts/build_entry.ps1","scripts/build_preflight.ps1","scripts/test_manager.ps1"});
    if(modulePathOnly){
        auto manager=read(root/L"scripts/test_manager.ps1");
        const std::string needle="$path = Get-PreflightProcessPath $process";
        const auto location=manager.find(needle);
        require(location!=std::string::npos,"Missing production test-runtime process path observer for original-fault control");
        manager.replace(location,needle.size(),"$path = $process.Path");
        write(root/L"scripts/test_manager.ps1",manager);
    }
    const auto source=root/L"source",output=root/L"output",neighbor=root/L"output-neighbor";
    fs::create_directories(source);fs::create_directories(output/L"Release");fs::create_directories(output/L"Debug");fs::create_directories(neighbor/L"Release");
    for(const auto& destination:{output/L"Release/SnowDesktop.exe",output/L"Debug/SnowDesktop.exe",neighbor/L"Release/SnowDesktop.exe"})
        fs::copy_file(standin(),destination);
    for(const auto& name:{L".build",L".build_debug"}){
        const auto result=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("New-Item -ItemType Junction -Path "+ps_literal(source/name)+" -Target "+ps_literal(output)+" -ErrorAction Stop | Out-Null")},root);
        require(result.code==0,"Cannot establish private output junction: "+result.out+result.err);
    }
    std::string query=R"PS(param([string]$ProbeRoot,[string]$ProbeConfiguration='Release',[uint32]$HideModulePathForPid=0)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/build_preflight.ps1')
function Get-Process {
 [CmdletBinding()]param([string[]]$Name)
 foreach($process in @(Microsoft.PowerShell.Management\Get-Process -Name $Name -ErrorAction SilentlyContinue)){
  if($process.Id -eq $HideModulePathForPid){
   [pscustomobject]@{Id=$process.Id;StartTime=$process.StartTime;Path=$null;ProcessName=$process.ProcessName}
  }else{$process}
 }
}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'scripts/test_manager.ps1'),[ref]$tokens,[ref]$errors)
$definition=$ast.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-HostRuntimeLocks'},$false)
. ([scriptblock]::Create($definition.Extent.Text))
$repositoryRoot=$ProbeRoot
)PS";
    if(oldPath)query+=R"PS(function Get-BuildOutputDirectories([string]$Root,[string]$Configuration='Release'){
 $build=if($Configuration -eq 'Debug'){'.build_debug'}else{'.build'}
 @([IO.Path]::GetFullPath((Join-Path (Join-Path $Root $build) $Configuration)))
}
)PS";
    query+="$observation=Get-ReadOnlyPreflight $ProbeRoot $ProbeConfiguration\n@{preflight=$observation;locks=@(Get-HostRuntimeLocks)}|ConvertTo-Json -Depth 8\n";
    write(root/L"query.ps1",query,true);Bridge bridge(repo,root);
    auto probe=[&](const std::string& configuration="Release",DWORD hiddenModulePath=0){
        return bridge.call(root/L"query.ps1",{"-ProbeRoot",utf8(source.wstring()),"-ProbeConfiguration",configuration,
            "-HideModulePathForPid",std::to_string(hiddenModulePath)});
    };
    auto contains=[&](const Json& value,DWORD pid){
        for(const auto& entry:value.at("preflight").at("owners").items)if(entry.at("pid").integer()==static_cast<int>(pid))return true;
        return false;
    };
    auto locksContain=[](const Json& value,DWORD pid){
        for(const auto& entry:value.at("locks").items)if(entry.str().find("PID "+std::to_string(pid))!=std::string::npos)return true;
        return false;
    };
    const auto otherMarker=root/L"other-ready",releaseMarker=root/L"release-ready",debugMarker=root/L"debug-ready",aliasMarker=root/L"alias-ready";
    Child other({(neighbor/L"Release/SnowDesktop.exe").wstring(),L"--wait",L"--marker",otherMarker.wstring()},root);
    until([&]{return fs::exists(otherMarker);});require(!contains(probe(),other.pid()),"Private preflight included a neighboring output owner");
    const auto hiddenNeighbour=probe("Release",other.pid());
    require(!contains(hiddenNeighbour,other.pid())&&!locksContain(hiddenNeighbour,other.pid()),
        "Unavailable module Path on an unrelated process must not block private test runtime");
    Child release({(output/L"Release/SnowDesktop.exe").wstring(),L"--wait",L"--marker",releaseMarker.wstring()},root);
    until([&]{return fs::exists(releaseMarker);});const auto physical=probe();
    require(contains(physical,release.pid())&&physical.at("preflight").at("status").str()!="clear",
        "Private output preflight falsely reported the physical application as clear");
    require(locksContain(physical,release.pid())&&!locksContain(physical,other.pid()),"Test runtime ownership missed the physical output or included its neighbor");
    const auto hiddenPhysical=probe("Release",release.pid());
    require(contains(hiddenPhysical,release.pid())&&locksContain(hiddenPhysical,release.pid()),
        "Unavailable module Path must retain a real private-output owner through its kernel image identity");
    require(!contains(probe("Debug"),release.pid()),"Debug preflight included a Release application");
    Child debug({(output/L"Debug/SnowDesktop.exe").wstring(),L"--wait",L"--marker",debugMarker.wstring()},root);
    until([&]{return fs::exists(debugMarker);});require(contains(probe("Debug"),debug.pid())&&!contains(probe(),debug.pid()),"Private Debug/Release ownership crossed configurations");
    Child alias({(source/L".build/Release/SnowDesktop.exe").wstring(),L"--wait",L"--marker",aliasMarker.wstring()},root);
    until([&]{return fs::exists(aliasMarker);});require(contains(probe(),alias.pid()),"Private preflight missed an application launched through the logical alias");
    const auto legacy=run({powershell(true).wstring(),L"-NoProfile",L"-File",(root/L"query.ps1").wstring(),L"-ProbeRoot",source.wstring()},root);
    require(legacy.code==0&&contains(Json::parse(legacy.out),release.pid()),"Windows PowerShell physical output observation failed: "+legacy.out+legacy.err);
    for(const auto& marker:{releaseMarker,debugMarker,aliasMarker})write(marker.wstring()+L".stop","stop only this fixture process");
    require(release.wait().code==0&&debug.wait().code==0&&alias.wait().code==0&&other.running(),"Read-only preflight changed unrelated process lifetime");
    require(!contains(probe(),release.pid())&&!contains(probe("Debug"),debug.pid()),"Exited private output remained occupied");
    write(otherMarker.wstring()+L".stop","stop fixture neighbor");require(other.wait().code==0,"Fixture neighbor did not exit");
    if(!oldPath){
        Coordinator c(repo,"candidate-owner",true,fixture_parent(repo));prepare(c,repo,"");const auto snapshot=capture(c);
        const auto slot=c.bridge->call(c.scripts/L"build_candidate.ps1",{"-Command","materialize","-Task","candidate","-Snapshot",snapshot,"-Slot","warm"});
        const auto privateOutput=fs::path(wide(slot.at("output").str()));fs::create_directories(privateOutput/L"Release");
        fs::copy_file(standin(),privateOutput/L"Release/SnowDesktop.exe");const auto heldMarker=c.stateRoot/L"held-candidate";
        Child held({(privateOutput/L"Release/SnowDesktop.exe").wstring(),L"--wait",L"--marker",heldMarker.wstring()},c.root);
        until([&]{return fs::exists(heldMarker);});const auto blocked=wait(c,submit(c,execution(snapshot,"release")),3);
        require(blocked.at("status").str()=="failed"&&held.running(),"Candidate Release preflight did not block before altering an occupied application");
        for(const auto& event:blocked.at("stageEvents").items)
            require(!event.has("name")||event.at("name").str()!="configure","Occupied candidate reached configuration before preflight");
        write(heldMarker.wstring()+L".stop","stop fixture candidate");require(held.wait().code==0,"Held candidate fixture did not exit");
    }
    if(!oldPath){
        const auto control=run({executable().wstring(),repo.wstring(),L"--preflight-old-path"},root,35);
        require(control.code==1&&control.err.find("Private output preflight falsely reported")!=std::string::npos,"Old logical-only preflight did not reach the ownership assertion: "+control.out+control.err);
    }
    std::cout<<"PASS physical/logical candidate output owners, Release/Debug separation, neighbor exclusion, PowerShell7/5.1 and original-path negative control\n";
}
void candidate_batches(const fs::path& repo,bool heldWindow,bool verificationOnly=false,bool oldPreset=false) {
    Coordinator c(repo,"candidate-batches",true,fixture_parent(repo));
    prepare(c,repo,heldWindow?"batches-held-window":(oldPreset?"verification-default-preset":""));
    if(verificationOnly){
        auto cmake=read(c.root/L"CMakeLists.txt");replace(cmake,"LABELS \"core\"","LABELS \"tools\"");write(c.root/L"CMakeLists.txt",cmake);
        auto presets=read(c.root/L"CMakePresets.json");
        replace(presets,"\"name\":\"tests\",\"inherits\":\"all-tests\"","\"name\":\"tests\",\"inherits\":\"all-tests\",\"filter\":{\"exclude\":{\"label\":\"tools\"}}");
        write(c.root/L"CMakePresets.json",presets);
        c.git({L"add",L"--",L"CMakeLists.txt",L"CMakePresets.json"});
        c.git({L"commit",L"-m",L"Tool-filtered verification fixture baseline"});
    }
    auto begin=[&](const std::string& task){return c.bridge->call(c.scripts/L"build_manager.ps1",{"begin",task});};
    const auto empty=begin("empty-requirement").at("batchId").str();
    c.plan("empty-requirement",empty,0,"none","","docs","src/value.h");
    const auto exempt=c.bridge->call(c.scripts/L"build_manager.ps1",{"ready-and-wait","empty-requirement","-Batch",empty,"-Revision","0","-WaitSeconds","15"});
    require(exempt.at("outcome").str()=="skipped"&&exempt.at("runId").kind==Json::Kind::Null&&exempt.at("commands").items.empty(),
        "Explicit empty requirements captured or launched an unnecessary candidate");
    const auto first=begin("candidate"),peer=begin("peer");const auto batch=first.at("batchId").str();
    require(peer.at("batchId").str()==batch&&c.state().at("current").at("protocolVersion").integer()==4,"New batch did not use the candidate protocol");
    c.plan("candidate",batch,0,"selected","candidate_rule","tool","src/value.h");
    c.plan("peer",batch,0,"none","","docs","src/value.h");
    auto ready=[&](const std::string& task,const std::string& id,const Env& env={}){
        const auto result=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait",task,"-Batch",id,"-Revision","0","-WaitSeconds","1"},env,15);
        require(result.code==2,"Fixture ready observer unexpectedly terminated its running candidate: "+result.out+result.err);
    };
    ready("candidate",batch);
    const auto outsider=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"begin","outsider","-WaitSeconds","1"});
    require(outsider.code==2&&c.state().at("current").at("participants").items.size()==2,
        "A new editor extended a cohort after its first member became ready");
    const auto gate=c.stateRoot/L"v4-gate";write(gate,"hold");
    const Env env={{L"CANDIDATE_FIXTURE_GATE",gate.wstring()}};
    ready("peer",batch,env);
    auto runId=[&](const std::string& id){
        std::string found;
        until([&]{
            if(fs::exists(c.stateRoot/wide(id+".json"))){found=c.result(id).at("runId").str();return !found.empty();}
            const auto state=c.state();
            if(state.at("current").kind==Json::Kind::Object&&state.at("current").at("id").str()==id&&state.at("current").has("runId"))found=state.at("current").at("runId").str();
            if(state.has("activeCandidates"))for(const auto& item:state.at("activeCandidates").items)if(item.at("id").str()==id)found=item.at("runId").str();
            return !found.empty();
        },15);
        const auto receipt=c.stateRoot/L"runs"/wide(found+".launch.json");until([&]{return fs::exists(receipt);},12);
        c.workers.push_back(Json::parse(read(receipt)));return found;
    };
    const auto firstRun=runId(batch);
    until([&]{return fs::exists(gate.wstring()+L".ready")||view(c,firstRun).at("terminal").boolean;},35);
    require(fs::exists(gate.wstring()+L".ready"),"Candidate batch never reached its real private build child");
    const auto editing=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"begin","next","-WaitSeconds","1"});
    require(editing.code==0,"Source capture did not release the editing window while its candidate compiled");
    if(heldWindow)return;
    const auto next=Json::parse(editing.out);const auto nextBatch=next.at("batchId").str();
    c.call({"claim","next","-Batch",nextBatch,"-Revision","0","-Files","src/value.h,CMakePresets.json"});
    write(c.root/L"src/value.h","#pragma once\ninline constexpr int value=11;\n");
    fs::remove(gate);wait(c,firstRun);
    const auto state=c.state(),result=c.result(batch);
    require(state.at("current").at("id").str()==nextBatch&&state.at("current").at("participants").items.front().at("state").str()=="editing",
        "An old candidate result retired the newer editing window");
    require(result.at("outcome").str()=="passed"&&result.at("inputCheck").str()=="stable"&&result.at("candidateBinding").at("runId").str()==firstRun,
        "Original checkout edits invalidated or rebound the frozen batch result");
    const auto saved=fs::path(wide(result.at("artifact").at("runtime").str()))/L"tests/SnowDesktopCandidateFixtureTests.exe";
    require(run({saved.wstring()},c.root).code==7,"Candidate batch did not preserve its passing program");
    const auto candidateOutput=fs::path(wide(result.at("candidateExecution").at("output").str()));
    const auto candidateProgram=candidateOutput/L"Release/tests/SnowDesktopCandidateFixtureTests.exe";
    const auto compiledTime=fs::last_write_time(candidateProgram);
    const auto originalPresets=read(c.root/L"CMakePresets.json");
    write(c.root/L"CMakePresets.json","An editor has not finished the next preset yet\n");
    const auto verified=wait(c,submit(c,{"-Action","tests","-Mode","verify","-PlanBatch",batch,"-Filter","candidate_rule","-Reason","Verify the captured candidate while the original checkout continues editing"}),oldPreset?1:0);
    write(c.root/L"CMakePresets.json",originalPresets);
    require(verified.at("status").str()=="passed"&&verified.at("record").at("verification").at("noBuild").boolean&&fs::last_write_time(candidateProgram)==compiledTime,
        "No-build candidate verification did not execute its selected tooling test or rebuilt its program");
    bool executed=false;
    for(const auto& event:verified.at("stageEvents").items){if(!event.has("name"))continue;const auto name=event.at("name").str();if(name=="test-execute")executed=true;require(name!="configure"&&name!="test-target-build"&&name!="arrange"&&name!="compile",
        "Execution-only candidate verification configured, built or arranged outputs");}
    require(executed&&verified.at("record").at("evidence").at("tests").items.size()==1&&
        verified.at("record").at("evidence").at("tests").items.front().at("attempts").items.front().at("report").str()!=result.at("coverage").at("report").str(),
        "Candidate verification reused an old report instead of executing the selected test");
    if(verificationOnly){
        const auto control=run({executable().wstring(),repo.wstring(),L"--tool-verification-old-preset"},c.root,100,{},true);
        require(control.code==1&&control.err.find("No-build candidate verification did not execute its selected tooling test")!=std::string::npos,
            "Default-preset control did not reach the tooling execution assertion: "+control.out+control.err);
        std::cout<<"PASS tool-labeled candidate no-build verification, unchanged binary, actual fresh report and default-preset control\n";return;
    }
    auto revision=[&](){const auto tasks=c.call({"task-list"});for(const auto& task:tasks.at("tasks").items)if(task.at("id").str()=="candidate")return std::to_string(task.at("revision").integer());throw std::runtime_error("Missing candidate task");};
    const auto acceptance=c.call({"acceptance-start","candidate","-Batch",batch,"-Revision","0","-TaskRevision",revision(),
        "-AcceptanceCriterion","Saved candidate returns7 while the live source contains11","-Files",".build/Release/tests/SnowDesktopCandidateFixtureTests.exe"}).at("acceptance");
    c.call({"acceptance-complete","candidate","-TaskRevision",revision(),"-AcceptanceId",acceptance.at("id").str(),"-Reporter","agent",
        "-Reference","native-fixture:captured-batch-runtime","-Notes","Executed saved7 after editing live source11; fixture evidence only"});
    c.plan("next",nextBatch,0,"selected","candidate_rule","tool","src/value.h");ready("next",nextBatch);
    const auto secondRun=runId(nextBatch);wait(c,secondRun,1);
    require(c.result(nextBatch).at("outcome").str()=="failed"&&run({saved.wstring()},c.root).code==7,
        "New candidate failure changed an older passing program or lost the actual failure");
    const auto stale=c.bridge->invoke(c.scripts/L"build_run.ps1",{"-Command","start","-Task","candidate","-Action","tests","-Mode","verify","-PlanBatch",batch,"-Filter","candidate_rule","-Reason","Do not rewind a compiler slot to verify a stale candidate"});
    require(stale.code==2&&stale.err.find("Verification candidate slot changed")!=std::string::npos,
        "Verification accepted a reused slot or failed for an unrelated reason: "+stale.out+stale.err);
    program(candidateOutput,11);
    Child unrelated({standin().wstring(),L"--wait",L"--marker",(c.stateRoot/L"unrelated-v4-process").wstring()},c.root);
    for(const auto& mode:{std::string("cancel"),std::string("owner-loss")}){
        const auto task="lifecycle-"+mode,opened=begin(task).at("batchId").str();
        c.call({"claim",task,"-Batch",opened,"-Revision","0","-Files","src/value.h","-AdoptExistingChanges"});
        write(c.root/L"src/value.h","#pragma once\ninline constexpr int value=7;\n");
        c.plan(task,opened,0,"selected","candidate_rule","tool","src/value.h");
        fs::remove(gate.wstring()+L".ready");write(gate,"hold");ready(task,opened,env);
        const auto id=runId(opened);
        until([&]{return fs::exists(gate.wstring()+L".ready")||view(c,id).at("terminal").boolean;},35);
        require(fs::exists(gate.wstring()+L".ready"),"Lifecycle candidate batch did not reach its owned child");
        const auto child=Json::parse(read(gate.wstring()+L".ready"));
        if(mode=="cancel"){
            c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","cancel","-Task",task,"-RunId",id});wait(c,id,4);
            require(c.result(opened).at("outcome").str()=="cancelled","Candidate cancellation lost the shared cancelled outcome");
        }else{
            stop_owner(view(c,id).at("record").at("owner"));until([&]{return view(c,id).at("status").str()=="interrupted";});
            const auto after=begin("after-owner-loss").at("batchId").str();
            const auto unrelatedLease=CreateFileW((c.stateRoot/L"build.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            require(unrelatedLease!=INVALID_HANDLE_VALUE,"Cannot hold unrelated original-output lease");
            const auto recovered=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"recover","-Batch",opened,"-ConfirmStopped","-Reason","Fixture owner exited; preserve captured source and interrupted result"});
            CloseHandle(unrelatedLease);
            require(recovered.code==0&&c.state().at("current").at("id").str()==after&&c.result(opened).at("outcome").str()=="interrupted",
                "Interrupted candidate recovery depended on an unrelated output lease or cleared a newer editing window");
        }
        until([&]{return !owner_alive(child);});require(unrelated.running(),"Candidate cleanup terminated an unrelated process");fs::remove(gate);
    }
    const auto control=run({executable().wstring(),repo.wstring(),L"--batches-held-window"},c.root,80,{},true);
    require(control.code==1&&control.err.find("Source capture did not release the editing window while its candidate compiled")!=std::string::npos,
        "Frozen-window control did not reach the original editing-blocked assertion: "+control.out+control.err);
    std::cout<<"PASS v4 cohort sealing, source capture, editing/build overlap, isolated result retirement, saved batch acceptance, failed next inputs and held-window control\n";
}
void artifacts(const fs::path& repo,bool slotReference,bool inputDenied=false) {
    Coordinator c(repo,"candidate-artifacts",true,fixture_parent(repo));
    prepare(c,repo,slotReference?"artifacts-slot-reference":"");
    if(inputDenied){auto script=read(c.scripts/L"build_acceptance.ps1");replace(script,"# Preserve the pre-v4 input-acceptance contract for linked tools.","throw 'Former v4 only accepts exported output files.'");write(c.scripts/L"build_acceptance.ps1",script,true);}
    write(c.root/L"scripts/plugin.js","export const version=1;\n");
    const auto snapshot=capture(c),id=submit(c,execution(snapshot,"tests"));
    const auto first=wait(c,id),record=first.at("record");
    const auto& saved=record.at("artifact");
    const auto runtime=fs::path(wide(saved.at("runtime").str()));
    const auto executableFile=runtime/L"tests/SnowDesktopCandidateFixtureTests.exe";
    require(saved.at("profile").str()=="test-files"&&saved.at("publicationComplete").boolean&&
        !saved.at("reuseAllowed").boolean&&!saved.at("productRuntime").boolean,
        "Selected test files claimed a complete product or reusable test result");
    require(run({executableFile.wstring()},c.root).code==7,"Saved candidate program did not retain its initial inputs");
    const auto editing=c.begin("editor");const auto batch=editing.at("batchId").str();
    c.call({"claim","editor","-Batch",batch,"-Revision","0","-Files","src/value.h"});
    write(c.root/L"src/value.h","#pragma once\ninline constexpr int value=11;\n");
    c.plan("editor",batch,0,"none","","tool","src/value.h");
    c.call({"ready-and-wait","editor","-Batch",batch,"-Revision","0","-WaitSeconds","30"});
    const auto second=wait(c,submit(c,execution(capture(c))));
    const auto output=fs::path(wide(second.at("record").at("candidateExecution").at("output").str()));
    program(output,11);
    require(run({executableFile.wstring()},c.root).code==7,
        "Saved candidate program changed with reused compiler slot");
    if(slotReference)return;
    const auto inspected=c.bridge->call(c.scripts/L"build_candidate.ps1",{"-Command","artifact","-Artifact",id});
    require(inspected.at("candidate").str()==snapshot&&inspected.at("runtime").str()==saved.at("runtime").str()&&
        inspected.at("sourceDigest").str()==record.at("inputEnd").at("digest").str(),"Artifact inspector lost the producing source/program identity");
    auto taskRevision=[&](){
        const auto tasks=c.call({"task-list"});
        for(const auto& task:tasks.at("tasks").items)if(task.at("id").str()=="candidate")return std::to_string(task.at("revision").integer());
        throw std::runtime_error("Missing candidate task");
    };
    auto start=[&](int code=0){return c.call({"acceptance-start","candidate","-CandidateRun",id,"-TaskRevision",taskRevision(),
        "-AcceptanceCriterion","Saved executable still returns its captured value7 after the slot builds value11",
        "-Files","tests/SnowDesktopCandidateFixtureTests.exe"},code);};
    auto complete=[&](const std::string& acceptance,const std::string& outcome,int code=0){
        return c.call({"acceptance-complete","candidate","-TaskRevision",taskRevision(),"-AcceptanceId",acceptance,
            "-AcceptanceOutcome",outcome,"-Reporter","agent","-Reference","native-fixture:saved-executable-exit-code",
            "-Notes","Executed saved program and observed7 while the reused compiler output returned11; fixture evidence only"},code);
    };
    const auto started=start().at("acceptance");
    require(started.at("editRevision").kind==Json::Kind::Null&&started.at("batchId").kind==Json::Kind::Null&&
        started.at("candidate").str()==snapshot&&started.at("runId").str()==id,"Standalone acceptance invented an editing grant or lost the candidate");
    require(complete(started.at("id").str(),"passed").at("acceptance").at("status").str()=="passed",
        "Unrelated live checkout edits invalidated saved-program acceptance");
    complete(started.at("id").str(),"passed",2);
    auto sourceStart=[&](int code=0){const auto result=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"acceptance-start","candidate","-CandidateRun",id,"-TaskRevision",taskRevision(),
        "-AcceptanceCriterion","Actual linked tool source matches its passed candidate","-Files","scripts/plugin.js"});
        require(result.code==code,"Candidate input could not start acceptance with its original identity: "+result.out+result.err);return result.code?Json{}:Json::parse(result.out);};
    const auto source=sourceStart().at("acceptance");
    require(source.at("artifacts").items.front().at("sourceAssociation").str()=="candidate input"&&source.at("candidate").str()==snapshot,
        "Linked tool acceptance was confused with an exported test executable");
    require(complete(source.at("id").str(),"passed").at("acceptance").at("status").str()=="passed","Unrelated source edits invalidated the unchanged linked tool input");
    write(c.root/L"scripts/plugin.js","export const version=2;\n");sourceStart(2);write(c.root/L"scripts/plugin.js","export const version=1;\n");
    const auto changedSource=sourceStart().at("acceptance").at("id").str();
    write(c.root/L"scripts/plugin.js","export const version=2;\n");complete(changedSource,"passed",2);complete(changedSource,"cancelled");write(c.root/L"scripts/plugin.js","export const version=1;\n");
    if(!inputDenied){const auto denied=run({executable().wstring(),repo.wstring(),L"--artifacts-input-denied"},c.root,100,{},true);
        require(denied.code==1&&denied.err.find("Candidate input could not start acceptance")!=std::string::npos,
            "Output-only control did not reach the linked-source acceptance assertion: "+denied.out+denied.err);}
    const auto tampered=start().at("acceptance").at("id").str();
    write(executableFile,read(executableFile)+"changed");
    c.bridge->call(c.scripts/L"build_candidate.ps1",{"-Command","artifact","-Artifact",id},2);
    complete(tampered,"passed",2);complete(tampered,"cancelled");program(output,11);
    c.call({"task-stage","candidate","-TaskRevision",taskRevision(),"-RequirementsRevision","1","-Progress","Fixture requirement changed"});
    start(2);
    const auto control=run({executable().wstring(),repo.wstring(),L"--artifacts-slot-reference"},c.root,100,{},true);
    require(control.code==1&&control.err.find("Saved candidate program changed with reused compiler slot")!=std::string::npos,
        "Mutable output negative control did not reach the observed program failure: "+control.out+control.err);
    std::cout<<"PASS saved runtime survives slot reuse, Run acceptance survives source edits, stale requirements and corruption reject, mutable-output control fails\n";
}
void scenario(const fs::path& repo,const std::string& control) {
    Coordinator c(repo,"x",true,fixture_parent(repo));prepare(c,repo,control);
    const auto snapshot=capture(c);
    const auto pendingSnapshot=control.empty()?capture(c):std::string{};
    const auto gate=c.stateRoot/L"candidate-gate";write(gate,"hold");
    const auto probe=c.stateRoot/L"candidate-read-probe";fs::create_directories(probe);
    const Env env={{L"CANDIDATE_FIXTURE_GATE",gate.wstring()},{L"CANDIDATE_FIXTURE_PROBE",probe.wstring()}};
    const auto first=submit(c,execution(snapshot),env);
    candidate_until(c,first,[&]{return fs::exists(gate.wstring()+L".ready")||view(c,first).at("terminal").boolean;},40,"Candidate configure/build gate was not reached");
    require(fs::exists(gate.wstring()+L".ready"),"Actual candidate configure/build did not reach its private child: "+view(c,first).dump());
    const auto running=view(c,first);const auto context=running.at("record").at("candidateExecution");
    const auto source=fs::path(wide(context.at("source").str())),output=fs::path(wide(context.at("output").str()));
    require(running.at("status").str()=="running"&&running.at("request").has("runnerRuntime"),"Candidate did not retain a frozen runtime association");
    const auto begin=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"begin","editor","-WaitSeconds","1"});
    require(begin.code==0,"Original checkout editing remained blocked by candidate execution");
    const auto edit=Json::parse(begin.out);const auto batch=edit.at("batchId").str();
    c.call({"claim","editor","-Batch",batch,"-Revision","0","-Files","src/value.h,scripts/build_command.ps1"});
    write(c.root/L"src/value.h","#pragma once\ninline constexpr int value=11;\n");
    const auto second=submit(c,execution(snapshot),env);
    candidate_until(c,second,[&]{const auto value=view(c,second);return value.at("record").at("waitingFor").str().find("Candidate slot is busy")!=std::string::npos||value.at("terminal").boolean;},15,"Second candidate did not report its occupied slot");
    require(view(c,second).at("status").str()=="queued","Same candidate slot admitted overlapping executors");
    if(control.empty()||control=="poll-reparse"){
        const auto owner=view(c,second).at("record").at("owner").at("pid").integer();
        const auto reads=probe/wide(std::to_string(owner)+".reads");
        const auto count=[&](const std::string& name){
            if(!fs::exists(reads))return std::size_t{0};
            const auto text=read(reads);std::size_t result=0,offset=0;
            while((offset=text.find(name+"\n",offset))!=std::string::npos){++result;offset+=name.size()+1;}
            return result;
        };
        candidate_until(c,second,[&]{return count("slot")>=4||view(c,second).at("terminal").boolean;},15,
            "Queued candidate did not make repeated real slot attempts");
        require(count("slot")>=4&&count("manifest")==1,
            "Waiting candidate reparsed its immutable manifest before acquiring the occupied slot");
    }
    const auto writeAttempt=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("try{[IO.File]::WriteAllText("+ps_literal(source/L"src/value.h")+",'changed');exit 0}catch{exit 9}")},c.root);
    require(writeAttempt.code==9,"Active candidate source accepted a concurrent writer");
    if(!control.empty())return;
    // A cached wait identity must never authorize changed metadata on admission.
    const auto invalidated=submit(c,execution(pendingSnapshot),env);
    candidate_until(c,invalidated,[&]{return view(c,invalidated).at("record").at("waitingFor").str().find("Candidate slot is busy")!=std::string::npos;},15,
        "Metadata-change candidate did not reach its occupied slot");
    const auto pendingManifest=c.root/L".git/snowdesktop-candidates/manifests"/wide(pendingSnapshot+".json");
    write(pendingManifest,read(pendingManifest)+"\n");
    const auto busy=c.bridge->invoke(c.scripts/L"build_candidate.ps1",{"-Command","materialize","-Task","candidate","-Snapshot",snapshot,"-Slot","warm"});
    require(busy.code==2&&busy.err.find("Candidate slot is busy")!=std::string::npos,"Public materialization overwrote an active executor");
    c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","cancel","-Task","candidate","-RunId",second});wait(c,second,4);
    const auto originalCommand=read(c.scripts/L"build_command.ps1");
    write(c.scripts/L"build_command.ps1",originalCommand+"\nthrow 'live checkout tool changed after submission'\n",true);
    const auto posted=c.call({"event-post","editor","-Target","candidate","-Importance","urgent","-Text","candidate fixture urgent event","-EventKey","candidate-urgent"});
    const auto urgent=c.bridge->invoke(c.scripts/L"build_run.ps1",{"-Command","wait","-Task","candidate","-RunId",first,"-Seconds","1"});
    require(urgent.code==6&&view(c,first).at("status").str()=="running","Candidate wait lost urgent messages or cancelled its executor");
    c.call({"event-ack","candidate","-Sequence",std::to_string(posted.at("sequence").integer()),"-AckLevel","agent"});
    fs::remove(gate);
    const auto completed=wait(c,first);program(output,7);
    candidate_until(c,invalidated,[&]{return view(c,invalidated).at("terminal").boolean;},25,
        "Changed candidate manifest was not rejected after slot acquisition");
    const auto rejected=view(c,invalidated);
    require(rejected.at("status").str()=="failed"&&rejected.at("record").at("error").str().find("Candidate manifest integrity check failed")!=std::string::npos,
        "Queued cached identity bypassed the full manifest validation on admission: "+rejected.dump());
    require(completed.at("record").at("inputStart").at("digest").str()==completed.at("record").at("inputEnd").at("digest").str(),"Live checkout edits invalidated the frozen source identity");
    require(!completed.at("stageEvents").items.empty()&&completed.at("resources").kind==Json::Kind::Object,"Candidate observations were not returned to the parent Run");
    require(read(source/L"scripts/build_command.ps1")==originalCommand,"Candidate tool used the modified live script");
    fs::copy_file(source/L"scripts/build_command.ps1",c.scripts/L"build_command.ps1",fs::copy_options::overwrite_existing);
    c.plan("editor",batch,0,"none","","tool","src/value.h,scripts/build_command.ps1");
    c.call({"ready-and-wait","editor","-Batch",batch,"-Revision","0","-WaitSeconds","30"});
    const auto exe=output/L"Release/tests/SnowDesktopCandidateFixtureTests.exe";const auto exeTime=fs::last_write_time(exe);
    write(output/L"cache-sentinel.txt","preserve warm cache");
    wait(c,submit(c,execution(snapshot)));program(output,7);
    const auto pollControl=run({executable().wstring(),repo.wstring(),L"--poll-reparse"},c.root,100,{},true);
    require(pollControl.code==1&&pollControl.err.find("Waiting candidate reparsed its immutable manifest")!=std::string::npos,
        "Former busy polling did not fail its repeated-manifest-read assertion: "+pollControl.out+pollControl.err);
    require(fs::last_write_time(exe)==exeTime&&read(output/L"cache-sentinel.txt")=="preserve warm cache","Same candidate discarded or rebuilt unchanged outputs");
    const auto tests=submit(c,execution(snapshot,"tests"));wait(c,tests);
    const auto coverage=Json::parse(read(c.stateRoot/L"runs"/wide(tests+".candidate-tests.json")));
    require(coverage.at("candidate").str()==snapshot&&coverage.at("runId").str()==tests&&coverage.at("exitCode").integer()==0&&!coverage.at("reuseAllowed").boolean&&
        coverage.at("selected").items.size()==1&&coverage.at("definitions").items.size()==1&&coverage.at("binaries").items.size()==1&&coverage.at("runtimeFiles").items.size()>0&&
        fs::exists(wide(coverage.at("report").str())),"Candidate tests lost their source/report/binary associations");
    const auto changed=capture(c);const auto next=wait(c,submit(c,execution(changed)));program(output,11);
    require(next.at("record").at("candidateExecution").at("output").str()==context.at("output").str()&&read(output/L"cache-sentinel.txt")=="preserve warm cache","Changed candidate abandoned its stable output cache");
    const auto failedTests=submit(c,execution(changed,"tests"));wait(c,failedTests,1);
    const auto failedCoverage=Json::parse(read(c.stateRoot/L"runs"/wide(failedTests+".candidate-tests.json")));
    require(failedCoverage.at("exitCode").integer()!=0&&fs::exists(wide(failedCoverage.at("report").str())),"Failed CTest execution lost its original report");
    const auto stale=run({powershell().wstring(),L"-NoProfile",L"-File",(source/L"scripts/build_entry.ps1").wstring(),L"-Action",L"ide"},source);
    require(stale.code!=0,"Expired candidate credential permitted an independent build");
    Child unrelated({standin().wstring(),L"--wait",L"--marker",(c.stateRoot/L"unrelated-process").wstring()},c.root);
    for(const auto& mode:{std::string("cancel"),std::string("owner-loss")}){
        fs::remove(gate.wstring()+L".ready");write(gate,"hold");
        const auto id=submit(c,execution(snapshot),env);
        until([&]{return fs::exists(gate.wstring()+L".ready")||view(c,id).at("terminal").boolean;},35);
        require(fs::exists(gate.wstring()+L".ready"),"Lifecycle candidate did not reach its owned child");
        const auto child=Json::parse(read(gate.wstring()+L".ready"));
        if(mode=="cancel"){c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","cancel","-Task","candidate","-RunId",id});wait(c,id,4);}
        else{stop_owner(view(c,id).at("record").at("owner"));until([&]{return view(c,id).at("status").str()=="interrupted";});}
        until([&]{return !owner_alive(child);});require(unrelated.running(),"Candidate lifecycle cleanup terminated an unrelated process");fs::remove(gate);
    }
    wait(c,submit(c,execution(snapshot)));program(output,7);
    std::cout<<"PASS private candidate compiler/test execution, editing overlap, frozen toolset, warm cache, slot queue/cancel, urgent messages, failed reports and owner loss\n";
}
void checkout_cache(const fs::path& repo) {
    Coordinator a(repo,"cache-a",true,fixture_parent(repo));
    require(a.git({L"config",L"core.autocrlf",L"false"}).code==0,"Cannot normalize fixture Git checkout bytes");
    prepare(a,repo,"");
    write(a.root/L"src/tool.cpp","int main(){return 0;}\n");
    // Raw command adapter; the production Run and Job own actual compiler children.
    write(a.scripts/L"build.bat","@echo off\r\n\"%SNOWDESKTOP_ENTRY_POWERSHELL%\" -NoProfile -File \"%~dp0build_command.ps1\" -Operation configure -Preset release\r\nif errorlevel 1 exit /b %ERRORLEVEL%\r\n\"%SNOWDESKTOP_ENTRY_POWERSHELL%\" -NoProfile -File \"%~dp0build_command.ps1\" -Operation build -Preset release -Targets SnowDesktopCandidateFixtureTests\r\nexit /b %ERRORLEVEL%\r\n");
    write(a.root/L"CMakeLists.txt",read(a.root/L"CMakeLists.txt")+R"CMAKE(
add_executable(SnowDesktopIndependentToolTests src/tool.cpp)
target_compile_options(SnowDesktopIndependentToolTests PRIVATE /W4 /WX /Z7)
set_target_properties(SnowDesktopIndependentToolTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/tests")
add_test(NAME independent_tool COMMAND SnowDesktopIndependentToolTests)
set_tests_properties(candidate_rule independent_tool PROPERTIES LABELS "tools" )
set_tests_properties(independent_tool PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopIndependentToolTests>")
)CMAKE");
    require(a.git({L"add",L"--all"}).code==0&&a.git({L"commit",L"-m",L"Two checkout cache fixture"}).code==0,"Cannot save cache fixture");
    Coordinator b(repo,"cache-b-unused",true,fixture_parent(repo));b.bridge.reset();
    b.root=temporary("cache-b",fixture_parent(repo));
    require(a.git({L"worktree",L"add",L"--detach",b.root.wstring(),L"HEAD"}).code==0,"Cannot create real linked checkout");
    b.scripts=b.root/L"scripts";b.stateRoot=b.root/L".build/collaboration";b.bridge=std::make_unique<Bridge>(repo,b.root,false,true);
    b.call({"task-open","candidate","-TaskName","Independent tool checkout"});
    Json measurements=Json::parse("[]");
    auto batch=[&](Coordinator& c,const std::string& test,const std::string& edit=""){
        const auto clock=std::chrono::steady_clock::now();
        const auto opened=c.bridge->call(c.scripts/L"build_manager.ps1",{"begin","candidate"});
        const auto id=opened.at("batchId").str();const auto rev=std::to_string(opened.at("editRevision").integer());
        c.call({"claim","candidate","-Batch",id,"-Revision",rev,"-Files",test=="independent_tool"?"src/value.h":"src/main.cpp"});
        if(!edit.empty())write(c.root/(test=="independent_tool"?L"src/value.h":L"src/main.cpp"),edit);
        c.plan("candidate",id,opened.at("editRevision").integer(),"selected",test,"tool",test=="independent_tool"?"src/value.h":"src/main.cpp");
        const auto completed=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","candidate","-Batch",id,"-Revision",rev},{},100);
        require(completed.code==0,"Cache scene failed: "+completed.out+completed.err);
        const auto result=c.result(id);require(result.at("inputCheck").str()=="stable","Cache scene inputs changed");
        const auto record=view(c,result.at("runId").str());
        Json measure=Json::parse("{}");measure["test"]=test;measure["batch"]=result;measure["run"]=record;
        measure["wallSeconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-clock).count();
        measurements.items.push_back(measure);atomic(a.root/L".build/cache-scenes.json",measurements.dump());
        return record.at("record");
    };
    const auto warm=batch(a,"candidate_rule");
    const auto output=fs::path(wide(warm.at("candidateExecution").at("output").str()));
    const auto object=output/L"SnowDesktopCandidateFixtureTests.dir/Release/main.obj";
    require(fs::exists(object),"Actual compiler object is missing");const auto original=fs::last_write_time(object);
    const auto unchanged=batch(a,"candidate_rule");
    require(fs::last_write_time(object)==original&&unchanged.at("candidateExecution").at("output").str()==warm.at("candidateExecution").at("output").str(),"Unchanged source lost the single workspace's warm compiler cache");
    const auto other=batch(b,"independent_tool","constexpr int value=11;\n");
    const auto repeat=batch(a,"candidate_rule");
    require(fs::last_write_time(object)!=original,"Restoring a different checkout's shared header failed to rebuild its dependent source");
    require(warm.at("candidateExecution").at("slot").str()=="workspace"&&repeat.at("candidateExecution").at("output").str()==other.at("candidateExecution").at("output").str()&&
        warm.at("candidateExecution").at("output").str()==other.at("candidateExecution").at("output").str(),"Linked checkouts allocated additional compiler workspaces");
    const auto changed=batch(a,"candidate_rule",read(a.root/L"src/main.cpp")+"\n// single translation-unit edit\n");
    require(fs::last_write_time(object)!=original&&changed.at("candidateExecution").at("slot").str()==warm.at("candidateExecution").at("slot").str(),"Single-file edit lost its warm checkout cache");
    program(output,7);
    const auto gate=a.stateRoot/L"concurrent-build-gate";write(gate,"hold");
    const auto product=submit(a,{"-Action","release","-CpuSlots","2","-MemoryMiB","768"},{{L"CANDIDATE_FIXTURE_GATE",gate.wstring()}});
    until([&]{return fs::exists(gate.wstring()+L".ready")||view(a,product).at("terminal").boolean;},35);
    require(fs::exists(gate.wstring()+L".ready"),"Concurrent compiler scene never reached its actual build child");
    const auto opened=b.bridge->call(b.scripts/L"build_manager.ps1",{"begin","candidate"});
    const auto toolBatch=opened.at("batchId").str(),revision=std::to_string(opened.at("editRevision").integer());
    b.call({"claim","candidate","-Batch",toolBatch,"-Revision",revision,"-Files","src/tool.cpp"});
    b.call({"plan","candidate","-Batch",toolBatch,"-Revision",revision,"-Scope","tool","-Suites","selected","-Tests","independent_tool","-Inputs","src/tool.cpp","-Reason","Isolated tool fixture with no desktop or external resource use","-ResourceCpuSlots","2","-ResourceMemoryMiB","768","-ResourceShared"});
    const auto launched=b.bridge->invoke(b.scripts/L"build_manager.ps1",{"ready-and-wait","candidate","-Batch",toolBatch,"-Revision",revision,"-WaitSeconds","1"},{},15);
    require(launched.code==0||launched.code==2,"Cannot launch shared tool validation: "+launched.out+launched.err);
    bool overlapped=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(32);
    while(std::chrono::steady_clock::now()<deadline){
        if(fs::exists(b.stateRoot/wide(toolBatch+".json"))){
            const auto result=b.result(toolBatch);
            if(result.has("outcome")){overlapped=result.at("outcome").str()=="passed"&&!view(a,product).at("terminal").boolean;break;}
        }
        Sleep(80);
    }
    fs::remove(gate);
    const auto productResult=wait(a,product);
    const auto toolResult=b.bridge->invoke(b.scripts/L"build_manager.ps1",{"wait","candidate","-Batch",toolBatch,"-Revision",revision},{},70);
    require(toolResult.code==0,"Shared tool validation failed: "+toolResult.out+toolResult.err);
    require(overlapped,"Independent tool validation waited behind the active product compiler");
    Json concurrent=Json::parse("{}");concurrent["scene"]="concurrent-compiler-and-tool";concurrent["product"]=productResult;concurrent["tool"]=view(b,b.result(toolBatch).at("runId").str());
    const auto toolOutput=fs::path(wide(concurrent.at("tool").at("record").at("candidateExecution").at("output").str()));
    const auto toolExecutable=toolOutput/L"Release/tests/SnowDesktopIndependentToolTests.exe";
    const auto originalBytes=read(toolExecutable);const auto originalTime=fs::last_write_time(toolExecutable);
    write(toolExecutable,originalBytes+"corrupted");
    const auto rejected=b.bridge->invoke(b.scripts/L"build_manager.ps1",{"wait","candidate","-Batch",toolBatch,"-Revision",revision},{},30);
    write(toolExecutable,originalBytes);fs::last_write_time(toolExecutable,originalTime);
    require(rejected.code!=0&&rejected.err.find("Test executable differs from recorded coverage")!=std::string::npos,"Candidate wait accepted changed executable bytes or failed outside its hash check: "+rejected.out+rejected.err);
    const auto frozenSource=fs::path(wide(concurrent.at("tool").at("record").at("candidateExecution").at("source").str()))/L"src/tool.cpp";
    const auto sourceBytes=read(frozenSource);const auto sourceTime=fs::last_write_time(frozenSource);
    write(frozenSource,sourceBytes+"\n// changed frozen input\n");
    const auto rejectedSource=b.bridge->invoke(b.scripts/L"build_manager.ps1",{"wait","candidate","-Batch",toolBatch,"-Revision",revision},{},30);
    write(frozenSource,sourceBytes);fs::last_write_time(frozenSource,sourceTime);
    require(rejectedSource.code!=0&&rejectedSource.err.find("Candidate source input changed during execution")!=std::string::npos,"Candidate wait accepted changed frozen source or failed outside its source identity check: "+rejectedSource.out+rejectedSource.err);
    const auto restored=b.bridge->invoke(b.scripts/L"build_manager.ps1",{"wait","candidate","-Batch",toolBatch,"-Revision",revision},{},30);
    require(restored.code==0,"Candidate wait failed after restoring the exact executable bytes: "+restored.out+restored.err);
    measurements.items.push_back(concurrent);atomic(a.root/L".build/cache-scenes.json",measurements.dump());
    std::cout<<"PASS one compiler workspace preserves unchanged objects and safely updates inputs across linked checkouts; measurements "<<utf8((a.root/L".build/cache-scenes.json").wstring())<<"\n";
}
void weighted_candidate(const fs::path& repo) {
    Coordinator c(repo,"weighted-candidate",true,fixture_parent(repo));prepare(c,repo,"");
    const auto snapshot=capture(c);
    auto weightedArguments=execution(snapshot);
    // This one-file compiler fixture measured under 344 MiB. Keep more than
    // twice that peak without reserving an unrelated 2 GiB of host capacity.
    weightedArguments.insert(weightedArguments.end(),{"-CpuSlots","2","-MemoryMiB","768"});
    const auto id=submit(c,weightedArguments);Json weighted;
    // Resource admission can legitimately outlive one bounded observation.
    // Keep observing this exact Run, never create a replacement or disable
    // headroom protection. The independent CTest case bounds the total wait.
    for(int observation=0;observation<5;++observation){
        const auto reply=c.bridge->invoke(c.scripts/L"build_run.ps1",{"-Command","wait","-Task","candidate","-RunId",id,"-Seconds","30"},{},40);
        require(reply.code==0||reply.code==10,"Weighted candidate observation failed: "+reply.out+reply.err);
        weighted=Json::parse(reply.out);
        require(weighted.at("stageError").str().empty(),"Weighted candidate lost its stage observations");
        if(weighted.at("terminal").boolean){require(reply.code==0,"Weighted candidate terminal exit code changed");break;}
        // Exit 10 is the CLI's bounded, nonterminal observation result.
        require(reply.code==10&&weighted.at("id").str()==id&&weighted.at("ownerState").str()=="alive","Weighted candidate lost its live execution owner");
    }
    require(weighted.at("terminal").boolean&&weighted.at("status").str()=="passed","Weighted candidate remained nonterminal: "+id+" / "+weighted.at("status").str()+" / "+weighted.at("record").at("waitingFor").str());
    program(fs::path(wide(weighted.at("record").at("candidateExecution").at("output").str())),7);
    require(weighted.at("record").at("resourceAdmission").at("bindingValid").boolean&&
        weighted.at("resources").at("requestedCpuSlots").integer()==2&&weighted.at("resources").at("requestedMemoryMiB").integer()==768&&
        weighted.at("resources").at("committedMemoryLimitBytes").number==805306368.0&&
        weighted.at("resources").at("finished").boolean&&weighted.at("resources").at("accountingAvailable").boolean,
        "Candidate private entry did not reuse its admitted runtime and parent CPU/memory budget");
    std::cout<<"PASS independent weighted candidate compiler execution, real admission and Job limits; bounded observers preserve the original Run\n";
}
void executor_control(const fs::path& repo,bool checkout) {
    const std::string mutation=checkout?"checkout-lock":"unlocked-source";
    const std::string assertion=checkout?"Original checkout editing remained blocked":"Active candidate source accepted a concurrent writer";
    const auto result=run({executable().wstring(),repo.wstring(),wide("--"+mutation)},repo,85,{},true);
    require(result.code==1&&result.err.find(assertion)!=std::string::npos,"Executor "+mutation+" negative control did not reach its business assertion: "+result.out+result.err);
    std::cout<<"PASS executor "<<mutation<<" negative control reached its business assertion\n";
}
void completed_verification(const fs::path& repo,bool noReference=false,bool compatible=false,bool exactOnly=false,bool automatic=false,bool oldAutomatic=false) {
    Coordinator c(repo,"completed-verification",true,fixture_parent(repo));prepare(c,repo,"");
    if(noReference){auto source=read(c.scripts/L"build_run.ps1");replace(source,"$referenceProposal=Get-CompletedVerificationProposal $root $verification","$referenceProposal=$null");write(c.scripts/L"build_run.ps1",source,true);}
    if(exactOnly){auto source=read(c.scripts/L"build_verification_reuse.ps1");replace(source,
        "$compatibility=Get-Field $Verification 'compatibility';$index=@(Get-Field $Entry 'compatibleTests' @())",
        "return $false # Controlled former exact-key-only completed history.");write(c.scripts/L"build_verification_reuse.ps1",source,true);}
    if(automatic){
        copy(repo,c.root,{"tools/build-dashboard/server.py"});
        write(c.root/L".gitignore",read(c.root/L".gitignore")+"!/tools/\n!/tools/build-dashboard/\n!/tools/build-dashboard/server.py\n");
        write(c.scripts/L"build_runtime.ps1",read(repo/L"scripts/build_runtime.ps1")+"\nfunction Ensure-BuildDashboard {}\n",true);
        if(oldAutomatic){auto source=read(c.scripts/L"build_test_selection.ps1");replace(source,
            "$proposal=Find-CompletedVerificationProof $context.runRoot $snapshot -BatchOrigins",
            "$proposal=$null # Controlled former pipeline executes every declaration.");write(c.scripts/L"build_test_selection.ps1",source,true);}
    }
    const auto observer=executable().parent_path()/L"SnowDesktopTestRuntimeObserver.exe";
    require(fs::exists(observer),"Completed verification needs the actual native runtime observer");
    auto cmake=read(c.root/L"CMakeLists.txt");replace(cmake,"VERSION 3.24","VERSION 3.29");
    cmake+="\nconfigure_file(\""+utf8(observer.generic_wstring())+"\" \"${CMAKE_BINARY_DIR}/Release/tests/SnowDesktopTestRuntimeObserver.exe\" COPYONLY)\n"
        "set_target_properties(SnowDesktopCandidateFixtureTests PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded TEST_LAUNCHER \"${CMAKE_BINARY_DIR}/Release/tests/SnowDesktopTestRuntimeObserver.exe;--test;candidate_rule;--\")\n";
    write(c.root/L"CMakeLists.txt",cmake);
    write(c.root/L"src/main.cpp",R"CPP(#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "value.h"
bool absent(const wchar_t* name){
    SetLastError(ERROR_SUCCESS);
    const auto size=GetEnvironmentVariableW(name,nullptr,0);
    return size==0&&GetLastError()==ERROR_ENVVAR_NOT_FOUND;
}
int main(int argc,char**){
    if(!absent(L"CODEX_TOOL_CALL_ID")||!absent(L"CODEX_THREAD_ID")||!absent(L"CODEX_SESSION_ID"))return 61;
    return argc>1?(value==7?0:1):value;
}
)CPP");
    if(compatible){
        auto main=read(c.root/L"src/main.cpp");
        replace(main,"int main(int argc,char**){",R"CPP(bool runtime_ok(){
    const auto file=CreateFileW(L"src/runtime.txt",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    char byte=0;DWORD count=0;
    const bool ok=ReadFile(file,&byte,1,&count,nullptr)!=FALSE;
    CloseHandle(file);return ok&&count==1&&byte=='7';
}
int main(int argc,char**){)CPP");
        replace(main,"return argc>1?(value==7?0:1):value;","return argc>1?(value==7&&runtime_ok()?0:1):value;");
        write(c.root/L"src/main.cpp",main);write(c.root/L"src/runtime.txt","7");
        write(c.root/L"CMakeLists.txt",read(c.root/L"CMakeLists.txt")+"\nset_tests_properties(candidate_rule PROPERTIES WORKING_DIRECTORY \"${CMAKE_SOURCE_DIR}\")\n");
    }
    if(automatic){write(c.root/L"CMakeLists.txt",read(c.root/L"CMakeLists.txt")+R"CMAKE(
add_test(NAME candidate_unreviewed COMMAND "${CMAKE_BINARY_DIR}/Release/tests/SnowDesktopCandidateFixtureTests.exe" check)
set_tests_properties(candidate_unreviewed PROPERTIES REQUIRED_FILES "$<TARGET_FILE:SnowDesktopCandidateFixtureTests>" WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" LABELS "core")
)CMAKE");}
    write(c.scripts/L"check-native-token.ps1",R"PS(
if(-not ('SnowDesktop.Verification.NativeToken' -as [type])){Add-Type -TypeDefinition ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build_verification_token.cs')))}
$encoding=[Console]::OutputEncoding
try{
 [Console]::OutputEncoding=[Text.Encoding]::GetEncoding(936);$first=[SnowDesktop.Verification.NativeToken]::Capture()
 [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);$second=[SnowDesktop.Verification.NativeToken]::Capture()
 @{stable=($first -ceq $second);hasRights=$first.Contains('privilege:');hasIntegrity=$first.Contains('integrity:')}|ConvertTo-Json
}finally{[Console]::OutputEncoding=$encoding}
)PS",true);
    const auto nativeToken=c.bridge->call(c.scripts/L"check-native-token.ps1",{});
    require(nativeToken.at("stable").boolean&&nativeToken.at("hasRights").boolean&&nativeToken.at("hasIntegrity").boolean,
        "Native token identity depended on console encoding or omitted rights/integrity");
    write(c.scripts/L"review-cache.ps1",R"PS(param([string]$Binary='')
$root=Split-Path $PSScriptRoot -Parent
$sources=@('src/main.cpp','src/value.h')|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $root $_)).Hash.ToLowerInvariant()}}
$contract=@{test='candidate_rule';kind='reviewed-file-inputs-v1';reviewedSources=@($sources);inputs=@()}
if($Binary){$contract.resultReuse=@{kind='reviewed-native-files-v1';binary='.build/Release/tests/SnowDesktopCandidateFixtureTests.exe';binarySha256=(Get-FileHash -LiteralPath $Binary).Hash.ToLowerInvariant();reviewedSources=@($sources)}}
@{schemaVersion=1;tests=@($contract)}|ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $PSScriptRoot 'test_runtime_contracts.json') -Encoding UTF8
@{reviewed=[bool]$Binary}|ConvertTo-Json
)PS",true);
    if(compatible){
        auto review=read(c.scripts/L"review-cache.ps1");
        replace(review,"inputs=@()","inputs=@(@{kind='file';path='src/runtime.txt';required=$true})");
        replace(review,"kind='reviewed-native-files-v1'","kind='reviewed-native-files-v2';crossCandidate=$true");
        write(c.scripts/L"review-cache.ps1",review,true);
        write(c.scripts/L"check-receipt-time.ps1",R"PS(param([string]$Stamps,[switch]$Legacy)
. (Join-Path $PSScriptRoot 'build_verification_reuse.ps1')
if($Legacy){function Format-VerificationUtcTimestamp([DateTime]$Value){return $Value.ToUniversalTime().ToString('o')}}
$rows=@(foreach($stamp in $Stamps.Split(',')){
 $value=[DateTime]::ParseExact($stamp,'o',[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::RoundtripKind)
 $text=Format-VerificationUtcTimestamp $value
 $before=@{checkedUtc=$text}|ConvertTo-Json -Compress
 $after=$before|ConvertFrom-Json|ConvertTo-Json -Compress
 $decoded=[DateTime]::Parse($text,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::RoundtripKind)
 @{before=$before;after=$after;originalTicks=$value.Ticks.ToString([Globalization.CultureInfo]::InvariantCulture);formattedTicks=$decoded.Ticks.ToString([Globalization.CultureInfo]::InvariantCulture)}
})
@{rows=$rows}|ConvertTo-Json -Depth 4
)PS",true);
        const auto checkTimes=[](const Json& values){require(values.at("rows").items.size()==4,"Receipt time cases were not all executed");
            for(const auto& row:values.at("rows").items)require(row.at("before").str()==row.at("after").str()&&row.at("originalTicks").str()==row.at("formattedTicks").str(),
                "Completed reference time changed during the real JSON round trip");};
        const auto stamps=L"2026-10-22T23:33:47.0000000Z,2026-10-22T23:33:47.1000000Z,2026-10-22T23:33:47.1126790Z,2026-10-22T23:33:47.1234567Z";
        for(const bool legacyShell:{false,true}){
            const auto times=run({powershell(legacyShell).wstring(),L"-NoProfile",L"-File",(c.scripts/L"check-receipt-time.ps1").wstring(),L"-Stamps",stamps},c.root,20);
            require(times.code==0,"Receipt JSON round trip could not execute: "+times.err);checkTimes(Json::parse(times.out));
        }
        const auto oldTimes=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"check-receipt-time.ps1").wstring(),L"-Stamps",stamps,L"-Legacy"},c.root,20);
        require(oldTimes.code==0,"Former time formatter failed outside JSON round trip: "+oldTimes.err);
        bool rejected=false;try{checkTimes(Json::parse(oldTimes.out));}catch(const std::exception& error){rejected=std::string(error.what())=="Completed reference time changed during the real JSON round trip";}
        require(rejected,"Former fixed-width UTC formatter escaped the JSON round-trip assertion");
    }
    c.bridge->call(c.scripts/L"review-cache.ps1",{});
    std::string selectedNames=automatic?"candidate_rule,candidate_unreviewed":"candidate_rule";
    auto baseline=[&]{
        const auto opened=c.bridge->call(c.scripts/L"build_manager.ps1",{"begin","candidate"});const auto id=opened.at("batchId").str();
        c.plan("candidate",id,0,"selected",selectedNames,"tool","src/main.cpp,src/value.h");
        auto finished=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"ready-and-wait","candidate","-Batch",id,"-Revision","0","-WaitSeconds","60"},{},75);
        const auto resultPath=c.stateRoot/wide(id+".json");
        const auto observationTimedOut=[](const Result& value){return value.code==2&&value.err.find("Wait timed out. Registration was retained;")!=std::string::npos;};
        // The coordinator's observation deadline does not end its durable Run.
        // Continue that exact batch once; never resubmit ready or invent a pass.
        if(observationTimedOut(finished)&&!fs::is_regular_file(resultPath))
            finished=c.bridge->invoke(c.scripts/L"build_manager.ps1",{"wait","candidate","-Batch",id,"-Revision","0","-WaitSeconds","60"},{},75);
        if(finished.code!=0&&!observationTimedOut(finished))
            throw std::runtime_error("Real native baseline observer failed; batch "+id+": "+finished.out+finished.err);
        require(fs::is_regular_file(resultPath),"Real native baseline observation remained incomplete; batch "+id+": "+finished.out+finished.err);
        const auto result=c.result(id);
        require(result.at("batchId").str()==id,"Real native baseline result belongs to another batch");
        if(result.at("outcome").str()!="passed"){const auto log=read(fs::path(wide(result.at("logPath").str())));throw std::runtime_error("Real native baseline failed; batch "+id+": "+log.substr(log.size()>4000?log.size()-4000:0));}
        return result;
    };
    const auto original=baseline();
    const auto loggerRoot=c.stateRoot/L"instrumentation";
    std::vector<std::pair<fs::path,std::string>> cachedLoggers;
    for(const auto& file:fs::directory_iterator(loggerRoot))if(file.path().extension()==L".dll")cachedLoggers.emplace_back(file.path(),read(file.path()));
    require(!cachedLoggers.empty(),"Nested candidate did not publish the logger at its owning Run root");
    const auto binary=fs::path(wide(original.at("candidateExecution").at("output").str()))/L"Release/tests/SnowDesktopCandidateFixtureTests.exe";
    c.bridge->call(c.scripts/L"review-cache.ps1",{"-Binary",utf8(binary.wstring())});
    const auto reviewed=baseline();auto batch=reviewed.at("batchId").str();
    for(const auto& [path,bytes]:cachedLoggers)require(read(path)==bytes,"Repeated candidate build overwrote an identity-bound logger cache entry");
    if(automatic){
        auto covered=[&](const Json& result,std::size_t executions,std::size_t references){
            const auto& coverage=result.at("coverage");
            require(coverage.at("selected").items.size()==executions+references&&coverage.at("completed").items.size()==executions&&
                coverage.at("references").items.size()==references,"Automatic mixed selection executed the reviewed test again or lost unknown-test coverage: "+coverage.dump());
            const auto& receipt=Json::parse(read(c.stateRoot/L"runs"/wide(result.at("runId").str()+".candidate-tests.json")));
            require(receipt.at("executedSelection").items.size()==executions&&receipt.at("references").items.size()==references,
                "Final candidate export conflated referenced and newly executed tests");
            const auto retry=Json::parse(read(c.stateRoot/wide(result.at("batchId").str()+".retry.json")));
            require(retry.at("tests").fields.size()==executions,"Retry ledger manufactured execution attempts for references");
            if(!executions)require(coverage.at("report").kind==Json::Kind::Null&&coverage.at("status").str()=="fulfilled"&&
                receipt.at("report").kind==Json::Kind::Null&&receipt.at("binaries").items.empty()&&
                receipt.at("runtimeFiles").items.empty()&&result.at("artifact").kind==Json::Kind::Null,
                "Reference-only selection fabricated a JUnit, passing execution or runtime artifact");
            else{
                const auto xml=read(fs::path(wide(coverage.at("report").str())));
                if(references)require(xml.find("name=\"candidate_rule\"")==std::string::npos&&xml.find("name=\"candidate_unreviewed\"")!=std::string::npos,
                    "Mixed selection JUnit includes referenced work or omits the unknown test");
            }
        };
        covered(reviewed,2,0);
        const auto originRun=reviewed.at("runId").str();
        until([&]{const auto snapshot=c.state();for(const auto& entry:snapshot.at("verificationLedger").at("entries").items)
            if(entry.at("runId").str()==originRun&&entry.at("execution").str()=="executed")return true;return false;},10);
        std::vector<std::pair<fs::path,std::string>> originals;
        for(const auto& path:std::vector<fs::path>{c.stateRoot/wide(batch+".json"),c.stateRoot/wide(batch+".coverage.json"),
            c.stateRoot/L"runs"/wide(originRun+".json"),c.stateRoot/L"runs"/wide(originRun+".request.json"),
            c.stateRoot/L"runs"/wide(originRun+".test-intent.json"),fs::path(wide(reviewed.at("coverage").at("report").str()))})originals.emplace_back(path,read(path));
        write(c.root/L"src/unrelated-note.txt","Unrelated to the reviewed native file-reading test.\n");
        const auto mixed=baseline();covered(mixed,1,1);
        const auto projected=run({production_python().wstring(),L"-B",L"-c",L"import importlib.util,json,sys;from pathlib import Path;s=importlib.util.spec_from_file_location('dashboard',sys.argv[1]);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);print(json.dumps(m.Store(Path(sys.argv[2]),False).batch(sys.argv[3])))",
            (repo/L"tools/build-dashboard/server.py").wstring(),c.stateRoot.wstring(),wide(mixed.at("batchId").str())},c.root,15);
        require(projected.code==0,"Actual mixed batch could not be projected by production Store: "+projected.err);
        const auto projectedCoverage=Json::parse(projected.out).at("coverage");
        require(projectedCoverage.at("summary").at("referenced").integer()==1&&projectedCoverage.at("summary").at("executed").integer()==1&&
            projectedCoverage.at("summary").at("passed").integer()==1&&projectedCoverage.at("references").items[0].at("status").str()=="recorded",
            "Production Store lost real compiled-origin proof or conflated reference/execution counts: "+projectedCoverage.dump());
        require(mixed.at("coverage").at("references").items[0].at("sourceRunId").str()==originRun&&
            mixed.at("coverage").at("completed").items[0].at("name").str()=="candidate_unreviewed",
            "Automatic mixed selection chose an indirect origin or skipped the unknown test");
        for(const auto& item:mixed.at("evidence").at("tests").items)if(item.at("test").str()=="candidate_rule")
            require(item.at("status").str()=="referenced"&&!item.at("newExecution").boolean&&item.at("attempts").items.empty(),
                "Referenced evidence contains a fabricated execution attempt");
        selectedNames="candidate_rule";const auto allReferences=baseline();covered(allReferences,0,1);
        require(allReferences.at("coverage").at("references").items[0].at("sourceRunId").str()==originRun,
            "A reference-only batch created a reference chain");
        for(const auto& [path,bytes]:originals)require(read(path)==bytes,"Automatic selection rewrote an original result, request, intent, coverage or JUnit");
        selectedNames="candidate_rule,candidate_unreviewed";write(c.root/L"src/runtime.txt","7\n");
        const auto changed=baseline();covered(changed,2,0);
        const auto changedReport=fs::path(wide(changed.at("coverage").at("report").str()));write(changedReport,read(changedReport)+" ");
        const auto corrupt=baseline();covered(corrupt,2,0);
        std::cout<<"PASS actual build/test pipeline mixes reviewed references and unknown execution, handles reference-only without JUnit, preserves origins and executes changed/corrupt inputs\n";
        return;
    }
    auto verify=[&](bool fresh=false,const std::string& correlation="repeated"){
        std::vector<std::string> args={"-Command","start","-Task","candidate","-Action","tests","-Mode","verify","-PlanBatch",batch,"-Filter","candidate_rule","-Reason","Review identical native execution evidence"};
        if(fresh)args.push_back("-Fresh");
        return c.bridge->call(c.scripts/L"build_run.ps1",args,0,{{L"CODEX_TOOL_CALL_ID",wide(correlation)},{L"CODEX_THREAD_ID",wide("thread-"+correlation)},{L"CODEX_SESSION_ID",wide("session-"+correlation)}});
    };
    const auto first=verify(false,"first");require(!first.at("reused").boolean,"First verification invented completed evidence");
    const auto id=first.at("id").str();const auto done=wait(c,id);
    require(done.at("record").at("evidence").at("tests").items.front().at("runtimeModules").at("status").str()=="observed",
        "Real native verification lost its actual runtime report");
    const auto resultPath=c.stateRoot/L"runs"/wide(id+".json"),requestPath=c.stateRoot/L"runs"/wide(id+".request.json");
    const auto coveragePath=c.stateRoot/L"runs"/wide(id+".verification.json");
    const auto resultBytes=read(resultPath),requestBytes=read(requestPath),coverageBytes=read(coveragePath);
    const auto report=fs::path(wide(Json::parse(coverageBytes).at("report").str()));const auto reportBytes=read(report);
    const auto requestsBefore=static_cast<std::size_t>(std::distance(fs::directory_iterator(c.stateRoot/L"runs"),fs::directory_iterator{}));
    const auto references=verify(false,"second");
    require(references.at("reused").boolean&&references.at("id").str()==id&&references.at("status").str()=="referenced"&&
        !references.at("reference").at("newExecution").boolean,"Completed native verification executed again instead of referencing its original Run: "+references.dump());
    require(verify().at("reference").at("id").str()==references.at("reference").at("id").str(),"Repeated completed citation was not idempotent");
    if(compatible){
        const auto binaryBytes=read(binary);
        write(c.root/L"src/unrelated-note.txt","This file is not a runtime or compiled input of candidate_rule.\n");
        const auto changed=baseline();batch=changed.at("batchId").str();
        require(changed.at("inputEnd").at("digest").str()!=reviewed.at("inputEnd").at("digest").str()&&read(binary)==binaryBytes,
            "Compatible fixture did not change candidate source while preserving its actual assertion binary");
        const auto across=verify();
        require(across.at("reused").boolean&&across.at("id").str()==id&&
            across.at("reference").at("scope").str()=="candidate-compatible-reviewed-binary"&&
            across.at("reference").at("originKey").str()!=across.at("reference").at("key").str(),
            "Unrelated candidate change executed again instead of referencing reviewed compatible evidence: "+across.dump());
        require(read(resultPath)==resultBytes&&read(requestPath)==requestBytes&&read(coveragePath)==coverageBytes&&read(report)==reportBytes,
            "Cross-candidate reference rewrote the original evidence");
        const auto state=Json::parse(read(c.stateRoot/L"state.json"));
        require(!state.at("verificationLedger").at("entries").items.front().at("compatibleTests").items.empty(),
            "Compatible per-test identity was not indexed in the existing verification ledger");
        write(c.root/L"src/runtime.txt","7\n");
        const auto inputChanged=baseline();batch=inputChanged.at("batchId").str();
        const auto rerun=verify();
        require(!rerun.at("reused").boolean&&rerun.at("id").str()!=id,
            "Changed reviewed runtime bytes reused prior evidence despite unchanged assertion binary");
        wait(c,rerun.at("id").str());
        const auto forced=verify(true);require(!forced.at("reused").boolean&&forced.at("id").str()!=rerun.at("id").str(),
            "Compatible identity ignored the explicit Fresh override");
        wait(c,forced.at("id").str());
        for(const auto& origin:{rerun,forced}){
            const auto saved=Json::parse(read(c.stateRoot/L"runs"/wide(origin.at("id").str()+".verification.json")));
            const auto raw=fs::path(wide(saved.at("report").str()));write(raw,read(raw)+" ");
        }
        const auto corrupt=verify();require(!corrupt.at("reused").boolean,
            "Corrupt compatible origin reports suppressed execution");wait(c,corrupt.at("id").str());
        std::cout<<"PASS compatible candidate source change references original evidence; changed runtime bytes, Fresh and corrupt origins execute; per-test identities indexed\n";
        return;
    }
    const std::vector<std::wstring> concurrentArgs={powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),L"-Command",L"start",L"-Task",L"candidate",L"-Action",L"tests",L"-Mode",L"verify",L"-PlanBatch",wide(batch),L"-Filter",L"candidate_rule",L"-Reason",L"Concurrent completed reference"};
    Child concurrentA(concurrentArgs,c.root,{{L"CODEX_TOOL_CALL_ID",L"concurrent-a"},{L"CODEX_THREAD_ID",L"thread-a"},{L"CODEX_SESSION_ID",L"session-a"}}),concurrentB(concurrentArgs,c.root,{{L"CODEX_TOOL_CALL_ID",L"concurrent-b"},{L"CODEX_THREAD_ID",L"thread-b"},{L"CODEX_SESSION_ID",L"session-b"}});
    const auto responseA=concurrentA.wait(45),responseB=concurrentB.wait(45);
    require(responseA.code==0&&responseB.code==0,"Concurrent citation callers failed: "+responseA.out+responseA.err+responseB.out+responseB.err);
    const auto citationA=Json::parse(responseA.out),citationB=Json::parse(responseB.out);
    require(citationA.at("reused").boolean&&citationB.at("reused").boolean&&citationA.at("id").str()==id&&citationB.at("id").str()==id,
        "Concurrent read-only reference checks launched duplicate executions");
    require(static_cast<std::size_t>(std::distance(fs::directory_iterator(c.stateRoot/L"runs"),fs::directory_iterator{}))==requestsBefore,
        "Completed reference created a new execution file");
    require(read(resultPath)==resultBytes&&read(requestPath)==requestBytes&&read(coveragePath)==coverageBytes&&read(report)==reportBytes,
        "Completed reference rewrote the original Run, request, coverage or JUnit");
    const auto forced=verify(true);require(!forced.at("reused").boolean&&forced.at("id").str()!=id,"Explicit fresh verification reused completed evidence");
    wait(c,forced.at("id").str());
    // A changed raw report cannot authorize a cached pass. Preserve the other
    // completed attempt too, so every available origin is checked.
    const auto forcedCoverage=Json::parse(read(c.stateRoot/L"runs"/wide(forced.at("id").str()+".verification.json")));
    const auto forcedReport=fs::path(wide(forcedCoverage.at("report").str()));
    write(report,reportBytes+" ");write(forcedReport,read(forcedReport)+" ");
    const auto corrupt=verify();require(!corrupt.at("reused").boolean&&corrupt.at("id").str()!=id,"Changed JUnit was reused as passing evidence");
    wait(c,corrupt.at("id").str());
    std::cout<<"PASS completed native verification references, immutable origin, idempotent citations, explicit fresh execution and corrupt-report fallback\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){
    const auto mode=argc>2?std::string(argv[2]):"";
    if(mode=="--automatic-selection"||mode=="--automatic-selection-old")completed_verification(repo,false,true,false,true,mode=="--automatic-selection-old");
    else if(mode=="--automatic-selection-control"){const auto result=run({executable().wstring(),repo.wstring(),L"--automatic-selection-old"},repo,240,{},true);require(result.code!=0&&result.err.find("Automatic mixed selection executed the reviewed test again")!=std::string::npos,"Former all-tests pipeline did not fail the mixed-selection business assertion: "+result.out+result.err);}
    else if(mode=="--completed-verification"||mode=="--completed-verification-without-reference")completed_verification(repo,mode=="--completed-verification-without-reference");
    else if(mode=="--compatible-verification"||mode=="--compatible-verification-old-key")completed_verification(repo,false,true,mode=="--compatible-verification-old-key");
    else if(mode=="--compatible-verification-control"){
        const auto negative=run({executable().wstring(),repo.wstring(),L"--compatible-verification-old-key"},repo,180,{},true);
        require(negative.code==1&&negative.err.find("Unrelated candidate change executed again instead of referencing reviewed compatible evidence")!=std::string::npos,
            "Exact-key-only compatibility control failed outside the duplicate-execution assertion: "+negative.out+negative.err);
        std::cout<<"PASS exact-key-only completed history fails the unrelated-candidate duplicate-execution assertion\n";
    }
    else if(mode=="--completed-verification-control"){
        const auto negative=run({executable().wstring(),repo.wstring(),L"--completed-verification-without-reference"},repo,150,{},true);
        require(negative.code==1&&negative.err.find("Completed native verification executed again instead of referencing its original Run")!=std::string::npos,
            "Completed reference control failed outside the duplicate-execution assertion: "+negative.out+negative.err);
        std::cout<<"PASS disabled completed-reference path fails the actual duplicate-execution assertion\n";
    }
    else if(mode=="--tool-verification"||mode=="--tool-verification-old-preset")candidate_batches(repo,false,true,mode=="--tool-verification-old-preset");
    else if(mode=="--batches"||mode=="--batches-held-window")candidate_batches(repo,mode=="--batches-held-window");
    else if(mode=="--artifacts"||mode=="--artifacts-slot-reference"||mode=="--artifacts-input-denied")artifacts(repo,mode=="--artifacts-slot-reference",mode=="--artifacts-input-denied");
    else if(mode=="--preflight"||mode=="--preflight-old-path"){
        private_preflight(repo,mode=="--preflight-old-path");
        if(mode=="--preflight"){
            const auto negative=run({executable().wstring(),repo.wstring(),L"--preflight-module-path-only"},repo,90,{},true);
            require(negative.code==1&&negative.err.find("Unavailable module Path on an unrelated process must not block private test runtime")!=std::string::npos,
                "Original managed-Path-only test-runtime observer did not fail at the unrelated-owner assertion: "+negative.out+negative.err);
            std::cout<<"PASS kernel image identity retains actual owners and rejects unrelated missing-module-path helpers; original observer rejected\n";
        }
    }
    else if(mode=="--preflight-module-path-only")private_preflight(repo,false,true);
    else if(mode=="--verify-checkout-lock")executor_control(repo,true);
    else if(mode=="--verify-source-lock")executor_control(repo,false);
    else if(mode=="--checkout-cache")checkout_cache(repo);
    else if(mode=="--weighted")weighted_candidate(repo);
    else if(mode=="--all"){scenario(repo,"");weighted_candidate(repo);executor_control(repo,true);executor_control(repo,false);}
    else scenario(repo,mode.empty()?"":mode.substr(2));
});}
