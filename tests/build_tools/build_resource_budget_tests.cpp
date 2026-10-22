#include "../build_tool_test_support.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
using namespace build_test;
namespace {
struct BudgetHarness {
    Coordinator first;
    fs::path secondRoot;
    std::unique_ptr<Bridge> second;
    int cpuCapacity=1;
    explicit BudgetHarness(const fs::path& repo):first(repo,"weighted-resources"),secondRoot(first.root/L"second worktree") {
        auto runtime=read(first.scripts/L"build_runtime.ps1");runtime+="\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='isolated resource fixture'}}\n";write(first.scripts/L"build_runtime.ps1",runtime,true);
        auto fake=read(first.scripts/L"fake.ps1");replace(fake,"if($Phase -eq 'build'){","if($Phase -eq 'build'){\n [IO.File]::WriteAllText((Join-Path $root 'active-run.txt'),$env:SNOWDESKTOP_RUN_ID)\n");write(first.scripts/L"fake.ps1",fake,true);
        first.call({"task-open","runner","-TaskName","weighted resource owner"});
        const auto proposed=call(0,{"-Command","budget"});
        require(!proposed.at("persisted").boolean&&!fs::exists(fs::path(wide(proposed.at("path").str()))),"Read-only budget query created a policy");
        cpuCapacity=std::min(2,proposed.at("policy").at("cpuCapacity").integer());
        require(proposed.at("policy").at("memoryCapacityMiB").integer()>=1024,"Resource fixture requires1GiB of project capacity");
        call(0,{"-Command","budget","-Task","runner","-CpuCapacity",std::to_string(cpuCapacity),"-MemoryCapacityMiB","1024","-Reason","controlled small capacity"});
        require(first.git({L"worktree",L"add",L"--detach",secondRoot.wstring(),L"HEAD"}).code==0,"Resource worktree creation failed");
        fs::copy(first.scripts,secondRoot/L"scripts",fs::copy_options::recursive|fs::copy_options::overwrite_existing);
        second=std::make_unique<Bridge>(repo,secondRoot,false,true);
        second->call(secondRoot/L"scripts/build_manager.ps1",{"task-open","runner","-TaskName","second weighted resource owner"});
        gate(0,true);gate(1,true);
    }
    fs::path root(int side) const {return side?secondRoot:first.root;}
    Bridge& bridge(int side){return side?*second:*first.bridge;}
    Json call(int side,const std::vector<std::string>& args,int code=0){return bridge(side).call(root(side)/L"scripts/build_run.ps1",args,code);}
    Json record(int side,const std::string& id){return Json::parse(read(root(side)/L".build/collaboration/runs"/wide(id+".json")));}
    void gate(int side,bool hold){const auto path=root(side)/L"hold-build";if(hold)write(path,"held");else fs::remove(path);}
    std::string start(int side,int cpu=1,int memory=512,bool legacy=false){
        std::vector<std::string> args={"-Command","start","-Task","runner","-Action","release"};
        if(!legacy)args.insert(args.end(),{"-CpuSlots",std::to_string(cpu),"-MemoryMiB",std::to_string(memory)});
        const auto id=call(side,args).at("id").str();
        const auto launched=root(side)/L".build/collaboration/runs"/wide(id+".launch.json");until([&]{return fs::exists(launched);},15);
        first.workers.push_back(Json::parse(read(launched)));return id;
    }
    Json running(int side,const std::string& id){
        until([&]{const auto state=record(side,id);return state.at("completedUtc").kind!=Json::Kind::Null||
            (state.at("status").str()=="running"&&fs::exists(root(side)/L"active-run.txt")&&read(root(side)/L"active-run.txt")==id);},25);
        const auto result=record(side,id);
        require(result.at("status").str()=="running","Resource fixture did not start: "+result.dump());return result;
    }
    Json queued(int side,const std::string& id,const std::string& reason){
        until([&]{const auto state=record(side,id);return state.at("completedUtc").kind!=Json::Kind::Null||state.at("status").str()=="running"||state.at("waitingFor").str().find(reason)!=std::string::npos;},25);
        const auto result=record(side,id);
        require(result.at("status").str()=="queued"&&result.at("waitingFor").str().find(reason)!=std::string::npos,"Budget did not queue for "+reason+": "+result.dump());return result;
    }
    Json wait(int side,const std::string& id,int code=0){return call(side,{"-Command","wait","-RunId",id,"-Seconds","20"},code);}
    void cancel(int side,const std::string& id){call(side,{"-Command","cancel","-Task","runner","-RunId",id});wait(side,id,4);}
    void finish(int side,const std::string& id){gate(side,false);require(wait(side,id).at("status").str()=="passed","Budget fixture did not finish");gate(side,true);}
};
void reservations(const fs::path& repo){
    BudgetHarness h(repo);
    write(h.root(0)/L"headroom-probe.ps1",R"PS(. $PSScriptRoot/scripts/build_entry.ps1
Initialize-ResourceRuntime
$directory=Join-Path $PSScriptRoot 'headroom-probe'
[void][IO.Directory]::CreateDirectory($directory)
$first=[SnowDesktop.Build.ResourceBroker]::TryReserve($directory,1,1,1,1,0,1073741824)
$second=[SnowDesktop.Build.ResourceBroker]::TryReserve($directory,1,1,1,1,2147483648,1073741824)
try {@{reason=$first.BusyKind;firstAdmitted=[bool]$first.Lease;afterFailureAdmitted=[bool]$second.Lease}|ConvertTo-Json}finally{if($second.Lease){$second.Lease.Dispose()}}
)PS",true);
    const auto headroom=h.bridge(0).call(h.root(0)/L"headroom-probe.ps1",{});
    require(headroom.at("reason").str()=="memory-headroom"&&!headroom.at("firstAdmitted").boolean&&headroom.at("afterFailureAdmitted").boolean,"Insufficient available memory admitted work or leaked partial reservation tokens");
    if(h.cpuCapacity>=2){
        const auto a=h.start(0);const auto ownerA=h.running(0,a);const auto b=h.start(1);h.running(1,b);
        require(owner_alive(ownerA.at("owner")),"Two small reservations failed to coexist");
        const auto policy=h.call(0,{"-Command","budget"});
        const auto refused=h.bridge(0).invoke(h.root(0)/L"scripts/build_run.ps1",{"-Command","budget","-Task","runner","-CpuCapacity","1","-MemoryCapacityMiB","1024","-Reason","must refuse live change"});
        require(refused.code==2&&h.call(0,{"-Command","budget"}).at("hash").str()==policy.at("hash").str(),"Live policy configuration changed reserved capacity");
        h.finish(1,b);h.finish(0,a);
    }
    const auto full=h.start(0,h.cpuCapacity);const auto fullOwner=h.running(0,full);
    const auto cpuWait=h.start(1);const auto queued=h.queued(1,cpuWait,"cpu-capacity");
    require(queued.at("resourceAdmission").at("lastObservation").at("occupiedCpuSlots").integer()==h.cpuCapacity,"CPU occupancy was fabricated or lost");
    const auto request=h.root(1)/L".build/collaboration/runs"/wide(cpuWait+".request.json");const auto bytes=read(request);
    h.cancel(1,cpuWait);
    require(read(request)==bytes&&owner_alive(fullOwner.at("owner")),"Queued CPU cancellation lost identity or cancelled its peer");h.finish(0,full);
    const auto memoryOwner=h.start(0,1,768);h.running(0,memoryOwner);
    const auto memoryWait=h.start(1,1,768);
    h.queued(1,memoryWait,h.cpuCapacity>=2?"memory-capacity":"cpu-capacity");
    h.cancel(0,memoryOwner);h.running(1,memoryWait);h.finish(1,memoryWait);
    const auto weighted=h.start(0);h.running(0,weighted);
    const auto old=h.start(1,1,512,true);h.queued(1,old,"heavy execution lane");h.cancel(1,old);h.finish(0,weighted);
    const auto oldOwner=h.start(0,1,512,true);h.running(0,oldOwner);
    const auto blocked=h.start(1);h.queued(1,blocked,"heavy execution lane");h.cancel(0,oldOwner);h.running(1,blocked);
    const auto child=Json::parse(read(h.root(1)/L"child.json"));const auto worker=h.record(1,blocked).at("owner");stop_owner(worker);
    until([&]{return !owner_alive(child);},10);
    const auto replacement=h.start(0,h.cpuCapacity,1024);h.running(0,replacement);h.finish(0,replacement);
    const auto lost=h.call(1,{"-Command","status","-RunId",blocked});require(lost.at("status").str()=="interrupted","Owner loss silently adopted another executor");
    const auto invalid=h.bridge(0).invoke(h.root(0)/L"scripts/build_run.ps1",{"-Command","start","-Task","runner","-Action","release","-CpuSlots","1"});
    require(invalid.code==2,"An incomplete weighted request silently used an unbounded budget");
    const auto policy=h.call(0,{"-Command","budget"});const auto policyPath=fs::path(wide(policy.at("path").str()));fs::remove(policyPath);
    require(h.bridge(0).invoke(h.root(0)/L"scripts/build_run.ps1",{"-Command","budget"}).code==2&&!fs::exists(policyPath),"A missing configured policy silently recreated or discarded resource state");
    h.call(0,{"-Command","budget","-Task","runner","-CpuCapacity",std::to_string(h.cpuCapacity),"-MemoryCapacityMiB","1024","-Reason","explicit quiescent policy recovery"});
    std::cout<<"PASS real-worktree weighted coexistence/capacity, immutable queued cancellation, v1 exclusion, policy fencing and owner-loss release\n";
}
void invalidation(const fs::path& repo){
    Coordinator c(repo,"budget-invalidation",true);
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+"\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='isolated policy invalidation'}}\n",true);
    const auto batch=c.begin("owner").at("batchId").str();
    c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","budget","-Task","owner","-CpuCapacity","1","-MemoryCapacityMiB","2048","-Reason","initial frozen budget"});
    auto plan=c.bound("plan","owner",batch);plan.insert(plan.end(),{"-Scope","tool","-Suites","selected","-Tests","Alpha","-Inputs","src/a.txt","-Reason","frozen resource policy","-ResourceCpuSlots","1","-ResourceMemoryMiB","1024"});c.call(plan);
    auto source=read(c.scripts/L"build_run.ps1");replace(source,"        $binding=Get-Field $request 'projectResources'",R"PS(        [IO.File]::WriteAllText((Join-Path $root 'resource-validation-held'),'queued owner persisted')
        while([IO.File]::Exists((Join-Path $root 'hold-validation'))){Start-Sleep -Milliseconds 20}
        $binding=Get-Field $request 'projectResources')PS");write(c.scripts/L"build_run.ps1",source,true);c.gate("hold-validation");
    auto args=c.bound("ready-and-wait","owner",batch);args.insert(args.end(),{"-WaitSeconds","40"});
    std::vector<std::wstring> command={powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_manager.ps1").wstring()};for(const auto& item:args)command.push_back(wide(item));
    Child observer(command,c.root,{},false,false,true);
    until([&]{return fs::exists(c.root/L"resource-validation-held")||!observer.running();},20);
    require(fs::exists(c.root/L"resource-validation-held"),"Frozen policy fixture never published its queued owner: "+observer.err());
    const auto id=c.state().at("current").at("runId").str();const auto recordPath=c.stateRoot/L"runs"/wide(id+".json");
    c.workers.push_back(Json::parse(read(recordPath)).at("owner"));const auto requestPath=c.stateRoot/L"runs"/wide(id+".request.json");const auto request=read(requestPath);
    c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","budget","-Task","owner","-CpuCapacity","1","-MemoryCapacityMiB","1024","-Reason","replace only before resource admission"});
    c.gate("hold-validation",false);const auto result=observer.wait(25);
    require(result.code==5&&Json::parse(result.out).at("outcome").str()=="invalidated","A changed frozen policy did not retire as invalidated: "+result.err+result.out);
    require(c.state().at("current").kind==Json::Kind::Null&&c.calls()==0&&read(requestPath)==request,"Pre-execution invalidation lost ownership, executed a child or mutated its request");
    std::cout<<"PASS policy replacement invalidates and retires the original batch before execution\n";
}
int job_probe(){
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION memory{};JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
    const bool memoryRead=QueryInformationJobObject(nullptr,JobObjectExtendedLimitInformation,&memory,sizeof(memory),nullptr)!=FALSE;
    const bool cpuRead=QueryInformationJobObject(nullptr,JobObjectCpuRateControlInformation,&cpu,sizeof(cpu),nullptr)!=FALSE;
    void* allocation=VirtualAlloc(nullptr,1024ULL*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);const bool allocated=allocation!=nullptr;
    if(allocation)VirtualFree(allocation,0,MEM_RELEASE);
    FILETIME created{},exited{},kernelBefore{},userBefore{},kernelAfter{},userAfter{};
    GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernelBefore,&userBefore);
    std::atomic<bool> stop{false};std::atomic<unsigned long long> checksum{0};std::vector<std::thread> workers;
    for(int i=0;i<4;++i)workers.emplace_back([&]{unsigned long long value=17;while(!stop.load(std::memory_order_relaxed)){for(int j=0;j<512;++j)value=value*6364136223846793005ULL+1;}checksum.fetch_xor(value,std::memory_order_relaxed);});
    std::this_thread::sleep_for(std::chrono::seconds(2));stop=true;for(auto& worker:workers)worker.join();
    GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernelAfter,&userAfter);
    const auto ticks=[](FILETIME value){ULARGE_INTEGER result{};result.LowPart=value.dwLowDateTime;result.HighPart=value.dwHighDateTime;return result.QuadPart;};
    const double used=static_cast<double>(ticks(kernelAfter)+ticks(userAfter)-ticks(kernelBefore)-ticks(userBefore))/10000000.0;
    std::cout<<Json::object({{"memoryRead",memoryRead},{"cpuRead",cpuRead},{"allocationSucceeded",allocated},{"memoryLimitBytes",static_cast<double>(memory.JobMemoryLimit)},
        {"cpuFlags",static_cast<int>(cpu.ControlFlags)},{"cpuRate",static_cast<int>(cpu.CpuRate)},{"observedCpuSeconds",used}}).dump()<<std::endl;return 0;
}
void limits(const fs::path& repo,const std::string& control={}){
    Coordinator c(repo,"job-budget");
    if(!control.empty()){
        auto source=read(c.scripts/L"build_job.cs");
        if(control=="memory")replace(source,"limits.basic.flags |= 0x200;","limits.basic.flags |= 0;");
        else replace(source,"CpuLimits cpu = new CpuLimits { flags = 5,","CpuLimits cpu = new CpuLimits { flags = 1,");
        write(c.scripts/L"build_job.cs",source);
    }
    const auto childCommand=quote(executable().wstring())+L" "+quote(c.root.wstring())+L" --job-probe";
    write(c.root/L"job-budget.ps1",". $PSScriptRoot/scripts/build_entry.ps1\nInitialize-ResourceRuntime\n[SnowDesktop.Build.Job]::ConfigureResourceBudget(1,[SnowDesktop.Build.Job]::LogicalProcessors(),512)\n[SnowDesktop.Build.Job]::ConfigureRunControl((Join-Path $PSScriptRoot 'cancel'),('a'*32),(Join-Path $PSScriptRoot 'metrics.json'))\n$code=[SnowDesktop.Build.Job]::RunLeasedCommand($PSScriptRoot,(Join-Path $PSScriptRoot 'probe.log'),"+ps_literal(utf8(childCommand))+")\n@{code=$code}|ConvertTo-Json\n",true);
    require(c.bridge->call(c.root/L"job-budget.ps1",{}).at("code").integer()==0,"Private budget probe failed");
    const auto probe=Json::parse(read(c.root/L"probe.log"));const auto metrics=Json::parse(read(c.root/L"metrics.json"));
    require(probe.at("memoryRead").boolean&&probe.at("cpuRead").boolean&&!probe.at("allocationSucceeded").boolean&&probe.at("memoryLimitBytes").number==536870912.0,"Real committed-memory limit did not reject a1GiB allocation in the512MiB Job");
    require((probe.at("cpuFlags").integer()&5)==5&&probe.at("cpuRate").number>0&&probe.at("observedCpuSeconds").number>0,"CPU hard-cap flags were not read back from the actual busy child Job");
    require(metrics.at("finished").boolean&&metrics.at("committedMemoryLimitBytes").number==536870912.0&&metrics.at("cpuLimitBasisPoints").integer()==probe.at("cpuRate").integer(),"Producer metrics lost actual kernel limits");
    if(control.empty()){
        for(const auto& item:std::vector<std::pair<std::string,std::string>>{{"memory","Real committed-memory limit did not reject"},{"cpu","CPU hard-cap flags were not read back"}}){
            const auto negative=run({executable().wstring(),repo.wstring(),wide("--limits-no-"+item.first)},c.root,45,{},true);
            require(negative.code==1&&negative.err.find(item.second)!=std::string::npos,"Disabled kernel limit failed to reach its business assertion: "+negative.out+negative.err);
        }
    }
    std::cout<<"PASS actual Job CPU flags/rate, busy-child CPU observation, committed-memory rejection and terminal metrics\n";
}
std::string generic(const fs::path& path){return utf8(path.generic_wstring());}
std::string xml(std::string value){for(const auto& item:std::vector<std::pair<std::string,std::string>>{{"&","&amp;"},{"\"","&quot;"},{"<","&lt;"},{">","&gt;"}}){size_t at=0;while((at=value.find(item.first,at))!=std::string::npos){value.replace(at,item.first.size(),item.second);at+=item.second.size();}}return value;}
void compilation(const fs::path& repo,bool control=false){
    Coordinator c(repo,"weighted-compiler",false,repo/L".codex-probes/weighted-resource-fixtures");
    write(c.scripts/L"build_runtime.ps1",read(c.scripts/L"build_runtime.ps1")+"\nfunction Get-BuildPython {[pscustomobject]@{available=$false;reason='weighted compiler fixture'}}\n",true);
    copy(repo,c.root,{"cmake/SnowDesktop.CompileParallelism.cmake"});
    if(control){auto source=read(c.scripts/L"build_telemetry.ps1");replace(source,"$budget=Get-EntryResourceBudget $Root","$budget=$null");write(c.scripts/L"build_telemetry.ps1",source,true);}
    write(c.root/L".gitignore",read(c.root/L".gitignore")+"!/cmake/\n!/CMakeLists.txt\n!/Directory.Build.targets\n");
    fs::create_directories(c.root/L"proxy");fs::copy_file(standin(),c.root/L"proxy/cl-budget.exe");
    write(c.root/L"CMakeLists.txt",R"CMAKE(cmake_minimum_required(VERSION 3.24)
project(WeightedCompiler LANGUAGES CXX)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
include("${CMAKE_SOURCE_DIR}/cmake/SnowDesktop.CompileParallelism.cmake")
file(WRITE "$ENV{SNOWDESKTOP_COMPILER_PROBE}/compiler-$ENV{SNOWDESKTOP_COMPILER_PROJECT}.txt" "${CMAKE_CXX_COMPILER}")
# Deliberately conflicting project scheduling defaults. The authenticated Run
# must override these without changing persistent CMake cache/default values.
list(APPEND CMAKE_VS_GLOBALS "UseMultiToolTask=false" "EnforceProcessCountAcrossBuilds=true" "MultiProcMaxCount=7" "CLToolPath=${CMAKE_SOURCE_DIR}/proxy/" "CLToolExe=cl-budget.exe")
add_executable(BudgetProgram src/main.cpp src/unit0.cpp src/unit1.cpp src/unit2.cpp)
target_include_directories(BudgetProgram PRIVATE src)
target_precompile_headers(BudgetProgram PRIVATE src/common.h)
target_compile_features(BudgetProgram PRIVATE cxx_std_20)
target_compile_options(BudgetProgram PRIVATE /W4 /WX /MP${SNOWDESKTOP_COMPILE_JOBS} /Z7)
)CMAKE");
    write(c.root/L"CMakePresets.json",R"JSON({"version":3,"configurePresets":[{"name":"release","generator":"Visual Studio 18 2026","architecture":"x64","binaryDir":"${sourceDir}/.build/cxx","cacheVariables":{"SNOWDESKTOP_BUILD_TOOL_JOBS":"0"}}],"buildPresets":[{"name":"release","configurePreset":"release","configuration":"Release","jobs":4,"targets":["BudgetProgram"]}]})JSON");
    write(c.root/L"Directory.Build.targets","<Project><Target Name=\"WeightedProbeGate\" BeforeTargets=\"ClCompile\" Condition=\"'$(MSBuildProjectName)' == 'BudgetProgram'\"><WriteLinesToFile File=\"$(SNOWDESKTOP_COMPILER_PROBE)/scheduling-$(SNOWDESKTOP_COMPILER_PROJECT).txt\" Lines=\"$(UseMultiToolTask)|$(EnforceProcessCountAcrossBuilds)|$(MultiProcMaxCount)|$(MSBuildNodeCount)\" Overwrite=\"true\" /><Exec Command=\"&quot;"+xml(generic(c.root/L"proxy/cl-budget.exe"))+"&quot; --budget-build-gate\" /></Target></Project>\n");
    write(c.root/L"src/common.h","#pragma once\n#include <array>\n#include <string>\n#include <vector>\ninline constexpr int value=11;\n");
    for(int i=0;i<3;++i)write(c.root/L"src"/wide("unit"+std::to_string(i)+".cpp"),"#include \"common.h\"\nint unit"+std::to_string(i)+"(){return value+"+std::to_string(i)+";}\n");
    write(c.root/L"src/main.cpp","#include \"common.h\"\nint unit0();int unit1();int unit2();int main(){return unit0()+unit1()+unit2();}\n");
    write(c.scripts/L"build.bat","@echo off\r\n\"%SNOWDESKTOP_ENTRY_POWERSHELL%\" -NoProfile -File \"%~dp0build_command.ps1\" -Operation configure -Preset release\r\nif errorlevel 1 exit /b %ERRORLEVEL%\r\n\"%SNOWDESKTOP_ENTRY_POWERSHELL%\" -NoProfile -File \"%~dp0build_command.ps1\" -Operation build -Preset release\r\nexit /b %ERRORLEVEL%\r\n");
    c.call({"task-open","runner","-TaskName","actual weighted MSBuild"});
    const auto proposed=c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","budget"}).at("policy");
    const int capacity=std::min(4,proposed.at("cpuCapacity").integer());
    require(capacity>=2&&proposed.at("memoryCapacityMiB").integer()>=6144,"Actual weighted compiler integration requires2 CPU slots and6GiB policy capacity");
    const int cpu=capacity/2;
    c.bridge->call(c.scripts/L"build_run.ps1",{"-Command","budget","-Task","runner","-CpuCapacity",std::to_string(capacity),"-MemoryCapacityMiB","6144","-Reason","two bounded real compiler graphs"});
    const auto other=c.root/L"second worktree";
    require(c.git({L"worktree",L"add",L"--detach",other.wstring(),L"HEAD"}).code==0,"Compiler worktree creation failed");
    for(const auto& directory:{L"scripts",L"src",L"cmake",L"proxy"})fs::copy(c.root/directory,other/directory,fs::copy_options::recursive|fs::copy_options::overwrite_existing);
    copy(c.root,other,{".gitignore","CMakeLists.txt","CMakePresets.json","Directory.Build.targets","tools/build-dashboard/server.py"});
    Bridge second(repo,other,false,true);second.call(other/L"scripts/build_manager.ps1",{"task-open","runner","-TaskName","second bounded compiler"});
    const auto probe=temporary("weighted-compiler-counter");
    const auto start=[&](Bridge& bridge,const fs::path& root,const std::string& side){
        const Env env={{L"SNOWDESKTOP_COMPILER_PROBE",probe.wstring()},{L"SNOWDESKTOP_COMPILER_PROJECT",wide(side)},{L"SNOWDESKTOP_REAL_CL_FILE",(probe/wide("compiler-"+side+".txt")).wstring()}};
        const auto id=bridge.call(root/L"scripts/build_run.ps1",{"-Command","start","-Task","runner","-Action","release","-CpuSlots",std::to_string(cpu),"-MemoryMiB","3072"},0,env).at("id").str();
        const auto launch=root/L".build/collaboration/runs"/wide(id+".launch.json");until([&]{return fs::exists(launch);},15);c.workers.push_back(Json::parse(read(launch)));return id;
    };
    const auto left=start(*c.bridge,c.root,"left"),right=start(second,other,"right");
    const auto finish=[&](Bridge& bridge,const fs::path& root,const std::string& id){
        const auto waited=bridge.invoke(root/L"scripts/build_run.ps1",{"-Command","wait","-Task","runner","-RunId",id,"-Seconds","60"},{},70);
        const auto view=Json::parse(waited.out);require(waited.code==0&&view.at("status").str()=="passed","Weighted actual build failed: "+view.dump()+waited.err);
        int units=0;bool mtt=false;
        for(const auto& event:view.at("stageEvents").items){
            if(event.at("name").str()=="compile"&&event.has("task")&&event.at("task").str()=="MultiToolTask")mtt=true;
            if(event.at("name").str()=="compile"&&event.at("kind").str()=="end"){require(event.at("unitCountKnown").boolean,"Actual compiler observation unavailable");units+=event.at("attemptedUnits").integer();}
            if(event.has("logPath")&&!event.at("logPath").str().empty()&&fs::exists(fs::path(wide(event.at("logPath").str())))){
                const auto log=read(fs::path(wide(event.at("logPath").str())));
                require(log.find("warning C")==std::string::npos&&log.find("warning D")==std::string::npos&&log.find("warning LNK")==std::string::npos,"Compiler/linker warning in actual weighted workload: "+log);
            }
        }
        require(mtt&&units==5,"Weighted compiler controls were not used by the actual MSBuild graph");
        require(view.at("resources").at("committedMemoryLimitBytes").number==3221225472.0&&view.at("resources").at("cpuLimitBasisPoints").number>0,"Actual compiler Job was not resource bounded");
        require(read(root/L".build/cxx/CMakeCache.txt").find("SNOWDESKTOP_BUILD_TOOL_JOBS:STRING=0")!=std::string::npos,"Weighted scheduling changed the persistent default compiler option");
        require(run({(root/L".build/cxx/Release/BudgetProgram.exe").wstring()},root).code==36,"Actual weighted compiler produced an incorrect program");
    };
    finish(*c.bridge,c.root,left);finish(second,other,right);
    for(const auto& side:{"left","right"})require(read(probe/wide(std::string("scheduling-")+side+".txt")).find("true|false|"+std::to_string(cpu)+"|1")!=std::string::npos,"Actual MSBuild scheduling properties did not reflect the Run budget");
    const auto counted=Json::parse(read(probe/L"counter.json"));
    require(counted.at("starts").integer()==10&&counted.at("active").integer()==0&&counted.at("maximum").integer()<=capacity,"Real compilers exceeded pooled capacity or lost provenance");
    if(!control){
        const auto negative=run({executable().wstring(),repo.wstring(),L"--compile-no-scheduling"},c.root,150,{},true);
        require(negative.code==1&&negative.err.find("Weighted compiler controls were not used")!=std::string::npos,"Removed MSBuild scheduling did not reach its actual compiler assertion: "+negative.out+negative.err);
    }
    std::cout<<"PASS two same-repository worktrees, real MSBuild/PCH units, bounded compiler processes and kernel limits, unchanged default cache and removed-scheduling control\n";
}
}
int main(int argc,char** argv){
    const std::string mode=argc>2?argv[2]:"";
    if(mode=="--job-probe")return job_probe();
    return test_main(argc,argv,[&](const fs::path& repo){if(mode=="--limits-no-memory"){limits(repo,"memory");return;}if(mode=="--limits-no-cpu"){limits(repo,"cpu");return;}if(mode=="--compile"||mode=="--compile-no-scheduling"){compilation(repo,mode=="--compile-no-scheduling");return;}reservations(repo);invalidation(repo);limits(repo);});
}
