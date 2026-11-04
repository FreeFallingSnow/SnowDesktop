#include "build_tool_test_support.h"
#include <algorithm>
#include <iostream>
#include <regex>
#include <thread>
using namespace build_test;
namespace {
void test(const fs::path& repo) {
    auto root=temporary("foreground-wait");copy(repo,root,{"scripts/build_wait_tasks.py","tools/build-dashboard/server.py"});auto folder=root/L".build/collaboration";fs::create_directories(folder);
    std::string batch(32,'a');auto me=owner(GetCurrentProcessId());
    auto state=Json::object({{"schemaVersion",1},{"repositoryRoot",utf8(root.wstring())},{"current",Json::object({{"id",batch},{"phase","building"},{"owner",me},{"participants",Json::array({Json::object({{"id","peer"},{"state","finished"}})})}})}});
    auto save=[&]{atomic(folder/L"state.json",state.dump());};save();std::vector<Json> owners;
    auto args=[&](const std::vector<std::string>& values){std::vector<std::wstring> command={production_python().wstring(),(root/L"scripts/build_wait_tasks.py").wstring(),L"watch"};for(const auto& value:values)command.push_back(wide(value));command.insert(command.end(),{L"--root",root.wstring()});return command;};
    auto cli=[&](const std::vector<std::string>& values){auto result=run(args(values),root,10);require(result.code==0,result.out+result.err);return Json::parse(result.out);};
    auto begin=[&](const std::string& actor,const std::vector<std::string>& extra=std::vector<std::string>{}){
        std::vector<std::string> values={"wait",actor};values.insert(values.end(),extra.begin(),extra.end());auto process=std::make_unique<Child>(args(values),root);
        std::string ticketId;until([&]{auto error=process->err();std::smatch match;bool found=std::regex_search(error,match,std::regex("ticket ([a-f0-9]{32})"));require(process->running()||found,"Foreground exited before ticket: "+error);if(found)ticketId=match[1].str();return found;},10);
        auto ticket=Json::parse(read(folder/wide(ticketId+".wait.json")));owners.push_back(ticket.at("owner"));return std::make_pair(std::move(process),ticket);
    };
    auto output=[](Child& process,const std::string& status,int code=0){auto result=process.wait(6);require(result.code==code,result.out+result.err);auto value=Json::parse(result.out);require(value.at("status").str()==status,"Wait condition returned "+value.dump());return value;};
    try {
        auto [p,t]=begin("window",{"--timeout","0"});require(t.at("deadlineUtc").kind==Json::Kind::Null,"Unlimited wait created a deadline");DWORD pid=p->pid();std::this_thread::sleep_for(std::chrono::seconds(3));require(p->running()&&p->pid()==pid,"Foreground wait did not keep same PID");
        auto [q,u]=begin("",{"--ticket",t.at("id").str()});std::this_thread::sleep_for(std::chrono::seconds(2));require(q->running()&&p->running()&&u.at("owner").dump()==t.at("owner").dump(),"Reconnect created another worker");
        auto started=std::chrono::steady_clock::now();state["current"]=Json{};save();auto first=output(*p,"eligible"),second=output(*q,"eligible");require(std::chrono::steady_clock::now()-started<std::chrono::seconds(4)&&first.at("id").str()==second.at("id").str()&&first.at("owner").dump()==t.at("owner").dump(),"Condition did not wake unchanged worker promptly");
        state["current"]=Json::object({{"id",batch},{"phase","building"},{"owner",me},{"participants",Json::array({Json::object({{"id","retirement-race"},{"state","finished"},{"editRevision",1}})})}});save();
        auto [r,rt]=begin("retirement-race",{"--condition","result","--batch",batch,"--revision","1"});
        require(rt.at("deadlineUtc").kind==Json::Kind::Null,"Default foreground wait created a deadline");
        require(rt.at("condition").str()=="result","Retirement wait lost its condition");
        HANDLE lock=CreateFileW((folder/L"state.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr);require(lock!=INVALID_HANDLE_VALUE,"Metadata lease unavailable");
        auto result=Json::object({{"batchId",batch},{"outcome","passed"},{"participants",Json::array({Json::object({{"id","retirement-race"},{"editRevision",1}})})}});
        try{atomic(folder/wide(batch+".json"),result.dump());std::this_thread::sleep_for(std::chrono::milliseconds(2200));require(r->running(),"Retirement transaction gap returned early");state["current"]=Json{};save();}catch(...){CloseHandle(lock);throw;}CloseHandle(lock);
        require(output(*r,"completed").at("result").dump()==result.dump(),"Common result changed");fs::remove(folder/wide(batch+".json"));
        // A captured build remains observable while a separate edit window opens.
        auto stoppedOwner=me;stoppedOwner["startTicks"]="0";
        auto unrelated=Json::object({{"id",std::string(32,'b')},{"phase","building"},{"owner",stoppedOwner},{"participants",Json::array({})}});
        auto captured=Json::object({{"id",batch},{"phase","building"},{"owner",me},{"participants",Json::array({
            Json::object({{"id","captured-result"},{"state","finished"},{"editRevision",1}}),
            Json::object({{"id","captured-peers"},{"state","finished"},{"editRevision",1}})})}});
        state["current"]=Json{};state["activeCandidates"]=Json::array({unrelated,captured});save();
        // Use terminal receipts, not observations persisted on the 15s heartbeat.
        auto [capturedResult,crt]=begin("captured-result",{"--condition","result","--batch",batch,"--revision","1","--timeout","3"});
        require(output(*capturedResult,"timed-out",2).at("observations").integer()>=2,
            "Captured result wait did not observe the running build before its deadline");
        state["current"]=Json::object({{"id",std::string(32,'c')},{"phase","editing"},{"participants",Json::array({
            Json::object({{"id","unrelated-editor"},{"state","finished"},{"check",Json::object({{"status","failed"}})}})})}});save();
        auto [capturedPeers,cpt]=begin("captured-peers",{"--condition","peers","--batch",batch,"--revision","1","--timeout","3"});
        require(output(*capturedPeers,"timed-out",2).at("observations").integer()>=2,
            "Captured peer wait did not observe the running build beside the edit window");
        const auto capturedResultValue=Json::object({{"batchId",batch},{"outcome","passed"},{"participants",captured.at("participants")}});
        lock=CreateFileW((folder/L"state.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,0,nullptr);require(lock!=INVALID_HANDLE_VALUE,"Captured retirement lease unavailable");
        try{atomic(folder/wide(batch+".json"),capturedResultValue.dump());state["activeCandidates"]=Json::array({unrelated});save();}catch(...){CloseHandle(lock);throw;}CloseHandle(lock);
        for(const auto* ticket:{&crt,&cpt}){
            const auto resumedTicket=cli({"resume","--ticket",ticket->at("id").str(),"--timeout","20","--reason","Captured fixture result is now durable"});
            owners.push_back(resumedTicket.at("owner"));
            auto reconnected=begin("",{"--ticket",ticket->at("id").str()});
            require(output(*reconnected.first,"completed").at("result").dump()==capturedResultValue.dump(),"Captured common result changed");
        }
        fs::remove(folder/wide(batch+".json"));
        captured["owner"]=stoppedOwner;captured["participants"]=Json::array({Json::object({{"id","captured-stopped"},{"state","finished"},{"editRevision",1}})});
        state["activeCandidates"]=Json::array({unrelated,captured});save();
        auto [stopped,st]=begin("captured-stopped",{"--condition","result","--batch",batch,"--revision","1"});
        require(output(*stopped,"attention",2).at("reason").str().find("Build owner exited")!=std::string::npos,
            "Captured owner loss did not require recovery");
        require(st.at("batchId").str()==batch,"Captured owner wait lost its batch");
        state["activeCandidates"]=Json::array({});
        state["current"]=Json::object({{"id",batch},{"phase","editing"},{"participants",Json::array({Json::object({{"id","file-editor"},{"state","editing"},{"ownedFiles",Json::array({"src/own.cpp"})}}),Json::object({{"id","peer"},{"state","editing"},{"ownedFiles",Json::array({"src/peer.cpp"})}})})}});save();
        auto [f,ft]=begin("file-editor",{"--condition","files","--files","src/peer.cpp"});std::this_thread::sleep_for(std::chrono::milliseconds(300));require(f->running(),"Busy peer files reported eligible");state["current"]["participants"].items[1]["state"]="finished";save();output(*f,"eligible");
        // The wait runs in another process; observe its durable handoff result.
        const auto persisted=Json::parse(read(folder/L"state.json"));
        const auto& participants=persisted.at("current").at("participants").items;
        const auto ownEdit=std::find_if(participants.begin(),participants.end(),[](const auto& value){return value.at("id").str()=="file-editor";});
        require(ownEdit!=participants.end()&&ownEdit->at("state").str()=="editing","Own edit was retired by peer handoff");
        require(ft.at("condition").str()=="files","File wait lost its condition");
        state["current"]=Json::object({{"id",batch},{"phase","editing"},{"participants",Json::array({Json::object({{"id","result"},{"editRevision",1},{"state","finished"},{"check",Json::object({{"status","passed"}})}}),Json::object({{"id","peer"},{"state","editing"}})})}});save();
        auto [check,ct]=begin("result",{"--condition","result","--batch",batch,"--revision","1"});std::this_thread::sleep_for(std::chrono::milliseconds(300));require(check->running(),"Editing peer did not block result wait");state["current"]["participants"].items[0]["check"]["status"]="failed";save();output(*check,"attention",2);
        require(ct.at("condition").str()=="result","Check wait lost its condition");
        state["current"]=Json::object({{"id",batch},{"phase","building"},{"owner",me},{"participants",Json::array({Json::object({{"id","peer"},{"state","finished"}})})}});save();
        auto [disconnected,dt]=begin("disconnected");disconnected->stop();auto [reconnect,ut]=begin("",{"--ticket",dt.at("id").str()});require(dt.at("owner").dump()==ut.at("owner").dump(),"Disconnect lost persistent worker");cli({"cancel","--ticket",dt.at("id").str()});output(*reconnect,"cancelled",2);std::this_thread::sleep_for(std::chrono::milliseconds(200));auto resumed=cli({"resume","--ticket",dt.at("id").str(),"--timeout","20","--reason","Explicit isolated cancelled-wait resume"});owners.push_back(resumed.at("owner"));
        auto [again,at]=begin("",{"--ticket",dt.at("id").str()});state["current"]=Json{};save();require(output(*again,"eligible").at("attempt").integer()==2,"Same-ticket recovery lost attempt");
        require(at.at("owner").dump()==resumed.at("owner").dump(),"Resumed worker identity changed");
        state["current"]=Json::object({{"id",batch},{"phase","building"},{"owner",me},{"participants",Json::array({})}});save();
        auto [crashed,crashTicket]=begin("crashed-worker");stop_owner(crashTicket.at("owner"));output(*crashed,"interrupted",2);resumed=cli({"resume","--ticket",crashTicket.at("id").str(),"--timeout","20","--reason","Verified fixture worker stopped"});owners.push_back(resumed.at("owner"));auto [recover,recoveredTicket]=begin("",{"--ticket",crashTicket.at("id").str()});state["current"]=Json{};save();require(output(*recover,"eligible").at("attempt").integer()==2,"Crash recovery attempt incorrect");
        require(recoveredTicket.at("owner").dump()==resumed.at("owner").dump(),"Crash reconnect changed worker");
        state["current"]=Json::object({{"id",batch},{"phase","building"},{"owner",me},{"participants",Json::array({})}});save();auto [bounded,bt]=begin("bounded",{"--timeout","1"});output(*bounded,"timed-out",2);
        require(bt.at("deadlineUtc").kind==Json::Kind::String,"Bounded request lacks deadline evidence");
        for(const auto& value:owners)stop_owner(value);
    } catch(...){for(const auto& value:owners)stop_owner(value);throw;}
    std::cout<<"PASS real foreground blocking, unchanged PID, reconnect, locked retirement gap, captured batch/result/owner tracking, file handoff, check attention, explicit cancel/resume, crash recovery and bounded timeout"<<std::endl;
}
}
int main(int argc,char** argv) {return test_main(argc,argv,test);}
