// Observations only; the native C++ manual test owns the expectations.
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import {createRequire} from 'node:module';
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
  connection.fail=true;
  const offline=await props.remote.snapshot();
  await installed.dispose();
  let withdrawn;
  try{const result=await retained();withdrawn=result.ok===false;}catch{withdrawn=true;}
  console.log(JSON.stringify({version,success,offline:{ok:offline.ok,code:offline.error?.code},withdrawn,calls}));
} finally {await root.fiber.dispose();}
