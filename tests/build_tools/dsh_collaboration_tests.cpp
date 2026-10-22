#include "../build_tool_test_support.h"
#include <iostream>
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
const ctx={services,logger:{warn(){}},workingDirectory:{get:()=>root},fs:{processPath:target=>target},agents:{list:()=>[]},tools:{register:value=>{tool=value;}},on:(name,handler)=>{hooks.set(name,handler);return()=>hooks.delete(name);},effect:effect=>disposers.push(effect())};
apply(ctx,{repositoryRoot:root,dashboardPort:8765,wakeResidentAgents:false});
let result;
try{
 await hooks.get('tools/pre-execute')({agent,name:'read'},()=>({kind:'allow'}));
 const denied=[];for(const name of ['fs/write-intent','fs/edit-intent']){try{await hooks.get(name)(path.join(root,'src/a.txt'),{agent},()=>({sentinel:'kept'}));denied.push(false);}catch{denied.push(true);}}
 const registration=JSON.parse((await tool.execute({action:'begin'},{agent})).json);await tool.execute({action:'claim',files:'src/a.txt',adoptExistingChanges:true},{agent});
 const kept=[];for(const name of ['fs/write-intent','fs/edit-intent'])kept.push(await hooks.get(name)(path.join(root,'src/a.txt'),{agent},()=>({sentinel:'kept'})));
 const bridge=services.snowCollaboration.bridge;
 await bridge.call('begin','synthetic-peer');
 await tool.execute({action:'plan',scope:'tool',suites:'none',inputs:'src/a.txt',reason:'isolated gate contract fixture; no host test needed'},{agent});
 const ownId=(await import(pathToFileURL(path.join(path.dirname(moduleFile),'core.js')))).taskId(agent.id);
 await promisify(execFile)(bridge.powershell,['-NoProfile','-File',bridge.script,'ready',ownId,'-Batch',registration.batchId,'-Revision',String(registration.editRevision)],{cwd:root,windowsHide:true,env:{...process.env,PSExecutionPolicyPreference:'Bypass'}});
 async function deniedBoth(){const result=[];for(const name of ['fs/write-intent','fs/edit-intent']){try{await hooks.get(name)(path.join(root,'src/a.txt'),{agent},()=>null);result.push(false);}catch{result.push(true);}}return result;}
 const ready=await deniedBoth();
 await bridge.call('begin',ownId);const stale=await deniedBoth();
 await tool.execute({action:'begin'},{agent});const rebound=await hooks.get('fs/edit-intent')(path.join(root,'src/a.txt'),{agent},()=>({sentinel:'fresh'}));
 const output=await tool.execute({action:'status'},{agent});
 await promisify(execFile)(bridge.powershell,['-NoProfile','-File',bridge.script,'event-post','synthetic-peer','-Target',ownId,'-EventKey','fixture-urgent','-EventKind','requirements','-Importance','urgent','-Text','fixture changed requirement'],{cwd:root,windowsHide:true,env:{...process.env,PSExecutionPolicyPreference:'Bypass'}});
 await poll();
 const deliveryDeadline=Date.now()+10000;
 while(notices.length<2&&Date.now()<deliveryDeadline)await new Promise(resolve=>setTimeout(resolve,10));
 // Exercise the real consumer contract: pending inbox rendering and next-step
 // admission both dereference source.kind, including when a notice is replayed.
 const replayed=JSON.parse(JSON.stringify(notices));
 const consumerKinds=replayed.map(message=>message.source.kind);
 result={denied,kept,ready,stale,rebound,notices:replayed,consumerKinds,rendered:tool.output.render({},output),tool:tool.name};
}finally{for(const dispose of disposers.reverse())if(typeof dispose==='function')await dispose();}
console.log(JSON.stringify(result));
)JS");
    const auto host=run({L"node",L"--import",L"./register-loader.mjs",(c.root/L"host.mjs").wstring(),(repo/L"tools/dsh-collaboration/index.js").wstring(),c.root.wstring()},c.root,40);
    require(host.code==0,"Production Host hook fixture failed: "+host.out+host.err);
    const auto observed=Json::parse(host.out);
    require(observed.at("denied").items.size()==2&&observed.at("denied").items[0].boolean&&observed.at("denied").items[1].boolean,"Host write/edit hooks did not deny missing grants");
    require(observed.at("notices").items.size()==2&&observed.at("kept").items[0].at("sentinel").str()=="kept"&&observed.at("kept").items[1].at("sentinel").str()=="kept","Plugin lost registration/event delivery or another policy's write intent");
    for(const auto& notice:observed.at("notices").items){
        require(notice.at("role").str()=="user"&&notice.at("source").at("kind").str()=="snowdesktop-collaboration"&&notice.at("source").at("form").str()=="notice"&&!notice.at("source").at("summary").str().empty()&&notice.at("source").at("summary").str().size()<=120,"Persisted coordination notice has no valid producer-owned source");
    }
    require(observed.at("consumerKinds").items.size()==2,"Replayed notices failed consumer source classification");
    for(const auto& phase:{"ready","stale"})require(observed.at(phase).items[0].boolean&&observed.at(phase).items[1].boolean,"Host gate accepted a ready or superseded cached grant");
    require(observed.at("rebound").at("sentinel").str()=="fresh","Explicit Host reacquisition failed");
    require(observed.at("rendered").items[0].at("type").str()=="text"&&observed.at("tool").str()=="snow_collaboration","Plugin tool output violated the inspected content contract");
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
function renderBudget(model,lang='en'){let index=0;React.useEffect=()=>{};React.useState=initial=>{const value=typeof initial==='function'?initial():initial;return [index++===0?(model||{snapshot:{runs:[run]}}):index===3?{...value,tab:'validation'}:value,()=>{}];};const rendered=Panel({...props,t:key=>dictionaries[lang][key]||key}),values=[];function visit(node){if(typeof node==='string'||typeof node==='number')values.push(String(node));else if(Array.isArray(node))node.forEach(visit);else if(node&&typeof node==='object'&&node.type!=='style')(node.children||[]).forEach(visit);}visit(rendered);return values.join(' ');}
const budgetText=renderBudget();run.resources={status:'unrecorded'};run.admission={policy:'project-weighted-v1'};const missingBudgetText=renderBudget();
const rich={id:'b'.repeat(32),test:'Passed with open dependencies',status:'passed',seconds:0.125,identityStatus:'recorded',reuseAllowed:false,binding:{assertionBinary:'c'.repeat(64)},dependencies:{status:'observed',sourceClosure:'unproven',sourceReports:3,issues:[],issueCount:0},runtimeInputs:{status:'stable',runtimeClosure:'unclosed',issues:[],issueCount:0},runtimeModules:{status:'observed',imageCount:23,processCount:2,runtimeClosure:'unclosed',timingComparableToUnobservedRun:false,issues:[],issueCount:0},attempts:[{attempt:1,status:'failed',seconds:0.2},{attempt:2,status:'passed',seconds:0.125}]};
const legacy={id:'d'.repeat(32),test:'Legacy missing observation',status:'passed',reuseAllowed:false};
const changed={...rich,id:'e'.repeat(32),test:'Changed input observation',identityStatus:'invalidated',runtimeInputs:{status:'changed'},runtimeModules:{status:'incomplete',imageCount:null,processCount:null,issues:['loaded-module-file-changed:fixture.dll'],issueCount:14}};
const evidence={status:'recorded',totalCount:3,tests:[rich,legacy,changed]};run.evidence=evidence;
const runEvidenceText=renderBudget(),evidenceModel={snapshot:{runs:[]},batch:{id:'f'.repeat(32),evidence}};
const batchEvidenceText=renderBudget(evidenceModel),batchEvidenceZh=renderBudget(evidenceModel,'zh');
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
console.log(JSON.stringify({inject:plugin.inject,events,firstSnapshot,offline,malformed,lastSnapshot:updates.filter(value=>value.index===0).at(-1)?.value?.snapshot,lateUpdated:updates.length!==beforeLate,timers:timers.size,rejected,rootVersion,versions,budgetText,missingBudgetText,runEvidenceText,batchEvidenceText,batchEvidenceZh,descriptor:{package:contribution.package,id:descriptor.id,namespace:descriptor.namespace,method:descriptor.method,service:descriptor.service,parameters:descriptor.parameters,resultMode:descriptor.result.mode,resultType:descriptor.result.jsonSchema.type}}));
)JS");
    const auto client=run({L"node",(c.root/L"client.mjs").wstring(),(repo/L"tools/dsh-collaboration/client.js").wstring()},c.root,15);
    require(client.code==0,"Production Client carrier failed: "+client.out+client.err);
    const auto clientObserved=Json::parse(client.out);
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
    require(observed.at("cold").at("configuredRepositoryIsolated").boolean,"Cold-start plugin did not bind its isolated repository configuration");
    bool locale=false,provider=false;
    for(const auto& name:observed.at("cold").at("names").items){locale|=name.str()=="locale";provider|=name.str()=="llm-deepseek";}
    require(locale&&provider,"Cold Host lost the welcome configuration namespaces");
    std::cout<<"PASS installed DSH "<<observed.at("version").str()<<" Cordis injection, strict Remote mount, Gateway results and disposal (carrier only, not UI acceptance)\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,[&](const fs::path& repo){if(argc>2&&std::string(argv[2])=="--installed-sdk")installed_sdk(repo);else run_tests(repo,argc>3&&std::string(argv[2])=="--preferred-port-control"?fs::path(wide(argv[3])):fs::path{});});}
