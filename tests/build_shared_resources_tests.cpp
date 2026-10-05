#include "build_tool_test_support.h"
#include <future>
#include <iostream>
#include <thread>
using namespace build_test;
namespace {
void test(const fs::path& repo) {
    auto root=temporary("shared-resource");copy(repo,root,{"scripts/powershell_runtime.py","scripts/build_shared_resources.py","scripts/shared_resources.json","scripts/build_wait_tasks.py","scripts/build_ownership.ps1","tools/build-dashboard/server.py"});
    auto folder=root/L".build/collaboration";fs::create_directories(folder);auto file=root/L"lang/en-US.json";
    write(file,"{\r\n  \"Alpha\": \"old-a\",\r\n  \"Beta\": \"old-b\",\r\n  \"Unrelated\": \"keep\"\r\n}\r\n",true);
    std::string batch(32,'a');
    auto actor=[](const std::string& id){return Json::object({{"id",id},{"state","editing"},{"editRevision",0},{"ownedFiles",Json::array({"lang/en-US.json"})}});};
    auto state=Json::object({{"schemaVersion",1},{"repositoryRoot",utf8(root.wstring())},{"current",Json::object({{"id",batch},{"protocolVersion",2},{"phase","editing"},{"participants",Json::array({actor("a"),actor("b")})}})}});
    auto save=[&]{write(folder/L"state.json",state.dump());};save();
    auto command=[&](const std::string& action,const std::string& id,const std::string& request,const std::string& keys,const std::vector<std::string>& extra){
        std::vector<std::wstring> args={production_python().wstring(),(root/L"scripts/build_shared_resources.py").wstring(),L"resource",wide(action),wide(id),L"--root",root.wstring(),L"--batch",wide(batch),L"--revision",L"0"};
        if(action=="prepare"){args.insert(args.end(),{L"--file",L"lang/en-US.json",L"--keys",wide(keys)});}
        if(!request.empty())args.insert(args.end(),{L"--request",wide(request)});
        for(const auto& value:extra)args.push_back(wide(value));return args;
    };
    auto call=[&](const std::string& action,const std::string& id="a",const std::string& request="",const std::string& keys="Alpha",int code=0,const std::vector<std::string>& extra=std::vector<std::string>{}){
        auto result=run(command(action,id,request,keys,extra),root,10,{{L"PYTHONDONTWRITEBYTECODE",L"1"}});
        require(result.code==code,"Resource CLI exit mismatch: "+result.out+result.err);
        return code==0?Json::parse(result.out):Json(result.err);
    };
    auto change=[](const Json& record,const Json& values){write(fs::path(wide(record.at("patchFile").str())),values.dump());};
    require(call("prepare","a","","Alpha",2,{"--file","src/unlisted.cpp"}).str().find("explicit shared language")!=std::string::npos,"Unlisted resource accepted");
    auto a=call("prepare"),b=call("prepare","b","","Beta");change(a,Json::object({{"Alpha","new-a"}}));change(b,Json::object({{"Beta","new-b"}}));
    auto apply=[&](const std::string& id,const Json& record){auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(true){auto result=run(command("apply",id,record.at("requestId").str(),"",{}),root,10);if(result.code==0)return Json::parse(result.out);require(result.err.find("lease is occupied")!=std::string::npos&&std::chrono::steady_clock::now()<end,result.err);std::this_thread::sleep_for(std::chrono::milliseconds(50));}};
    auto one=std::async(std::launch::async,[&]{return apply("a",a);});auto two=std::async(std::launch::async,[&]{return apply("b",b);});auto first=one.get(),second=two.get();
    require(first.at("status").str()=="applied"&&second.at("status").str()=="applied","Independent simultaneous patches failed");
    auto raw=read(file);auto values=Json::parse(raw);require(values.at("Alpha").str()=="new-a"&&values.at("Beta").str()=="new-b"&&values.at("Unrelated").str()=="keep","Concurrent keys lost");
    size_t lines=0;for(size_t pos=0;(pos=raw.find("\r\n",pos))!=std::string::npos;pos+=2)++lines;
    require(raw.rfind("\xef\xbb\xbf",0)==0&&lines==5&&raw.find("  \"Unrelated\": \"keep\"")!=std::string::npos,"BOM/CRLF/order changed");
    require(call("apply","a",a.at("requestId").str()).at("afterHash").str()==first.at("afterHash").str()&&raw==read(file),"Completed request replay changed bytes");
    auto x=call("prepare"),y=call("prepare","b");change(x,Json::object({{"Alpha","first"}}));change(y,Json::object({{"Alpha","second"}}));call("apply","a",x.at("requestId").str());
    auto before=read(file);require(call("apply","b",y.at("requestId").str(),"Alpha",2).str().find("Same-key conflict")!=std::string::npos&&before==read(file),"Stale same-key overwrite accepted");
    auto added=call("prepare","a","","New.Key");change(added,Json::object({{"New.Key","added"}}));call("apply","a",added.at("requestId").str());require(Json::parse(read(file)).at("New.Key").str()=="added","New key missing");
    auto expired=call("prepare","a","","Alpha",0,{"--timeout","1"});std::this_thread::sleep_for(std::chrono::milliseconds(1050));before=read(file);require(call("apply","a",expired.at("requestId").str(),"Alpha",2).str().find("expired")!=std::string::npos&&before==read(file),"Expired patch applied");
    auto late=call("prepare");auto& current=state["current"];auto& peers=current["participants"].items;peers[0]["editRevision"]=1;save();require(call("apply","a",late.at("requestId").str(),"Alpha",2).str().find("Stale edit token")!=std::string::npos,"Old revision accepted");
    peers[0]["editRevision"]=0;peers[0]["state"]="finished";save();require(call("apply","a",late.at("requestId").str(),"Alpha",2).str().find("Stale edit token")!=std::string::npos,"Finished editor patch accepted");
    peers[0]["state"]="editing";current["phase"]="building";save();require(call("apply","a",late.at("requestId").str(),"Alpha",2).str().find("frozen")!=std::string::npos,"Frozen batch write accepted");
    current["phase"]="editing";peers[1]["ownedFiles"]=Json::array({"lang"});save();require(call("prepare","a","","Alpha",2).str().find("structural")!=std::string::npos,"Structural edit ignored");
    peers[1]["ownedFiles"]=Json::array({});save();auto unknown=call("prepare");auto recordPath=folder/wide(unknown.at("requestId").str()+".resource.json");auto record=Json::parse(read(recordPath));record["status"]="writing";record["afterHash"]=std::string(64,'0');write(recordPath,record.dump());before=read(file);require(call("apply","a",unknown.at("requestId").str(),"Alpha",2).str().find("uncertain outcome")!=std::string::npos&&before==read(file),"Uncertain write replayed");
    auto& peer=peers[1];peer["state"]="finished";peer["testPlan"]=Json::object({{"source","task-declared"},{"inputs",Json::array({"lang/en-US.json"})},{"suites",Json::array({"selected"})},{"tests",Json::array({"Alpha"})}});peer["check"]=Json::object({{"status","passed"}});peer["waiter"]=owner(GetCurrentProcessId());save();
    auto transfer=call("prepare","a","","Beta");change(transfer,Json::object({{"Beta","shared follow-up"}}));call("apply","a",transfer.at("requestId").str());auto carried=Json::parse(read(folder/L"state.json")).at("current").at("participants").items[1];
    require(carried.at("check").at("status").str()=="pending"&&carried.at("testPlan").at("source").str()=="repair-carried"&&carried.at("testPlan").at("handoffCarried").boolean&&carried.at("testPlan").at("tests").items[0].str()=="Alpha"&&carried.at("handoffPreviousCheck").at("status").str()=="passed","Ready peer requirements/check evidence lost");
    write(root/L"ownership.ps1",R"PS(param([string]$Mode)
$ErrorActionPreference='Stop'
function Get-Field($Object,$Name,$Default=$null){if($Object -and $Object.PSObject.Properties[$Name]){return $Object.$Name};return $Default}
function Set-Field($Object,$Name,$Value){$Object|Add-Member -NotePropertyName $Name -NotePropertyValue $Value -Force}
function Get-EditRevision($Object){return $Object.editRevision}
function Start-ReadyWorker($Current,$Entry){return [pscustomobject]@{pid=1;startTicks='1';editRevision=$Entry.editRevision}}
$repositoryRoot=$PSScriptRoot
. (Join-Path $PSScriptRoot 'scripts/build_ownership.ps1')
$a=[pscustomobject]@{id='a';state='finished';editRevision=0;ownedFiles=@('src/a.cpp');testPlan=[pscustomobject]@{source='task-declared';inputIdentity='old'};check=[pscustomobject]@{status='passed'}}
$b=[pscustomobject]@{id='b';state='editing';editRevision=0;ownedFiles=@()}
$current=[pscustomobject]@{participants=@($a,$b)};$path='src/a.cpp'
if($Mode -ne 'finished'){$a.state='editing'}
if($Mode -in @('language','structural')){$a.ownedFiles=@('lang/en-US.json');$path=if($Mode -eq 'language'){'lang/en-US.json'}else{'lang'}}
$accepted=$false;$error=''
try{Set-Ownership $current $b $path $true;$accepted=$true}catch{$error=$_.Exception.Message}
@{accepted=$accepted;error=$error;a=$a;b=$b}|ConvertTo-Json -Depth 15
)PS",true);
    Bridge bridge(repo,root);
    auto observed=bridge.call(root/L"ownership.ps1",{"-Mode","finished"});require(observed.at("accepted").boolean&&observed.at("a").at("ownedFiles").items[0].str()=="src/a.cpp"&&observed.at("b").at("ownedFiles").items[0].str()=="src/a.cpp"&&observed.at("a").at("check").at("status").str()=="pending"&&observed.at("a").at("testPlan").at("handoffCarried").boolean,"Finished ownership handoff/provenance failed");
    require(!bridge.call(root/L"ownership.ps1",{"-Mode","active"}).at("accepted").boolean,"Active code ownership conflict accepted");
    require(bridge.call(root/L"ownership.ps1",{"-Mode","language"}).at("accepted").boolean&&!bridge.call(root/L"ownership.ps1",{"-Mode","structural"}).at("accepted").boolean,"Shared-language/structural ownership boundary incorrect");
    std::cout<<"PASS simultaneous independent keys, BOM/CRLF/order, idempotency, stale/expired/frozen/uncertain-write rejection, ready peer requirements and actual ownership handoff"<<std::endl;
}
}
int main(int argc,char** argv) {return test_main(argc,argv,test);}
