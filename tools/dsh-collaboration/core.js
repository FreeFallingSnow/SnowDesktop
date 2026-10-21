import {execFile} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {promisify} from 'node:util';

const execFileAsync=promisify(execFile);
const allowedCommands=new Set(['task-open','task-stage','task-list','event-read','event-wait','event-ack','begin','claim','edit-verify','plan','status']);
export function taskId(agentId){return 'dsh-'+createHash('sha256').update(agentId).digest('hex').slice(0,24);}
export function inside(root,target){const rel=path.relative(root,target);return rel===''||(!rel.startsWith('..'+path.sep)&&rel!=='..'&&!path.isAbsolute(rel));}
export function canonicalTarget(file){
 let cursor=path.resolve(file),tail=[];
 while(!fs.existsSync(cursor)){
  const parent=path.dirname(cursor);if(parent===cursor)throw new Error('No existing path ancestor');
  tail.unshift(path.basename(cursor));cursor=parent;
 }
 return path.join(fs.realpathSync.native(cursor),...tail);
}
export function stateAt(root){
 const file=path.join(root,'.build/collaboration/state.json');
 if(!fs.existsSync(file))return {schemaVersion:1,repositoryRoot:root,current:null};
 const bytes=fs.readFileSync(file);if(bytes.length>6*1024*1024)throw new Error('Coordinator metadata exceeds byte budget');
 const state=JSON.parse(bytes.toString('utf8').replace(/^\uFEFF/,''));
 if(state.schemaVersion!==1||path.resolve(state.repositoryRoot).toLowerCase()!==root.toLowerCase())throw new Error('Coordinator root/version mismatch');
 return state;
}
export function grantFor(state,id,files){
 const batch=state.current,entry=batch?.participants?.find(x=>x.id===id);
 if(!batch||batch.phase!=='editing'||!entry||entry.state!=='editing')throw new Error('Use snow_collaboration begin and claim before writing; no active editing authority.');
 for(const file of files){
  if(!entry.ownedFiles?.some(owned=>file.toLowerCase()===owned.toLowerCase()||file.toLowerCase().startsWith(owned.toLowerCase().replace(/\/$/,'')+'/')))throw new Error('Unclaimed write path: '+file);
 }
 return {batchId:batch.id,revision:entry.editRevision??0};
}
export function powershellPath(){
 const candidates=[process.env.SNOWDESKTOP_ENTRY_POWERSHELL,path.join(process.env.ProgramFiles||'C:/Program Files','PowerShell/7/pwsh.exe'),path.join(process.env.SystemRoot||'C:/Windows','System32/WindowsPowerShell/v1.0/powershell.exe')];
 const found=candidates.find(file=>file&&path.isAbsolute(file)&&/^(pwsh|powershell)\.exe$/i.test(path.basename(file))&&fs.existsSync(file));
 if(!found)throw new Error('A supported PowerShell runtime is required');return found;
}
export class CollaborationBridge{
 constructor(root,{port=8765,powershell=powershellPath()}={}){
  this.root=fs.realpathSync.native(root);this.port=port;this.powershell=powershell;this.grants=new Map();
  if(!Number.isInteger(port)||port<1024||port>65535)throw new Error('Invalid dashboard port');
  this.script=path.join(this.root,'scripts/build_manager.ps1');
  if(!fs.existsSync(this.script)||!fs.existsSync(path.join(this.root,'.git')))throw new Error('Not a SnowDesktop coordinator checkout');
 }
 state(){return stateAt(this.root);}
 grant(id,files=[]){
  const current=grantFor(this.state(),id,files),accepted=this.grants.get(id);
  if(!accepted)throw new Error('This plugin has not acquired a grant; call begin before claim or write.');
  if(current.batchId!==accepted.batchId||current.revision!==accepted.revision){this.grants.delete(id);throw new Error('Edit grant was superseded; re-evaluate and explicitly begin again.');}
  return accepted;
 }
 async call(command,id,args=[],{timeout=15000}={}){
  if(!allowedCommands.has(command))throw new Error('Unsupported plugin coordinator operation');
  if(id&&!/^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$/.test(id))throw new Error('Invalid task ID');
  const argv=['-NoProfile','-File',this.script,command,...(id?[id]:[]),...args];
  try{
   const result=await execFileAsync(this.powershell,argv,{cwd:this.root,windowsHide:true,shell:false,timeout,maxBuffer:8*1024*1024,env:{...process.env,PSExecutionPolicyPreference:'Bypass'}});
   return JSON.parse(result.stdout.replace(/^\uFEFF/,''));
  }catch(error){
   if(error.code===6&&error.stdout){return JSON.parse(error.stdout.replace(/^\uFEFF/,''));}
   throw new Error(String(error.stderr||error.message).trim());
  }
 }
 async open(agentId){return this.call('task-open',taskId(agentId),['-TaskName','DSH '+agentId.slice(0,12),'-Adapter','dsh']);}
 async verifyWrite(agentId,file){
  const target=canonicalTarget(file);if(!inside(this.root,target))return null;
  const relative=path.relative(this.root,target).replaceAll('\\','/');
  if(!relative)throw new Error('Writing the repository root is not supported');
  const id=taskId(agentId),grant=this.grant(id,[relative]);
  return this.call('edit-verify',id,['-Batch',grant.batchId,'-Revision',String(grant.revision),'-Files',relative]);
 }
 async progress(agentId,text){
  const task=this.state().taskLedger?.tasks?.find(x=>x.id===taskId(agentId));if(!task||['completed','cancelled'].includes(task.phase))return;
  return this.call('task-stage',task.id,['-TaskRevision',String(task.revision),'-TaskPhase',task.phase,'-Progress',text.slice(0,2000)]);
 }
 async snapshot(){
  const receiptFile=path.join(this.root,'.build/dashboard/server-'+this.port+'.json');
  const receipt=JSON.parse(fs.readFileSync(receiptFile,'utf8').replace(/^\uFEFF/,''));
  const base='http://127.0.0.1:'+this.port;
  const read=async route=>{
   const response=await fetch(base+route,{signal:AbortSignal.timeout(3000),redirect:'error'});
   if(!response.ok)throw new Error('Dashboard HTTP '+response.status);
   const bytes=await response.text();if(bytes.length>8*1024*1024)throw new Error('Dashboard response exceeds budget');return JSON.parse(bytes);
  };
  const health=await read('/api/health');
  if(health.service!=='SnowDesktop build dashboard'||health.instance!==receipt.instance||health.fixture!==false||receipt.fixture!==false)throw new Error('Dashboard listener identity did not match this checkout');
  return read('/api/status');
 }
 async operate(agentId,args){
  const id=taskId(agentId),action=args.action;
  if(action==='status')return this.call('status');
  if(action==='open')return this.open(agentId);
  if(action==='progress')return this.progress(agentId,args.text||'');
  if(action==='begin'){
   const registered=await this.call('begin',id,['-WaitSeconds','1']);
   this.grants.set(id,{batchId:registered.batchId,revision:registered.editRevision});return registered;
  }
  if(action==='messages')return this.call('event-read',id,['-AfterSequence',String(args.afterSequence||0)]);
  if(action==='wait-messages')return this.call('event-wait',id,['-AfterSequence',String(args.afterSequence||0),'-WaitSeconds','30'],{timeout:35000});
  if(action==='acknowledge')return this.call('event-ack',id,['-Sequence',String(args.sequence),'-AckLevel','agent']);
  const grant=this.grant(id),bound=['-Batch',grant.batchId,'-Revision',String(grant.revision)];
  if(action==='claim'){
   if(!args.files)throw new Error('Explicit files are required');
   return this.call('claim',id,[...bound,'-Files',args.files,...(args.adoptExistingChanges?['-AdoptExistingChanges']:[])]);
  }
  if(action==='plan'){
   if(!args.inputs||!args.reason)throw new Error('Inputs and reviewed test mapping are required');
   return this.call('plan',id,[...bound,'-Scope',args.scope||'unknown','-Suites',args.suites||'selected','-Tests',args.tests||'','-Inputs',args.inputs,'-Reason',args.reason]);
  }
  throw new Error('Unknown plugin action');
 }
}
