#include "build_tool_test_support.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <thread>
using namespace build_test;
namespace {
bool contains(const std::vector<std::string>& args,const std::string& value) {return std::find(args.begin(),args.end(),value)!=args.end();}
std::string argument(const std::vector<std::string>& args,const std::string& key) {auto item=std::find(args.begin(),args.end(),key);require(item!=args.end()&&item+1!=args.end(),"Stand-in missing argument "+key);return *(item+1);}
void append(const fs::path& path,const std::string& value) {fs::create_directories(path.parent_path());std::ofstream output(path,std::ios::binary|std::ios::app);output<<value;}
std::string target(const std::string& name) {return name=="theme_workflow"?"SnowDesktopThemeWorkflowTests":name=="widget_author_preview_cli"?"SnowDesktopWidgetAuthorPreviewCliTests":"SnowDesktop"+name+"Tests";}
int execute(const std::vector<std::string>& args) {
    auto root=fs::current_path();auto name=utf8(executable().stem().wstring());
    if(contains(args,"--shell-descendant")){Sleep(60000);return 0;}
    if(contains(args,"--selected-probe")){auto selected=argument(args,"--selected-probe");append(root/L"executed.txt",selected+"\n");return selected=="Gamma"?23:selected=="Beta"&&fs::exists(root/L"fail-beta")?31:0;}
    if(name=="python") {
        auto mode=utf8(environment(L"SNOWDESKTOP_STANDIN_MODE"));
        if(contains(args,"-c"))return mode=="old"?78:0;
        append(root/L"enhancement.count","once\n");
        if(mode=="watch") {Json forwarded=Json::array({});for(size_t i=1;i<args.size();++i)forwarded.items.push_back(args[i]);std::cout<<forwarded.dump();return 23;}
        return 19;
    }
    if(name=="cmake") {
        const auto mode=utf8(environment(L"SNOWDESKTOP_STANDIN_MODE"));
        if(mode=="configure-failure")return 19;
        if(mode=="shell") {
            write(root/L"native-token.txt",utf8(environment(L"SNOWDESKTOP_EXECUTION_TOKEN")));
            if(!fs::exists(root/L"native-child.json")){
                auto command=quote(executable().wstring())+L" --shell-descendant";STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
                require(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&child)!=0,"Shell stand-in child failed");
                write(root/L"native-child.json",owner(child.dwProcessId).dump());CloseHandle(child.hThread);CloseHandle(child.hProcess);
            }
            write(root/L"native-started","started");if(fs::exists(root/L"hold-native"))Sleep(60000);return 0;
        }
        if(contains(args,"--build")){append(root/L"build.count","build\n");std::string joined;for(const auto& item:args)joined+=item+" ";append(root/L"build-args.txt",joined+"\n");}
        for(const auto& item:{"Alpha","Beta","theme_workflow","widget_author_preview_cli"})write(root/L".build/Release/tests"/wide(target(item)+".exe"),"fixture binary "+std::string(item));
        fs::create_directories(root/L".build/Release/SnowDesktop.Runtime");std::cout<<"mock configure/build\n";return 0;
    }
    if(name=="ctest") {
        const bool retry=fs::exists(root/L".build/control");
        auto mode=fs::exists(root/L"behavior")?read(root/L"behavior"):retry?read(root/L".build/control"):"pass";
        std::vector<std::string> names=retry?std::vector<std::string>{"build_dashboard","Deterministic"}:std::vector<std::string>{"Alpha","Beta","theme_workflow","widget_author_preview_cli"};
        if(contains(args,"--show-only=json-v1")) {
            Json inventory=Json::object({{"tests",Json::array({})}});
            for(const auto& item:names) {
                if(contains(args,"-R")&&!std::regex_search(item,std::regex(argument(args,"-R"))))continue;
                Json test=Json::object({{"name",item},{"properties",Json::array({})}});
                auto labels=Json::array({retry&&item=="build_dashboard"&&mode!="unreviewed"?"retry-isolated-resource":"core"});
                bool host=item=="theme_workflow"||item=="widget_author_preview_cli";
                if(host&&mode!="legacy")labels.items.push_back("host-runtime");
                if(item=="Beta"&&mode=="environment")labels.items.push_back("environment-blocked");
                test["properties"].items.push_back(Json::object({{"name","LABELS"},{"value",labels}}));
                auto path=utf8((root/L".build/Release/tests"/wide(target(item)+".exe")).wstring());
                test["properties"].items.push_back(Json::object({{"name","REQUIRED_FILES"},{"value",Json::array({path})}}));
                if(mode!="cold"&&!retry){test["command"]=Json::array({path});if(host)test["command"].items.push_back(utf8((root/L".build/Release/SnowDesktop.exe").wstring()));}
                inventory["tests"].items.push_back(test);
            }
            std::cout<<inventory.dump()<<std::endl;return 0;
        }
        auto report=fs::path(wide(argument(args,"--output-junit")));
        auto pattern=std::regex(argument(args,"-R"));std::string xml="<testsuites><testsuite>";
        bool failed=retry?(mode=="always"||mode=="change"):mode=="failed";
        for(const auto& item:names)if(std::regex_search(item,pattern))xml+="<testcase name='"+item+"' time='0.1'>"+(mode=="skipped"?"<skipped/>":failed&&(item=="Beta"||item=="build_dashboard")?"<failure message='fixture failure'/>":"")+"</testcase>";
        write(report,xml+"</testsuite></testsuites>");
        if(retry) {
            append(root/L".build/ctest.count","once\n");
            if(failed) {
                auto signal=Json::object({{"test","build_dashboard"},{"runToken",utf8(environment(L"SNOWDESKTOP_RETRY_RUN_TOKEN"))},{"exitCode",75},{"failureClass","isolated-port-race"},{"cleanupComplete",true},{"sideEffects","none"},{"reason","controlled resource failure"}});
                write(fs::path(environment(L"SNOWDESKTOP_RETRY_SIGNAL_DIR"))/L"build_dashboard.json",signal.dump());
            }
            if(mode=="change")write(root/L"source.cpp","changed during retry");
        }
        std::cout<<"mock test run "<<mode<<std::endl;return failed?(retry?8:17):0;
    }
    if(contains(args,"--crash-retry")) {
        auto folder=fs::path(wide(argument(args,"--folder")));auto batch=argument(args,"--batch");
        write(folder.parent_path()/wide(batch+".retry.json"),Json::object({{"batchId",batch},{"status","retrying"},{"owner",owner(GetCurrentProcessId())},{"tests",Json::object({{"build_dashboard",Json::object({{"status","retrying"}})}})}}).dump());
        write(folder/L"side-effect.count","once");write(folder/L"build_dashboard.attempt2.json",Json::object({{"attempt",2},{"status","passed"},{"exitCode",0}}).dump());return 19;
    }
    if(contains(args,"--wait")) {auto marker=fs::path(wide(argument(args,"--marker")));write(marker,"started");while(!fs::exists(marker.wstring()+L".stop"))std::this_thread::sleep_for(std::chrono::milliseconds(50));return 0;}
    return 0;
}
}
int main(int argc,char** argv) {try {std::vector<std::string> args;for(int i=1;i<argc;++i)args.emplace_back(argv[i]);return execute(args);}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
