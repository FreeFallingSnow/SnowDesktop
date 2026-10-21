#include "../build_tool_test_support.h"
#include <iostream>
using namespace build_test;
namespace {
void run_tests(const fs::path& repo) {
    Coordinator c(repo,"dsh-plugin");
    const auto script=c.root/L"exercise.mjs";
    write(script,R"JS(import fs from 'node:fs';
import path from 'node:path';
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
}
console.log(JSON.stringify(result));
)JS");
    auto node=[&](const std::string& mode){
        const auto result=run({L"node",script.wstring(),(repo/L"tools/dsh-collaboration/core.js").wstring(),c.root.wstring(),wide(mode)},c.root,30);
        require(result.code==0,"Node plugin bridge failed: "+result.out+result.err);
        return Json::parse(result.out);
    };
    const auto opened=node("open");
    require(opened.at("current").kind==Json::Kind::Null&&!opened.at("write").at("allowed").boolean&&read(c.root/L"src/a.txt")=="original\n",
            "Investigation blocked a batch or an unregistered write mutated source");
    const auto granted=node("grant");
    const auto id=granted.at("id").str(),batch=granted.at("registration").at("batchId").str();
    require(granted.at("write").at("allowed").boolean&&read(c.root/L"src/a.txt")=="authorized write","Claimed write did not reach the real coordinator gate");
    require(node("outside").at("outside").kind==Json::Kind::Null&&!fs::exists(c.root.parent_path()/(c.root.filename().wstring()+L"-outside.txt")),"Plugin claimed authority outside its configured checkout");
    require(node("bad-command").at("rejected").boolean,"Plugin exposed peer recovery/withdrawal");
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
const events=[],updates=[],componentCleanup=[],timers=new Map();let registration,props,Panel,stateIndex=0,timerId=0,mounted=false,contribution;
let response={ok:true,value:JSON.stringify({marker:'known-good',taskPresence:[],events:[]})};
const remote={snapshot:async()=>{if(!mounted)throw new Error('withdrawn');return await response;}};
const React={createElement:(type,props,...children)=>({type,props,children}),useState:initial=>{const index=stateIndex++;return [typeof initial==='function'?initial():initial,value=>updates.push({index,value})];},useEffect:effect=>componentCleanup.push(effect())};
vm.runInNewContext(fs.readFileSync(process.argv[2],'utf8'),{window:{__ModuleLoader__:{load:value=>{registration=value;}}},localStorage:{getItem:()=>null,setItem(){}},setTimeout:callback=>{const id=++timerId;timers.set(id,callback);return id;},clearTimeout:id=>timers.delete(id),console});
const plugin=registration.factory(name=>{if(name==='react')return React;if(name==='zod')return {z:{string:()=>({})}};throw new Error('Unexpected Client import: '+name);});
function context(rejectSlot=false){const effects=[];const ctx={remote:{$mount:async value=>{contribution=value;events.push('mount');mounted=true;ctx.remote.snowCollaboration=remote;return async()=>{events.push('unmount');mounted=false;delete ctx.remote.snowCollaboration;};}},effect:effect=>effects.push(effect()),locale:{register:()=>()=>{},bind:()=>key=>key},slots:{inject:(name,register)=>{events.push('slot');if(rejectSlot)throw new Error('controlled slot failure');return register();},register:(options,component)=>{props=options.inject();Panel=component;}}};return {ctx,effects};}
const first=context();const unmount=await plugin.apply(first.ctx);Panel(props);
await new Promise(resolve=>setImmediate(resolve));
const firstSnapshot=updates.find(value=>value.index===0)?.value;
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
console.log(JSON.stringify({inject:plugin.inject,events,firstSnapshot,offline,malformed,lastSnapshot:updates.filter(value=>value.index===0).at(-1)?.value,lateUpdated:updates.length!==beforeLate,timers:timers.size,rejected,descriptor:{package:contribution.package,id:descriptor.id,namespace:descriptor.namespace,method:descriptor.method,service:descriptor.service,parameters:descriptor.parameters,resultMode:descriptor.result.mode,resultType:descriptor.result.jsonSchema.type}}));
)JS");
    const auto client=run({L"node",(c.root/L"client.mjs").wstring(),(repo/L"tools/dsh-collaboration/client.js").wstring()},c.root,15);
    require(client.code==0,"Production Client carrier failed: "+client.out+client.err);
    const auto clientObserved=Json::parse(client.out);
    bool rootRemote=false,unavailableRemote=false;
    for(const auto& value:clientObserved.at("inject").items){rootRemote|=value.str()=="remote";unavailableRemote|=value.str()=="remote.snowCollaboration";}
    require(rootRemote&&!unavailableRemote&&clientObserved.at("events").items[0].str()=="mount"&&clientObserved.at("events").items[1].str()=="slot","Client waited on an unmounted namespace or registered its slot before mounting");
    require(clientObserved.at("descriptor").at("id").str()=="@local/snowdesktop-collaboration#snowCollaboration/snapshot"&&clientObserved.at("descriptor").at("parameters").items.empty()&&clientObserved.at("descriptor").at("resultMode").str()=="strict"&&clientObserved.at("descriptor").at("resultType").str()=="string","Client snapshot Remote descriptor changed its read-only contract");
    require(clientObserved.at("firstSnapshot").at("marker").str()=="known-good"&&clientObserved.at("offline").str()=="controlled offline"&&!clientObserved.at("malformed").str().empty()&&clientObserved.at("lastSnapshot").at("marker").str()=="known-good","Client failed RemoteResult decoding or lost the last snapshot after a carrier/data error");
    require(!clientObserved.at("lateUpdated").boolean&&clientObserved.at("timers").integer()==0&&clientObserved.at("rejected").boolean&&clientObserved.at("events").items.back().str()=="unmount","Client updated after disposal, leaked a timer, or retained a failed mount");
    const auto manifest=Json::parse(read(repo/L"tools/dsh-collaboration/package.json"));
    for(const auto& file:{"index.js","core.js","client.js","typert.host.js","typert.remote-client.js","cordis.patch.yml","locale/en.json","locale/zh.json","icon.svg","README.md"})
        require(fs::is_regular_file(repo/L"tools/dsh-collaboration"/wide(file)),"Plugin package resource missing: "+std::string(file));
    require(!manifest.has("scripts")&&manifest.at("dsh").at("client").at("platform").str()=="web","Plugin added install scripts or changed target surface");
    std::cout<<"PASS production DSH bridge: early presence, denied writes, claimed/reopened paths and retired grants; Host loading and overlay require DSH acceptance\n";
}
}
int main(int argc,char** argv){return test_main(argc,argv,run_tests);}
