#include "../build_tool_test_support.h"
#include <iostream>

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
        if(file.is_regular_file())fs::copy_file(file.path(),c.scripts/file.path().filename(),fs::copy_options::overwrite_existing);
    }
    copy(repo,c.root,{"cmake/SnowDesktop.SharedBuildGuard.cmake","cmake/SnowDesktop.PowerShell.cmake"});
    // Optional UI service is outside this execution fixture's resources.
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+
        "\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='Candidate fixture has no dashboard'}}\n",true);
    if(control=="checkout-lock"){
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
void private_preflight(const fs::path& repo,bool oldPath) {
    const auto root=temporary("candidate-preflight",fixture_parent(repo));
    copy(repo,root,{"scripts/build_entry.ps1","scripts/build_preflight.ps1","scripts/test_manager.ps1"});
    const auto source=root/L"source",output=root/L"output",neighbor=root/L"output-neighbor";
    fs::create_directories(source);fs::create_directories(output/L"Release");fs::create_directories(output/L"Debug");fs::create_directories(neighbor/L"Release");
    for(const auto& destination:{output/L"Release/SnowDesktop.exe",output/L"Debug/SnowDesktop.exe",neighbor/L"Release/SnowDesktop.exe"})
        fs::copy_file(standin(),destination);
    for(const auto& name:{L".build",L".build_debug"}){
        const auto result=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("New-Item -ItemType Junction -Path "+ps_literal(source/name)+" -Target "+ps_literal(output)+" -ErrorAction Stop | Out-Null")},root);
        require(result.code==0,"Cannot establish private output junction: "+result.out+result.err);
    }
    std::string query=R"PS(param([string]$ProbeRoot,[string]$ProbeConfiguration='Release')
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'scripts/build_preflight.ps1')
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
    auto probe=[&](const std::string& configuration="Release"){
        return bridge.call(root/L"query.ps1",{"-ProbeRoot",utf8(source.wstring()),"-ProbeConfiguration",configuration});
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
    Child release({(output/L"Release/SnowDesktop.exe").wstring(),L"--wait",L"--marker",releaseMarker.wstring()},root);
    until([&]{return fs::exists(releaseMarker);});const auto physical=probe();
    require(contains(physical,release.pid())&&physical.at("preflight").at("status").str()!="clear",
        "Private output preflight falsely reported the physical application as clear");
    require(locksContain(physical,release.pid())&&!locksContain(physical,other.pid()),"Test runtime ownership missed the physical output or included its neighbor");
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
void artifacts(const fs::path& repo,bool slotReference) {
    Coordinator c(repo,"candidate-artifacts",true,fixture_parent(repo));
    prepare(c,repo,slotReference?"artifacts-slot-reference":"");
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
    const auto snapshot=capture(c);const auto gate=c.stateRoot/L"candidate-gate";write(gate,"hold");
    const Env env={{L"CANDIDATE_FIXTURE_GATE",gate.wstring()}};
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
    const auto writeAttempt=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("try{[IO.File]::WriteAllText("+ps_literal(source/L"src/value.h")+",'changed');exit 0}catch{exit 9}")},c.root);
    require(writeAttempt.code==9,"Active candidate source accepted a concurrent writer");
    if(!control.empty())return;
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
    require(completed.at("record").at("inputStart").at("digest").str()==completed.at("record").at("inputEnd").at("digest").str(),"Live checkout edits invalidated the frozen source identity");
    require(!completed.at("stageEvents").items.empty()&&completed.at("resources").kind==Json::Kind::Object,"Candidate observations were not returned to the parent Run");
    require(read(source/L"scripts/build_command.ps1")==originalCommand,"Candidate tool used the modified live script");
    fs::copy_file(source/L"scripts/build_command.ps1",c.scripts/L"build_command.ps1",fs::copy_options::overwrite_existing);
    c.plan("editor",batch,0,"none","","tool","src/value.h,scripts/build_command.ps1");
    c.call({"ready-and-wait","editor","-Batch",batch,"-Revision","0","-WaitSeconds","30"});
    const auto exe=output/L"Release/tests/SnowDesktopCandidateFixtureTests.exe";const auto exeTime=fs::last_write_time(exe);
    write(output/L"cache-sentinel.txt","preserve warm cache");
    wait(c,submit(c,execution(snapshot)));program(output,7);
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
    auto weightedArguments=execution(snapshot);
    weightedArguments.insert(weightedArguments.end(),{"-CpuSlots","2","-MemoryMiB","2048"});
    const auto weighted=wait(c,submit(c,weightedArguments));program(output,7);
    require(weighted.at("record").at("resourceAdmission").at("bindingValid").boolean&&
        weighted.at("resources").at("requestedCpuSlots").integer()==2&&weighted.at("resources").at("requestedMemoryMiB").integer()==2048&&
        weighted.at("resources").at("finished").boolean&&weighted.at("resources").at("accountingAvailable").boolean,
        "Candidate private entry did not reuse its admitted runtime and parent CPU/memory budget");
    std::cout<<"PASS private candidate compiler/test execution, editing overlap, frozen toolset, warm cache, slot queue/cancel, urgent messages, failed reports and owner loss\n";
}
void executor_control(const fs::path& repo,bool checkout) {
    const std::string mutation=checkout?"checkout-lock":"unlocked-source";
    const std::string assertion=checkout?"Original checkout editing remained blocked":"Active candidate source accepted a concurrent writer";
    const auto result=run({executable().wstring(),repo.wstring(),wide("--"+mutation)},repo,85,{},true);
    require(result.code==1&&result.err.find(assertion)!=std::string::npos,"Executor "+mutation+" negative control did not reach its business assertion: "+result.out+result.err);
    std::cout<<"PASS executor "<<mutation<<" negative control reached its business assertion\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){
    const auto mode=argc>2?std::string(argv[2]):"";
    if(mode=="--tool-verification"||mode=="--tool-verification-old-preset")candidate_batches(repo,false,true,mode=="--tool-verification-old-preset");
    else if(mode=="--batches"||mode=="--batches-held-window")candidate_batches(repo,mode=="--batches-held-window");
    else if(mode=="--artifacts"||mode=="--artifacts-slot-reference")artifacts(repo,mode=="--artifacts-slot-reference");
    else if(mode=="--preflight"||mode=="--preflight-old-path")private_preflight(repo,mode=="--preflight-old-path");
    else if(mode=="--verify-checkout-lock")executor_control(repo,true);
    else if(mode=="--verify-source-lock")executor_control(repo,false);
    else if(mode=="--all"){scenario(repo,"");executor_control(repo,true);executor_control(repo,false);}
    else scenario(repo,mode.empty()?"":mode.substr(2));
});}
