// Observations only; the native C++ manual test owns the expectations.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import vm from 'node:vm';
import {createRequire,registerHooks} from 'node:module';
import {pathToFileURL} from 'node:url';

const base=path.join(path.dirname(process.execPath),'resources/app.asar/dsh/package.json');
const requireSdk=createRequire(base);
const sdk=name=>import(pathToFileURL(requireSdk.resolve(name)));
const {Context,Service}=await sdk('@deepseek-ai/cordis');
const registry=await sdk('@deepseek-ai/dsh-typert-registry/client');
const gateway=await sdk('@deepseek-ai/dsh-api-gateway/client');
const version=JSON.parse(fs.readFileSync(requireSdk.resolve('@deepseek-ai/dsh-api-gateway/package.json'),'utf8')).version;
let declaration,props,connection;
const calls=[];
let carrier;
vm.runInNewContext(fs.readFileSync(process.argv[2],'utf8'),{window:{__ModuleLoader__:{load:value=>{declaration=value;}}},console});
const plugin=declaration.factory(name=>name==='react'?{createElement(){}}:requireSdk(name));
const root=new Context();
class Connection extends Service {
  interactions='none';isLoopback=true;fail=false;
  rpc={open(){throw new Error('unused stream');},call:async(route,endpoint,payload)=>{calls.push({route,endpoint,args:payload.args});if(this.fail)throw new Error('controlled offline');return {ok:true,value:JSON.stringify({marker:'actual-sdk'})};}};
  constructor(ctx){super(ctx,'connection');connection=this;}
}
class Slots extends Service {
  constructor(ctx){super(ctx,'slots');}
  inject(_name,register){return register();}
  register(options,_component){props=options.inject();return()=>{};}
}
class Locale extends Service {
  constructor(ctx){super(ctx,'locale');}
  register(){return()=>{};}
  bind(){return key=>key;}
}
try {
  await root.plugin(registry);await root.plugin(Connection);await root.plugin(gateway);await root.plugin(Slots);await root.plugin(Locale);
  const installed=root.plugin(plugin);await installed;
  const retained=props.remote.snapshot;
  const success=await props.remote.snapshot();
  const detail=await props.remote.read('/api/batches/'+'a'.repeat(32));
  connection.fail=true;
  const offline=await props.remote.snapshot();
  await installed.dispose();
  let withdrawn;
  try{const result=await retained();withdrawn=result.ok===false;}catch{withdrawn=true;}
  carrier={version,success,detail,offline:{ok:offline.ok,code:offline.error?.code},withdrawn,calls};
} finally {await root.fiber.dispose();}

