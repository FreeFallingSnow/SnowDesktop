// Node 22 built-ins only; a separate headless Edge profile is owned by tests.
const fs=require('fs');
const [port,url,expected,screenshot,mode,fixtureRoot]=process.argv.slice(2);
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
let liveSocket;
(async()=>{
 const info=await (await fetch(`http://127.0.0.1:${port}/json/version`)).json();
 const ws=new WebSocket(info.webSocketDebuggerUrl),pending=new Map();let next=0;const errors=[];
 liveSocket=ws;
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
 if(mode==="--view-state"){
  const inspect=()=>evaluate('JSON.stringify({task:document.querySelector("#tasks details")?.open,retry:document.querySelector("#evidence details")?.open,pause:document.getElementById("pause").textContent,top:document.getElementById("log").scrollTop,preview:document.querySelector("#evidence pre")?.textContent,batch:document.getElementById("batch").textContent,hash:location.hash})').then(JSON.parse);
  const reload=async()=>{await send('Page.reload',{},sessionId);for(let i=0;i<60;i++){await delay(100);if(await evaluate('document.getElementById("connection")?.textContent')==='已连接')return;}throw new Error('Reload never connected');};
  await evaluate('document.querySelector("#tasks details").open=true;document.querySelector("#evidence details").open=true;document.querySelector("#evidence details button").click();document.getElementById("pause").click();document.getElementById("log").scrollTop=90;');
  await delay(3200);
  let saved=await inspect();
  if(!saved.task||!saved.retry||saved.pause!=='恢复跟随'||Math.abs(saved.top-90)>1||!saved.preview.includes('controlled retry log'))throw new Error('Auto refresh lost view state '+JSON.stringify(saved));
  await reload();saved=await inspect();
  // Log preview is refetched once on a new document; its contents are never stored.
  for(let i=0;i<20&&!saved.preview?.includes('controlled retry log');i++){await delay(100);saved=await inspect();}
  if(!saved.task||!saved.retry||saved.pause!=='恢复跟随'||Math.abs(saved.top-90)>1||!saved.preview.includes('controlled retry log'))throw new Error('Reload lost view state '+JSON.stringify(saved));
  await evaluate('document.querySelector("#history button").click()');await delay(500);const previous=await inspect();
  if(!/^#[a-f0-9]{32}$/.test(previous.hash)||previous.hash.slice(1)!==previous.batch)throw new Error('History selection has no valid URL bookmark');
  await reload();const restored=await inspect();if(restored.batch!==previous.batch||restored.hash!==previous.hash)throw new Error('History selection lost on reload');
  await evaluate('document.getElementById("current").click()');await delay(500);saved=await inspect();
  if(saved.hash||!saved.task||!saved.retry)throw new Error('Current batch folds lost when returning from history');
  await evaluate('document.querySelector("#tasks details").open=false;document.querySelector("#evidence details").open=false;document.getElementById("pause").click()');await delay(100);await reload();saved=await inspect();
  if(saved.task||saved.retry||saved.pause!=='暂停跟随')throw new Error('Closed/following preferences not retained');
  await evaluate('localStorage.setItem("SnowDesktop.build-monitor.ui.v1","invalid JSON")');await reload();
  if(await evaluate('document.querySelector("#tasks details").open'))throw new Error('Corrupt storage did not use safe defaults');
  await send('Page.addScriptToEvaluateOnNewDocument',{source:'Object.defineProperty(window,"localStorage",{get(){throw new Error("fixture storage denied")}})'},sessionId);await reload();
  await evaluate('document.querySelector("#tasks details").open=true');await delay(3200);
  if(!await evaluate('document.querySelector("#tasks details").open'))throw new Error('Denied storage lost in-memory fold state');
  console.log('PASS view state: auto-refresh, reload, open/closed folds, paused log/scroll, history bookmark, retry preview, corrupt/denied storage');
 }
 if(mode==="--wait-list"){
  const path=require('path');
  if(!fixtureRoot||!path.basename(fixtureRoot).startsWith('SnowDesktop-dashboard-')||!await evaluate('!document.getElementById("fixture").hidden'))throw new Error('Wait-list writes require a named simulated fixture');
  const dir=path.join(fixtureRoot,'.build','collaboration'),ids=Array.from({length:35},(_,i)=>(i+1).toString(16).padStart(32,'0'));
  const record=id=>({id,participant:'long-wait-'+id,condition:'files',status:'waiting',attempt:1,reason:'long-list fixture',next:'begin / claim',deadlineUtc:'2026-10-03T00:00:00Z'});
  for(const id of ids)fs.writeFileSync(path.join(dir,id+'.wait.json'),JSON.stringify(record(id)));
  const inspect=()=>evaluate('JSON.stringify({open:document.getElementById("waitPanel").open,top:document.getElementById("waits").scrollTop,height:document.getElementById("waits").clientHeight,total:document.getElementById("waits").scrollHeight,card:document.querySelector(".local-waits").getBoundingClientRect().height,count:document.getElementById("waits").children.length,body:document.getElementById("waits").innerText,overflow:document.documentElement.scrollWidth>innerWidth})').then(JSON.parse);
  let value;
  for(let i=0;i<60;i++){await delay(150);value=await inspect();if(value.count>=35)break;}
  if(!value.open||value.count<35||value.height>400||value.total<=value.height)throw new Error('Long wait list must be bounded and scrollable '+JSON.stringify(value));
  await evaluate('document.getElementById("waits").scrollTop=180');await delay(3200);value=await inspect();
  if(Math.abs(value.top-180)>1)throw new Error('Auto refresh reset wait scroll '+JSON.stringify(value));
  fs.writeFileSync(path.join(dir,ids[0]+'.wait.json'),JSON.stringify({...record(ids[0]),reason:'updated wait marker'}));fs.unlinkSync(path.join(dir,ids[1]+'.wait.json'));
  for(let i=0;i<60;i++){await delay(150);value=await inspect();if(value.body.includes('updated wait marker')&&value.count===35)break;}
  if(!value.body.includes('updated wait marker')||value.count!==35||Math.abs(value.top-180)>1)throw new Error('Update/delete lost bounded list state '+JSON.stringify(value));
  await send('Page.reload',{},sessionId);await delay(1200);value=await inspect();
  if(!value.open||Math.abs(value.top-180)>1)throw new Error('Manual refresh reset expanded wait state '+JSON.stringify(value));
  await evaluate('document.getElementById("waitPanel").open=false');await delay(3200);value=await inspect();
  if(value.open||value.card>120||value.body)throw new Error('Collapsed card grew during refresh '+JSON.stringify(value));
  await send('Page.reload',{},sessionId);await delay(1200);value=await inspect();
  if(value.open)throw new Error('Manual refresh lost collapsed card');
  await evaluate('document.getElementById("waitPanel").open=true');await delay(200);value=await inspect();
  if(Math.abs(value.top-180)>1)throw new Error('Reopening lost wait scroll');
  await send('Emulation.setDeviceMetricsOverride',{width:360,height:800,deviceScaleFactor:1,mobile:false},sessionId);await delay(200);value=await inspect();
  if(value.overflow||value.height>320||value.total<=value.height)throw new Error('Narrow window overflow '+JSON.stringify(value));
  console.log('PASS wait list: 36 long rows, bounded internal scroll, update/delete, auto/manual refresh, collapse/reopen, preserved scroll, narrow 360px window');
 }
 const metrics=await evaluate('JSON.stringify({fixture:!document.getElementById("fixture").hidden,tasks:document.querySelectorAll(".task-row").length,body:document.body.innerText,overflow:document.documentElement.scrollWidth>innerWidth,xss:!!window.dashboardXss})');
 const data=JSON.parse(metrics);const icon=await evaluate('document.querySelector(".app-icon").naturalWidth');if(!icon)throw new Error("Software icon did not load");if(!data.fixture||data.xss||data.overflow)throw new Error('Unsafe or incorrectly labeled fixture DOM '+metrics);
 if(errors.length)throw new Error('Browser errors '+JSON.stringify(errors));
 if(screenshot){const shot=await send('Page.captureScreenshot',{format:'png',captureBeyondViewport:true},sessionId);fs.writeFileSync(screenshot,Buffer.from(shot.data,'base64'));}
 console.log(JSON.stringify({headline,tasks:data.tasks,fixture:true,jsErrors:errors.length,overflow:data.overflow}));
 await send('Target.closeTarget',{targetId});ws.close();
})().catch(error=>{console.error(error);if(liveSocket)liveSocket.close();process.exitCode=1;});
