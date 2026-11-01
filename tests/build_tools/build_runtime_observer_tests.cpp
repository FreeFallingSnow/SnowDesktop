#include "../build_tool_test_support.h"
#include <cstring>
#include <iostream>
#include <set>
#include <thread>
using namespace build_test;
namespace {
const Json& module(const Json& report,const std::wstring& filename){for(const auto& image:report.at("images").items)if(fs::path(wide(image.at("path").str())).filename()==filename)return image;throw std::runtime_error("Actual DLL load was absent from runtime observation");}
int fixture_processes(const Json& report,const fs::path& executable){int count=0;for(const auto& image:report.at("images").items)if(image.at("kind").str()=="executable"&&fs::path(wide(image.at("path").str())).filename()==executable.filename())++count;return count;}
std::set<std::string> root_modules(const Json& report){
    require(report.at("status").str()=="observed","Module comparison requires complete observations");std::set<std::string> modules;
    for(const auto& image:report.at("images").items)if(image.at("pid").integer()==report.at("rootPid").integer())modules.insert(image.at("kind").str()+"|"+image.at("path").str()+"|"+image.at("sha256").str());
    return modules;
}
void raw(const fs::path& root,const std::string& mode){
    if(mode=="--exit-seven")ExitProcess(7);
    if(mode=="--env-check"){require(environment(L"SNOWDESKTOP_TEST_RUNTIME_REPORT_ROOT").empty(),"Observer telemetry leaked into the test environment");return;}
    if(mode=="--wait"){
        write(root/L"alive.json",Json::object({{"pid",static_cast<double>(GetCurrentProcessId())}}).dump());
        while(!fs::exists(root/L"stop"))std::this_thread::sleep_for(std::chrono::milliseconds(25));return;
    }
    if(mode=="--spawn"){
        const auto child=run({executable().wstring(),root.wstring(),L"--load"},root);
        require(child.code==0,"Observed descendant failed: "+child.out+child.err);return;
    }
    require(mode=="--load","Unknown runtime fixture mode");
    try{throw std::runtime_error("Handled fixture exception");}catch(const std::runtime_error&){}
    HMODULE loaded=LoadLibraryW((root/L"观察模块.dll").c_str());require(loaded!=nullptr,"Cannot load Unicode runtime fixture DLL");
    const auto symbol=GetProcAddress(loaded,"runtime_observer_value");
    if(!symbol){FreeLibrary(loaded);throw std::runtime_error("Runtime fixture export is absent");}
    using Function=int(__cdecl*)();Function value=nullptr;static_assert(sizeof(value)==sizeof(symbol));std::memcpy(&value,&symbol,sizeof(value));
    const int result=value();FreeLibrary(loaded);require(result==std::stoi(read(root/L"expected.txt")),"Loaded DLL behavior differs from the selected fixture");
}
void tests(const fs::path& repo){
    const auto root=temporary("module 观察");const auto observed=root/L"运行 测试.exe";fs::copy_file(executable(),observed);
    const auto observer=executable().parent_path()/L"SnowDesktopTestRuntimeObserver.exe";
    const auto fixtureA=executable().parent_path()/L"SnowDesktopRuntimeObserverFixtureA.dll",fixtureB=executable().parent_path()/L"SnowDesktopRuntimeObserverFixtureB.dll";
    require(fs::is_regular_file(observer)&&fs::is_regular_file(fixtureA)&&fs::is_regular_file(fixtureB),"Native runtime observer fixtures were not built");
    fs::copy_file(fixtureA,root/L"观察模块.dll");write(root/L"expected.txt","7");
    const auto arguments=[&](const std::string& name,const std::wstring& mode){return std::vector<std::wstring>{observer.wstring(),L"--test",wide(name),L"--report",(root/L"reports"/wide(name+".manual.json")).wstring(),L"--",observed.wstring(),root.wstring(),mode};};
    const auto trace=[&](const std::string& name,const std::wstring& mode,int expected=0){const auto result=run(arguments(name,mode),root,20);require(result.code==expected,"Runtime observer returned unexpected exit: "+result.out+result.err);return Json::parse(read(root/L"reports"/wide(name+".manual.json")));};
    const auto first=trace("Alpha",L"--load");const auto& loaded=module(first,L"观察模块.dll");
    require(first.at("rootExitCode").integer()==0&&fixture_processes(first,observed)==1&&loaded.at("sha256").str().size()==64&&loaded.at("unloaded").boolean&&!loaded.at("startup").boolean&&loaded.at("identitySource").str()=="load-event-handle","Transient Unicode DLL was not associated with its actual load handle/phase");
    const auto descendant=trace("Child",L"--spawn");require(fixture_processes(descendant,observed)==2&&module(descendant,L"观察模块.dll").at("pid").integer()!=descendant.at("rootPid").integer(),"Descendant process DLL loads were omitted");
    const auto failure=trace("Failed",L"--exit-seven",7);require(failure.at("rootExitCode").integer()==7,"Observed test failure was replaced by observer success");
    auto probeArguments=arguments("Probe",L"--wait");probeArguments.insert(probeArguments.begin()+1,L"--probe");
    const auto probe=run(probeArguments,root,15);require(probe.code==0&&!fs::exists(root/L"alive.json"),"Module probe executed the fixture main function: "+probe.out+probe.err);
    const auto probeReport=Json::parse(read(root/L"reports/Probe.manual.json"));require(probeReport.at("mode").str()=="probe"&&probeReport.at("probeStopped").boolean,"Probe was reported as an executed test");
    auto entryArguments=arguments("Entry",L"--wait");entryArguments.insert(entryArguments.begin()+1,L"--entry-probe");
    const auto entry=run(entryArguments,root,15);require(entry.code==0&&!fs::exists(root/L"alive.json"),"Entry probe executed fixture main: "+entry.out+entry.err);
    const auto entryReport=Json::parse(read(root/L"reports/Entry.manual.json"));
    require(entryReport.at("mode").str()=="entry-probe"&&entryReport.at("imageEntryReached").boolean&&entryReport.at("probeStopped").boolean&&entryReport.at("entryPointRva").integer()>0&&entryReport.at("dllInitializationMayRun").boolean&&!entryReport.at("reuseAllowed").boolean,"Entry probe lost its boundary or was reported as reusable execution evidence");
    const auto fullModules=root_modules(first),entryModules=root_modules(entryReport);
    require(entryReport.at("runtimeClosure").str()=="unclosed"&&!entryReport.at("reuseAllowed").boolean,
        "Entry observation must not claim reusable runtime closure");
    for(const auto& image:entryModules)require(fullModules.contains(image),
        "Entry image identity was absent or changed in full execution of the same owned binary");
    require(entryModules.size()<fullModules.size(),
        "The real transient DLL must distinguish full execution from entry-only observation");
    const Env telemetry={{L"SNOWDESKTOP_TEST_RUNTIME_REPORT_ROOT",(root/L"telemetry").wstring()}};
    const auto env=run({observer.wstring(),L"--test",L"Env",L"--",observed.wstring(),root.wstring(),L"--env-check"},root,15,telemetry);
    require(env.code==0&&fs::is_directory(root/L"telemetry"),"Observer report routing changed its child environment: "+env.out+env.err);
    const auto oldReport=read(root/L"reports/Alpha.manual.json");const auto collision=run(arguments("Alpha",L"--wait"),root,10);
    require(collision.code==2&&!fs::exists(root/L"alive.json")&&read(root/L"reports/Alpha.manual.json")==oldReport,"Existing report was overwritten or its target was executed again");
    copy(repo,root,{"scripts/build_test_dependencies.ps1","scripts/build_test_runtime_inputs.ps1","scripts/build_test_runtime_modules.ps1"});
    Json command=Json::array({utf8(observer.wstring()),"--test","Alpha","--",utf8(observed.wstring()),utf8(root.wstring()),"--load"});
    write(root/L"tests.json",Json::array({Json::object({{"name","Alpha"},{"command",command},{"properties",Json::array({Json::object({{"name","WORKING_DIRECTORY"},{"value",utf8(root.wstring())}})})}})}).dump());
    write(root/L"inspect.ps1",R"PS(Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'scripts/build_test_dependencies.ps1')
. (Join-Path $PSScriptRoot 'scripts/build_test_runtime_modules.ps1')
$tests=@([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'tests.json'))|ConvertFrom-Json)
Get-TestRuntimeModuleObservations $PSScriptRoot $tests (Join-Path $PSScriptRoot 'reports')|ConvertTo-Json -Depth 12
)PS",true);
    Bridge bridge(repo,root);const auto inspected=bridge.call(root/L"inspect.ps1",{});const auto& row=inspected.at("tests").items[0];
    require(row.at("status").str()!="unavailable"&&row.at("imageCount").integer()>2&&row.at("reportSha256").str().size()==64&&!row.at("reuseAllowed").boolean,"Actual module report did not bind to per-test evidence: "+inspected.dump());
    fs::copy_file(fixtureB,root/L"观察模块.dll",fs::copy_options::overwrite_existing);write(root/L"expected.txt","9");const auto changed=trace("Changed",L"--load");
    require(module(changed,L"观察模块.dll").at("sha256").str()!=loaded.at("sha256").str(),"Changed DLL behavior retained its old module identity");
    const auto stale=bridge.call(root/L"inspect.ps1",{});require(stale.at("tests").items[0].at("status").str()=="incomplete"&&stale.dump().find("loaded-module-file-changed")!=std::string::npos,"Old successful observation accepted replaced DLL bytes");
    auto forged=Json::parse(oldReport);forged["mode"]="probe";write(root/L"reports/Alpha.manual.json",forged.dump());require(bridge.call(root/L"inspect.ps1",{}).at("tests").items[0].at("status").str()=="unavailable","Probe was accepted as runtime execution evidence");write(root/L"reports/Alpha.manual.json",oldReport);
    forged["mode"]="entry-probe";write(root/L"reports/Alpha.manual.json",forged.dump());require(bridge.call(root/L"inspect.ps1",{}).at("tests").items[0].at("status").str()=="unavailable","Entry probe was accepted as runtime execution evidence");write(root/L"reports/Alpha.manual.json",oldReport);
    Child unrelated({standin().wstring(),L"--wait",L"--marker",(root/L"unrelated").wstring()},root);
    until([&]{return fs::exists(root/L"unrelated")||!unrelated.running();});
    require(fs::exists(root/L"unrelated"),"Unrelated fixture exited before its marker: "+unrelated.out()+unrelated.err());
    auto timed=arguments("Timed",L"--wait");timed.insert(timed.begin()+1,{L"--timeout-ms",L"1000"});
    Child timeout(timed,root);until([&]{return fs::exists(root/L"alive.json")||!timeout.running();});require(fs::exists(root/L"alive.json"),"Timed fixture exited before its marker: "+timeout.out()+timeout.err());const auto owner=Json::parse(read(root/L"alive.json"));
    HANDLE target=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(owner.at("pid").integer()));require(target!=nullptr,"Cannot bind timeout fixture process handle");
    const auto timedResult=timeout.wait(15);const bool exited=WaitForSingleObject(target,3000)==WAIT_OBJECT_0;CloseHandle(target);
    require(timedResult.code==2&&exited&&unrelated.running(),"Observer timeout failed to contain its own process lifetime");
    require(Json::parse(read(root/L"reports/Timed.manual.json")).at("timedOut").boolean,"Observer timeout lost its original report");fs::remove(root/L"alive.json");
    auto heldArguments=arguments("Killed",L"--wait");Child held(heldArguments,root);until([&]{return fs::exists(root/L"alive.json")||!held.running();});require(fs::exists(root/L"alive.json"),"Observer-loss fixture exited before its marker: "+held.out()+held.err());
    const auto child=Json::parse(read(root/L"alive.json"));target=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(child.at("pid").integer()));require(target!=nullptr,"Cannot bind observer-loss fixture process handle");
    HANDLE observerProcess=OpenProcess(PROCESS_TERMINATE,FALSE,held.pid());require(observerProcess!=nullptr,"Cannot bind owned observer process");require(TerminateProcess(observerProcess,55)!=0,"Cannot terminate owned observer fixture");CloseHandle(observerProcess);held.wait(15);
    const bool contained=WaitForSingleObject(target,3000)==WAIT_OBJECT_0;CloseHandle(target);require(contained&&unrelated.running(),"Observer loss orphaned its child or stopped unrelated work");
    write(root/L"unrelated.stop","done");require(unrelated.wait().code==0,"Unrelated fixture did not stop normally");
    std::cout<<"PASS actual startup/transient/descendant DLL loads, Unicode paths, DLL replacement, probe isolation, exit codes, report collision, endpoint validation and contained timeout/observer loss\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){const std::string mode=argc>2?argv[2]:"";if(mode.empty())tests(repo);else raw(repo,mode);});}
