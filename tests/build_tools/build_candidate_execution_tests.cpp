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
    require(result.code==expected,"Candidate program used live checkout inputs: "+std::to_string(result.code));
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
        replace(script,"$candidateExecution=Enter-CandidateExecution $root $request","$originalEntry=Enter-BuildEntry $root; $candidateExecution=Enter-CandidateExecution $root $request");
        replace(script,"$runnerLocks=@();$stage=$null","$runnerLocks=@();$stage=$null;$originalEntry=$null");
        replace(script,"foreach($stream in $runnerLocks){$stream.Dispose()}","foreach($stream in $runnerLocks){$stream.Dispose()}; if($originalEntry){Exit-BuildEntry $originalEntry}");
        write(c.scripts/L"build_run.ps1",script,true);
    }else if(control=="unlocked-source"){
        auto script=read(c.scripts/L"build_candidate_execution.ps1");
        replace(script,"if($file.state -eq 'present'){$inputLocks+=@([IO.File]::Open($path,'Open','Read',[IO.FileShare]::Read))}","# Controlled former executor holds no source handles.");
        write(c.scripts/L"build_candidate_execution.ps1",script,true);
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
 [IO.File]::WriteAllText($gate+'.ready',(@{pid=$PID;startTicks=(Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks.ToString()}|ConvertTo-Json))
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while([IO.File]::Exists($gate)){if($clock.Elapsed.TotalSeconds -gt 50){throw 'Candidate fixture gate expired'};Start-Sleep -Milliseconds 40}
}
)PS",true);
    require(c.git({L"add",L"--all"}).code==0&&c.git({L"commit",L"-m",L"Candidate executor fixture baseline"}).code==0,"Cannot establish candidate executor baseline");
    c.call({"task-open","candidate","-TaskName","Candidate execution acceptance fixture"});
}
void scenario(const fs::path& repo,const std::string& control) {
    Coordinator c(repo,"x",true,fixture_parent(repo));prepare(c,repo,control);
    const auto snapshot=capture(c);const auto gate=c.stateRoot/L"candidate-gate";write(gate,"hold");
    const Env env={{L"CANDIDATE_FIXTURE_GATE",gate.wstring()}};
    const auto first=submit(c,execution(snapshot),env);
    until([&]{return fs::exists(gate.wstring()+L".ready")||view(c,first).at("terminal").boolean;},40);
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
    until([&]{const auto value=view(c,second);return value.at("record").at("waitingFor").str().find("Candidate slot is busy")!=std::string::npos||value.at("terminal").boolean;},15);
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
    for(const auto& mutation:std::vector<std::pair<std::string,std::string>>{{"checkout-lock","Original checkout editing remained blocked"},{"unlocked-source","Active candidate source accepted a concurrent writer"}}){
        const auto result=run({executable().wstring(),repo.wstring(),wide("--"+mutation.first)},c.root,85,{},true);
        require(result.code==1&&result.err.find(mutation.second)!=std::string::npos,"Executor negative control did not reach its business assertion: "+result.out+result.err);
    }
    std::cout<<"PASS private candidate compiler/test execution, editing overlap, frozen toolset, warm cache, slot queue/cancel, urgent messages, failed reports, owner loss and both legacy controls\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){scenario(repo,argc>2?std::string(argv[2]).substr(2):"");});}
