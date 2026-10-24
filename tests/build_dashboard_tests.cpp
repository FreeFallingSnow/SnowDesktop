#include <winsock2.h>
#include "build_tool_test_support.h"
#include <winhttp.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <thread>
using namespace build_test;
namespace {
struct Internet {HINTERNET value=nullptr;Internet()=default;explicit Internet(HINTERNET v):value(v){require(v!=nullptr,"WinHTTP handle unavailable");}~Internet(){if(value)WinHttpCloseHandle(value);}Internet(const Internet&)=delete;Internet& operator=(const Internet&)=delete;};
struct HttpResult {int code;std::string body,headers;};
int free_port(){WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"WSAStartup failed");SOCKET sock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(sock!=INVALID_SOCKET,"Fixture socket failed");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=0;int size=sizeof(address);bool bound=bind(sock,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&getsockname(sock,reinterpret_cast<sockaddr*>(&address),&size)==0;closesocket(sock);WSACleanup();require(bound,"Fixture bind failed");return ntohs(address.sin_port);}
HttpResult http(int port,const std::string& path="/api/status",const std::wstring& headers=L"",const wchar_t* method=L"GET"){
    Internet session(WinHttpOpen(L"SnowDesktop native test",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));WinHttpSetTimeouts(session.value,2000,2000,3000,3000);Internet connection(WinHttpConnect(session.value,L"127.0.0.1",static_cast<INTERNET_PORT>(port),0));Internet request(WinHttpOpenRequest(connection.value,method,wide(path).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_ESCAPE_DISABLE));if(!headers.empty())require(WinHttpAddRequestHeaders(request.value,headers.c_str(),static_cast<DWORD>(-1L),WINHTTP_ADDREQ_FLAG_ADD|WINHTTP_ADDREQ_FLAG_REPLACE)!=0,"HTTP fixture headers failed");require(WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)!=0&&WinHttpReceiveResponse(request.value,nullptr)!=0,"HTTP request failed: "+std::to_string(GetLastError()));DWORD status=0,size=sizeof(status);require(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)!=0,"HTTP status missing");size=0;WinHttpQueryHeaders(request.value,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,nullptr,&size,WINHTTP_NO_HEADER_INDEX);std::wstring raw(size/sizeof(wchar_t),0);require(WinHttpQueryHeaders(request.value,WINHTTP_QUERY_RAW_HEADERS_CRLF,WINHTTP_HEADER_NAME_BY_INDEX,raw.data(),&size,WINHTTP_NO_HEADER_INDEX)!=0,"HTTP headers missing");std::string body;char buffer[8192];DWORD count=0;do{require(WinHttpReadData(request.value,buffer,sizeof(buffer),&count)!=0,"HTTP read failed");body.append(buffer,count);require(body.size()<=16*1024*1024,"HTTP response exceeded bound");}while(count);return {static_cast<int>(status),body,utf8(raw)};
}
class Cdp {
    Internet session,connection,request,socket;int sequence=0;
public:
    std::string sessionId,lastExpression;std::vector<std::string> errors;
    Cdp(int port,const std::string& path):session(WinHttpOpen(L"SnowDesktop native CDP",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)),connection(WinHttpConnect(session.value,L"127.0.0.1",static_cast<INTERNET_PORT>(port),0)),request(WinHttpOpenRequest(connection.value,L"GET",wide(path).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0)){
        WinHttpSetTimeouts(session.value,3000,3000,5000,5000);require(WinHttpSetOption(request.value,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0)!=0&&WinHttpSendRequest(request.value,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)!=0&&WinHttpReceiveResponse(request.value,nullptr)!=0,"CDP upgrade failed");socket.value=WinHttpWebSocketCompleteUpgrade(request.value,0);require(socket.value!=nullptr,"CDP socket missing");
    }
    ~Cdp(){
        if(socket.value&&std::uncaught_exceptions()>0){
            const auto expression=lastExpression;
            try{std::cerr<<"Browser failure expression: "<<expression.substr(0,700)<<"\nBrowser view: "<<evaluate("({title:document.getElementById('workTitle')?.textContent,batch:document.getElementById('batch')?.textContent,headline:document.getElementById('headline')?.textContent,tab:document.querySelector('[role=tab][aria-selected=true]')?.id,focus:document.activeElement?.tagName})").dump()<<"\n";}catch(...){}
        }
        if(socket.value)WinHttpWebSocketClose(socket.value,WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,nullptr,0);
    }
    Json send(const std::string& method,Json params=Json::object({}),bool page=true){
        Json command=Json::object({{"id",++sequence},{"method",method},{"params",params}});if(page&&!sessionId.empty())command["sessionId"]=sessionId;auto text=command.dump();require(WinHttpWebSocketSend(socket.value,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,text.data(),static_cast<DWORD>(text.size()))==NO_ERROR,"CDP send failed");
        for(;;){std::string response;WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};do{char buffer[65536];DWORD bytes=0;auto code=WinHttpWebSocketReceive(socket.value,buffer,sizeof(buffer),&bytes,&type);require(code==NO_ERROR&&type!=WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE,"CDP receive failed");response.append(buffer,bytes);require(response.size()<32*1024*1024,"CDP response too large");}while(type==WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE||type==WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);auto message=Json::parse(response);if(message.has("method")&&(message.at("method").str()=="Runtime.exceptionThrown"||message.at("method").str()=="Log.entryAdded"))errors.push_back(response);if(message.has("id")&&message.at("id").integer()==sequence){require(!message.has("error"),"CDP error: "+response);return message.at("result");}}
    }
    Json evaluate(const std::string& expression){lastExpression=expression;auto value=send("Runtime.evaluate",Json::object({{"expression",expression},{"returnByValue",true},{"awaitPromise",true}}));require(!value.has("exceptionDetails"),"Browser expression failed: "+value.dump());auto result=value.at("result");return result.has("value")?result.at("value"):Json{};}
    void connected(){until([&]{auto value=evaluate("document.getElementById('connection')?.textContent");return value.kind==Json::Kind::String&&value.str()=="已连接";},15);}
    void reload(){auto previous=evaluate("performance.timeOrigin").number;send("Page.reload");until([&]{try{return evaluate("performance.timeOrigin").number!=previous&&evaluate("document.readyState").str()=="complete";}catch(...){return false;}},15);connected();}
};
void refresh(){std::this_thread::sleep_for(std::chrono::milliseconds(2100));}
void dashboard_listener(const fs::path& repo,bool formerBehavior=false){
    const auto root=temporary("dashboard-listener");
    copy(repo,root,{"tools/build-dashboard/server.py"});
    const auto serverFile=root/L"tools/build-dashboard/server.py";
    if(formerBehavior){auto source=read(serverFile);replace(source,"server = DashboardHTTPServer((","server = ThreadingHTTPServer((");write(serverFile,source);}
    write(root/L".build/collaboration/state.json","{}");
    const int port=free_port();
    const auto receiptPath=root/L".build/dashboard"/wide("server-"+std::to_string(port)+".json");
    const std::vector<std::wstring> command={production_python().wstring(),serverFile.wstring(),L"--fixture-root",root.wstring(),L"--port",std::to_wstring(port)};
    Child first(command,root);
    until([&]{try{return http(port,"/api/health").code==200;}catch(...){return false;}},10);
    const auto before=read(receiptPath),nonce=Json::parse(before).at("instance").str();
    {
        Child duplicate(command,root);
        until([&]{return !duplicate.running()||read(receiptPath)!=before;},5);
        require(!duplicate.running(),"Duplicate dashboard listener replaced the live instance receipt");
        const auto rejected=duplicate.wait(2);
        require(rejected.code==3&&rejected.err.find("Port unavailable")!=std::string::npos,
            "Duplicate listener did not report a bounded port conflict");
        require(read(receiptPath)==before&&Json::parse(http(port,"/api/health").body).at("instance").str()==nonce,
            "Rejected duplicate changed the original listener identity");
    }
    write(root/L".build/dashboard"/wide("stop-"+nonce+".request"),nonce);
    require(first.wait(5).code==0,"Dashboard did not stop through its normal request");
    Child restarted(command,root);
    until([&]{try{return http(port,"/api/health").code==200;}catch(...){return false;}},10);
    const auto after=Json::parse(read(receiptPath));
    require(after.at("instance").str()!=nonce&&Json::parse(http(port,"/api/health").body).at("instance").str()==after.at("instance").str(),
        "Normal restart did not publish the new listener identity");
    if(!formerBehavior){
        const auto control=run({executable().wstring(),repo.wstring(),L"--old-dashboard-listener"},root,15);
        require(control.code==1&&control.err.find("Duplicate dashboard listener replaced the live instance receipt")!=std::string::npos,
            "Former Windows socket reuse escaped the duplicate-listener business assertion: "+control.out+control.err);
    }
    std::cout<<"PASS duplicate listener rejects without replacing identity, normal restart reconnects; former listener fails\n";
}
void dashboard_startup(const fs::path& repo,bool formerBehavior=false,bool formerLaunch=false){
    auto root=temporary("dashboard-startup");
    copy(repo,root,{"scripts/build_runtime.ps1","tools/build-dashboard/manage.py","tools/build-dashboard/server.py"});
    if(formerBehavior){auto source=read(root/L"scripts/build_runtime.ps1");replace(source,"if(Test-BuildDashboardHealthy $Root $Port){return}","# Former behavior always probes and invokes Python");write(root/L"scripts/build_runtime.ps1",source,true);}
    if(formerLaunch){auto source=read(root/L"tools/build-dashboard/manage.py");replace(source,"creationflags=flags | subprocess.CREATE_BREAKAWAY_FROM_JOB","creationflags=flags");write(root/L"tools/build-dashboard/manage.py",source);}
    const int port=free_port();
    const auto receiptPath=root/L".build/dashboard"/wide("server-"+std::to_string(port)+".json");
    Json serverOwner;
    struct Cleanup {Json& process;~Cleanup(){try{if(process.kind==Json::Kind::Object)stop_owner(process);}catch(...){}}} cleanup{serverOwner};
    {
        Child launcher({production_python().wstring(),(root/L"tools/build-dashboard/manage.py").wstring(),L"start",L"--root",root.wstring(),L"--port",std::to_wstring(port)},root,{},false,false,true);
        const auto result=launcher.wait(15);require(result.code==0,"Managed dashboard startup failed: "+result.out+result.err);
        serverOwner=owner(static_cast<DWORD>(Json::parse(read(receiptPath)).at("pid").integer()));
    }
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,static_cast<DWORD>(serverOwner.at("pid").integer()));
    require(process!=nullptr,"Dashboard stopped when its caller Job closed");
    const bool survived=WaitForSingleObject(process,500)==WAIT_TIMEOUT;CloseHandle(process);
    require(survived&&owner_alive(serverOwner),"Dashboard stopped when its caller Job closed");
    const auto receipt=Json::parse(read(receiptPath));
    require(!receipt.at("fixture").boolean,"Startup fixture must exercise a real non-fixture identity");
    fs::create_directories(root/L"bin");fs::copy_file(standin(),root/L"bin/python.exe");
    Env env={{L"PATH",(root/L"bin").wstring()+L";"+(fs::path(environment(L"SystemRoot"))/L"System32").wstring()},{L"SNOWDESKTOP_STANDIN_MODE",L"dashboard"}};
    write(root/L"scripts/probe.ps1",". $PSScriptRoot/build_runtime.ps1\nEnsure-BuildDashboard -Port "+std::to_string(port)+"\n",true);
    write(root/L"scripts/preferred.ps1",". $PSScriptRoot/build_runtime.ps1\nEnsure-BuildDashboard\n",true);
    Bridge bridge(repo,root);
    const auto ensure=[&]{const auto result=bridge.invoke(root/L"scripts/probe.ps1",{},env);require(result.code==0,"Dashboard startup invocation failed: "+result.out+result.err);};
    for(int i=0;i<3;++i)ensure();
    const auto preferred=Json::parse(read(root/L".build/dashboard/preferred-port.json"));
    require(preferred.at("port").integer()==port,"Managed port preference lost its isolated listener");
    const auto implicit=bridge.invoke(root/L"scripts/preferred.ps1",{},env);require(implicit.code==0,"Preferred dashboard lookup failed: "+implicit.err);
    const auto preferencePath=root/L".build/dashboard/preferred-port.json";
    const auto preferenceBytes=read(preferencePath);
    const auto observed=run({production_python().wstring(),(root/L"tools/build-dashboard/manage.py").wstring(),L"status",L"--root",root.wstring()},root,10);
    require(observed.code==0&&Json::parse(observed.out).at("port").integer()==port&&read(preferencePath)==preferenceBytes,"Readonly status lost or changed the saved port");
    write(preferencePath,"{invalid");const auto invalidPreference=bridge.invoke(root/L"scripts/preferred.ps1",{},env);
    require(invalidPreference.code==0&&invalidPreference.err.find("Optional local dashboard unavailable")!=std::string::npos,"Invalid preference silently selected a different port");
    atomic(preferencePath,preferenceBytes);
    require(!fs::exists(root/L"python-probe.count")&&!fs::exists(root/L"enhancement.count"),"Healthy dashboard unnecessarily started Python");
    auto wrong=receipt;wrong["instance"]=std::string(32,'0');atomic(receiptPath,wrong.dump());ensure();
    require(read(root/L"python-probe.count")=="once\n"&&read(root/L"enhancement.count")=="once\n","Mismatched nonce was silently accepted instead of using the normal manager");
    require(Json::parse(http(port,"/api/health").body).at("instance").str()==receipt.at("instance").str(),"Unknown listener was stopped/replaced");
    wrong=receipt;wrong["fixture"]=true;atomic(receiptPath,wrong.dump());ensure();
    require(read(root/L"enhancement.count")=="once\nonce\n","Fixture receipt was accepted as a production instance");
    write(receiptPath,std::string(4097,' '));ensure();
    require(read(root/L"enhancement.count")=="once\nonce\nonce\n","Oversized receipt was accepted or startup fallback was skipped");
    const auto beforeProbe=read(root/L"python-probe.count"),beforeStart=read(root/L"enhancement.count");
    fs::rename(root/L"tools/build-dashboard/manage.py",root/L"tools/build-dashboard/manage.retained.py");ensure();
    require(read(root/L"python-probe.count")==beforeProbe&&read(root/L"enhancement.count")==beforeStart,"Missing dashboard entry unnecessarily probed Python");
    fs::rename(root/L"tools/build-dashboard/manage.retained.py",root/L"tools/build-dashboard/manage.py");
    atomic(receiptPath,receipt.dump());stop_owner(serverOwner);const auto started=std::chrono::steady_clock::now();ensure();
    require(read(root/L"enhancement.count")=="once\nonce\nonce\nonce\n"&&std::chrono::steady_clock::now()-started<std::chrono::seconds(5),"Stale receipt prevented bounded normal startup fallback");
    const auto controlRoot=temporary("old-dashboard-startup-parent");
    const auto control=run({executable().wstring(),repo.wstring(),L"--old-dashboard-startup"},controlRoot,25);
    require(control.code==1&&control.err.find("Healthy dashboard unnecessarily started Python")!=std::string::npos,"Former startup behavior escaped the Python-process assertion: "+control.out+control.err);
    const auto lifetime=run({executable().wstring(),repo.wstring(),L"--old-dashboard-lifetime"},controlRoot,25);
    require(lifetime.code==1&&lifetime.err.find("Dashboard stopped when its caller Job closed")!=std::string::npos,"Former launch behavior escaped the caller-Job lifetime assertion: "+lifetime.out+lifetime.err);
    std::cout<<"PASS managed dashboard survives caller Job, remembers its port, avoids Python on fresh identity, and rejects invalid identities; former controls fail\n";
}
Json task(const std::string& name="task-a",const std::string& date="2026-10-02T10:00:00Z"){return Json::object({{"id",name},{"state","finished"},{"editRevision",1},{"registeredUtc",date},{"reopenedUtc",date},{"testPlan",Json::object({{"scope","module"},{"suites",Json::array({"selected"})},{"tests",Json::array({"Alpha"})},{"reason","fixture scoped input"}})},{"ownedFiles",Json::array({"src/fixture.cpp"})}});}
Json seed_dashboard_references(const fs::path& repo,const fs::path& root,const Json& base,const std::string& observedRunId,
    Json& runRecord,Json& runRequest,int port,const std::string& bid,bool exercise){
    const auto state=root/L".build/collaboration",runRoot=state/L"runs";
    auto current=[&](){return base;};auto save=[&](const std::string& name,const Json& value){atomic(state/name,value.dump());};
    const std::string intentKey(64,'a');
    runRequest["mode"]="verify";runRequest["planBatch"]=bid;
    runRequest["verification"]=Json::object({{"key",intentKey},{"binding",Json::object({{"names",Json::array({"Alpha"})}})}});
    auto subscribed=current();subscribed["verificationLedger"]=Json::object({{"schemaVersion",1},{"entries",Json::array({Json::object({{"runId",observedRunId},{"key",intentKey},{"subscriptions",Json::array({Json::object({{"id",std::string(32,'3')},{"task","observer"},{"requirementsRevision",2},{"withdrawnUtc",Json{}},{"reason","Verify existing candidate"}})})}})})}});
    save("state.json",subscribed);atomic(runRoot/wide(observedRunId+".request.json"),runRequest.dump());if(exercise){refresh();
    const auto standalone=Json::parse(http(port).body).at("runs").items[0].at("verification");
    require(standalone.at("noBuild").boolean&&!standalone.at("reuseAllowed").boolean&&standalone.at("baseline").str()==bid&&standalone.at("names").items[0].str()=="Alpha"&&standalone.at("subscriptions").items[0].at("requirementsRevision").integer()==2,"HTTP projection lost standalone verification/subscriber provenance");
    }
    runRecord["exitCode"]=0;atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());
    const auto referenceRoot=root/L".build/verification/references",originCoverage=runRoot/wide(observedRunId+".verification.json");
    write(originCoverage,Json::object({{"schemaVersion",1},{"runId",observedRunId},{"status","passed"},{"selected",Json::array({"Alpha"})}}).dump());
    write(root/L"reference-hashes.ps1","param([string]$Run,[string]$Coverage)\n@{run=(Get-FileHash -LiteralPath $Run -Algorithm SHA256).Hash.ToLowerInvariant();coverage=(Get-FileHash -LiteralPath $Coverage -Algorithm SHA256).Hash.ToLowerInvariant()}|ConvertTo-Json\n",true);
    const auto hashResult=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"reference-hashes.ps1").wstring(),(runRoot/wide(observedRunId+".json")).wstring(),originCoverage.wstring()},root,10);
    require(hashResult.code==0,"Cannot capture actual original reference hashes: "+hashResult.err);
    const auto hashes=Json::parse(hashResult.out);
    auto reference=Json::object({{"schemaVersion",1},{"kind","completed-verification-reference"},{"id",std::string(32,'7')},{"task","reference-observer"},{"requirementsRevision",2},
        {"baseline",bid},{"sourceRunId",observedRunId},{"key",intentKey},{"checkedUtc","2026-10-02T10:04:00Z"},{"execution","referenced"},{"newExecution",false},{"reuseAllowed",true},{"scope","exact-candidate-reviewed-binary"},
        {"sourceRunSha256",hashes.at("run")},{"coverageSha256",hashes.at("coverage")},{"review",Json::object({{"policy","reviewed-native-files-v1"}})},
        {"tests",Json::array({Json::object({{"test","Alpha"},{"evidenceId",std::string(32,'6')}})})}});
    const auto referencePath=referenceRoot/wide(reference.at("id").str()+".json");write(referencePath,reference.dump());
    auto missingReference=reference;missingReference["id"]=std::string(32,'8');
    subscribed["verificationLedger"]["entries"].items[0]["references"]=Json::array({reference,missingReference});save("state.json",subscribed);if(!exercise)return subscribed;refresh();
    auto references=Json::parse(http(port).body).at("runs").items[0].at("verification");
    require(references.at("referenceCount").integer()==2&&references.at("references").items[0].at("status").str()=="recorded"&&
        !references.at("references").items[0].at("newExecution").boolean&&references.at("references").items[1].at("status").str()=="unavailable"&&
        !references.at("reuseAllowed").boolean,"Completed-reference HTTP projection invented execution/reuse or lost independent receipt integrity");
    const auto referenceBytes=read(referencePath),originBytes=read(originCoverage),runBytes=read(runRoot/wide(observedRunId+".json"));
    write(referencePath,"{}");refresh();
    require(Json::parse(http(port).body).at("runs").items[0].at("verification").at("references").items[0].at("status").str()=="unavailable","Mismatched reference receipt was displayed as recorded");
    write(referencePath,referenceBytes);write(originCoverage,originBytes+" ");refresh();
    require(Json::parse(http(port).body).at("runs").items[0].at("verification").at("references").items[0].at("status").str()=="unavailable","Changed original coverage was displayed as a valid reference");
    write(originCoverage,originBytes);refresh();
    require(read(referencePath)==referenceBytes&&read(originCoverage)==originBytes&&read(runRoot/wide(observedRunId+".json"))==runBytes,"Readonly reference projection changed original execution or independent receipt");
    auto compatibleReference=reference;
    compatibleReference["id"]=std::string(32,'9');compatibleReference["scope"]="candidate-compatible-reviewed-binary";
    compatibleReference["originKey"]=intentKey;compatibleReference["key"]=std::string(64,'b');
    compatibleReference["currentCandidate"]=Json::object({{"snapshot",std::string(32,'c')}});
    compatibleReference["originCandidate"]=Json::object({{"snapshot",std::string(32,'d')}});
    compatibleReference["review"]["candidateCompatibility"]="same-output-reviewed-inputs-v1";
    compatibleReference["compatibleTests"]=Json::array({Json::object({{"test","Alpha"},{"key",std::string(64,'e')}})});
    write(referenceRoot/wide(compatibleReference.at("id").str()+".json"),compatibleReference.dump());
    auto wrongOrigin=compatibleReference;wrongOrigin["id"]=std::string(32,'a');wrongOrigin["originKey"]=std::string(64,'f');
    write(referenceRoot/wide(wrongOrigin.at("id").str()+".json"),wrongOrigin.dump());
    subscribed["verificationLedger"]["entries"].items[0]["references"]=Json::array({reference,missingReference,compatibleReference,wrongOrigin});
    save("state.json",subscribed);refresh();references=Json::parse(http(port).body).at("runs").items[0].at("verification");
    require(references.at("references").items[2].at("status").str()=="recorded"&&
        references.at("references").items[2].at("originKey").str()==intentKey&&
        references.at("references").items[2].at("currentSnapshot").str()==std::string(32,'c')&&
        references.at("references").items[3].at("status").str()=="unavailable"&&
        !references.at("reuseAllowed").boolean,"Compatible-reference HTTP projection lost origin identity or granted new execution authorization");
    require(read(referencePath)==referenceBytes&&read(originCoverage)==originBytes&&read(runRoot/wide(observedRunId+".json"))==runBytes,
        "Compatible-reference HTTP projection changed the original execution");
    auto precisionFile=reference;precisionFile["id"]=std::string(32,'b');precisionFile["checkedUtc"]="2026-10-02T10:04:00.1234560Z";
    auto precisionIndex=precisionFile;precisionIndex["checkedUtc"]="2026-10-02T10:04:00.123456Z";
    const auto precisionPath=referenceRoot/wide(precisionFile.at("id").str()+".json");write(precisionPath,precisionFile.dump());
    auto changedTick=precisionFile;changedTick["id"]=std::string(32,'c');write(referenceRoot/wide(changedTick.at("id").str()+".json"),changedTick.dump());changedTick["checkedUtc"]="2026-10-02T10:04:00.1234561Z";
    auto zeroFile=reference;zeroFile["id"]=std::string(32,'d');zeroFile["checkedUtc"]="2026-10-02T10:04:00.0000000Z";
    const auto zeroPath=referenceRoot/wide(zeroFile.at("id").str()+".json");write(zeroPath,zeroFile.dump());auto zeroIndex=zeroFile;zeroIndex["checkedUtc"]="2026-10-02T10:04:00Z";
    subscribed["verificationLedger"]["entries"].items[0]["references"]=Json::array({precisionIndex,changedTick,zeroIndex});save("state.json",subscribed);refresh();
    const auto checkReferenceTimes=[](const Json& snapshot){const auto& times=snapshot.at("runs").items[0].at("verification").at("references").items;
        require(times.size()==3&&times[0].at("status").str()=="recorded"&&times[1].at("status").str()=="unavailable"&&times[2].at("status").str()=="recorded",
            "Equivalent UTC precision was displayed as missing reference evidence");};
    checkReferenceTimes(Json::parse(http(port).body));
    require(read(precisionPath)==precisionFile.dump()&&read(zeroPath)==zeroFile.dump(),"Reference time normalization rewrote an immutable receipt");
    {
        auto strict=read(repo/L"tools/build-dashboard/server.py");
        replace(strict,"if not reference_receipt_matches(read_json(directory, item[\"id\"] + \".json\"), item):","if read_json(directory, item[\"id\"] + \".json\") != item:");
        const auto controlFile=root/L"tools/build-dashboard/server-reference-time-control.py";write(controlFile,strict);
        const int controlPort=free_port();Child control({production_python().wstring(),controlFile.wstring(),L"--fixture-root",root.wstring(),L"--port",std::to_wstring(controlPort)},root);
        until([&]{try{return http(controlPort).code==200;}catch(...){return false;}},10);
        bool rejected=false;try{checkReferenceTimes(Json::parse(http(controlPort).body));}catch(const std::exception& error){rejected=std::string(error.what())=="Equivalent UTC precision was displayed as missing reference evidence";}
        require(rejected,"Former strict receipt equality escaped the actual HTTP timestamp assertion");
    }
    // Keep the original compact seed for dashboard/browser presence checks.
    subscribed["verificationLedger"]["entries"].items[0]["references"]=Json::array({reference,missingReference});save("state.json",subscribed);
    const auto receiptHash=run({powershell().wstring(),L"-NoProfile",L"-File",(root/L"reference-hashes.ps1").wstring(),referencePath.wstring(),originCoverage.wstring()},root,10);
    require(receiptHash.code==0,"Cannot capture mixed selection reference receipt identity");
    const std::string consumer(32,'2');
    auto mixed=Json::object({{"batchId",consumer},{"mode","selected"},{"status","passed"},{"selected",Json::array({"Alpha","Beta"})},
        {"completed",Json::array({Json::object({{"name","Beta"},{"status","passed"},{"seconds",0.2}})})},
        {"references",Json::array({Json::object({{"test","Alpha"},{"newExecution",false},{"sourceRunId",observedRunId},{"originKey",intentKey},
            {"receipts",Json::array({Json::object({{"id",reference.at("id")},{"task","reference-observer"},{"sha256",Json::parse(receiptHash.out).at("run")}})})}})})}});
    save(consumer+".json",Json::object({{"batchId",consumer},{"outcome","passed"},{"inputCheck","stable"},{"participants",Json::array({})}}));
    save(consumer+".coverage.json",mixed);refresh();
    auto mixedView=Json::parse(http(port,"/api/batches/"+consumer).body).at("coverage");
    require(mixedView.at("summary").at("executed").integer()==1&&mixedView.at("summary").at("passed").integer()==1&&
        mixedView.at("summary").at("referenced").integer()==1&&mixedView.at("summary").at("unreported").integer()==0&&
        mixedView.at("references").items[0].at("status").str()=="recorded","Mixed HTTP coverage counted a reference as a new execution/pass or lost provenance");
    fs::remove(referencePath);refresh();mixedView=Json::parse(http(port,"/api/batches/"+consumer).body).at("coverage");
    require(mixedView.at("summary").at("referenced").integer()==0&&mixedView.at("summary").at("unreported").integer()==1&&
        mixedView.at("references").items[0].at("status").str()=="unavailable","Missing mixed reference receipt still fulfilled coverage");
    write(referencePath,referenceBytes);
    require(read(originCoverage)==originBytes&&read(runRoot/wide(observedRunId+".json"))==runBytes,"Mixed coverage projection rewrote original execution evidence");
    return subscribed;
}
void dashboard_references(const fs::path& repo){
    const auto root=temporary("dashboard-references"),state=root/L".build/collaboration",runRoot=state/L"runs";
    fs::create_directories(runRoot);const int port=free_port();const std::string bid(32,'d'),runId(32,'5');
    auto base=Json::object({{"schemaVersion",1},{"repositoryRoot",utf8(root.wstring())},{"current",Json{}}});
    auto record=Json::object({{"schemaVersion",1},{"id",runId},{"repositoryRoot",utf8(root.wstring())},{"nonce",std::string(32,'9')},
        {"status","passed"},{"owner",owner(GetCurrentProcessId())},{"createdUtc","2026-10-02T10:00:00Z"},{"completedUtc","2026-10-02T10:04:00Z"},{"exitCode",0},{"stages",Json::array({})}});
    auto request=Json::object({{"schemaVersion",1},{"id",runId},{"repositoryRoot",utf8(root.wstring())},{"nonce",std::string(32,'9')},
        {"task","observer"},{"action","tests"},{"mode","list"},{"createdUtc","2026-10-02T10:00:00Z"}});
    atomic(state/L"state.json",base.dump());atomic(runRoot/wide(runId+".json"),record.dump());atomic(runRoot/wide(runId+".request.json"),request.dump());
    Child server({production_python().wstring(),(repo/L"tools/build-dashboard/server.py").wstring(),L"--fixture-root",root.wstring(),L"--port",std::to_wstring(port)},root);
    until([&]{try{return http(port).code==200;}catch(...){return false;}},10);
    (void)seed_dashboard_references(repo,root,base,runId,record,request,port,bid,true);
    const auto info=Json::parse(read(root/L".build/dashboard"/wide("server-"+std::to_string(port)+".json")));
    write(root/L".build/dashboard"/wide("stop-"+info.at("instance").str()+".request"),info.at("instance").str());
    require(server.wait(8).code==0,"Independent reference dashboard did not stop with its verified nonce");
    std::cout<<"PASS independent reference HTTP: exact/compatible receipts, immutable origins, missing/corrupt proofs, lossless UTC comparison and strict-equality control\n";
}
void candidate_logs(const fs::path& repo){
    // Real HTTP and filesystem boundaries: only the build producer is replaced.
    // Two slots deliberately use the same entry filename to detect log mixing.
    const auto root=temporary("dashboard-candidate-logs"),state=root/L".build/collaboration",runs=state/L"runs";
    const std::string batch(32,'b'),entry="entry-"+std::string(32,'1')+".log",snapshot(32,'c'),digest(64,'a'),nonce(32,'9');
    fs::create_directories(runs);write(state/L"state.json","{}");
    const auto port=free_port();Child server({production_python().wstring(),(repo/L"tools/build-dashboard/server.py").wstring(),L"--fixture-root",root.wstring(),L"--port",std::to_wstring(port)},root);
    until([&]{require(server.running(),"Candidate log server exited: "+server.err());try{return http(port,"/api/health").code==200;}catch(...){return false;}},10);
    auto view=[&](const std::string& id){atomic(state/wide(batch+".json"),Json::object({{"batchId",batch},{"runId",id}}).dump());return Json::parse(http(port,"/api/batches/"+batch).body).at("run");};
    auto make=[&](char letter,const std::string& name,const fs::path& storage,bool singleton=false){
        const std::string id(32,letter),slot=singleton?"workspace":std::string(16,'a')+"-"+name;
        const auto directory=singleton?storage/L".build/build-workspace":storage/L".build/candidate-slots"/wide(slot),source=directory/L"source",output=directory/L"output";
        fs::create_directories(source);fs::create_directories(output/L"collaboration");
        const auto aliases=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("$ErrorActionPreference='Stop'; foreach($name in @('.build','.build_debug')){New-Item -ItemType Junction -Path (Join-Path "+ps_literal(source)+" $name) -Target "+ps_literal(output)+"|Out-Null}")},root,10);
        require(aliases.code==0,"Candidate log output aliases unavailable: "+aliases.out+aliases.err);
        auto request=Json::object({{"schemaVersion",1},{"id",id},{"repositoryRoot",utf8(root.wstring())},{"nonce",nonce},{"task","candidate-log-fixture"},{"action","candidate"},{"candidateAction","release"},{"candidateSlot",name},{"snapshot",snapshot},{"snapshotManifestDigest",digest},{"configuration","Release"}});
        auto context=Json::object({{"schemaVersion",1},{"runRoot",utf8(root.wstring())},{"runId",id},{"nonce",nonce},{"snapshot",snapshot},{"manifestDigest",digest},{"source",utf8(source.wstring())},{"output",utf8(output.wstring())},{"slot",slot},{"configuration","Release"}});
        auto record=Json::object({{"schemaVersion",1},{"id",id},{"repositoryRoot",utf8(root.wstring())},{"nonce",nonce},{"status","running"},{"owner",owner(GetCurrentProcessId())},{"candidateExecution",context}});
        auto event=Json::object({{"schemaVersion",1},{"runId",id},{"kind","start"},{"id",std::string(32,'2')},{"utc","2026-10-24T08:00:00Z"},{"name","release-pipeline"},{"logPath",utf8((source/L".build/collaboration"/wide(entry)).wstring())}});
        atomic(runs/wide(id+".request.json"),request.dump());atomic(runs/wide(id+".json"),record.dump());
        write(runs/wide(id+".stages.fixture.jsonl"),event.dump()+"\n");write(runs/wide(id+".stdout.log"),"");write(runs/wide(id+".stderr.log"),"child remains active\n");
        std::string text="early-"+name+"\n";for(int i=0;i<220;++i)text+="compile "+name+" "+std::to_string(i)+".cpp\n";
        text+="tail-"+name+" token=fixture-private\n";write(output/L"collaboration"/wide(entry),text);
        return Json::object({{"id",id},{"request",request},{"record",record},{"event",event},{"source",utf8(source.wstring())},{"output",utf8(output.wstring())}});
    };
    auto first=make('d',"default",root),second=make('e',"desktop-layer",root);
    for(const auto& item:{first,second}){
        const auto id=item.at("id").str(),name=item.at("request").at("candidateSlot").str();
        const auto displayed=view(id);const auto text=displayed.at("log").at("lines").dump();
        require(text.find("tail-"+name)!=std::string::npos&&text.find("fixture-private")==std::string::npos&&displayed.at("logs").items[0].at("key").str()=="product","Candidate product log tail missing, misclassified or not redacted");
        require(displayed.at("logs").items[0].at("log").at("lines").items.size()<=180,"Candidate log tail became unbounded");
        const auto downloaded=http(port,"/api/runs/"+id+"/logs/product");
        require(downloaded.code==200&&downloaded.body.find("early-"+name)!=std::string::npos&&downloaded.body.find("tail-"+name)!=std::string::npos&&downloaded.body.find("fixture-private")==std::string::npos,"Candidate full log download lost original lines or exposed secrets");
    }
    const auto id=first.at("id").str();const auto recordPath=runs/wide(id+".json"),requestPath=runs/wide(id+".request.json"),eventsPath=runs/wide(id+".stages.fixture.jsonl");
    auto rejected=[&](const std::string& why){require(http(port,"/api/runs/"+id+"/logs/product").code==503,"Candidate log accepted "+why);};
    auto changed=first.at("event");changed["logPath"]=second.at("event").at("logPath");write(eventsPath,changed.dump()+"\n");rejected("another slot's stage path");
    changed=first.at("event");changed["runId"]=second.at("id");write(eventsPath,changed.dump()+"\n");rejected("another Run's event");
    changed=first.at("event");changed["logPath"]=utf8((root/L"outside"/wide(entry)).wstring());write(eventsPath,changed.dump()+"\n");rejected("an external path");
    write(eventsPath,first.at("event").dump()+"\n");
    for(const auto& field:{"nonce","snapshot","manifestDigest","configuration","source","output","slot"}){
        auto invalid=first.at("record");invalid["candidateExecution"][field]="wrong";atomic(recordPath,invalid.dump());rejected(std::string("changed ")+field);
    }
    atomic(recordPath,first.at("record").dump());
    const auto source=fs::path(wide(first.at("source").str())),output=fs::path(wide(first.at("output").str()));
    auto point_alias=[&](const fs::path& target){fs::remove(source/L".build");const auto result=run({powershell().wstring(),L"-NoProfile",L"-Command",wide("New-Item -ItemType Junction -Path "+ps_literal(source/L".build")+" -Target "+ps_literal(target)+"|Out-Null")},root,10);require(result.code==0,"Candidate alias fixture failed: "+result.err);};
    point_alias(fs::path(wide(second.at("output").str())));rejected("a redirected source output junction");point_alias(output);
    // Tail and download must both advance after the initial read.
    {std::ofstream stream(output/L"collaboration"/wide(entry),std::ios::app);stream<<"live-append-marker\n";}
    require(view(id).at("log").at("lines").dump().find("live-append-marker")!=std::string::npos&&http(port,"/api/runs/"+id+"/logs/product").body.find("live-append-marker")!=std::string::npos,"Candidate logs stopped following appended compiler output");
    auto testRequest=first.at("request");testRequest["candidateAction"]="tests";atomic(requestPath,testRequest.dump());
    require(view(id).at("logs").items[0].at("key").str()=="tests"&&http(port,"/api/runs/"+id+"/logs/tests").code==200,"Candidate test logs did not share the normal source/download interface");
    auto debugRequest=first.at("request"),debugRecord=first.at("record"),debugEvent=first.at("event");
    debugRequest["configuration"]="Debug";debugRequest["candidateAction"]="debug";debugRecord["candidateExecution"]["configuration"]="Debug";
    debugEvent["logPath"]=utf8((source/L".build_debug/collaboration"/wide(entry)).wstring());
    atomic(requestPath,debugRequest.dump());atomic(recordPath,debugRecord.dump());write(eventsPath,debugEvent.dump()+"\n");
    require(view(id).at("log").at("lines").dump().find("live-append-marker")!=std::string::npos&&http(port,"/api/runs/"+id+"/logs/product").code==200,"Debug candidate alias could not show or download its compiler log");
    auto batchRequest=first.at("request"),batchRecord=first.at("record");batchRequest["action"]="batch";batchRequest["batch"]=batch;
    batchRecord["candidateExecution"]["slot"]=std::string(16,'a')+"-batches";
    // Bind a real batches directory without moving either existing slot.
    const auto batchFixture=make('a',"batches",root);batchRecord["candidateExecution"]=batchFixture.at("record").at("candidateExecution");
    batchRecord["candidateExecution"]["runId"]=id;batchRecord["candidateExecution"]["batchId"]=batch;
    batchRecord["candidateBinding"]=Json::object({{"runId",id},{"nonce",nonce},{"batchId",batch},{"snapshot",snapshot},{"manifestDigest",digest}});
    auto batchEvent=first.at("event");batchEvent["logPath"]=batchFixture.at("event").at("logPath");
    atomic(requestPath,batchRequest.dump());atomic(recordPath,batchRecord.dump());write(eventsPath,batchEvent.dump()+"\n");
    write(state/wide(batch+".log.product.log"),"batch-product-marker\n");write(state/wide(batch+".log"),"batch-tests-marker\n");
    require(view(id).at("log").at("lines").dump().find("batch-product-marker")!=std::string::npos&&http(port,"/api/runs/"+id+"/logs/tests").body.find("batch-tests-marker")!=std::string::npos,"Captured batch lost its separate product and test logs");
    auto legacyRequest=first.at("request"),legacyRecord=first.at("record"),legacyEvent=first.at("event");
    legacyRequest["action"]="ide";legacyRecord.fields.erase("candidateExecution");legacyEvent["logPath"]=utf8((state/wide(entry)).wstring());
    atomic(requestPath,legacyRequest.dump());atomic(recordPath,legacyRecord.dump());write(eventsPath,legacyEvent.dump()+"\n");write(state/wide(entry),"legacy-shared-entry-marker\n");
    require(view(id).at("log").at("lines").dump().find("legacy-shared-entry-marker")!=std::string::npos&&http(port,"/api/runs/"+id+"/logs/execution").code==200,"Shared-output Run lost its legacy entry log");
    atomic(requestPath,first.at("request").dump());
    atomic(recordPath,first.at("record").dump());write(eventsPath,first.at("event").dump()+"\n");
    const auto single=make('8',"ignored-old-name",root,true);
    require(view(single.at("id").str()).at("log").at("lines").dump().find("tail-ignored-old-name")!=std::string::npos&&http(port,"/api/runs/"+single.at("id").str()+"/logs/product").code==200,"Single fixed compiler workspace did not expose its product log");
    auto migrated=first.at("record");migrated["candidateExecution"]["slot"]="workspace";
    atomic(root/L".build/candidate-workspace.json",Json::object({{"schemaVersion",1},{"commonGitDirectory",utf8((root/L".git").wstring())},{"directory",utf8(source.parent_path().wstring())}}).dump());
    atomic(recordPath,migrated.dump());require(view(id).at("log").at("lines").dump().find("live-append-marker")!=std::string::npos,"Migrated fixed workspace lost its original warm directory's log");
    fs::remove(root/L".build/candidate-workspace.json");atomic(recordPath,first.at("record").dump());
    const auto archived=state/L"workspace-archive"/source.parent_path().filename()/wide(entry);fs::create_directories(archived.parent_path());
    fs::rename(output/L"collaboration"/wide(entry),archived);fs::remove(source/L".build");
    require(view(id).at("log").at("lines").dump().find("live-append-marker")!=std::string::npos&&http(port,"/api/runs/"+id+"/logs/product").code==200,"Retiring redundant compiler files lost the preserved historical log");
    // A linked checkout uses the main checkout's common slots, not local slots.
    const auto main=root/L"main-checkout",gitDir=main/L".git/worktrees/linked";fs::create_directories(gitDir);
    write(root/L".git","gitdir: "+utf8(gitDir.wstring())+"\n");write(gitDir/L"commondir","../..\n");
    const auto linked=make('f',"linked",main);require(view(linked.at("id").str()).at("log").at("lines").dump().find("tail-linked")!=std::string::npos&&http(port,"/api/runs/"+linked.at("id").str()+"/logs/product").code==200,"Linked checkout could not read its common Git candidate slot");
    const auto receipt=Json::parse(read(root/L".build/dashboard"/wide("server-"+std::to_string(port)+".json")));
    write(root/L".build/dashboard"/wide("stop-"+receipt.at("instance").str()+".request"),receipt.at("instance").str());require(server.wait(8).code==0,"Candidate log fixture server did not stop");
    std::cout<<"PASS independent candidate slot log tails, downloads, live append, identity/path isolation and linked checkout\n";
}
void dashboard(const fs::path& repo,bool browser){
    auto root=temporary("dashboard");auto state=root/L".build/collaboration";fs::create_directories(state);int port=free_port();std::string bid(32,'d'),largeId(32,'a'),child(32,'e'),ticket(32,'f');
    auto current=[&](const std::string& phase="editing",Json participants=Json::array({}),Json process=Json{}){if(participants.items.empty()){auto b=task("task-b");b["state"]="editing";participants=Json::array({task(),b});}return Json::object({{"schemaVersion",1},{"repositoryRoot",utf8(root.wstring())},{"current",Json::object({{"id",bid},{"protocolVersion",2},{"phase",phase},{"participants",participants},{"createdUtc","2026-10-02T10:00:00Z"},{"owner",process},{"inputStart",Json::object({{"algorithm","fixture"},{"digest",std::string(64,'f')},{"fileCount",2}})}})}});};
    auto save=[&](const std::string& name,const Json& value){atomic(state/name,value.dump());};save("state.json",current());Child server({production_python().wstring(),(repo/L"tools/build-dashboard/server.py").wstring(),L"--fixture-root",root.wstring(),L"--port",std::to_wstring(port)},root);
    try{until([&]{require(server.running(),"Dashboard server exited before assertions: "+server.err());try{return http(port,"/api/health").code==200;}catch(...){return false;}},10);}catch(...){auto result=server.wait(1);if(result.code==3){auto directory=environment(L"SNOWDESKTOP_RETRY_SIGNAL_DIR");auto token=utf8(environment(L"SNOWDESKTOP_RETRY_RUN_TOKEN"));if(!directory.empty()&&token.size()==32&&fs::is_directory(directory)){std::string name=browser?"build_dashboard_browser":"build_dashboard";atomic(fs::path(directory)/wide(name+".json"),Json::object({{"schemaVersion",1},{"test",name},{"runToken",token},{"exitCode",75},{"failureClass","isolated-port-race"},{"cleanupComplete",true},{"sideEffects","none"},{"reason","Fixture server bind failed before assertions; fixture process exited"}}).dump());throw std::runtime_error("RESOURCE-RACE-75");}}throw;}
    require(http(port).code==200&&Json::parse(http(port).body).at("fixture").boolean,"Dashboard not labeled fixture");
    const std::string capturedId(32,'b'),capturedRun(32,'c');
    auto captured=current("building",Json::array({task("captured-task")}),owner(GetCurrentProcessId())).at("current");
    captured["id"]=capturedId;captured["protocolVersion"]=4;captured["runId"]=capturedRun;captured["capturedUtc"]="2026-10-02T10:00:01Z";
    captured["candidateBinding"]=Json::object({{"snapshot",std::string(32,'9')},{"sourceDigest",std::string(64,'7')},{"nonce","never-project-this-candidate-nonce"}});
    auto showCaptured=[&](){
        auto editing=current();editing["activeCandidates"]=Json::array({captured});save("state.json",editing);
        fs::create_directories(state/L"runs");
        save("runs/"+capturedRun+".json",Json::object({{"id",capturedRun},{"status","queued"},{"owner",owner(GetCurrentProcessId())},{"waitingFor","fixture capacity queue"}}));
    };
    auto hideCaptured=[&](){save("state.json",current());fs::remove(state/L"runs"/wide(capturedRun+".json"));};
    showCaptured();std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const auto queueResponse=http(port);const auto queueView=Json::parse(queueResponse.body);
    require(queueView.at("current").at("id").str()==bid&&queueView.at("activeCandidates").items.size()==1&&
        queueView.at("activeCandidates").items.front().at("runId").str()==capturedRun&&queueResponse.body.find("never-project-this-candidate-nonce")==std::string::npos,
        "Dashboard lost concurrent editing/candidate state or exposed a private binding nonce");
    const auto capturedView=Json::parse(http(port,"/api/batches/"+capturedId).body);
    require(capturedView.at("id").str()==capturedId&&!capturedView.at("historical").boolean,"Captured candidate could not be opened while another batch was editing");
    hideCaptured();
    for(const auto& headers:{L"Host: evil.example\r\n",L"Origin: http://evil.example\r\n",L"Sec-Fetch-Site: cross-site\r\n"})require(http(port,"/api/status",headers).code==403,"Foreign origin accepted");for(const auto& path:{"/api/status?path=C:/Windows","/api/batches/%2e%2e%2fstate.json"})require(http(port,path).code==400,"Arbitrary path accepted");require(http(port,"/api/status",L"",L"POST").code==405&&http(port,"/api/exec").code==404,"Write/exec route accepted");auto headers=http(port,"/").headers;require(headers.find("script-src 'self'")!=std::string::npos&&headers.find("Access-Control-Allow-Origin")==std::string::npos,"Dashboard CSP/CORS changed");auto conflict=run({production_python().wstring(),(repo/L"tools/build-dashboard/manage.py").wstring(),L"start",L"--root",root.wstring(),L"--port",std::to_wstring(port)},root);require(conflict.code==3&&http(port,"/api/health").code==200,"Occupied listener was replaced");auto before=read(state/L"state.json");for(int i=0;i<5;++i)http(port);require(before==read(state/L"state.json"),"Readonly HTTP changed state");
    // Real producer-shaped Run records and append-only task events, including
    // overlapping intervals, partial writes and explicit unknown coverage.
    const std::string observedRunId(32,'d'),stageOne(32,'1'),stageTwo(32,'2');
    const auto runRoot=state/L"runs";fs::create_directories(runRoot);
    auto runRequest=Json::object({{"schemaVersion",1},{"id",observedRunId},{"repositoryRoot",utf8(root.wstring())},{"nonce",std::string(32,'9')},{"task","observed-native"},{"action","ide"},{"configuration","Release"},{"batch",bid}});
    auto runRecord=Json::object({{"schemaVersion",1},{"id",observedRunId},{"repositoryRoot",utf8(root.wstring())},{"nonce",std::string(32,'9')},{"status","running"},{"owner",owner(GetCurrentProcessId())},{"createdUtc","2026-10-02T10:00:00Z"},{"completedUtc",Json{}},{"waitingFor",""},{"stages",Json::array({Json::object({{"name","queue"},{"elapsedSeconds",0.25},{"completedUtc","2026-10-02T10:00:01Z"}})})},{"waitIntervals",Json::array({Json::object({{"resource","shared-output"},{"reason","controlled lease wait"},{"elapsedSeconds",0.2}})})}});
    runRecord["artifact"]=Json::object({{"id",observedRunId},{"candidate",std::string(32,'c')},{"manifestDigest",std::string(64,'a')},{"runtime","fixture-runtime"},{"files",2},{"productRuntime",true},{"publicationComplete",true}});
    runRecord["activation"]=Json::object({{"runId",observedRunId},{"candidate",std::string(32,'c')},{"artifactDigest",std::string(64,'a')},{"status","activated"},{"completedUtc","2026-10-02T10:00:02Z"},{"path","fixture-runtime/SnowDesktop.exe"},{"sha256",std::string(64,'b')},{"backup","private-backup-marker"},{"dataDirectory","private-desktop-marker"}});
    atomic(runRoot/wide(observedRunId+".request.json"),runRequest.dump());atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());
    const auto resourcePath=runRoot/wide(observedRunId+".resources.json");
    auto resourceRecord=Json::object({{"cpuSeconds",1.5},{"peakJobMemoryBytes",1048576},{"sampledUtc","2026-10-02T10:00:02Z"},{"accountingAvailable",true},{"finished",false}});
    atomic(resourcePath,resourceRecord.dump());
    auto event=[&](const std::string& kind){return Json::object({{"schemaVersion",1},{"runId",observedRunId},{"kind",kind},{"utc","2026-10-02T10:00:02Z"}});};
    auto first=event("start");first["id"]=stageOne;first["name"]="compile";first["project"]="<script>window.dashboardXss=1</script>";
    auto second=first;second["id"]=stageTwo;
    auto end=event("end");end["id"]=stageOne;end["name"]="compile";end["elapsedSeconds"]=4.0;end["exitCode"]=0;end["unitCountKnown"]=true;end["attemptedUnits"]=2;
    const auto streamPath=runRoot/wide(observedRunId+".msbuild.fixture.jsonl");
    auto events=event("collector").dump()+"\n"+first.dump()+"\n"+second.dump()+"\n"+end.dump()+"\n";
    write(streamPath,events+"{partial");auto activeRun=current();activeRun["current"]["runId"]=observedRunId;save("state.json",activeRun);refresh();
    auto observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("activation").at("status").str()=="activated"&&!observed.at("activation").has("backup")&&!observed.at("activation").has("dataDirectory"),"Activation receipt was omitted or exposed backup/user-data paths");
    {
        auto otherRun=runRecord;otherRun["activation"]["runId"]=std::string(32,'0');atomic(runRoot/wide(observedRunId+".json"),otherRun.dump());refresh();
        require(Json::parse(http(port).body).at("current").at("run").at("activation").at("status").str()=="unavailable","Foreign activation receipt was shown as this Run's delivery");
        auto laterFailure=runRecord;laterFailure["status"]="failed";atomic(runRoot/wide(observedRunId+".json"),laterFailure.dump());refresh();
        const auto failureView=Json::parse(http(port).body).at("current").at("run");
        require(failureView.at("status").str()=="failed"&&failureView.at("activation").at("status").str()=="activated","Later Run failure erased an already completed activation");
        atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());refresh();
    }
    require(observed.at("compiler").at("attemptedUnits").integer()==2&&observed.at("compiler").at("unknownTasks").integer()==1&&!observed.at("compiler").at("complete").boolean&&observed.at("pendingLine").boolean,"Partial Run/compiler coverage became complete");
    require(observed.at("phaseSummary").items[0].at("taskSeconds").number==4.0&&observed.at("phaseSummary").items[0].at("running").integer()==1&&observed.at("resources").at("cpuSeconds").number==1.5&&observed.at("waitIntervals").items[0].at("elapsedSeconds").number==0.2,"Run phase, resource or lease-wait values were inferred or lost");
    require(observed.at("resources").at("status").str()=="sampling","A live intermediate resource sample lost its sampling state");
    require(observed.at("admission").kind==Json::Kind::Null&&observed.at("resources").at("cpuLimitBasisPoints").kind==Json::Kind::Null,"Legacy Run acquired an invented budget or CPU cap");
    runRecord["resourceAdmission"]=Json::object({{"policy","project-weighted-v1"},{"compatibilityMode","shared"},{"bindingValid",true},{"cpuSlots",2},{"cpuCapacity",6},{"memoryMiB",3072},{"reservedMemoryMiB",3072},{"memoryCapacityMiB",6144},{"policyPath","private-policy-path"},{"policyHash","private-policy-hash"},{"lastObservation",Json::object({{"occupiedCpuSlots",6},{"occupiedMemoryMiB",4096},{"availablePhysicalBytes",8589934592.0},{"reason","cpu-capacity"},{"observedUtc","2026-10-02T10:00:01Z"}})}});
    resourceRecord["requestedCpuSlots"]=2;resourceRecord["requestedMemoryMiB"]=3072;resourceRecord["cpuLimitBasisPoints"]=2500;resourceRecord["committedMemoryLimitBytes"]=3221225472.0;
    atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());atomic(resourcePath,resourceRecord.dump());refresh();observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("admission").at("cpuSlots").integer()==2&&observed.at("admission").at("lastObservation").at("occupiedCpuSlots").integer()==6&&observed.at("resources").at("cpuLimitBasisPoints").integer()==2500&&observed.at("resources").at("committedMemoryLimitBytes").number==3221225472.0,"Budget requests, queue observations and actual Job limits were conflated or lost");
    require(!observed.at("admission").has("policyPath")&&!observed.at("admission").has("policyHash")&&observed.at("admission").at("headroomMiB").kind==Json::Kind::Null,"Budget projection exposed private metadata or invented missing capacity");
    runRecord["status"]="cancelled";runRecord["completedUtc"]="2026-10-02T10:00:03Z";atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());refresh();observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("phaseSummary").items[0].at("running").integer()==0&&observed.at("phaseSummary").items[0].at("incomplete").integer()==1&&!observed.at("compiler").at("complete").boolean,"Cancelled unfinished work was reported as running/completed");
    require(observed.at("resources").at("status").str()=="incomplete"&&observed.at("resources").at("cpuSeconds").number==1.5,"Terminal Run promoted an intermediate resource sample to complete");
    auto secondEnd=end;secondEnd["id"]=stageTwo;secondEnd["attemptedUnits"]=0;secondEnd["elapsedSeconds"]=0.01;auto collectorEnd=event("collector-end");collectorEnd["unfinishedTasks"]=0;
    write(streamPath,events+secondEnd.dump()+"\n"+collectorEnd.dump()+"\n");runRecord["status"]="passed";atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());refresh();observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("compiler").at("complete").boolean&&observed.at("compiler").at("attemptedUnits").integer()==2&&observed.at("compiler").at("unknownTasks").integer()==0,"Closed compiler observations lost their coverage identity");
    require(observed.at("resources").at("status").str()=="incomplete","Successful Run concealed incomplete resource accounting");
    auto unavailable=resourceRecord;unavailable["accountingAvailable"]=false;unavailable["cpuSeconds"]=0;unavailable["peakJobMemoryBytes"]=0;
    atomic(resourcePath,unavailable.dump());refresh();observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("resources").at("status").str()=="unavailable"&&observed.at("resources").at("cpuSeconds").kind==Json::Kind::Null,"Unavailable Job statistics became measured zero usage");
    atomic(resourcePath,Json::object({{"cpuSeconds",1.5},{"peakJobMemoryBytes",1048576},{"finished",true}}).dump());refresh();observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("resources").at("status").str()=="unknown","Legacy resource values were inferred to be complete");
    fs::remove(resourcePath);refresh();observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("resources").at("status").str()=="unrecorded","Missing resource record became a measured result");
    resourceRecord["finished"]=true;atomic(resourcePath,resourceRecord.dump());refresh();observed=Json::parse(http(port).body).at("current").at("run");
    require(observed.at("resources").at("status").str()=="complete"&&observed.at("resources").at("accountingAvailable").boolean,"Valid terminal accounting was not preserved");
    auto subscribed=seed_dashboard_references(repo,root,current(),observedRunId,runRecord,runRequest,port,bid,false);
    runRequest["mode"]="list";atomic(runRoot/wide(observedRunId+".request.json"),runRequest.dump());save("state.json",current());refresh();
    auto wrongRun=runRecord;wrongRun["nonce"]=std::string(32,'8');atomic(runRoot/wide(observedRunId+".json"),wrongRun.dump());save("state.json",current());refresh();
    auto badRuns=Json::parse(http(port).body).at("runs");require(badRuns.items[0].at("status").str()=="unavailable","Mismatched Run nonce was projected as valid");
    atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());refresh();
    {
        const auto batchLogPath=state/wide(bid+".log");
        const bool hadLog=fs::is_regular_file(batchLogPath);
        const auto originalLog=hadLog?read(batchLogPath):std::string{};
        auto batchRequest=runRequest;batchRequest["action"]="batch";
        atomic(runRoot/wide(observedRunId+".request.json"),batchRequest.dump());
        std::string product="early-product-marker\n";
        for(int i=0;i<5000;++i)product+="Building retained source unit "+std::to_string(i)+std::string(40,'x')+"\n";
        product+="warning retained-product-tail token=fixture-secret\n{\"nonce\":\"fixture-json-secret\",\"apiKey\":\"fixture-api-secret\"}\n";
        write(state/wide(bid+".log.product.log"),product);
        write(state/wide(bid+".log"),"test-stage-marker\n1/1 Test #1 passed\n");
        refresh();const auto snapshot=Json::parse(http(port).body);Json logs;for(const auto& item:snapshot.at("runs").items)if(item.at("id").str()==observedRunId)logs=item;
        require(logs.at("log").at("lines").dump().find("retained-product-tail")!=std::string::npos&&logs.at("log").at("lines").dump().find("test-stage-marker")!=std::string::npos,
                "Background Run omitted product or test pipeline logs");
        auto download=http(port,"/api/runs/"+observedRunId+"/logs/product");
        require(download.code==200&&download.body.find("early-product-marker")!=std::string::npos&&download.body.find("retained-product-tail")!=std::string::npos&&download.body.find("fixture-secret")==std::string::npos&&download.body.find("fixture-json-secret")==std::string::npos&&download.body.find("fixture-api-secret")==std::string::npos&&download.headers.find("attachment;")!=std::string::npos,
                "Full product download lost early lines or exposed credentials");
        require(http(port,"/api/runs/"+observedRunId+"/logs/tests").body.find("test-stage-marker")!=std::string::npos&&http(port,"/api/runs/"+std::string(32,'0')+"/logs/product").code==404&&http(port,"/api/runs/"+observedRunId+"/logs/profile").code==404,
                "Log download crossed the fixed Run/source boundary");
        auto foreign=runRecord;foreign["nonce"]=std::string(32,'0');atomic(runRoot/wide(observedRunId+".json"),foreign.dump());
        require(http(port,"/api/runs/"+observedRunId+"/logs/product").code==503,"Log download bypassed Run/request identity validation");
        atomic(runRoot/wide(observedRunId+".json"),runRecord.dump());atomic(runRoot/wide(observedRunId+".request.json"),runRequest.dump());
        if(hadLog)write(batchLogPath,originalLog);else fs::remove(batchLogPath);
        refresh();
    }
    // Observe actual native sharing errors and atomic replacement via HTTP.
    save(largeId+".json",Json::object({{"batchId",largeId},{"outcome","passed"}}));HANDLE locked=CreateFileW((state/wide(largeId+".json")).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);require(locked!=INVALID_HANDLE_VALUE,"Native sharing fixture unavailable");auto timer=std::chrono::steady_clock::now();auto denied=http(port,"/api/batches/"+largeId);CloseHandle(locked);require(denied.code==503&&std::chrono::steady_clock::now()-timer<std::chrono::seconds(3),"Persistent sharing became passing/unbounded");require(http(port,"/api/batches/"+largeId).code==200,"Reader did not recover fresh file after sharing release");write(state/wide(largeId+".json"),"{invalid");require(http(port,"/api/batches/"+largeId).code==503,"Invalid metadata treated as absent/pass");write(state/wide(largeId+".json"),std::string(8*1024*1024+1,' '));require(http(port,"/api/batches/"+largeId).code==503,"Metadata bound removed");
    Json large=Json::object({{"schemaVersion",1},{"batchId",largeId},{"outcome","passed"},{"exitCode",0},{"participants",Json::array({task()})},{"testRetry",Json::object({{"tests",Json::object({})}})}});for(int i=0;i<600;++i)large["testRetry"]["tests"]["fixture-"+std::to_string(i)]=Json::object({{"status","passed"},{"attempts",Json::array({Json::object({{"attempt",1},{"reason",std::string(900,'x')}})})}});save(largeId+".json",large);before=read(state/wide(largeId+".json"));require(before.size()>256*1024,"Large metadata fixture too small");refresh();auto live=Json::parse(http(port).body);require(live.at("current").at("id").str()==bid&&http(port,"/api/batches/"+largeId).code==200&&before==read(state/wide(largeId+".json")),"Large history broke live state/changed evidence");
    std::string log;for(int i=0;i<25000;++i)log+="compile.cpp\n";log+="1/2 Test #1: Alpha .... Passed\n2/2 Test #2: Beta .... Failed\ntoken=secret-value\n<script>window.dashboardXss=1</script>\n";write(state/wide(bid+".log"),log);refresh();auto response=http(port);auto view=Json::parse(response.body);auto tail=view.at("current").at("log");require(tail.at("lines").items.size()<=180&&tail.at("testCompleted").integer()==2&&tail.at("testTotal").integer()==2&&tail.at("prefixOmitted").boolean&&response.body.find("secret-value")==std::string::npos,"Long log tail/count/redaction failed");
    // Selected requests are not execution results. Preserve partial failures,
    // explicit not-run coverage, recorded seconds and PowerShell UTC precision.
    auto selectedCoverage=Json::object({{"batchId",bid},{"mode","selected"},{"status","failed"},
        {"selected",Json::array({"Alpha","Beta","Gamma"})},
        {"completed",Json::array({Json::object({{"name","Alpha"},{"status","passed"},{"seconds",0.25}}),
                                  Json::object({{"name","Beta"},{"status","failed"},{"seconds","0.75"}})})},
        {"tasks",Json::array({Json::object({{"participant","task-a"},{"status","not-run"},{"requested",Json::array({"Gamma"})}})})}});
    save(bid+".coverage.json",selectedCoverage);refresh();
    auto counts=Json::parse(http(port).body).at("current").at("coverage");
    require(counts.at("summary").at("selected").integer()==3&&counts.at("summary").at("executed").integer()==2&&
            counts.at("summary").at("passed").integer()==1&&counts.at("summary").at("failed").integer()==1&&
            counts.at("summary").at("notRun").integer()==1&&counts.at("summary").at("unreported").integer()==0&&
            std::abs(counts.at("completed").items[1].at("seconds").number-0.75)<0.001,
            "Dashboard confused selected, executed, failed or not-run tests");
    selectedCoverage["completed"]=Json::array({});
    selectedCoverage["tasks"]=Json::array({Json::object({{"participant","task-a"},{"status","not-run"},{"requested",Json::array({"Alpha","Beta","Gamma"})}})});
    save(bid+".coverage.json",selectedCoverage);refresh();
    counts=Json::parse(http(port).body).at("current").at("coverage").at("summary");
    require(counts.at("executed").integer()==0&&counts.at("passed").integer()==0&&counts.at("notRun").integer()==3,
            "A failed build became executed or passing test coverage");
    fs::remove(state/wide(bid+".coverage.json"));
    auto precise=task("precise","2026-10-02T10:00:00.1234567Z");
    save("state.json",current("editing",Json::array({precise,task("task-b")})));refresh();
    require(Json::parse(http(port).body).at("current").at("tasks").items[0].at("ageSeconds").kind==Json::Kind::Number,
            "PowerShell seven-digit UTC timestamps lost their registration age");
    auto presenceFixture=current();
    presenceFixture["current"]["runId"]=observedRunId;
    presenceFixture["verificationLedger"]=subscribed.at("verificationLedger");runRequest["mode"]="verify";atomic(runRoot/wide(observedRunId+".request.json"),runRequest.dump());
    presenceFixture["taskLedger"]=Json::object({{"schemaVersion",1},{"sequence",1},
        {"tasks",Json::array({Json::object({{"id","investigator"},{"displayName","Investigate wait"},{"phase","blocked"},{"progress","Located owner"},{"blocker","Awaiting dependency"},{"adapter","dsh"},{"revision",2},{"requirementsRevision",1},{"lastHeartbeatUtc","2026-10-02T10:00:00.1234567Z"},{"capabilities",Json::object({{"idleWake",false}})}})})},
        {"events",Json::array({Json::object({{"sequence",1},{"sender","investigator"},{"target","*"},{"text","Review requirement"},{"recipients",Json::array({"investigator","task-b"})},{"receipts",Json::array({Json::object({{"task","investigator"},{"level","adapter"}})})}})})}});
    presenceFixture["current"]["evidence"]=Json::object({{"schemaVersion",1},{"status","recorded"},{"tests",Json::array({Json::object({{"id",std::string(32,'7')},{"test","Alpha observation"},{"status","failed"},{"seconds","0.5"},{"requesters",Json::array({"investigator"})},{"runId",observedRunId},{"binding",Json::object({{"sourceDigest",std::string(64,'a')},{"assertionBinary",std::string(64,'b')},{"invocationDigest",std::string(64,'c')},{"environmentDigest",std::string(64,'d')}})},{"reuseAllowed",false}})})}});
    presenceFixture["taskLedger"]["acceptances"]=Json::array({Json::object({{"id",std::string(32,'8')},{"task","investigator"},{"batchId",bid},{"criterion","Sample accepted by user"},{"status","passed"},{"requirementsRevision",1},{"startedUtc","2026-10-02T10:00:00Z"},{"completedUtc","2026-10-02T10:02:00Z"},{"elapsedSeconds",120.0},{"durationStatus","monotonic"},{"reporter","user"},{"reference","fixture:user-feedback"},{"notes","Independent manual observation"},{"artifacts",Json::array({Json::object({{"file","runtime.exe"}})})}})});
    auto& rich=presenceFixture["current"]["evidence"]["tests"].items[0];
    rich["identityStatus"]="recorded";rich["inputCheck"]="stable";
    rich["batchId"]=bid;rich["logicalBatchId"]=bid;
    rich["dependencies"]=Json::object({{"manifestSha256",std::string(64,'e')},{"observation",Json::object({{"status","observed"},{"sourceClosure","unproven"},{"sourceReports",3},{"compilerReadLogs",2},{"linkerReadLogs",1},{"issues",Json::array({})}})}});
    rich["runtimeInputs"]=Json::object({{"status","stable"},{"beforeDigest",std::string(64,'f')},{"afterDigest",std::string(64,'f')},{"runtimeClosure","unclosed"},{"issues",Json::array({})}});
    rich["runtimeModules"]=Json::object({{"status","observed"},{"imageCount",23},{"processCount",2},{"rootExitCode",0},{"digest",std::string(64,'1')},{"reportSha256",std::string(64,'2')},{"runtimeClosure","unclosed"},{"timingComparableToUnobservedRun",false},{"issues",Json::array({})}});
    rich["attempts"]=Json::array({Json::object({{"attempt",1},{"status","failed"},{"seconds",0.5},{"reportSha256",std::string(64,'3')}})});
    auto legacy=Json::object({{"id",std::string(32,'6')},{"test","Legacy observation"},{"status","passed"},{"reuseAllowed",false}});
    auto stale=rich;stale["id"]=std::string(32,'5');stale["test"]="Changed module observation";stale["status"]="passed";stale["identityStatus"]="invalidated";stale["inputCheck"]="changed";
    stale["runtimeInputs"]["status"]="changed";stale["runtimeModules"]["status"]="incomplete";stale["runtimeModules"]["imageCount"]=true;stale["runtimeModules"]["processCount"]=-1;
    auto reasons=Json::array({"<img src=x onerror=window.evidenceXss=1> password=fixture-secret", "loaded-module-file-changed:fixture.dll"});
    for(int i=0;i<12;++i)reasons.items.emplace_back("additional runtime reason "+std::to_string(i));
    stale["runtimeModules"]["issues"]=reasons;
    presenceFixture["current"]["evidence"]["tests"].items.push_back(legacy);presenceFixture["current"]["evidence"]["tests"].items.push_back(stale);
    save("state.json",presenceFixture);refresh();
    auto presenceView=Json::parse(http(port).body);
    require(presenceView.at("taskPresence").items[0].at("phase").str()=="blocked"&&presenceView.at("current").at("tasks").items.size()==2,
            "Investigation hidden or inserted into editing barrier");
    require(presenceView.at("events").items[0].at("adapterReceived").integer()==1&&presenceView.at("events").items[0].at("agentAcknowledged").integer()==0&&presenceView.at("events").items[0].at("recipientCount").integer()==2,
            "Saved event or adapter receipt became agent acknowledgement");
    require(presenceView.at("events").items[0].at("recipients").items[1].str()=="task-b",
            "Broadcast projection lost the selected activity recipient");
    require(presenceView.at("current").at("evidence").at("tests").items[0].at("status").str()=="failed"&&!presenceView.at("current").at("evidence").at("tests").items[0].at("reuseAllowed").boolean&&presenceView.at("acceptances").items[0].at("status").str()=="passed"&&presenceView.at("current").at("acceptances").items[0].at("elapsedSeconds").number==120.0,"Per-test evidence and independent manual acceptance were conflated or inferred reusable");
    const auto checkDependencies=[](const Json& snapshot){const auto& row=snapshot.at("current").at("evidence").at("tests").items[0];require(row.has("dependencies")&&row.at("dependencies").at("sourceClosure").str()=="unproven"&&row.at("dependencies").at("sourceReports").integer()==3&&row.at("runtimeInputs").at("status").str()=="stable"&&row.at("runtimeModules").at("imageCount").integer()==23&&!row.at("reuseAllowed").boolean,"Per-test dependency observations were omitted or treated as reusable closure");};
    checkDependencies(presenceView);
    const auto& old=presenceView.at("current").at("evidence").at("tests").items[1];const auto& invalid=presenceView.at("current").at("evidence").at("tests").items[2];
    require(old.at("dependencies").at("status").str()=="not-recorded"&&old.at("runtimeModules").at("imageCount").kind==Json::Kind::Null&&invalid.at("runtimeModules").at("imageCount").kind==Json::Kind::Null&&invalid.at("runtimeModules").at("processCount").kind==Json::Kind::Null,"Missing/invalid dependency counts became measured zeros");
    require(invalid.at("runtimeModules").at("issues").items.size()==12&&invalid.at("runtimeModules").at("issueCount").integer()==14&&http(port).body.find("fixture-secret")==std::string::npos,"Dependency reasons lost their bound, omission count or credential redaction");
    {
        auto without=read(repo/L"tools/build-dashboard/server.py");replace(without,"**evidence_dependencies(entry),","**{},");
        const auto controlFile=root/L"tools/build-dashboard/server-evidence-control.py";write(controlFile,without);
        const int controlPort=free_port();Child control({production_python().wstring(),controlFile.wstring(),L"--fixture-root",root.wstring(),L"--port",std::to_wstring(controlPort)},root);
        until([&]{try{return http(controlPort).code==200;}catch(...){return false;}},10);
        bool rejected=false;try{checkDependencies(Json::parse(http(controlPort).body));}catch(const std::exception& error){rejected=std::string(error.what())=="Per-test dependency observations were omitted or treated as reusable closure";}
        require(rejected,"Removing the server evidence projection escaped its actual HTTP contract assertion");
    }
    if(browser){runRequest["action"]="batch";atomic(runRoot/wide(observedRunId+".request.json"),runRequest.dump());refresh();}
    std::unique_ptr<Child> edge;std::unique_ptr<Cdp> cdp;
    auto render=[&](const std::string& name,const std::string& expected){refresh();cdp->send("Page.navigate",Json::object({{"url","http://127.0.0.1:"+std::to_string(port)+"/"}}));cdp->connected();cdp->evaluate("document.getElementById('tab-builds').click()");until([&]{return cdp->evaluate("document.getElementById('headline').textContent").str().find(expected)!=std::string::npos;},10);require(cdp->evaluate("document.visibilityState").str()=="visible","Interaction fixture must be foreground");auto data=cdp->evaluate("({fixture:!document.getElementById('fixture').hidden,xss:!!window.dashboardXss,overflow:document.documentElement.scrollWidth>innerWidth,icon:document.querySelector('.app-icon').naturalWidth})");require(data.at("fixture").boolean&&!data.at("xss").boolean&&!data.at("overflow").boolean&&data.at("icon").integer()>0&&cdp->errors.empty(),"Unsafe/incorrect browser fixture: "+data.dump());auto shot=cdp->send("Page.captureScreenshot",Json::object({{"format","png"},{"captureBeyondViewport",true}}));write(root/wide(name+".png"),unbase64(shot.at("data").str()));};
    if(browser){auto path=fs::path(environment(L"ProgramFiles(x86)"))/L"Microsoft/Edge/Application/msedge.exe";require(fs::is_regular_file(path),"Edge unavailable");int debug=free_port();edge=std::make_unique<Child>(std::vector<std::wstring>{path.wstring(),L"--headless=new",L"--disable-gpu",L"--remote-debugging-port="+std::to_wstring(debug),L"--user-data-dir="+(root/L"edge-profile").wstring(),L"about:blank"},root);Json info;until([&]{try{info=Json::parse(http(debug,"/json/version").body);return info.has("webSocketDebuggerUrl");}catch(...){return false;}},15);auto endpoint=info.at("webSocketDebuggerUrl").str();cdp=std::make_unique<Cdp>(debug,endpoint.substr(endpoint.find('/',5)));auto target=cdp->send("Target.createTarget",Json::object({{"url","about:blank"}}),false).at("targetId").str();cdp->sessionId=cdp->send("Target.attachToTarget",Json::object({{"targetId",target},{"flatten",true}}),false).at("sessionId").str();cdp->send("Target.activateTarget",Json::object({{"targetId",target}}),false);cdp->send("Page.enable");cdp->send("Runtime.enable");cdp->send("Emulation.setFocusEmulationEnabled",Json::object({{"enabled",true}}));cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",1440},{"height",1080},{"deviceScaleFactor",1},{"mobile",false}}));render("waiting","编辑中");require(cdp->evaluate("document.getElementById('runTelemetry').textContent.includes('2 · 采集完成') && document.getElementById('runTelemetry').textContent.includes('Job CPU')").boolean,"Browser lost Run compiler coverage or resource projection");require(cdp->evaluate("document.getElementById('workList').textContent.includes('Investigate wait') && document.getElementById('collaborationEvents').textContent.includes('智能体确认 0 / 2')").boolean,"Investigation/message UI lost independent state or receipt meaning");
        {
            cdp->evaluate("document.getElementById('tab-delivery').click()");
            require(cdp->evaluate("document.getElementById('deliveryRecords').textContent.includes('此轮已切换') && document.getElementById('deliveryRecords').textContent.includes('本轮结果')").boolean,"Delivery tab lost recorded activation or conflated it with the Run result");
            cdp->evaluate("document.getElementById('tab-builds').click();document.getElementById('logSource').value='product';document.getElementById('logSource').dispatchEvent(new Event('change'))");
            require(cdp->evaluate("document.getElementById('log').textContent.includes('retained-product-tail') && !document.getElementById('log').textContent.includes('Beta') && !document.getElementById('logDownload').hidden && document.getElementById('logDownload').getAttribute('href').endsWith('/logs/product')").boolean,"Product log source selection or full download entry missing");
            cdp->reload();require(cdp->evaluate("document.getElementById('logSource').value==='product' && document.getElementById('log').textContent.includes('retained-product-tail')").boolean,"Reload lost selected product log source");
            cdp->evaluate("document.getElementById('logSource').value='all';document.getElementById('logSource').dispatchEvent(new Event('change'))");
        }
        {
            const auto originalPresence=read(state/L"state.json");
            auto ledgerState=Json::parse(originalPresence);
            const auto originalTask=ledgerState.at("taskLedger").at("tasks").items.front();
            auto recentTask=originalTask,closedTask=originalTask;
            recentTask["id"]="recent-ledger";recentTask["displayName"]="Latest investigation";
            recentTask["progress"]="Selected diagnostic progress";recentTask["blocker"]="Searchable blocker marker";
            recentTask["lastProgressUtc"]="2026-10-02T10:05:00Z";
            closedTask["id"]="completed-ledger";closedTask["displayName"]="Completed investigation";
            closedTask["phase"]="completed";closedTask["blocker"]="";closedTask["lastProgressUtc"]="2026-10-02T10:10:00Z";
            ledgerState["taskLedger"]["tasks"]=Json::array({closedTask,originalTask,recentTask});
            atomic(state/L"state.json",ledgerState.dump());refresh();
            until([&]{return cdp->evaluate("!!document.querySelector('[data-work-key=\"task:recent-ledger\"]')").boolean;},10);
            require(cdp->evaluate("Array.from(document.querySelectorAll('#workList button')).findIndex(x=>x.dataset.workKey==='task:completed-ledger') < Array.from(document.querySelectorAll('#workList button')).findIndex(x=>x.dataset.workKey==='task:recent-ledger')").boolean,
                    "Activity list did not sort historical and current registrations by actual time");
            cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",360},{"height",800},{"deviceScaleFactor",1},{"mobile",false}}));
            cdp->evaluate("document.getElementById('workSearch').value='searchable blocker';document.getElementById('workSearch').dispatchEvent(new Event('input'))");
            auto ledgerSearch=[&]{return cdp->evaluate("document.querySelectorAll('#workList button').length===1 && document.querySelector('#workList button').dataset.workKey==='task:recent-ledger' && document.documentElement.scrollWidth<=innerWidth").boolean;};
            require(ledgerSearch(),"Activity search did not filter investigation progress/blockers at360px");
            cdp->reload();require(ledgerSearch(),"Reload lost activity filtering");
            cdp->evaluate("document.querySelector('#workList button').click()");
            until([&]{return cdp->evaluate("document.getElementById('workTitle').textContent==='Latest investigation'").boolean;});
            cdp->evaluate("document.getElementById('tab-participants').click()");
            ledgerState["taskLedger"]["tasks"].items.back()["progress"]="Updated diagnostic progress";
            atomic(state/L"state.json",ledgerState.dump());refresh();
            until([&]{return cdp->evaluate("document.getElementById('participantCards').textContent.includes('Updated diagnostic progress')").boolean;},10);
            require(ledgerSearch(),"Polling discarded activity filtering");
            cdp->evaluate("document.getElementById('workSearch').value='no-ledger-match';document.getElementById('workSearch').dispatchEvent(new Event('input'))");
            require(cdp->evaluate("document.querySelectorAll('#workList button').length===0 && !document.getElementById('workEmpty').hidden").boolean,"Unmatched activity query showed unfiltered records");
            atomic(state/L"state.json",originalPresence);refresh();
            cdp->evaluate("document.getElementById('workSearch').value='';document.getElementById('workSearch').dispatchEvent(new Event('input'));Array.from(document.querySelectorAll('#workList button')).find(x=>x.dataset.workKey==='batch:dddddddddddddddddddddddddddddddd').click()");
            until([&]{return cdp->evaluate("document.getElementById('batch').textContent").str()==bid;});
            cdp->evaluate("document.getElementById('tab-builds').click()");
            cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",1440},{"height",1080},{"deviceScaleFactor",1},{"mobile",false}}));
        }
        require(cdp->evaluate("document.getElementById('runTelemetry').textContent.includes('资源统计') && document.getElementById('runTelemetry').textContent.includes('完整') && !document.getElementById('runTelemetry').textContent.includes('不完整')").boolean,"Browser lost explicit terminal resource accounting state");
        require(cdp->evaluate("document.getElementById('runTelemetry').textContent.includes('CPU 预留 / 总额度') && document.getElementById('runTelemetry').textContent.includes('2 / 6 核心额度') && document.getElementById('runTelemetry').textContent.includes('25.00% / 3072 MiB') && document.getElementById('runTelemetry').textContent.includes('CPU 额度不足')").boolean,"Browser confused reservations, actual Job limits or queue observations");
        cdp->evaluate("document.getElementById('tab-validation').click();document.querySelector('[data-result-references]').open=true;document.querySelector('[data-reference-id]').open=true");
        require(cdp->evaluate("document.querySelector('[data-result-references]').innerText.includes('引用既有结果 · 2') && document.querySelector('[data-result-references]').innerText.includes('未新增测试执行') && document.querySelector('[data-reference-id]').innerText.includes('reference-observer') && document.querySelector('[data-reference-id]').innerText.includes('Alpha')").boolean,"Browser lost original execution/reference separation or receipt details");
        cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",360},{"height",800},{"deviceScaleFactor",1},{"mobile",false}}));
        cdp->evaluate("document.querySelector('[data-result-references]').scrollIntoView({block:'start'})");
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));
        require(cdp->evaluate("document.documentElement.scrollWidth<=innerWidth && document.querySelector('[data-result-references]').open && document.querySelector('[data-reference-id]').open").boolean,"Narrow reference view overflowed or lost expanded receipt details on refresh");
        const auto referenceShot=cdp->send("Page.captureScreenshot",Json::object({{"format","png"},{"captureBeyondViewport",false}}));write(root/L"references-narrow.png",unbase64(referenceShot.at("data").str()));
        cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",1440},{"height",1080},{"deviceScaleFactor",1},{"mobile",false}}));
        cdp->evaluate("document.querySelector('#evidence [data-test-evidence]').open=true;document.querySelector('#evidence [data-evidence-id]').open=true");
        require(cdp->evaluate("document.querySelector('#evidence [data-evidence-id]').innerText.includes('完整性未证明') && document.querySelector('#evidence [data-evidence-id]').innerText.includes('编译器报告 3') && document.querySelector('#evidence [data-evidence-id]').innerText.includes('加载图像 23 / 进程 2') && document.querySelector('#evidence [data-evidence-id]').innerText.includes('起止一致')").boolean,"Browser omitted actual dependency/module scope from an expanded test observation");
        require(cdp->evaluate("document.querySelector('[data-evidence-id=\"66666666666666666666666666666666\"]').textContent.includes('加载图像 未记录 / 进程 未记录') && document.querySelector('[data-evidence-id=\"55555555555555555555555555555555\"]').textContent.includes('另有 2 条原因未展开') && document.querySelector('[data-evidence-id=\"55555555555555555555555555555555\"]').textContent.includes('输入已改变') && !document.querySelector('#evidence img') && !window.evidenceXss").boolean,"Browser inferred missing counts, lost invalidation/omission reasons or executed issue markup");
        require(cdp->evaluate("document.querySelector('#evidence [data-evidence-id]').textContent.includes('第 1 次执行') && document.querySelector('#evidence [data-evidence-id]').textContent.includes('未自动复用结果')").boolean,"Browser lost original attempts or presented observations as result reuse");
        cdp->evaluate("document.querySelector('#evidence [data-test-evidence]').scrollIntoView({block:'start'})");
        const auto evidenceShot=cdp->send("Page.captureScreenshot",Json::object({{"format","png"},{"captureBeyondViewport",false}}));write(root/L"evidence-detail.png",unbase64(evidenceShot.at("data").str()));
        cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",360},{"height",800},{"deviceScaleFactor",1},{"mobile",false}}));
        cdp->evaluate("document.querySelector('[data-evidence-id=\"55555555555555555555555555555555\"]').open=true;document.querySelector('#evidence [data-test-evidence]').scrollIntoView({block:'start'})");
        refresh();
        require(cdp->evaluate("document.documentElement.scrollWidth<=innerWidth && document.querySelector('#evidence [data-test-evidence]').open && document.querySelector('#evidence [data-evidence-id]').open && document.querySelector('[data-evidence-id=\"55555555555555555555555555555555\"]').open && parseFloat(getComputedStyle(document.querySelector('#evidence .test-observation>.detail')).fontSize)>=13").boolean,"Narrow evidence view overflowed, lost expanded observations on refresh or reduced main fact text");
        const auto narrowEvidenceShot=cdp->send("Page.captureScreenshot",Json::object({{"format","png"},{"captureBeyondViewport",false}}));write(root/L"evidence-narrow.png",unbase64(narrowEvidenceShot.at("data").str()));
        cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",1440},{"height",1080},{"deviceScaleFactor",1},{"mobile",false}}));
        cdp->evaluate("document.querySelector('#evidence [data-evidence-id]').open=false;document.querySelector('[data-evidence-id=\"55555555555555555555555555555555\"]').open=false;document.querySelector('#evidence [data-test-evidence]').open=false");
        const auto beforeCapturedBrowser=read(state/L"state.json");showCaptured();render("candidate-queue","编辑中");
        require(cdp->evaluate("document.getElementById('workList').textContent.includes('captured-task')").boolean,
                "Activity list hid a captured candidate while a new editing window was open");
        cdp->evaluate("Array.from(document.querySelectorAll('#workList button')).find(x=>x.textContent.includes('captured-task')).click()");
        until([&]{return cdp->evaluate("document.getElementById('batch').textContent").str().find(capturedId.substr(0,8))!=std::string::npos;},10);
        hideCaptured();atomic(state/L"state.json",beforeCapturedBrowser);render("resume-editing","编辑中");
        resourceRecord["finished"]=false;atomic(resourcePath,resourceRecord.dump());refresh();
        until([&]{return cdp->evaluate("document.getElementById('runTelemetry').textContent.includes('不完整（仅有过程采样）') && document.getElementById('runTelemetry').textContent.includes('最后采样')").boolean;},5);
        resourceRecord["finished"]=true;atomic(resourcePath,resourceRecord.dump());
        require(cdp->evaluate("document.getElementById('evidence').textContent.includes('逐条测试证据') && document.getElementById('evidence').textContent.includes('Alpha observation') && document.getElementById('evidence').textContent.includes('人工验收') && document.getElementById('evidence').textContent.includes('用户反馈')").boolean,"Browser lost per-test provenance or independent manual acceptance");
        cdp->evaluate("document.getElementById('taskSearch').value='task-b';document.getElementById('taskSearch').dispatchEvent(new Event('input'))");
        require(cdp->evaluate("document.getElementById('visibleTasks').textContent").str()=="1 / 2 项","Task search did not filter real task rows");
        cdp->evaluate("document.getElementById('logSearch').value='Beta';document.getElementById('logSearch').dispatchEvent(new Event('input'))");
        std::this_thread::sleep_for(std::chrono::milliseconds(3200));
        require(cdp->evaluate("document.getElementById('visibleTasks').textContent").str()=="1 / 2 项"&&
                cdp->evaluate("document.getElementById('log').textContent.includes('Beta')&&!document.getElementById('log').textContent.includes('compile.cpp')").boolean,
                "Refresh lost task or log search");
        cdp->reload();
        require(cdp->evaluate("document.getElementById('taskSearch').value").str()=="task-b"&&
                cdp->evaluate("document.getElementById('visibleTasks').textContent").str()=="1 / 2 项","Reload lost task filtering");
        cdp->evaluate("document.getElementById('taskSearch').value='';document.getElementById('taskSearch').dispatchEvent(new Event('input'));document.getElementById('taskFilter').value='editing';document.getElementById('taskFilter').dispatchEvent(new Event('change'))");
        require(cdp->evaluate("document.getElementById('visibleTasks').textContent").str()=="1 / 2 项","Task state filtering did not use actual status");
        cdp->evaluate("document.getElementById('taskFilter').value='all';document.getElementById('taskFilter').dispatchEvent(new Event('change'))");
        auto pending=task();pending["check"]=Json::object({{"status","pending"},{"source","builtin-basic"},{"editRevision",1}});save("state.json",current("editing",Json::array({pending,task("peer")})));render("checking","编辑中");save("state.json",current("building",Json::array({task(),task("peer")}),owner(GetCurrentProcessId())));render("running","构建 / 测试中");
        require(cdp->evaluate("document.getElementById('testCount').textContent==='2 / 2'&&document.getElementById('coverage').textContent.includes('日志')").boolean,
                "Live log progress disappeared while final coverage was not yet recorded");
        save("state.json",current("editing",Json::array({task("stale","2000-01-01T00:00:00Z"),task("peer")})));render("stale","编辑中");save("state.json",current("building",Json::array({task()}),Json::object({{"pid",2147483647},{"startTicks","0"}})));render("interrupted","运行已中断");save("state.json",Json::object({{"schemaVersion",1},{"repositoryRoot",utf8(root.wstring())},{"current",Json{}}}));auto history=Json::object({{"schemaVersion",1},{"batchId",bid},{"outcome","passed"},{"exitCode",0},{"completedUtc","2026-10-02T10:00:00Z"},{"participants",Json::array({task("<script>window.dashboardXss=1</script>")})},{"inputCheck","stable"},{"inputEnd",Json::object({{"algorithm","fixture"},{"digest",std::string(64,'c')},{"fileCount",2}})}});save(bid+".json",history);render("passed","历史 · 验证通过");history["outcome"]="failed";history["exitCode"]=17;history["error"]="fixture failure <script>window.dashboardXss=1</script>";save(bid+".json",history);render("failed","历史 · 验证失败");}
    save(ticket+".wait.json",Json::object({{"id",ticket},{"participant","task-a"},{"condition","files"},{"status","eligible"},{"attempt",1},{"reason","编辑窗口曾可用"},{"next","begin / claim 后才能写入"},{"createdUtc","2026-10-02T10:00:00Z"},{"deadlineUtc","2026-10-02T10:30:00Z"}}));auto value=current("building",Json::array({task()}),browser?owner(GetCurrentProcessId()):Json{});value["current"]["id"]=child;value["current"]["logicalBatchId"]=bid;value["current"]["repairOf"]=bid;value["current"]["attempt"]=2;save("state.json",value);save(child+".coverage.json",Json::object({{"batchId",child},{"mode","selected"},{"status","passed-after-retry"},{"selected",Json::array({"build_dashboard"})},{"completed",Json::array({Json::object({{"name","build_dashboard"},{"status","passed-after-retry"}})})},{"tasks",Json::array({Json::object({{"participant","task-a"},{"requested",Json::array({"build_dashboard"})},{"status","passed-after-retry"}})})}}));save(child+".retry.json",Json::object({{"batchId",child},{"status","passed-after-retry"},{"selected",Json::array({"build_dashboard"})},{"policy",Json::object({{"maxAttempts",3}})},{"tests",Json::object({{"build_dashboard",Json::object({{"status","passed-after-retry"},{"failureCount",1},{"firstReason","首次端口竞争 <script>window.dashboardXss=1</script>"},{"finalReason","重试通过"},{"attempts",Json::array({Json::object({{"attempt",1},{"status","failed"}}),Json::object({{"attempt",2},{"status","passed"},{"reason","已通过"}})})}})}})}}));write(state/wide(child+".retry/build_dashboard.attempt2.log"),"controlled retry log\ntoken=fixture-secret\n");refresh();view=Json::parse(http(port).body);require(view.at("waits").items[0].at("status").str()=="eligible"&&view.at("current").at("repairOf").str()==bid&&view.at("current").at("attempt").integer()==2&&view.at("current").at("retry").at("tests").items[0].at("failureCount").integer()==1&&view.at("current").at("coverage").at("selected").dump()==Json::array({"build_dashboard"}).dump(),"Wait/repair/retry view lost evidence");response=http(port,"/api/retries/"+child+"/build_dashboard/2/log");require(response.code==200&&response.body.find("fixture-secret")==std::string::npos&&http(port,"/api/retries/"+child+"/Other/2/log").code==404&&http(port,"/api/retries/"+child+"/build_dashboard/4/log").code==404,"Retry log path/redaction bound failed");
    if(browser){log.clear();for(int i=0;i<220;++i)log+="view-state fixture line "+std::to_string(i)+"\n";write(state/wide(child+".log"),log);render("repair-and-retry","构建 / 测试中");cdp->evaluate("document.querySelector('#tasks details').open=true;document.querySelector('#evidence details').open=true;document.querySelector('#evidence details button').click();document.getElementById('pause').click();document.getElementById('log').scrollTop=90");auto inspect=[&]{return cdp->evaluate("({task:document.querySelector('#tasks details')?.open,retry:document.querySelector('#evidence details')?.open,pause:document.getElementById('pause').textContent,top:document.getElementById('pane-builds').hidden?(view.logs[document.getElementById('batch').textContent]||0):document.getElementById('log').scrollTop,preview:document.querySelector('#evidence pre')?.textContent,batch:document.getElementById('batch').textContent,hash:location.hash})");};auto checkSaved=[&]{auto saved=inspect();require(saved.at("task").boolean&&saved.at("retry").boolean&&saved.at("pause").str()=="恢复跟随"&&std::abs(saved.at("top").number-90)<2&&saved.at("preview").str().find("controlled retry log")!=std::string::npos,"Browser lost view state: "+saved.dump());};cdp->evaluate("document.getElementById('tab-participants').click();document.getElementById('taskPanel').open=true;document.querySelector('#tasks details summary').focus()");
        std::this_thread::sleep_for(std::chrono::milliseconds(3200));checkSaved();
        require(cdp->evaluate("document.activeElement.tagName==='SUMMARY'&&document.activeElement.closest('#tasks')!==null").boolean,"Refresh lost keyboard focus from task details");
        cdp->reload();until([&]{auto saved=inspect();return saved.at("preview").kind==Json::Kind::String&&saved.at("preview").str().find("controlled retry log")!=std::string::npos;});checkSaved();cdp->evaluate("document.querySelector('#history button').click()");std::this_thread::sleep_for(std::chrono::milliseconds(400));auto selected=inspect();require(selected.at("hash").str().find("batch="+selected.at("batch").str())!=std::string::npos,"History missing URL bookmark");cdp->reload();require(inspect().at("hash").str()==selected.at("hash").str(),"Reload lost history bookmark");cdp->evaluate("document.getElementById('current').click()");std::this_thread::sleep_for(std::chrono::milliseconds(400));require(inspect().at("hash").str().find("batch=")==std::string::npos&&inspect().at("task").boolean,"Returning current lost folds");cdp->evaluate("document.querySelector('#tasks details').open=false;document.querySelector('#evidence details').open=false;document.getElementById('pause').click()");cdp->reload();require(!inspect().at("task").boolean&&!inspect().at("retry").boolean&&inspect().at("pause").str()=="暂停跟随","Closed/following preference not persisted");cdp->evaluate("localStorage.setItem('SnowDesktop.build-monitor.ui.v1','invalid JSON')");cdp->reload();require(!inspect().at("task").boolean,"Corrupt storage lost safe defaults");auto deniedStorage=cdp->send("Page.addScriptToEvaluateOnNewDocument",Json::object({{"source","Object.defineProperty(window,'localStorage',{get(){throw new Error('fixture storage denied')}})"}})).at("identifier").str();cdp->reload();cdp->evaluate("document.querySelector('#tasks details').open=true");std::this_thread::sleep_for(std::chrono::milliseconds(3200));require(inspect().at("task").boolean,"Denied storage lost in-memory state");cdp->send("Page.removeScriptToEvaluateOnNewDocument",Json::object({{"identifier",deniedStorage}}));cdp->reload();
        std::vector<std::string> ids;for(int i=1;i<=35;++i){std::string id(32,'0');const char* hex="0123456789abcdef";id[30]=hex[i/16];id[31]=hex[i%16];ids.push_back(id);save(id+".wait.json",Json::object({{"id",id},{"participant","task-a"},{"condition","files"},{"status","waiting"},{"attempt",1},{"reason","long-list fixture"},{"next","begin / claim"},{"deadlineUtc","2026-10-03T00:00:00Z"}}));}cdp->evaluate("document.getElementById('tab-activity').click();document.getElementById('waitPanel').open=true");auto waits=[&]{return cdp->evaluate("({open:document.getElementById('waitPanel').open,top:document.getElementById('waits').scrollTop,height:document.getElementById('waits').clientHeight,total:document.getElementById('waits').scrollHeight,card:document.querySelector('.local-waits').getBoundingClientRect().height,count:document.getElementById('waits').children.length,body:document.getElementById('waits').innerText,overflow:document.documentElement.scrollWidth>innerWidth,viewTop:view.waits.top,visible:document.visibilityState,trace:window.nativeScrollTrace||[]})");};until([&]{return waits().at("count").integer()>=35;});auto list=waits();require(list.at("open").boolean&&list.at("height").number<=400&&list.at("total").number>list.at("height").number,"Long wait list not bounded/scrollable");cdp->evaluate("window.nativeScrollTrace=[];document.getElementById('waits').addEventListener('scroll',()=>{window.nativeScrollTrace.push({top:document.getElementById('waits').scrollTop,saved:view.waits.top,visible:document.visibilityState})});document.getElementById('waits').scrollTop=180");std::this_thread::sleep_for(std::chrono::milliseconds(3200));list=waits();require(std::abs(list.at("top").number-180)<2,"Refresh reset wait scroll: "+list.dump());auto record=Json::parse(read(state/wide(ids[0]+".wait.json")));record["reason"]="updated wait marker";save(ids[0]+".wait.json",record);fs::remove(state/wide(ids[1]+".wait.json"));until([&]{auto listNow=waits();return listNow.at("body").str().find("updated wait marker")!=std::string::npos&&listNow.at("count").integer()==35;});require(std::abs(waits().at("top").number-180)<2,"Update/delete reset wait scroll");cdp->reload();require(waits().at("open").boolean&&std::abs(waits().at("top").number-180)<2,"Reload lost wait scroll");cdp->evaluate("document.getElementById('waitPanel').open=false");std::this_thread::sleep_for(std::chrono::milliseconds(3200));list=waits();require(!list.at("open").boolean&&list.at("card").number<=120&&list.at("body").str().empty(),"Collapsed wait card grew during refresh");cdp->reload();require(!waits().at("open").boolean,"Reload reopened collapsed wait card");cdp->evaluate("document.getElementById('waitPanel').open=true");std::this_thread::sleep_for(std::chrono::milliseconds(200));require(std::abs(waits().at("top").number-180)<2,"Reopen lost wait scroll");cdp->send("Emulation.setDeviceMetricsOverride",Json::object({{"width",360},{"height",800},{"deviceScaleFactor",1},{"mobile",false}}));std::this_thread::sleep_for(std::chrono::milliseconds(200));list=waits();require(!list.at("overflow").boolean&&list.at("height").number<=320&&list.at("total").number>list.at("height").number,"Narrow browser overflow: "+list.dump());cdp->evaluate("document.getElementById('backToList').click()");cdp->reload();require(cdp->evaluate("!document.getElementById('workDetail').classList.contains('mobile-open') && getComputedStyle(document.querySelector('.work-sidebar')).display!=='none'").boolean,"Reload lost explicit return to activity list");cdp->evaluate("Array.from(document.querySelectorAll('#workList button')).find(x=>x.dataset.workKey===workView.key).click()");until([&]{return waits().at("height").number>0;});require(waits().at("open").boolean&&std::abs(waits().at("top").number-180)<2,"Reopening activity lost narrow detail/wait state");require(cdp->errors.empty(),"Browser emitted JS errors");cdp.reset();edge.reset();}
    auto info=Json::parse(read(root/L".build/dashboard"/wide("server-"+std::to_string(port)+".json")));write(root/L".build/dashboard"/wide("stop-"+info.at("instance").str()+".request"),info.at("instance").str());require(server.wait(8).code==0,"Verified nonce stop failed");std::cout<<"PASS readonly dashboard HTTP, native metadata failures, bounded history/logs, wait/repair/retry evidence"<<(browser?" and real browser":"")<<"\n";
}
}
int main(int argc,char** argv){try{
    require(argc>=2&&argc<=3,"Repository argument and at most one dashboard fixture mode required");
    const auto repo=fs::path(wide(argv[1]));const std::string mode=argc==3?argv[2]:"";
    if(mode=="--old-dashboard-startup"||mode=="--old-dashboard-lifetime"){
        dashboard_startup(repo,mode=="--old-dashboard-startup",mode=="--old-dashboard-lifetime");
    }else if(mode=="--startup"){dashboard_startup(repo);dashboard_listener(repo);}
    else if(mode=="--old-dashboard-listener")dashboard_listener(repo,true);
    else if(mode=="--references")dashboard_references(repo);
    else if(mode=="--candidate-logs")candidate_logs(repo);
    else if(mode.empty()||mode=="--browser")dashboard(repo,mode=="--browser");
    else if(mode=="--all"){dashboard_startup(repo);dashboard(repo,false);dashboard_references(repo);}
    else throw std::runtime_error("Unknown dashboard fixture mode");
    return 0;
}catch(const std::exception& error){std::cerr<<"FAILED: "<<error.what()<<std::endl;return std::string(error.what())=="RESOURCE-RACE-75"?75:1;}}
