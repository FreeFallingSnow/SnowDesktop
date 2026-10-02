// Node 22 built-ins only; a separate headless Edge profile is owned by tests.
const fs=require('fs');
const [port,url,expected,screenshot]=process.argv.slice(2);
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
(async()=>{
 const info=await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
 const ws=new WebSocket(info.webSocketDebuggerUrl),pending=new Map();let next=0;const errors=[];
 ws.addEventListener('message',event=>{const message=JSON.parse(event.data);if(message.id){const call=pending.get(message.id);if(call){pending.delete(message.id);message.error?call.reject(new Error(JSON.stringify(message.error))):call.resolve(message.result);}}if(message.method==='Runtime.exceptionThrown')errors.push(message.params.exceptionDetails.text);});
 await new Promise((resolve,reject)=>{ws.addEventListener('open',resolve);ws.addEventListener('error',reject);});
 function send(method,params={},sessionId){const id=++next;ws.send(JSON.stringify({id,method,params,sessionId}));return new Promise((resolve,reject)=>{pending.set(id,{resolve,reject});setTimeout(()=>{if(pending.has(id)){pending.delete(id);reject(new Error('CDP timeout '+method));}},10000).unref();});}
 const {targetId}=await send('Target.createTarget',{url:'about:blank'}),{sessionId}=await send('Target.attachToTarget',{targetId,flatten:true});
 await send('Runtime.enable',{},sessionId);await send('Page.enable',{},sessionId);
 await send('Emulation.setDeviceMetricsOverride',{width:1440,height:1080,deviceScaleFactor:1,mobile:false},sessionId);
 await send('Page.navigate',{url},sessionId);
 const evaluate=async expression=>(await send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},sessionId)).result.value;
 let state;
 for(let i=0;i<60;i++){state=await evaluate('document.getElementById("connection")?.textContent');if(state==='已连接')break;await delay(150);}
 if(state!=='已连接')throw new Error('Dashboard never connected: '+state);
 if(expected.startsWith('历史')){await evaluate('document.querySelector("#history button")?.click()');await delay(400);}
 let headline=await evaluate('document.getElementById("headline").textContent');
 for(let i=0;i<30&&!headline.includes(expected);i++){await delay(150);headline=await evaluate('document.getElementById("headline").textContent');}
 if(!headline.includes(expected))throw new Error('Expected '+expected+', got '+headline);
 const metrics=await evaluate('JSON.stringify({fixture:!document.getElementById("fixture").hidden,tasks:document.querySelectorAll(".task-row").length,body:document.body.innerText,overflow:document.documentElement.scrollWidth>innerWidth,xss:!!window.dashboardXss})');
 const data=JSON.parse(metrics);const icon=await evaluate('document.querySelector(".app-icon").naturalWidth');if(!icon)throw new Error("Software icon did not load");if(!data.fixture||data.xss||data.overflow)throw new Error('Unsafe or incorrectly labeled fixture DOM '+metrics);
 if(errors.length)throw new Error('Browser errors '+JSON.stringify(errors));
 if(screenshot){const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true},sessionId);fs.writeFileSync(screenshot,Buffer.from(shot.data,'base64'));}
 console.log(JSON.stringify({headline,tasks:data.tasks,fixture:true,jsErrors:errors.length,overflow:data.overflow}));
 await send('Target.closeTarget',{targetId});ws.close();
})().catch(error=>{console.error(error);process.exitCode=1;});
