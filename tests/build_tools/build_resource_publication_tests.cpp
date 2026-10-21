#include "../build_tool_test_support.h"
#include <iostream>
#include <thread>
using namespace build_test;

namespace {
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
}
void resource_publication_tests(const fs::path& repo,bool formerBehavior){
    resource_case(repo,formerBehavior,false);
    if(!formerBehavior){
        resource_case(repo,false,true);
        const auto root=temporary("resource-writer-control");
        const auto control=run({executable().wstring(),repo.wstring(),L"--resource-single-attempt"},root,55,{},true);
        require(control.code==1&&control.err.find("Terminal resource record was not published after the reader released")!=std::string::npos,
            "Former single-attempt writer did not fail at terminal resource publication: "+control.out+control.err);
    }
    std::cout<<"PASS real terminal Job resources survive a bounded reader conflict without pipeline replay\n";
}
