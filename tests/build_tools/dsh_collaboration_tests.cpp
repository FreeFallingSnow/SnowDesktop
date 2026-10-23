#include "../build_tool_test_support.h"
#include <future>
#include <iostream>
#include <memory>
using namespace build_test;
namespace {
void run_tests(const fs::path& repo,const fs::path& preferredModule={}) {
    Coordinator c(repo,"dsh-plugin");
    const auto script=c.root/L"exercise.mjs";
    write(script,R"JS(import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import {pathToFileURL} from 'node:url';
const [moduleFile,root,mode]=process.argv.slice(2);
const {CollaborationBridge,taskId}=await import(pathToFileURL(moduleFile));
const bridge=new CollaborationBridge(root),agent='fixture-agent',id=taskId(agent);
const target=path.join(root,'src/a.txt');
async function attempt(){try{await bridge.verifyWrite(agent,target);fs.writeFileSync(target,'authorized write');return {allowed:true};}catch(error){return {allowed:false,error:String(error.message)};}}
let result;
if(mode==='open'){
 result={opened:await bridge.open(agent),write:await attempt(),current:bridge.state().current,id};
}else if(mode==='unicode'){
 const values=[];
 for(const runtime of [path.join(process.env.ProgramFiles,'PowerShell/7/pwsh.exe'),path.join(process.env.SystemRoot,'System32/WindowsPowerShell/v1.0/powershell.exe')]){
  const reader=new CollaborationBridge(root,{powershell:runtime});
  const updated=await reader.progress(agent,'协作消息 🧊 \'"; $(literal) ` |');
  const received=await reader.operate(agent,{action:'messages',afterSequence:0});
  values.push({runtime,progress:updated.task.progress,event:received.events.find(event=>event.key==='fixture-unicode').text});
 }
 result={values};
}else if(mode==='grant'){
 const registration=await bridge.operate(agent,{action:'begin'});
 const claim=await bridge.operate(agent,{action:'claim',files:'src/a.txt'});
 result={registration,claim,write:await attempt(),id};
}else if(mode==='write'){
 result={write:await attempt(),id};
}else if(mode==='rebind'){
 const registration=await bridge.operate(agent,{action:'begin'});result={write:await attempt(),registration,id};
}else if(mode==='outside'){
 result={outside:await bridge.verifyWrite(agent,path.join(root,'../'+path.basename(root)+'-outside.txt')),id};
}else if(mode==='escape'){
 const outside=root+'-escape';fs.mkdirSync(outside);fs.writeFileSync(path.join(outside,'keep.txt'),'outside original');
 fs.symlinkSync(outside,path.join(root,'src/link'),'junction');
 try{await bridge.verifyWrite(agent,path.join(root,'src/link/keep.txt'));result={denied:false};}catch(error){result={denied:true,error:String(error.message)};}
}else if(mode==='bad-command'){
 try{await bridge.call('recover',id);result={rejected:false};}catch(error){result={rejected:true,error:String(error.message)};}
}else if(mode==='dashboard-preferred'){
 const calls=[];let mismatch=false,fixture=false;
 const servers=['first','second'].map(tag=>http.createServer((request,response)=>{calls.push({tag,route:request.url});response.setHeader('Content-Type','application/json');response.end(JSON.stringify(request.url==='/api/health'?{service:'SnowDesktop build dashboard',instance:mismatch?'different':tag,fixture}:{tag}));}));
 await Promise.all(servers.map(server=>new Promise(resolve=>server.listen(0,'127.0.0.1',resolve))));
 const directory=path.join(root,'.build/dashboard'),preference=path.join(directory,'preferred-port.json'),ports=servers.map(server=>server.address().port);
 fs.mkdirSync(directory,{recursive:true});
 const save=port=>fs.writeFileSync(preference,JSON.stringify({schemaVersion:1,port}));
 const reader=new CollaborationBridge(root);let followed,refreshed,error;
 try{
  servers.forEach((server,index)=>fs.writeFileSync(path.join(directory,'server-'+ports[index]+'.json'),JSON.stringify({instance:['first','second'][index],fixture:false})));
  try{save(ports[0]);followed=(await reader.snapshot()).tag;save(ports[1]);refreshed=(await reader.snapshot()).tag;}catch(caught){error=String(caught.message);}
  const explicit=(await new CollaborationBridge(root,{port:ports[0]}).snapshot()).tag;
  const before=calls.length,denied=[];
  for(const content of ['invalid JSON',JSON.stringify({schemaVersion:2,port:ports[0]}),JSON.stringify({schemaVersion:1,port:1023}),JSON.stringify({schemaVersion:1,port:String(ports[0])}),' '.repeat(1025)]){
   fs.writeFileSync(preference,content);try{await reader.snapshot();denied.push(false);}catch{denied.push(true);}
  }
  const invalidNoRequests=calls.length===before;
  const retained=directory+'-retained',external=path.join(root,'port-external');fs.mkdirSync(external);fs.renameSync(directory,retained);fs.symlinkSync(external,directory,'junction');
  let junctionDenied=false;try{await reader.snapshot();}catch(caught){junctionDenied=String(caught.message).includes('must stay in this checkout');}finally{fs.unlinkSync(directory);fs.renameSync(retained,directory);}
  fs.unlinkSync(preference);let defaultMissing=false;try{await reader.snapshot();}catch(caught){defaultMissing=caught.code==='ENOENT'&&caught.path===path.join(directory,'server-8765.json');}
  save(ports[1]);mismatch=true;let identityDenied=false;const beforeIdentity=calls.length;try{await reader.snapshot();}catch(caught){identityDenied=String(caught.message).includes('identity did not match');}
  const identityNoData=calls.length===beforeIdentity+1;mismatch=false;fixture=true;let fixtureDenied=false;try{await reader.snapshot();}catch(caught){fixtureDenied=String(caught.message).includes('identity did not match');}
  result={followed,refreshed,error,explicit,denied,invalidNoRequests,junctionDenied,defaultMissing,identityDenied,identityNoData,fixtureDenied};
 }finally{fs.unlinkSync(preference);await Promise.all(servers.map(server=>new Promise(resolve=>server.close(resolve))));}
}else if(mode==='dashboard'){
 const calls=[];let mismatch=false;
 const server=http.createServer((request,response)=>{calls.push(request.url);response.setHeader('Content-Type','application/json');response.end(JSON.stringify(request.url==='/api/health'?{service:'SnowDesktop build dashboard',instance:mismatch?'different':'fixture-identity',fixture:false}:{route:request.url,marker:'read-only-data'}));});
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 try{
  const port=server.address().port,directory=path.join(root,'.build/dashboard');fs.mkdirSync(directory,{recursive:true});fs.writeFileSync(path.join(directory,'server-'+port+'.json'),JSON.stringify({instance:'fixture-identity',fixture:false}));
  const reader=new CollaborationBridge(root,{port}),batch='/api/batches/'+'a'.repeat(32),retry='/api/retries/'+'a'.repeat(32)+'/build_dashboard/2/log';
  const values=[await reader.snapshot(),await reader.read(batch),await reader.read(retry)];
  const before=calls.length,denied=[];
  for(const route of ['http://127.0.0.1:1/api/status','/api/status?x=1','/api/batches/../state.json','/api/retries/'+'a'.repeat(32)+'/build_run/2/log','/api/retries/'+'a'.repeat(32)+'/build_dashboard/1/log']){try{await reader.read(route);denied.push(false);}catch{denied.push(true);}}
  const noRequests=calls.length===before;mismatch=true;let identityDenied=false;try{await reader.read(batch);}catch{identityDenied=true;}
  result={values,denied,noRequests,identityDenied,calls};
 }finally{await new Promise(resolve=>server.close(resolve));}
}
console.log(JSON.stringify(result));
)JS");
    auto node=[&](const std::string& mode){
        const auto module=mode=="dashboard-preferred"&&!preferredModule.empty()?preferredModule:repo/L"tools/dsh-collaboration/core.js";
        const auto result=run({L"node",script.wstring(),module.wstring(),c.root.wstring(),wide(mode)},c.root,30);
        require(result.code==0,"Node plugin bridge failed: "+result.out+result.err);
        return Json::parse(result.out);
    };
    const auto preferred=node("dashboard-preferred");
    require(preferred.has("followed")&&preferred.at("followed").str()=="first"&&preferred.has("refreshed")&&preferred.at("refreshed").str()=="second","Default dashboard port did not follow this checkout's updated preference");
    require(preferred.at("explicit").str()=="first"&&preferred.at("defaultMissing").boolean,"Explicit dashboard port or missing-preference compatibility changed");
    require(preferred.at("denied").items.size()==5&&preferred.at("invalidNoRequests").boolean&&preferred.at("junctionDenied").boolean,"Invalid or linked preference reached a dashboard listener");
    for(const auto& denied:preferred.at("denied").items)require(denied.boolean,"Malformed, out-of-range or oversized dashboard preference was accepted");
    require(preferred.at("identityDenied").boolean&&preferred.at("identityNoData").boolean&&preferred.at("fixtureDenied").boolean,"Preferred port bypassed checkout listener identity checks");
    const auto opened=node("open");
    require(opened.at("current").kind==Json::Kind::Null&&!opened.at("write").at("allowed").boolean&&read(c.root/L"src/a.txt")=="original\n",
            "Investigation blocked a batch or an unregistered write mutated source");
    const std::string unicode="协作消息 🧊 '\"; $(literal) ` |";
    c.call({"task-open","unicode-sender","-TaskName","Unicode event fixture"});
    c.call({"event-post","unicode-sender","-Target",opened.at("id").str(),"-EventKey","fixture-unicode","-Text",unicode});
    const auto roundTrip=node("unicode");
    require(roundTrip.at("values").items.size()==2,"PowerShell 7 and 5.1 Unicode bridge coverage was incomplete");
    for(const auto& value:roundTrip.at("values").items)
        require(value.at("progress").str()==unicode&&value.at("event").str()==unicode,"PowerShell bridge damaged Unicode progress/event text or interpreted argument data");
    const auto granted=node("grant");
    const auto id=granted.at("id").str(),batch=granted.at("registration").at("batchId").str();
    require(granted.at("write").at("allowed").boolean&&read(c.root/L"src/a.txt")=="authorized write","Claimed write did not reach the real coordinator gate");
    require(node("outside").at("outside").kind==Json::Kind::Null&&!fs::exists(c.root.parent_path()/(c.root.filename().wstring()+L"-outside.txt")),"Plugin claimed authority outside its configured checkout");
    require(node("bad-command").at("rejected").boolean,"Plugin exposed peer recovery/withdrawal");
    const auto dashboard=node("dashboard");
    require(dashboard.at("values").items.size()==3&&dashboard.at("values").items[0].at("route").str()=="/api/status"&&dashboard.at("values").items[1].at("route").str()=="/api/batches/"+std::string(32,'a')&&dashboard.at("values").items[2].at("route").str().find("/build_dashboard/2/log")!=std::string::npos,"Production dashboard reader lost snapshot, history or retry-log routes");
    for(const auto& denied:dashboard.at("denied").items)require(denied.boolean,"Dashboard reader accepted a non-allowlisted destination");
    require(dashboard.at("denied").items.size()==5&&dashboard.at("noRequests").boolean&&dashboard.at("identityDenied").boolean&&dashboard.at("calls").items.size()==7,"Invalid routes reached HTTP or a mismatching listener served batch data");
    c.begin("peer");
    c.plan(id,batch,0,"none","","tool","src/a.txt");c.ready(id,batch);
    const auto before=read(c.root/L"src/a.txt");
    require(!node("write").at("write").at("allowed").boolean&&before==read(c.root/L"src/a.txt"),"Ready registration retained write permission");
    const auto reopened=c.begin(id);
    require(reopened.at("editRevision").integer()==1&&!node("write").at("write").at("allowed").boolean&&node("rebind").at("write").at("allowed").boolean,"Reloaded plugin silently adopted an unseen edit round or explicit reacquisition failed");
    c.plan(id,batch,1,"none","","tool","src/a.txt");
    c.ready(id,batch,1);c.finish("peer",batch);
    require(!node("write").at("write").at("allowed").boolean,"Retired grant permitted later source mutation");
    require(node("escape").at("denied").boolean&&read(fs::path(c.root.wstring()+L"-escape")/L"keep.txt")=="outside original","Repository junction bypassed the write gate or changed external data");
    // Execute the production Host module against a small public-API carrier;
    // the actual state and write decisions still cross the real coordinator.
    // This is not a DSH runtime or UI acceptance fixture.
    write(c.root/L"sdk-fixture.mjs",R"JS(const chain=new Proxy(function(){return chain;},{get(){return chain;}});
export default chain;
export const defineTool=value=>value;
export const createUserMessage=value=>structuredClone({...value,role:'user',id:'fixture-message-'+Math.random()});
export const Remote=()=>{};
export class TypertRemoteService{constructor(ctx,name){this.ctx=ctx;ctx.services[name]=this;}}
)JS");
    // URL resolution avoids Windows drive-letter and Unicode pathname loss.
    write(c.root/L"loader.mjs",R"JS(export async function resolve(specifier,context,next){if(specifier.startsWith('@deepseek-ai/'))return {url:new URL('./sdk-fixture.mjs',import.meta.url).href,shortCircuit:true};return next(specifier,context);}
)JS");
    write(c.root/L"register-loader.mjs","import {register} from 'node:module'; register('./loader.mjs',import.meta.url);\n");
    write(c.root/L"host.mjs",R"JS(import fs from 'node:fs';import path from 'node:path';import {pathToFileURL} from 'node:url';import {execFile} from 'node:child_process';import {promisify} from 'node:util';
const [moduleFile,root]=process.argv.slice(2),{apply}=await import(pathToFileURL(moduleFile));
const hooks=new Map(),disposers=[],notices=[],services={};let tool,poll;
const nativeSetInterval=globalThis.setInterval;globalThis.setInterval=(callback,delay,...args)=>{if(delay===2000){poll=callback;return {unref(){}};}return nativeSetInterval(callback,delay,...args);};
const agent={id:'host-fixture-agent',session:{},status:'idle',inject:message=>notices.push(message)};
const ctx={services,inject(){},logger:{warn(){}},workingDirectory:{get:()=>root},fs:{resolve:async(target,options)=>({targetKey:path.resolve(options.cwd,target)}),processPath:target=>target.targetKey},agents:{list:()=>[]},tools:{register:value=>{tool=value;}},on:(name,handler)=>{hooks.set(name,handler);return()=>hooks.delete(name);},effect:effect=>disposers.push(effect())};
apply(ctx,{repositoryRoot:root,dashboardPort:8765,wakeResidentAgents:false});
const mutate=(name,next=()=>null)=>hooks.get('tools/execute')({name,arguments:{file_path:'src/a.txt'},agent},next);
let result;
try{
 await hooks.get('tools/pre-execute')({agent,name:'read'},()=>({kind:'allow'}));
 const denied=[];for(const name of ['write','edit']){try{await mutate(name,()=>({sentinel:'kept'}));denied.push(false);}catch{denied.push(true);}}
 const registration=JSON.parse((await tool.execute({action:'begin'},{agent})).json);await tool.execute({action:'claim',files:'src/a.txt',adoptExistingChanges:true},{agent});
 const kept=[];for(const name of ['write','edit'])kept.push(await mutate(name,()=>({sentinel:'kept'})));
 const bridge=services.snowCollaboration.bridge;
 await tool.execute({action:'progress',text:'Meaningful progress 中文'},{agent});
 const beforeHeartbeat=bridge.state().taskLedger.tasks.find(task=>task.id.startsWith('dsh-')&&task.displayName.startsWith('DSH host-fixture'));
 const realNow=Date.now,originalCall=bridge.call;let heartbeatCall;
 bridge.call=(...args)=>{heartbeatCall=originalCall.apply(bridge,args);return heartbeatCall;};
 try{
  Date.now=()=>realNow()+61000;agent.status='running';poll();
  if(!heartbeatCall)throw new Error('Production timer did not request a coordinator heartbeat');
  await heartbeatCall;
 }finally{Date.now=realNow;agent.status='idle';bridge.call=originalCall;}
 const afterHeartbeat=bridge.state().taskLedger.tasks.find(task=>task.id===beforeHeartbeat.id);
 const utcKey=value=>value.replace(/(\.\d*?)0+Z$/,'$1Z').replace(/\.Z$/,'Z');
 const heartbeat={capabilities:afterHeartbeat.capabilities,progress:afterHeartbeat.progress,revisionPreserved:afterHeartbeat.revision===beforeHeartbeat.revision,
  progressTimePreserved:utcKey(afterHeartbeat.lastProgressUtc)===utcKey(beforeHeartbeat.lastProgressUtc),
  refreshed:afterHeartbeat.lastHeartbeatUtc!==beforeHeartbeat.lastHeartbeatUtc};
 await bridge.call('begin','synthetic-peer');
 await tool.execute({action:'plan',scope:'tool',suites:'none',inputs:'src/a.txt',reason:'isolated gate contract fixture; no host test needed'},{agent});
 const ownId=(await import(pathToFileURL(path.join(path.dirname(moduleFile),'core.js')))).taskId(agent.id);
 await promisify(execFile)(bridge.powershell,['-NoProfile','-File',bridge.script,'ready',ownId,'-Batch',registration.batchId,'-Revision',String(registration.editRevision)],{cwd:root,windowsHide:true,env:{...process.env,PSExecutionPolicyPreference:'Bypass'}});
 async function deniedBoth(){const result=[];for(const name of ['write','edit']){try{await mutate(name);result.push(false);}catch{result.push(true);}}return result;}
 const ready=await deniedBoth();
 await bridge.call('begin',ownId);const stale=await deniedBoth();
 await tool.execute({action:'begin'},{agent});const rebound=await mutate('edit',()=>({sentinel:'fresh'}));
 const output=await tool.execute({action:'status'},{agent});
 await promisify(execFile)(bridge.powershell,['-NoProfile','-File',bridge.script,'event-post','synthetic-peer','-Target',ownId,'-EventKey','fixture-urgent','-EventKind','requirements','-Importance','urgent','-Text','fixture changed requirement'],{cwd:root,windowsHide:true,env:{...process.env,PSExecutionPolicyPreference:'Bypass'}});
 await poll();
 const deliveryDeadline=Date.now()+10000;
 while(notices.length<2&&Date.now()<deliveryDeadline)await new Promise(resolve=>setTimeout(resolve,10));
 // Exercise the real consumer contract: pending inbox rendering and next-step
 // admission both dereference source.kind, including when a notice is replayed.
 const replayed=JSON.parse(JSON.stringify(notices));
 const consumerKinds=replayed.map(message=>message.source.kind);
 result={denied,kept,ready,stale,rebound,heartbeat,notices:replayed,consumerKinds,rendered:tool.output.render({},output),tool:tool.name};
}finally{for(const dispose of disposers.reverse())if(typeof dispose==='function')await dispose();}
console.log(JSON.stringify(result));
)JS");
    const auto host=run({L"node",L"--import",L"./register-loader.mjs",(c.root/L"host.mjs").wstring(),(repo/L"tools/dsh-collaboration/index.js").wstring(),c.root.wstring()},c.root,40);
    require(host.code==0,"Production Host hook fixture failed: "+host.out+host.err);
    const auto observed=Json::parse(host.out);
    const auto& heartbeat=observed.at("heartbeat");
    for(const auto& capability:{"beforeWriteGate","boundedWait","liveProgress","activeTurnSteering"})require(heartbeat.at("capabilities").at(capability).boolean,"Production Host failed to declare an installed adapter capability");
    require(!heartbeat.at("capabilities").at("idleWake").boolean,"Disabled cold wake was advertised as available");
    require(heartbeat.at("progress").str()=="Meaningful progress 中文"&&heartbeat.at("revisionPreserved").boolean&&heartbeat.at("progressTimePreserved").boolean&&heartbeat.at("refreshed").boolean,
            "Production Host heartbeat overwrote meaningful progress or failed to refresh liveness");
    require(observed.at("denied").items.size()==2&&observed.at("denied").items[0].boolean&&observed.at("denied").items[1].boolean,"Host write/edit hooks did not deny missing grants");
    require(observed.at("notices").items.size()==2&&observed.at("kept").items[0].at("sentinel").str()=="kept"&&observed.at("kept").items[1].at("sentinel").str()=="kept","Plugin lost registration/event delivery or another policy's write intent");
    for(const auto& notice:observed.at("notices").items){
        require(notice.at("role").str()=="user"&&notice.at("source").at("kind").str()=="snowdesktop-collaboration"&&notice.at("source").at("form").str()=="notice"&&!notice.at("source").at("summary").str().empty()&&notice.at("source").at("summary").str().size()<=120,"Persisted coordination notice has no valid producer-owned source");
    }
    require(observed.at("consumerKinds").items.size()==2,"Replayed notices failed consumer source classification");
    for(const auto& phase:{"ready","stale"})require(observed.at(phase).items[0].boolean&&observed.at(phase).items[1].boolean,"Host gate accepted a ready or superseded cached grant");
    require(observed.at("rebound").at("sentinel").str()=="fresh","Explicit Host reacquisition failed");
    require(observed.at("rendered").items[0].at("type").str()=="text"&&observed.at("tool").str()=="snow_collaboration","Plugin tool output violated the inspected content contract");
    // Keep the real coordinator and production delivery loop. Only the Agent
    // API is a carrier; actual idle-model startup is a separate live check.
    write(c.root/L"wake.mjs",R"JS(import path from 'node:path';import {pathToFileURL} from 'node:url';import {execFile} from 'node:child_process';import {promisify} from 'node:util';
const [moduleFile,root,mode]=process.argv.slice(2),enabled=mode==='enabled';
const {apply}=await import(pathToFileURL(moduleFile));
const {taskId}=await import(pathToFileURL(path.join(path.dirname(moduleFile),'core.js')));
const hooks=new Map(),disposers=[],services={},notices=[],errors=[];let poll,notify,acks=0;
const nativeSetInterval=globalThis.setInterval;
globalThis.setInterval=(callback,delay,...args)=>{if(delay===2000){poll=callback;return {unref(){}};}return nativeSetInterval(callback,delay,...args);};
const record=(agent,method,message)=>{notices.push({agent,method,message});notify?.();};
const make=(name,status,cwd=root)=>({id:'wake-'+mode+'-'+name,session:{cwd},status,inject:message=>record(name,'inject',message),followup:message=>record(name,'followup',message)});
const idle=make('idle','idle'),active=make('active','running'),released=make('released','idle'),outside=make('outside','idle',root+'-outside');
const ctx={services,inject(){},logger:{warn:error=>errors.push(String(error))},workingDirectory:{get:session=>session.cwd},fs:{},agents:{list:()=>[idle,active,released,outside]},tools:{register(){}},on:(name,handler)=>{hooks.set(name,handler);return()=>hooks.delete(name);},effect:effect=>disposers.push(effect())};
async function delivered(count){if(notices.length>=count)return;await new Promise((resolve,reject)=>{const timeout=setTimeout(()=>reject(new Error('Production event delivery did not complete')),10000);notify=()=>{if(notices.length>=count){clearTimeout(timeout);notify=null;resolve();}};notify();});}
let result;
try{
 apply(ctx,{repositoryRoot:root,dashboardPort:8765,wakeResidentAgents:enabled});
 for(const agent of [idle,active,released,outside])await hooks.get('tools/pre-execute')({agent,name:'read'},()=>null);
 hooks.get('agent/disposed')({agent:released});
 const bridge=services.snowCollaboration.bridge,call=bridge.call.bind(bridge);
 bridge.call=(...args)=>{if(args[0]==='event-ack')acks++;return call(...args);};
 const post=(key,importance)=>promisify(execFile)(bridge.powershell,['-NoProfile','-File',bridge.script,'event-post','synthetic-peer','-Target','*','-EventKey','wake-'+mode+'-'+key,'-Importance',importance,'-Text','Isolated authorized delivery check'],{cwd:root,windowsHide:true,env:{...process.env,PSExecutionPolicyPreference:'Bypass'}});
 const registered=notices.length;
 await post('normal','normal');poll();
 const normal={notices:notices.length-registered,acks};
 await post('urgent','urgent');poll();await delivered(registered+2);
 const event=bridge.state().taskLedger.events.find(event=>event.key==='wake-'+mode+'-urgent');
 const deliveredAcks=acks,deliveredNotices=notices.length;poll();
 const duplicate={acks:acks-deliveredAcks,notices:notices.length-deliveredNotices};
 for(const dispose of disposers.reverse())if(typeof dispose==='function')await dispose();
 await post('unloaded','urgent');poll();
 const unloaded={acks:acks-deliveredAcks,notices:notices.length-deliveredNotices};
 result={enabled,normal,duplicate,unloaded,deliveries:notices.slice(registered),registered:registered,
  receiptTasks:event.receipts.map(receipt=>receipt.task).sort(),expectedTasks:[taskId(idle.id),taskId(active.id)].sort(),
  receiptLevels:event.receipts.map(receipt=>receipt.level),outsideRegistered:bridge.state().taskLedger.tasks.some(task=>task.id===taskId(outside.id)),errors};
}finally{for(const dispose of disposers.reverse())if(typeof dispose==='function')await dispose();}
console.log(JSON.stringify(result));
)JS");
    for(const std::wstring mode:{L"disabled",L"enabled"}){
        const auto execution=run({L"node",L"--import",L"./register-loader.mjs",(c.root/L"wake.mjs").wstring(),(repo/L"tools/dsh-collaboration/index.js").wstring(),c.root.wstring(),mode},c.root,30);
        require(execution.code==0,"Production wake delivery carrier failed: "+execution.out+execution.err);
        const auto wake=Json::parse(execution.out);
        require(wake.at("normal").at("notices").integer()==0&&wake.at("normal").at("acks").integer()==0,"Normal progress woke or notified a model");
        require(wake.at("registered").integer()==3&&!wake.at("outsideRegistered").boolean&&wake.at("deliveries").items.size()==2,"Broadcast reached a released/outside Agent or lost a resident recipient");
        bool idleSeen=false,activeSeen=false;
        for(const auto& delivery:wake.at("deliveries").items){
            const auto name=delivery.at("agent").str();
            if(name=="idle"){idleSeen=true;require(delivery.at("method").str()==(mode==L"enabled"?"followup":"inject"),"Idle event did not respect the configured wake boundary");}
            else if(name=="active"){activeSeen=true;require(delivery.at("method").str()=="inject","An active model received a competing followup");}
            else require(false,"Broadcast reached a non-resident Agent");
            require(delivery.at("message").at("source").at("kind").str()=="snowdesktop-collaboration","Wake notice lost its producer identity");
        }
        require(idleSeen&&activeSeen&&wake.at("receiptTasks").items.size()==2,"Broadcast recipient or adapter receipt missing");
        for(size_t index=0;index<2;++index)require(wake.at("receiptTasks").items[index].str()==wake.at("expectedTasks").items[index].str()&&wake.at("receiptLevels").items[index].str()=="adapter","Delivery manufactured Agent acknowledgement or acknowledged a wrong recipient");
        for(const auto& phase:{"duplicate","unloaded"})require(wake.at(phase).at("acks").integer()==0&&wake.at(phase).at("notices").integer()==0,"Repeated polling or unloaded plugin delivered an event");
        require(wake.at("errors").items.empty(),"Production delivery reported an unexpected error");
    }
    write(c.root/L"cold-recovery.mjs",R"JS(import path from 'node:path';import {pathToFileURL} from 'node:url';import {execFile} from 'node:child_process';import {promisify} from 'node:util';
const [moduleFile,root,mode]=process.argv.slice(2),{apply}=await import(pathToFileURL(moduleFile));
const {CollaborationBridge,taskId}=await import(pathToFileURL(path.join(path.dirname(moduleFile),'core.js')));
const bridge=new CollaborationBridge(root),hooks=new Map(),services={},disposers=[],notices=[],errors=[],resumed=[];let poll,listCalls=0,releaseResume;
const nativeSetInterval=globalThis.setInterval;
globalThis.setInterval=(callback,delay,...args)=>{if(delay===2000){poll=callback;return {unref(){}};}return nativeSetInterval(callback,delay,...args);};
const turn=()=>new Promise(resolve=>setImmediate(resolve));
const wait=async predicate=>{const until=Date.now()+10000;while(!predicate()){if(Date.now()>until)throw new Error('Cold recovery carrier deadline');await new Promise(resolve=>setTimeout(resolve,5));}};
const make=(name,cwd=root)=>({id:'cold-'+mode+'-'+name,session:{cwd},status:'idle',inject:message=>notices.push({name,method:'inject',message}),followup:message=>notices.push({name,method:'followup',message})});
const main=make('main'),failed=make('failed'),closed=make('closed'),outside=make('outside',root+'-outside'),released=make('released'),child=make('child'),unknown=make('unknown');
for(const agent of [main,failed,closed,outside,released,child])await bridge.open(agent.id);
const controller={list:async(_request,signal)=>{signal.throwIfAborted();listCalls++;return {items:[main,failed,closed,outside,released,child,unknown].map(agent=>({sessionId:agent.id,cwd:agent.session.cwd,...(agent===child?{origin:'subagent'}:{})}))};},resolveAgent:async id=>{resumed.push(id);if(mode==='unload')return await new Promise(resolve=>{releaseResume=resolve;});if(id===failed.id)return {error:new Error('Controlled session restoration failure')};return {agent:main};}};
const ctx={services,logger:{warn:error=>errors.push(String(error))},workingDirectory:{get:session=>session.cwd},fs:{},agents:{list:()=>[]},tools:{register(){}},on:(name,handler)=>{hooks.set(name,handler);return()=>hooks.delete(name);},effect:effect=>disposers.push(effect()),inject:(_deps,apply)=>{if(mode!=='missing')apply({...ctx,sessionController:controller});}};
const command=(action,id,args=[])=>promisify(execFile)(bridge.powershell,['-NoProfile','-File',bridge.script,action,id,...args],{cwd:root,windowsHide:true,env:{...process.env,PSExecutionPolicyPreference:'Bypass'}});
const post=(key,importance)=>command('event-post','synthetic-peer',['-Target','*','-EventKey','cold-'+mode+'-'+key,'-Importance',importance,'-Text','Addressed project coordination data, not new permission']);
const dispose=()=>{while(disposers.length){const disposer=disposers.pop();if(typeof disposer==='function')disposer();}};
let result;
try{
 await command('task-open','synthetic-peer',['-TaskName','Isolated cold-session sender']);
 apply(ctx,{repositoryRoot:root,dashboardPort:8765,wakeResidentAgents:mode!=='disabled'});
 await hooks.get('tools/pre-execute')({agent:released,name:'read'},()=>null);hooks.get('agent/disposed')({agent:released});
 const registered=notices.length,currentBefore=JSON.stringify(bridge.state().current);
 await post('normal','normal');poll();await turn();const normal={lists:listCalls,resumes:resumed.length,notices:notices.length-registered};
 await post('urgent','urgent');await command('task-close',taskId(closed.id),['-TaskRevision','0','-TaskPhase','completed']);poll();
 if(mode==='enabled')await wait(()=>notices.some(row=>row.method==='followup')&&errors.length===1);
 else if(mode==='unload'){await wait(()=>Boolean(releaseResume));dispose();releaseResume({agent:main});await turn();await turn();}
 else await turn();
 const beforeDuplicate={lists:listCalls,resumes:resumed.length,notices:notices.length};poll();await turn();
 const duplicate={lists:listCalls-beforeDuplicate.lists,resumes:resumed.length-beforeDuplicate.resumes,notices:notices.length-beforeDuplicate.notices};
 dispose();await post('unloaded','urgent');poll();await turn();
 const state=bridge.state(),event=state.taskLedger.events.find(event=>event.key==='cold-'+mode+'-urgent');
 result={mode,normal,resumed,listCalls,deliveries:notices.slice(registered),duplicate,unloaded:{lists:listCalls-beforeDuplicate.lists,resumes:resumed.length-beforeDuplicate.resumes,notices:notices.length-beforeDuplicate.notices},receipts:event.receipts,errors,currentPreserved:JSON.stringify(state.current)===currentBefore,mainTask:state.taskLedger.tasks.find(task=>task.id===taskId(main.id)),expectedMain:main.id,expectedFailed:failed.id,expectedTask:taskId(main.id)};
}finally{dispose();}
console.log(JSON.stringify(result));
)JS");
    const auto requireRestored=[](const Json& cold){
        require(cold.at("listCalls").integer()==1&&cold.at("resumed").items.size()==2&&cold.at("resumed").items[0].str()==cold.at("expectedMain").str()&&cold.at("resumed").items[1].str()==cold.at("expectedFailed").str(),"Addressed urgent event did not restore its persisted project session");
        require(cold.at("errors").items.size()==1&&cold.at("errors").items[0].str().find("Controlled session restoration failure")!=std::string::npos,"Failed restoration was hidden or retried");
        require(cold.at("deliveries").items.size()==2&&cold.at("deliveries").items[0].at("method").str()=="inject"&&cold.at("deliveries").items[1].at("method").str()=="followup","Restored Agent did not register before receiving its wake notice");
        const auto& message=cold.at("deliveries").items[1].at("message");
        require(message.at("source").at("kind").str()=="snowdesktop-collaboration"&&message.at("source").at("form").str()=="notice","Cold recovery promoted project data to an unqualified user command");
        require(cold.at("receipts").items.size()==1&&cold.at("receipts").items[0].at("task").str()==cold.at("expectedTask").str()&&cold.at("receipts").items[0].at("level").str()=="adapter","Cold recovery manufactured Agent acknowledgement or acknowledged failed/outside recipients");
    };
    const auto checkCold=[&](const std::wstring& mode,const fs::path& recoveryRoot){
        const auto execution=run({L"node",L"--import",L"./register-loader.mjs",(c.root/L"cold-recovery.mjs").wstring(),(repo/L"tools/dsh-collaboration/index.js").wstring(),recoveryRoot.wstring(),mode},c.root,40);
        require(execution.code==0,"Production cold-session recovery carrier failed: "+execution.out+execution.err);
        const auto cold=Json::parse(execution.out);
        require(cold.at("normal").at("lists").integer()==0&&cold.at("normal").at("resumes").integer()==0&&cold.at("normal").at("notices").integer()==0,"Normal progress caused cold-session discovery or activation");
        require(cold.at("currentPreserved").boolean&&cold.at("mainTask").at("revision").integer()==0&&cold.at("mainTask").at("phase").str()=="investigating","Cold recovery changed an editing grant or business task revision");
        for(const auto& phase:{"duplicate","unloaded"})require(cold.at(phase).at("lists").integer()==0&&cold.at(phase).at("resumes").integer()==0&&cold.at(phase).at("notices").integer()==0,"Repeated poll or disposed plugin repeated cold recovery");
        if(mode==L"enabled"){
            require(cold.at("mainTask").at("capabilities").at("idleWake").boolean,"Available configured Session Controller wake was not declared");
            requireRestored(cold);
        }else{
            require(cold.at("deliveries").items.empty()&&cold.at("receipts").items.empty()&&cold.at("errors").items.empty(),"Disabled/unavailable/disposed recovery delivered or acknowledged an event");
            require(cold.at("resumed").items.size()==(mode==L"unload"?1u:0u),"Recovery ignored its configuration or service lifetime");
        }
    };
    auto residentOnly=read(repo/L"tools/dsh-collaboration/index.js");
    replace(residentOnly,"if(config.wakeResidentAgents&&cold&&await resumePending(state))state=bridge.state();","// Former resident-only path leaves cold recipients inactive.");
    write(c.root/L"cold-old/index.js",residentOnly);write(c.root/L"cold-old/core.js",read(repo/L"tools/dsh-collaboration/core.js"));write(c.root/L"cold-old/package.json","{\"type\":\"module\"}");
    // Construct roots serially; run only independent process/state owners in
    // three groups, matching this test's existing PROCESSORS3 reservation.
    std::vector<std::unique_ptr<Coordinator>> recoveryRoots;
    for(int index=0;index<4;++index)recoveryRoots.push_back(std::make_unique<Coordinator>(repo,"dsh-cold-recovery"));
    Coordinator former(repo,"dsh-cold-former");
    auto coldPrimary=std::async(std::launch::async,[&]{
        checkCold(L"enabled",recoveryRoots[0]->root);
        const auto oldRun=run({L"node",L"--import",L"./register-loader.mjs",(c.root/L"cold-recovery.mjs").wstring(),(c.root/L"cold-old/index.js").wstring(),former.root.wstring(),L"legacy"},c.root,40);
        require(oldRun.code==0,"Former cold-session path did not reach its business observation: "+oldRun.out+oldRun.err);
        const auto oldCold=Json::parse(oldRun.out);bool oldRejected=false;
        try{requireRestored(oldCold);}catch(const std::exception& error){oldRejected=std::string(error.what())=="Addressed urgent event did not restore its persisted project session";}
        require(oldRejected&&oldCold.at("receipts").items.empty(),"Former resident-only path did not fail the same persisted-session wake assertion");
    });
    auto coldDisabled=std::async(std::launch::async,[&]{checkCold(L"disabled",recoveryRoots[1]->root);});
    auto coldOther=std::async(std::launch::async,[&]{checkCold(L"missing",recoveryRoots[2]->root);checkCold(L"unload",recoveryRoots[3]->root);});
    coldPrimary.get();coldDisabled.get();coldOther.get();
    write(c.root/L"client.mjs",R"JS(import fs from 'node:fs';import vm from 'node:vm';
const events=[],updates=[],componentCleanup=[],timers=new Map();let registration,props,Panel,stateIndex=0,timerId=0,mounted=false,contribution,dictionaries;
let response={ok:true,value:JSON.stringify({marker:'known-good',taskPresence:[],events:[]})};
const remote={snapshot:async()=>{if(!mounted)throw new Error('withdrawn');return await response;}};
// Only the polling/disposal boundary is exercised by this carrier, never layout.
const React={createElement:(type,props,...children)=>({type,props,children}),useRef:initial=>({current:initial}),useLayoutEffect(){},useState:initial=>{const index=stateIndex++;return [typeof initial==='function'?initial():initial,value=>updates.push({index,value})];},useEffect:effect=>componentCleanup.push(effect())};
vm.runInNewContext(fs.readFileSync(process.argv[2],'utf8'),{window:{__ModuleLoader__:{load:value=>{registration=value;}}},localStorage:{getItem:()=>null,setItem(){}},setTimeout:callback=>{const id=++timerId;timers.set(id,callback);return id;},clearTimeout:id=>timers.delete(id),console});
const plugin=registration.factory(name=>{if(name==='react')return React;if(name==='zod')return {z:{string:()=>({})}};throw new Error('Unexpected Client import: '+name);});
function context(rejectSlot=false){const effects=[];const ctx={inject:async(_dependencies,apply)=>apply(ctx),remote:{$mount:async value=>{contribution=value;events.push('mount');mounted=true;ctx.remote.snowCollaboration=remote;return async()=>{events.push('unmount');mounted=false;delete ctx.remote.snowCollaboration;};}},effect:effect=>effects.push(effect()),locale:{register:(_namespace,value)=>{dictionaries=value;return ()=>{};},bind:()=>key=>dictionaries.en[key]||key},slots:{inject:(name,register)=>{events.push('slot');if(rejectSlot)throw new Error('controlled slot failure');return register();},register:(options,component)=>{props=options.inject();Panel=component;}}};return {ctx,effects};}
const first=context();const unmount=await plugin.apply(first.ctx);const panel=Panel(props),versions=[];
function displayedVersions(node){if(typeof node==='string'&&/^0\.\d+\.\d+$/.test(node))versions.push(node);if(node&&typeof node==='object')for(const child of node.children||[])displayedVersions(child);}
displayedVersions(panel);const rootVersion=panel.props['data-version'];
// Render the production validation view against the same public projection used
// by the web dashboard. This carrier observes content, not native layout.
const originalState=React.useState,originalEffect=React.useEffect;
const run={id:'a'.repeat(32),task:'budget-owner',status:'queued',resources:{status:'sampling',cpuLimitBasisPoints:2500,committedMemoryLimitBytes:3221225472},admission:{policy:'project-weighted-v1',compatibilityMode:'shared',cpuSlots:2,cpuCapacity:6,memoryMiB:3072,reservedMemoryMiB:3072,memoryCapacityMiB:6144,lastObservation:{occupiedCpuSlots:6,occupiedMemoryMiB:4096,reason:'cpu-capacity'}}};
let lastRendered;
function renderBudget(model,lang='en',tab='validation',preferences={}){let index=0;React.useEffect=()=>{};React.useState=initial=>{const value=typeof initial==='function'?initial():initial;return [index++===0?(model||{snapshot:{runs:[run]}}):index===3?{...value,tab,...preferences}:value,()=>{}];};const rendered=Panel({...props,t:key=>dictionaries[lang][key]||key}),values=[];lastRendered=rendered;function visit(node){if(typeof node==='string'||typeof node==='number')values.push(String(node));else if(Array.isArray(node))node.forEach(visit);else if(node&&typeof node==='object'&&node.type!=='style')(node.children||[]).forEach(visit);}visit(rendered);return values.join(' ');}
const budgetText=renderBudget();run.resources={status:'unrecorded'};run.admission={policy:'project-weighted-v1'};const missingBudgetText=renderBudget();
const rich={id:'b'.repeat(32),test:'Passed with open dependencies',status:'passed',seconds:0.125,identityStatus:'recorded',reuseAllowed:false,binding:{assertionBinary:'c'.repeat(64)},dependencies:{status:'observed',sourceClosure:'unproven',sourceReports:3,issues:[],issueCount:0},runtimeInputs:{status:'stable',runtimeClosure:'unclosed',issues:[],issueCount:0},runtimeModules:{status:'observed',imageCount:23,processCount:2,runtimeClosure:'unclosed',timingComparableToUnobservedRun:false,issues:[],issueCount:0},attempts:[{attempt:1,status:'failed',seconds:0.2},{attempt:2,status:'passed',seconds:0.125}]};
const legacy={id:'d'.repeat(32),test:'Legacy missing observation',status:'passed',reuseAllowed:false};
const changed={...rich,id:'e'.repeat(32),test:'Changed input observation',identityStatus:'invalidated',runtimeInputs:{status:'changed'},runtimeModules:{status:'incomplete',imageCount:null,processCount:null,issues:['loaded-module-file-changed:fixture.dll'],issueCount:14}};
const evidence={status:'recorded',totalCount:3,tests:[rich,legacy,changed]};run.evidence=evidence;
const runEvidenceText=renderBudget(),evidenceModel={snapshot:{runs:[]},batch:{id:'f'.repeat(32),evidence}};
const batchEvidenceText=renderBudget(evidenceModel),batchEvidenceZh=renderBudget(evidenceModel,'zh');
run.verification={baseline:'f'.repeat(32),names:['Alpha'],referenceCount:2,references:[{id:'1'.repeat(32),task:'reference-observer',status:'recorded',sourceRunId:run.id,baseline:'f'.repeat(32),checkedUtc:'2026-10-02T10:04:00Z',tests:[{test:'Alpha'}],reviewPolicy:'reviewed-native-files-v1',sourceRunSha256:'2'.repeat(64),coverageSha256:'3'.repeat(64)},{id:'4'.repeat(32),task:'missing-receipt',status:'unavailable',reason:'Reference receipt is missing'}]};
const referencesText=renderBudget(),referencesZh=renderBudget(undefined,'zh');
const mixedModel={snapshot:{runs:[]},batch:{id:'f'.repeat(32),coverage:{status:'passed',summary:{selected:2,executed:1,passed:1,failed:0,referenced:1,notRun:0,unreported:0},completed:[{name:'Beta',status:'passed',seconds:0.1}],references:[{test:'Alpha',status:'recorded',sourceRunId:'9'.repeat(32),checkedUtc:'2026-10-02T10:04:00Z',receipts:[{task:'reference-owner',id:'8'.repeat(32)}]}]}}};
mixedModel.batch.evidence={totalCount:1,tests:[{id:'7'.repeat(32),test:'Alpha',status:'referenced',attempts:[]}]};
const mixedText=renderBudget(mixedModel),mixedZh=renderBudget(mixedModel,'zh');
mixedModel.batch.coverage.summary={...mixedModel.batch.coverage.summary,selected:1,executed:0,passed:0};mixedModel.batch.coverage.status='fulfilled';mixedModel.batch.coverage.completed=[];
const referenceOnlyText=renderBudget(mixedModel);
const logical='5'.repeat(32),current='6'.repeat(32),priorRun='7'.repeat(32);
const workspaceSnapshot={current:{id:current,logicalBatchId:logical,attempt:2,phase:'editing',createdUtc:'2026-10-02T10:30:00Z',tasks:[{id:'former-agent',state:'finished',display:'waiting',check:{status:'passed'},plan:{}},{id:'current-editor',state:'editing',display:'editing',check:{status:'invalidated'},plan:{}}],log:{lines:[]}},
 taskPresence:[{id:'former-agent',displayName:'Former conversation',phase:'waiting',lastHeartbeatUtc:'2020-01-01T00:00:00Z'},{id:'current-editor',displayName:'Current editor',phase:'editing',lastHeartbeatUtc:'2020-01-01T00:00:00Z'},{id:'old-investigation',displayName:'Old investigation',phase:'investigating',lastHeartbeatUtc:'2020-01-01T00:00:00Z',lastProgressUtc:'2020-01-01T00:00:00Z'}],
 history:[{batchId:logical,logicalBatchId:logical,attempt:1,outcome:'failed',createdUtc:'2026-10-02T10:00:00Z',completedUtc:'2026-10-02T10:30:00Z',participants:['Former conversation','Current editor']}],
 runs:[{id:priorRun,batchId:logical,status:'failed',task:'former-agent',log:{lines:['previous-only-log']}}],events:[]};
const workspaceModel={snapshot:workspaceSnapshot,batch:workspaceSnapshot.current};
renderBudget(workspaceModel,'en','participants',{detail:true,workKey:'batch:'+logical});
function nodeText(node){if(typeof node==='string'||typeof node==='number')return String(node);if(Array.isArray(node))return node.map(nodeText).join(' ');return node&&typeof node==='object'&&node.type!=='style'?(node.children||[]).map(nodeText).join(' '):'';}
function collectAttribute(node,attribute,result=[]){if(Array.isArray(node))node.forEach(item=>collectAttribute(item,attribute,result));else if(node&&typeof node==='object'){if(node.props?.[attribute])result.push({key:node.props[attribute],text:nodeText(node)});(node.children||[]).forEach(item=>collectAttribute(item,attribute,result));}return result;}
const workspaceParticipants=collectAttribute(lastRendered,'data-participant'),workspaceActivities=collectAttribute(lastRendered,'data-activity');
const workspaceBuilds=renderBudget(workspaceModel,'en','builds',{detail:true,workKey:'batch:'+logical});
const workspaceParticipantsZh=renderBudget(workspaceModel,'zh','participants',{detail:true,workKey:'batch:'+logical});
workspaceSnapshot.events=[{sequence:1,sender:'outside-task',target:'*',recipients:['current-editor'],text:'broadcast-for-selected-activity'},
 {sequence:2,sender:'outside-task',target:'*',recipients:['other-project-task'],text:'broadcast-for-another-activity'}];
const workspaceActivityText=renderBudget(workspaceModel,'en','activity',{detail:true,workKey:'batch:'+logical});
React.useState=originalState;React.useEffect=originalEffect;
await new Promise(resolve=>setImmediate(resolve));
const firstSnapshot=updates.find(value=>value.index===0)?.value?.snapshot;
async function poll(){const [id,callback]=timers.entries().next().value;timers.delete(id);await callback();}
response={ok:false,error:{message:'controlled offline'}};await poll();
const offline=updates.filter(value=>value.index===1).at(-1)?.value;
response={ok:true,value:'invalid JSON'};await poll();
const malformed=updates.filter(value=>value.index===1).at(-1)?.value;
let finish;response=new Promise(resolve=>{finish=resolve;});const pending=poll();
for(const cleanup of componentCleanup)cleanup();const beforeLate=updates.length;
await unmount();finish({ok:true,value:JSON.stringify({marker:'late'})});await pending;
let rejected=false;const second=context(true);try{await plugin.apply(second.ctx);}catch{rejected=true;}
for(const cleanup of [...first.effects,...second.effects])if(typeof cleanup==='function')await cleanup();
const descriptor=contribution.descriptors[0];
console.log(JSON.stringify({inject:plugin.inject,events,firstSnapshot,offline,malformed,lastSnapshot:updates.filter(value=>value.index===0).at(-1)?.value?.snapshot,lateUpdated:updates.length!==beforeLate,timers:timers.size,rejected,rootVersion,versions,budgetText,missingBudgetText,runEvidenceText,batchEvidenceText,batchEvidenceZh,referencesText,referencesZh,mixedText,mixedZh,referenceOnlyText,workspaceParticipants,workspaceActivities,workspaceBuilds,workspaceParticipantsZh,workspaceActivityText,descriptor:{package:contribution.package,id:descriptor.id,namespace:descriptor.namespace,method:descriptor.method,service:descriptor.service,parameters:descriptor.parameters,resultMode:descriptor.result.mode,resultType:descriptor.result.jsonSchema.type}}));
)JS");
    const auto client=run({L"node",(c.root/L"client.mjs").wstring(),(repo/L"tools/dsh-collaboration/client.js").wstring()},c.root,15);
    require(client.code==0,"Production Client carrier failed: "+client.out+client.err);
    const auto clientObserved=Json::parse(client.out);
    const auto& participantRows=clientObserved.at("workspaceParticipants").items;
    require(participantRows.size()==2&&participantRows[0].at("text").str().find("Ready")!=std::string::npos&&participantRows[1].at("text").str().find("Editing")!=std::string::npos,
        "Current participant phases were replaced by heartbeat freshness or lost explicit batch readiness");
    const auto& activityRows=clientObserved.at("workspaceActivities").items;
    require(activityRows.size()==2&&activityRows[0].at("key").str()=="batch:"+std::string(32,'5')&&activityRows[1].at("text").str().find("Last recorded")!=std::string::npos,
        "Overlay split repair attempts or presented an old investigation as currently running");
    require(activityRows[0].at("text").str().find("Failed")==std::string::npos,
        "Overlay replaced current repair phase with equal-time historical failure");
    require(clientObserved.at("workspaceBuilds").str().find("previous-only-log")==std::string::npos&&clientObserved.at("workspaceParticipantsZh").str().find("已就绪")!=std::string::npos,
        "Unstarted overlay attempt borrowed prior Run logs or participant status was untranslated");
    require(clientObserved.at("workspaceActivityText").str().find("broadcast-for-selected-activity")!=std::string::npos&&
        clientObserved.at("workspaceActivityText").str().find("broadcast-for-another-activity")==std::string::npos,
        "Overlay omitted a received broadcast or mixed another activity's messages");
    require(clientObserved.at("mixedText").str().find("Referenced original result (not executed this time)")!=std::string::npos&&
        clientObserved.at("mixedZh").str().find("引用原结果（本次未执行）")!=std::string::npos&&
        clientObserved.at("mixedText").str().find("Dependency closure pending; no automatic reuse")==std::string::npos&&
        clientObserved.at("referenceOnlyText").str().find("Executed / selected · 0 / 1")!=std::string::npos,
        "Overlay conflated mixed/reference-only coverage with newly executed passes or lost translated receipt details");
    const auto budgetText=clientObserved.at("budgetText").str(),missingBudgetText=clientObserved.at("missingBudgetText").str();
    require(budgetText.find("CPU reservation / capacity")!=std::string::npos&&budgetText.find("2 / 6 CPU slots")!=std::string::npos&&budgetText.find("25.00% / 3072 MiB")!=std::string::npos&&budgetText.find("Insufficient CPU capacity")!=std::string::npos,"Overlay lost budget, kernel limit or queue content from the public Run projection");
    require(missingBudgetText.find("Not recorded / Not recorded CPU slots")!=std::string::npos&&missingBudgetText.find("25.00% / 3072 MiB")==std::string::npos,"Overlay inferred or retained missing budget values");
    for(const auto* key:{"runEvidenceText","batchEvidenceText"}){
        const auto text=clientObserved.at(key).str();
        require(text.find("Completeness unproven")!=std::string::npos&&text.find("Compiler reports 3")!=std::string::npos&&text.find("Loaded images 23 / Processes 2")!=std::string::npos&&text.find("Endpoints match")!=std::string::npos&&text.find("Dependency closure pending; no automatic reuse")!=std::string::npos,"Overlay batch/Run evidence lost observed scope or inferred reuse permission");
        require(text.find("Loaded images Not recorded / Processes Not recorded")!=std::string::npos&&text.find("loaded-module-file-changed:fixture.dll")!=std::string::npos&&text.find("Inputs changed")!=std::string::npos&&text.find("Attempt 1 Failed")!=std::string::npos&&text.find("Attempt 2 Passed")!=std::string::npos,"Overlay lost legacy absence, invalidation reasons or original execution attempts");
    }
    const auto chinese=clientObserved.at("batchEvidenceZh").str();
    require(chinese.find("完整性未证明")!=std::string::npos&&chinese.find("编译器报告 3")!=std::string::npos&&chinese.find("加载图像 23 / 进程 2")!=std::string::npos&&chinese.find("尚未自动复用")!=std::string::npos,"Overlay Chinese evidence fields were missing or untranslated");
    const auto referencesText=clientObserved.at("referencesText").str(),referencesZh=clientObserved.at("referencesZh").str();
    require(referencesText.find("Existing result references")!=std::string::npos&&referencesText.find("no new test execution")!=std::string::npos&&referencesText.find("Reference receipt unavailable")!=std::string::npos&&referencesText.find("reference-observer")!=std::string::npos&&referencesText.find("Alpha")!=std::string::npos,"Overlay lost independent completed-reference provenance or treated missing receipts as recorded");
    require(referencesZh.find("引用既有结果")!=std::string::npos&&referencesZh.find("未新增测试执行")!=std::string::npos&&referencesZh.find("引用凭据不可核对")!=std::string::npos,"Overlay completed-reference fields were untranslated");
    bool rootRemote=false,unavailableRemote=false;
    for(const auto& value:clientObserved.at("inject").items){rootRemote|=value.str()=="remote";unavailableRemote|=value.str()=="remote.snowCollaboration";}
    require(rootRemote&&!unavailableRemote&&clientObserved.at("events").items[0].str()=="mount"&&clientObserved.at("events").items[1].str()=="slot","Client waited on an unmounted namespace or registered its slot before mounting");
    require(clientObserved.at("descriptor").at("id").str()=="@local/snowdesktop-collaboration#snowCollaboration/snapshot"&&clientObserved.at("descriptor").at("parameters").items.empty()&&clientObserved.at("descriptor").at("resultMode").str()=="strict"&&clientObserved.at("descriptor").at("resultType").str()=="string","Client snapshot Remote descriptor changed its read-only contract");
    require(clientObserved.at("firstSnapshot").at("marker").str()=="known-good"&&clientObserved.at("offline").str()=="controlled offline"&&!clientObserved.at("malformed").str().empty()&&clientObserved.at("lastSnapshot").at("marker").str()=="known-good","Client failed RemoteResult decoding or lost the last snapshot after a carrier/data error");
    require(!clientObserved.at("lateUpdated").boolean&&clientObserved.at("timers").integer()==0&&clientObserved.at("rejected").boolean&&clientObserved.at("events").items.back().str()=="unmount","Client updated after disposal, leaked a timer, or retained a failed mount");
    const auto manifest=Json::parse(read(repo/L"tools/dsh-collaboration/package.json"));
    require(clientObserved.at("rootVersion").str()==manifest.at("version").str()&&clientObserved.at("versions").items.size()==1&&clientObserved.at("versions").items[0].str()==manifest.at("version").str(),"Loaded overlay identity and displayed version disagreed with the package");
    for(const auto& file:{"index.js","core.js","client.js","typert.host.js","typert.remote-client.js","cordis.patch.yml","locale/en.json","locale/zh.json","icon.svg","README.md"})
        require(fs::is_regular_file(repo/L"tools/dsh-collaboration"/wide(file)),"Plugin package resource missing: "+std::string(file));
    require(!manifest.has("scripts")&&manifest.at("dsh").at("client").at("platform").str()=="web","Plugin added install scripts or changed target surface");
    std::cout<<"PASS production DSH bridge: early presence, denied writes, claimed/reopened paths and retired grants; Host loading and overlay require DSH acceptance\n";
}
void installed_sdk(const fs::path& repo){
    const auto engine=fs::path(environment(L"LOCALAPPDATA"))/L"Programs/DeepSeek Harness/DeepSeek Harness.exe";
    require(fs::is_regular_file(engine),"Installed DSH SDK unavailable; this explicit manual test requires DeepSeek Harness");
    const auto result=run({engine.wstring(),L"--expose-internals",(repo/L"tests/build_tools/fixtures/dsh_sdk_client.mjs").wstring(),(repo/L"tools/dsh-collaboration/client.js").wstring()},repo,30,{{L"ELECTRON_RUN_AS_NODE",L"1"}});
    require(result.code==0,"Installed DSH SDK integration failed: "+result.out+result.err);
    const auto observed=Json::parse(result.out);
    require(observed.at("success").at("ok").boolean&&Json::parse(observed.at("success").at("value").str()).at("marker").str()=="actual-sdk","Actual Gateway result contract was not preserved");
    require(observed.at("detail").at("ok").boolean,"Actual Gateway did not mount the additive read endpoint");
    require(!observed.at("offline").at("ok").boolean&&observed.at("offline").at("code").str()=="gateway/internal"&&observed.at("withdrawn").boolean,"Actual Gateway failed to fold an offline carrier or withdraw a retained method");
    require(observed.at("calls").items.size()==3&&observed.at("calls").items[0].at("endpoint").str()=="snowCollaboration/snapshot"&&observed.at("calls").items[0].at("args").fields.empty()&&observed.at("calls").items[1].at("endpoint").str()=="snowCollaboration/read"&&observed.at("calls").items[1].at("args").at("route").str()=="/api/batches/"+std::string(32,'a'),"SDK mounted the wrong endpoint or changed the fixed snapshot/named read argument contracts");
    require(observed.at("projections").items.size()==4,"SDK did not exercise normal and strict input projections");
    for(const auto& projection:observed.at("projections").items)require(!projection.has("error")&&projection.at("schema").at("type").str()=="object","Generated Host input codec failed normal or strict projection");
    require(observed.at("cold").at("ok").boolean&&observed.at("cold").at("http").integer()==200,"Cold-start welcome settings.describe RPC failed: "+result.out+result.err);
    require(observed.at("cold").at("requestedPort").integer()>=49152&&observed.at("cold").at("actualPort").integer()==observed.at("cold").at("requestedPort").integer(),"Isolated SDK cold-start ignored its reserved browser-compatible high port");
    require(observed.at("cold").at("configuredRepositoryIsolated").boolean,"Cold-start plugin did not bind its isolated repository configuration");
    const auto& recoveryApi=observed.at("cold").at("recoveryApi");
    require(recoveryApi.at("available").boolean&&recoveryApi.at("coldListIsArray").boolean&&recoveryApi.at("agentsPreserved").boolean,"Installed Session Controller lacks the public cold-list/resolution contract or activated an Agent while listing");
    const auto& gate=observed.at("cold").at("fileGate");
    require(gate.at("readAllowed").boolean&&gate.at("writeDenied").boolean&&gate.at("editDenied").boolean&&
        gate.at("observationPolicyPreserved").boolean&&gate.at("newFileAbsent").boolean&&gate.at("existingUnchanged").boolean,
        "Installed filesystem tools bypassed the collaboration gate or changed files without an owner: "+gate.dump());
    bool locale=false,provider=false;
    for(const auto& name:observed.at("cold").at("names").items){locale|=name.str()=="locale";provider|=name.str()=="llm-deepseek";}
    require(locale&&provider,"Cold Host lost the welcome configuration namespaces");
    std::cout<<"PASS installed DSH "<<observed.at("version").str()<<" Cordis injection, strict Remote mount, Gateway results, filesystem tool gate and disposal (isolated Host, not live UI acceptance)\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){if(argc>2&&std::string(argv[2])=="--installed-sdk")installed_sdk(repo);else run_tests(repo,argc>3&&std::string(argv[2])=="--preferred-port-control"?fs::path(wide(argv[3])):fs::path{});});}