// Real generated-input projections, then the actual cold Host and welcome RPC.
// No live profile, credentials, sessions or UI are used by this protocol fixture.
const addresses=new Map(['zod','@deepseek-ai/dsh-typert-protocol/remote-info'].map(name=>[name,pathToFileURL(requireSdk.resolve(name)).href]));
const hook=registerHooks({resolve(specifier,context,next){return next(addresses.get(specifier)||specifier,context);}});
const projections=[];
try{
 const {TYPERT_REMOTE}=await import(pathToFileURL(path.join(path.dirname(process.argv[2]),'typert.remote-client.js')));
 for(const [name,method] of Object.entries(TYPERT_REMOTE.methods)){
  for(const strict of [false,true]){
   try{const input=method.input({strict});projections.push({name,strict,parsed:input.create().parse(name.endsWith('/read')?{route:'/api/status'}:{}),schema:input.jsonSchema});}
   catch(error){projections.push({name,strict,error:String(error.message||error)});}
  }
 }
}finally{hook.deregister();}
const temporary=fs.mkdtempSync(path.join(os.tmpdir(),'SnowDesktop-dsh-cold-'+process.pid+'-'));
const profile=path.join(temporary,'profile'),repository=path.join(temporary,'repository'),link=path.join(profile,'node_modules/@local/snowdesktop-collaboration');
process.env.DSH_HOME=path.join(temporary,'home');process.env.DSH_AGENTS_HOME=path.join(temporary,'agents');
fs.mkdirSync(path.dirname(link),{recursive:true});fs.symlinkSync(path.dirname(process.argv[2]),link,'junction');
fs.mkdirSync(path.join(repository,'scripts'),{recursive:true});fs.mkdirSync(path.join(repository,'.git'));
fs.writeFileSync(path.join(repository,'scripts/build_manager.ps1'),'# No coordinator operations in this empty startup fixture\n');
fs.writeFileSync(path.join(profile,'package.json'),JSON.stringify({name:'dsh-cold-start-fixture',private:true,dependencies:{'@local/snowdesktop-collaboration':'link:'+path.dirname(process.argv[2])},dsh:{profile:{bundles:['@deepseek-ai/dsh-base','@deepseek-ai/dsh-web-app','@local/snowdesktop-collaboration']}}}));
fs.writeFileSync(path.join(profile,'cordis.yml'),'[]\n');
fs.writeFileSync(path.join(profile,'cordis.patch.yml'),JSON.stringify([{id:'snowdesktop-collaboration',config:{repositoryRoot:repository,dashboardPort:18765,wakeResidentAgents:false}},{id:'desktop-product-telemetry',disabled:true},{id:'product-analytics',disabled:true}]));
let app,cold;
const originalWrite=process.stdout.write;
const bootOutput=[];
process.stdout.write=function(chunk,encoding,callback){const text=String(chunk).replace(/([?&]token=)[^\s]+/g,'$1[redacted]');bootOutput.push(text.slice(-8192));if(bootOutput.length>20)bootOutput.shift();const done=typeof encoding==='function'?encoding:callback;done?.();return true;};
try{
 const {loadProfileDirectory,loadLayeredEnv}=await sdk('@deepseek-ai/dsh-app-boot');
 const {runProfile}=await sdk('@deepseek-ai/dsh/profile-boot');
 const installAnchor=requireSdk.resolve('@deepseek-ai/dsh/package.json');
 app=await runProfile({environment:loadLayeredEnv('dsh',profile),profile:'desktop',resolvedProfile:{profile:loadProfileDirectory('dsh',profile,installAnchor),installAnchor},patchFiles:[],args:['--no-open','--host','127.0.0.1','--port','0']});
 const address=app.ctx.connection.authenticatedUrl(`${app.ctx.webServer.protocol}//127.0.0.1:${app.ctx.webServer.port}`);
 const authenticated=await fetch(address,{redirect:'manual'});await authenticated.body?.cancel();
 const cookie=authenticated.headers.getSetCookie().map(value=>value.split(';')[0]).join('; ');
 const method='settings/describe',rpcId='cold-start-welcome';
 const response=await fetch(new URL('/api/'+method,address),{method:'POST',headers:{'Content-Type':'application/json',Cookie:cookie},body:JSON.stringify({type:'client-request',rpcId,method,payload:{args:{}}})});
 const envelope=await response.json();
 cold={http:response.status,ok:envelope.result?.ok,error:envelope.result?.error,names:(envelope.result?.value?.namespaces||[]).map(item=>item.ns),configuredRepositoryIsolated:app.ctx.get('snowCollaboration')?.bridge.root===repository};
}catch(error){cold={ok:false,error:String(error.stack||error),bootOutput};}
finally{
 try{if(app)await app.shutdown.shutdown(0);}finally{
  process.stdout.write=originalWrite;
  // Remove the junction itself before recursively cleaning only the verified owned root.
  fs.unlinkSync(link);
  const relative=path.relative(os.tmpdir(),temporary);
  if(!relative.startsWith('..')&&!path.isAbsolute(relative)&&path.basename(temporary).startsWith('SnowDesktop-dsh-cold-'+process.pid+'-'))fs.rmSync(temporary,{recursive:true,force:true});
 }
}
console.log(JSON.stringify({...carrier,projections,cold}));
