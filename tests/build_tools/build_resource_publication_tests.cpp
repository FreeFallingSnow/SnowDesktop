#include "../build_tool_test_support.h"
#include <iostream>
#include <thread>
using namespace build_test;

namespace {
void queue_case(const fs::path& repo,bool formerBehavior,bool heldToTerminal){
    Coordinator c(repo,"queue-counter-publication");auto source=read(c.scripts/L"build_job.cs");
    if(formerBehavior)replace(source,"const int ResourcePublicationBudgetMilliseconds = 1000;","const int ResourcePublicationBudgetMilliseconds = 0;");
    replace(source,"AtomicPublication.Publish(temporary, counter);","File.WriteAllText(counter + \".publication-attempt\", \"ready\"); AtomicPublication.Publish(temporary, counter);");
    write(c.scripts/L"build_job.cs",source);
    const auto script=c.root/L"join-queue.ps1",resource=c.root/L"resource";
    write(script,R"PS(param([string]$Id)
. $PSScriptRoot/scripts/build_entry.ps1
Initialize-ResourceRuntime
$waiter=$null
try {
 $waiter=[SnowDesktop.Build.ResourceWaiter]::Join((Join-Path $PSScriptRoot 'resource'),$Id)
 @{sequence=$waiter.Sequence;position=$waiter.Inspect().Position}|ConvertTo-Json -Compress
}catch{
 $failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException}
 @{native=($failure.HResult -band 0xffff);error=$failure.Message}|ConvertTo-Json -Compress
 exit 2
}finally{if($waiter){$waiter.Dispose()}}
)PS",true);
    require(c.bridge->call(script,{"-Id",std::string(32,'a')}).at("sequence").integer()==1,"Cannot establish original queue sequence");
    const auto counter=resource/L"admission/sequence",marker=fs::path(counter.wstring()+L".publication-attempt");fs::remove(marker);
    struct Reader {HANDLE handle=INVALID_HANDLE_VALUE;~Reader(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}void close(){require(CloseHandle(handle)!=FALSE,"Cannot close queue counter reader");handle=INVALID_HANDLE_VALUE;}} reader;
    reader.handle=CreateFileW(counter.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    require(reader.handle!=INVALID_HANDLE_VALUE,"Cannot hold real queue counter reader");
    Child publisher({powershell().wstring(),L"-NoProfile",L"-File",script.wstring(),L"-Id",std::wstring(32,L'b')},c.root);
    until([&]{return fs::exists(marker);},12);const auto started=std::chrono::steady_clock::now();
    if(!heldToTerminal){std::this_thread::sleep_for(std::chrono::milliseconds(350));reader.close();}
    const auto result=publisher.wait(6);
    if(heldToTerminal){
        reader.close();const auto error=Json::parse(result.out);const auto native=error.at("native").integer();
        require(result.code==2&&(native==32||native==33||native==1175)&&read(counter)=="1"&&std::chrono::steady_clock::now()-started<std::chrono::seconds(5),"Held queue reader changed the original sequence or escaped bounded failure");
    }else{
        require(result.code==0&&Json::parse(result.out).at("sequence").integer()==2&&read(counter)=="2","Queue counter was not published after the reader released");
    }
    for(const auto& file:fs::directory_iterator(counter.parent_path()))require(file.path().extension()!=L".ticket","Queue publication leaked or duplicated a scheduling ticket");
}
void resource_case(const fs::path& repo,bool formerBehavior,bool heldToTerminal){
    Coordinator c(repo,formerBehavior?"metric-single-attempt":(heldToTerminal?"metric-reader-held":"metric-reader-release"));
    auto source=read(c.scripts/L"build_job.cs");
    if(formerBehavior)replace(source,"const int ResourcePublicationBudgetMilliseconds = 1000;",
        "const int ResourcePublicationBudgetMilliseconds = 0;");
    // Observe the real terminal Sample entry; do not replace its writer,
    // accounting, Job, process lifetime or successful pipeline outcome.
    replace(source,"string temporary = metricsPath +",
        "if (finished) File.WriteAllText(metricsPath + \".terminal-attempt\", \"ready\");\n            string temporary = metricsPath +");
    write(c.scripts/L"build_job.cs",source);
    c.call({"task-open","metrics","-TaskName","terminal resource publication"});
    c.gate("hold-build");
    const auto submitted=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),
        L"-Command",L"start",L"-Task",L"metrics",L"-Action",L"release"},c.root,15,{},true);
    require(submitted.code==0,"Resource publication submission failed: "+submitted.out+submitted.err);
    const auto id=Json::parse(submitted.out).at("id").str();
    const auto metric=c.stateRoot/L"runs"/wide(id+".resources.json");
    until([&]{return fs::exists(c.root/L"build-started")&&fs::exists(metric);},20);
    struct Reader {
        HANDLE handle=INVALID_HANDLE_VALUE;
        ~Reader(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
        void close(){require(handle!=INVALID_HANDLE_VALUE&&CloseHandle(handle)!=FALSE,"Metric reader close failed");handle=INVALID_HANDLE_VALUE;}
    } reader;
    reader.handle=CreateFileW(metric.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    require(reader.handle!=INVALID_HANDLE_VALUE,"Cannot hold the controlled non-delete-sharing reader");
    c.gate("hold-build",false);
    until([&]{return fs::exists(fs::path(metric.wstring()+L".terminal-attempt"));},15);
    if(!heldToTerminal){
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        reader.close();
    }
    const auto waited=run({powershell().wstring(),L"-NoProfile",L"-File",(c.scripts/L"build_run.ps1").wstring(),
        L"-Command",L"wait",L"-RunId",wide(id),L"-Seconds",L"8"},c.root,18);
    const auto view=Json::parse(waited.out);
    require(waited.code==0&&view.at("status").str()=="passed","Successful pipeline outcome changed while publishing resources: "+waited.out+waited.err);
    const auto final=Json::parse(read(metric));
    if(heldToTerminal){
        reader.close();
        require(!final.at("finished").boolean&&read(c.stateRoot/L"runs"/wide(id+".stderr.log")).find("Terminal resource sample unavailable:")!=std::string::npos,
            "Held reader did not produce a bounded and explicitly incomplete terminal sample");
    }else{
        require(final.at("finished").boolean,"Terminal resource record was not published after the reader released");
        require(final.at("accountingAvailable").boolean&&!final.at("cancelled").boolean&&final.at("cpuSeconds").number>=0,
            "Terminal resources lost their actual Job accounting");
    }
    require(c.calls()==1,"Resource publication replayed the pipeline");
}
void run_record_case(const fs::path& repo,bool formerBehavior,bool heldToTerminal){
    Coordinator c(repo,"run-record-publication");
    if(formerBehavior){auto source=read(c.scripts/L"build_job.cs");replace(source,
        "const int ResourcePublicationBudgetMilliseconds = 1000;","const int ResourcePublicationBudgetMilliseconds = 0;");write(c.scripts/L"build_job.cs",source);}
    const auto script=c.root/L"publish-run-record.ps1",record=c.root/L"record.json",marker=c.root/L"publication-ready";
    write(record,"{\"sequence\":1}");
    // Load the exact production writer AST without executing its CLI entry.
    // The adapter only invokes that producer and returns raw exception data;
    // native C++ owns the reader lifetime and every acceptance assertion.
    write(script,R"PS(
. $PSScriptRoot/scripts/build_entry.ps1
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'scripts/build_run.ps1'),[ref]$tokens,[ref]$errors)
$definition=$ast.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Write-RunJson'},$false)
. ([ScriptBlock]::Create($definition.Extent.Text))
Initialize-ResourceRuntime
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'publication-ready'),'ready')
[IO.File]::AppendAllText((Join-Path $PSScriptRoot 'write-calls'),"1`n")
try {
 Write-RunJson ([pscustomobject]@{sequence=2}) (Join-Path $PSScriptRoot 'record.json')
 @{published=$true}|ConvertTo-Json -Compress
}catch{
 $failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException}
 @{native=($failure.HResult -band 0xffff);error=$failure.Message}|ConvertTo-Json -Compress
 exit 2
}
)PS",true);
    struct Reader{HANDLE handle=INVALID_HANDLE_VALUE;~Reader(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}void close(){require(CloseHandle(handle)!=FALSE,"Cannot close Run record reader");handle=INVALID_HANDLE_VALUE;}} reader;
    reader.handle=CreateFileW(record.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    require(reader.handle!=INVALID_HANDLE_VALUE,"Cannot hold actual Run record read handle");
    Child publisher({powershell().wstring(),L"-NoProfile",L"-File",script.wstring()},c.root);
    until([&]{return fs::exists(marker);},12);const auto started=std::chrono::steady_clock::now();
    if(!heldToTerminal){std::this_thread::sleep_for(std::chrono::milliseconds(350));reader.close();}
    const auto result=publisher.wait(6);
    if(heldToTerminal){
        reader.close();const auto error=Json::parse(result.out);const auto native=error.at("native").integer();
        require(result.code==2&&(native==32||native==33||native==1175)&&Json::parse(read(record)).at("sequence").integer()==1&&
            std::chrono::steady_clock::now()-started<std::chrono::seconds(5),"Held Run reader changed original JSON or escaped bounded failure");
    }else{
        require(result.code==0&&Json::parse(read(record)).at("sequence").integer()==2,
            "Run record was not published after the reader released: "+result.out+result.err);
    }
    require(read(c.root/L"write-calls")=="1\n","Run JSON publication replayed the producer");
    for(const auto& item:fs::directory_iterator(c.root))require(item.path().extension()!=L".tmp","Run JSON publication leaked a temporary file");
}
}
void run_record_publication_tests(const fs::path& repo,bool formerBehavior){
    run_record_case(repo,formerBehavior,false);
    if(!formerBehavior){
        run_record_case(repo,false,true);
        const auto control=run({executable().wstring(),repo.wstring(),L"--run-record-single-attempt"},repo,35,{},true);
        require(control.code==1&&control.err.find("Run record was not published after the reader released")!=std::string::npos,
            "Former single-attempt Run writer failed outside its reader-publication assertion: "+control.out+control.err);
    }
    std::cout<<"PASS production Run JSON writer survives a released reader, bounds persistent sharing failure and does not replay its producer\n";
}
void resource_queue_publication_tests(const fs::path& repo,bool formerBehavior){
    queue_case(repo,formerBehavior,false);
    if(!formerBehavior){
        queue_case(repo,false,true);
        const auto control=run({executable().wstring(),repo.wstring(),L"--queue-single-attempt"},repo,35,{},true);
        require(control.code==1&&control.err.find("Queue counter was not published after the reader released")!=std::string::npos,"Single-attempt queue control did not reach its publication assertion: "+control.out+control.err);
    }
}
void resource_publication_tests(const fs::path& repo,bool formerBehavior){
    resource_case(repo,formerBehavior,false);
    if(!formerBehavior){
        resource_case(repo,false,true);
        const auto root=temporary("resource-writer-control");
        const auto control=run({executable().wstring(),repo.wstring(),L"--resource-single-attempt"},root,55,{},true);
        require(control.code==1&&control.err.find("Terminal resource record was not published after the reader released")!=std::string::npos,
            "Former single-attempt writer did not fail at terminal resource publication: "+control.out+control.err);
        resource_queue_publication_tests(repo,false);
    }
    std::cout<<"PASS real terminal Job resources survive a bounded reader conflict without pipeline replay\n";
}
