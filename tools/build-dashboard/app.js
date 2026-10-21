"use strict";
const $ = id => document.getElementById(id);
const names = {verified:"完成 · 共享验证通过",exempt:"完成 · 宿主验证豁免","batch-ended":"本批已结束",editing:"编辑中",checking:"轻量检查中",waiting:"已就绪 · 等待",attention:"检查需处理",withdrawn:"已显式退出",building:"构建 / 测试中",passed:"验证通过",failed:"验证失败",invalidated:"输入已改变",interrupted:"运行已中断",cancelled:"已取消",skipped:"豁免宿主验证",none:"无所有者",alive:"执行者存活",exited:"执行者已退出",unknown:"未知",pending:"排队",running:"执行中",notrun:"未运行","not-run":"未运行","not-required":"不要求宿主测试","not-recorded":"尚无记录","retirement-interrupted":"结果已保存 · 待恢复登记",configure:"配置",compile:"编译",link:"链接",test:"测试"};
Object.assign(names,{eligible:"可恢复操作",completed:"等待结束","timed-out":"等待超时",retrying:"有限重试中","passed-after-retry":"重试后通过 / flaky","exhausted-failed":"重试耗尽","environment-blocked":"环境阻断",window:"编辑窗口",files:"文件归属",peers:"其他编辑者",result:"共同结果"});
// Only view preferences are stored; build state remains read-only server data.
const viewKey = "SnowDesktop.build-monitor.ui.v1";
const view = {paused:false, folds:Object.create(null), logs:Object.create(null), previews:Object.create(null), waits:{open:true,top:0}, filters:{task:"",state:"all",history:"all"}};
try {
 const raw = localStorage.getItem(viewKey);
 const saved = raw && raw.length <= 65536 ? JSON.parse(raw) : null;
 if(saved && saved.schema===1){
  view.paused=saved.paused===true;
  if(saved.filters){for(const key of ["task","state","history"]){if(typeof saved.filters[key]==="string"&&saved.filters[key].length<=200)view.filters[key]=saved.filters[key];}}
  if(saved.waits){view.waits.open=saved.waits.open!==false;view.waits.top=Number.isFinite(saved.waits.top)&&saved.waits.top>=0?saved.waits.top:0;}
  for(const [name,limit] of [["folds",250],["logs",50],["previews",50]]){
   const entries=saved[name];
   if(entries && typeof entries==="object" && !Array.isArray(entries)){
    for(const [key,value] of Object.entries(entries).slice(-limit)){
     if(key.length<=300 && (name==="logs" ? Number.isFinite(value)&&value>=0 : typeof value==="boolean"))view[name][key]=value;
    }
   }
  }
 }
}catch(_){/* Denied or corrupt storage still permits in-memory view state. */}
function saveView(){try{localStorage.setItem(viewKey,JSON.stringify({schema:1,...view}));}catch(_){}}
function remember(name,key,value,limit){delete view[name][key];view[name][key]=value;for(const stale of Object.keys(view[name]).slice(0,-limit))delete view[name][stale];saveView();}
function retainFold(node,key){node.dataset.foldKey=key;node.open=view.folds[key]===true;node.addEventListener("toggle",()=>remember("folds",key,node.open,250));}
function historyBatch(){const id=location.hash.slice(1);return /^[a-f0-9]{32}$/.test(id)?id:null;}
let selected = historyBatch(), paused = view.paused, inflight = false, logView = null;
function selectBatch(id){selected=id;history.replaceState(null,"",location.pathname+location.search+(id?"#"+id:""));poll();}
const previewRequests=new Map();
async function showPreview(url,output){
 if(!previewRequests.has(url))previewRequests.set(url,request(url).then(value=>value.lines.join("\n")));
 try{output.textContent=await previewRequests.get(url);}catch(error){previewRequests.delete(url);output.textContent=error.message;}
}
const text = (id, value) => { $(id).textContent = value == null ? "—" : String(value); };
function element(tag, content, klass){const node=document.createElement(tag);if(content!=null)node.textContent=String(content);if(klass)node.className=klass;return node;}
function label(value){return names[value] || value || "未知";}
function tone(state){return ["passed","waiting","verified"].includes(state)?"good":["failed","invalidated","interrupted","attention"].includes(state)?"bad":["editing","checking","pending","skipped"].includes(state)?"warn":"";}
function detail(parent,title,value,klass=""){const row=element("div",null,"detail "+klass);row.append(element("span",title,"label"),element("div",value));parent.append(row);}
function time(value){if(!value)return "—";const date=new Date(value);return Number.isNaN(date.getTime())?"—":date.toLocaleString();}
function duration(seconds){if(seconds==null||!Number.isFinite(Number(seconds)))return "未记录";seconds=Math.max(0,Math.floor(Number(seconds)));return seconds<60?seconds+" 秒":seconds<3600?Math.floor(seconds/60)+" 分 "+seconds%60+" 秒":Math.floor(seconds/3600)+" 小时 "+Math.floor(seconds%3600/60)+" 分";}
function renderTask(task,batchId,batch){
 const row=element("article",null,"task-row"),top=element("div",null,"task-top");
 row.dataset.taskId=task.id;
 const main=element("div",null,"task-main");
 top.append(element("span",task.name||task.id,"task-name"),element("span",label(task.display),"badge "+tone(task.display)));main.append(top);
 main.append(element("p","r"+task.editRevision+" · 登记 "+duration(task.ageSeconds)+(task.reopenedUtc?" · 已重开":""),"task-meta"));row.append(main);
 const context=element("div",null,"task-context");context.append(element("p",taskSituation(task,batch)));context.append(element("small",task.check.completedUtc?"检查完成 "+time(task.check.completedUtc):task.check.startedUtc?"检查开始 "+time(task.check.startedUtc):"进展时间未记录"));row.append(context);
 const planBox=element("div",null,"task-plan");planBox.append(element("p",planTitle(task.plan)));planBox.append(element("small",task.ownedFiles.length+" 项文件声明"));row.append(planBox);
 const more=element("details"),summary=element("summary","查看文件声明、测试计划与证据");more.append(summary);retainFold(more,JSON.stringify(["task",batchId,task.id]));
 if(task.stale)detail(more,"登记超过 24 小时","需要核实任务是否仍在进行。","warn");
 if(task.check.status!=="not-recorded")detail(more,"检查 · "+(task.check.source||"未记录来源"),label(task.check.status)+(task.check.reason?" · "+task.check.reason:""),tone(task.check.status));
 for(const issue of task.check.issues||[])detail(more,"检查发现",issue,"bad");
 more.append(element("p",((task.fileRole||(task.state==="editing"?"active-editor":"origin-record"))==="active-editor"?"当前编辑声明：":"来源会话记录（可由后续任务接手）：")+(task.ownedFiles.join(", ")||"未声明，无法自动判定其他会话的同文件冲突")));
 const plan=task.plan;more.append(element("p","测试计划："+(plan.suites||[]).join(", ")+((plan.tests||[]).length?" / "+plan.tests.join(", "):"")+(plan.requiredFull?" · 要求全量自动测试":"")));
 more.append(element("p","依据："+(plan.reason||"旧协议 / 未声明影响范围")));
 const identity=task.check.inputEnd||task.check.inputStart;if(identity)more.append(element("p","检查输入："+identity.digest));
 if(task.withdrawalReason)more.append(element("p","退出原因："+task.withdrawalReason));row.append(more);return row;
}
function render(snapshot,batch){
 renderPresence(snapshot);
 text("waitCount",(snapshot.waits||[]).length+" 条 · "+(view.waits.open?"点击收起":"点击展开"));
 $("waits").replaceChildren(...(snapshot.waits||[]).map(wait=>{const box=element("article",null,"task-row");detail(box,wait.participant+" / "+label(wait.condition),label(wait.status),tone(wait.status));detail(box,"票据",wait.id);detail(box,"原因",wait.reason);detail(box,"恢复入口",wait.next);detail(box,"截止",time(wait.deadlineUtc));return box;}));
 $("waits").scrollTop=view.waits.top;
 $("fixture").hidden=!snapshot.fixture;text("updated","更新时间 "+time(snapshot.updatedUtc));
 $("history").replaceChildren(...snapshot.history.map(item=>{const row=element("div",null,"history-row"),btn=element("button",item.batchId+" / 第 "+(item.attempt||1)+" 次");btn.type="button";btn.addEventListener("click",()=>{selectBatch(item.batchId);});row.append(btn,element("span",label(item.outcome),"badge "+tone(item.outcome)),element("time",time(item.completedUtc)));return row;}));
 if(!batch){text("headline","暂无活动批次");text("description","开始修改前调用 begin。历史结果可在下方查看。只读查询不会创建编辑登记。");text("batch","—");text("stamp","—");text("taskCount",0);text("readyCount","暂无登记");text("stage","未知");text("stageNote","尚无本批日志");text("testCount","未知");text("coverage","没有冻结计划");text("freshness","未冻结");text("inputNote","尚无输入记录");text("owner","—");text("logHint","暂无日志");text("log","尚无本批日志。");$("tasks").replaceChildren();$("evidence").replaceChildren();return;}
 text("headline",(batch.historical?"历史 · ":"")+(batch.outcome==="cancelled"&&!batch.buildStartedUtc?"登记已撤回 · 未开始构建":label(batch.phase)));
 text("description",batchDescription(batch));
 text("batch",batch.id);text("stamp","第 "+batch.attempt+" 次 · 逻辑批次 "+batch.logicalBatchId.slice(0,8)+" · "+time(batch.completedUtc||batch.createdUtc));
 text("taskCount",batch.tasks.length);text("readyCount",batch.tasks.filter(x=>x.display==="waiting").length+" 已就绪 / "+batch.tasks.filter(x=>x.state==="editing").length+" 编辑中");
 const log=batch.log;text("stage",batch.phase==="editing"?"等待就绪":batch.outcome?label(batch.outcome):label(log.stage));text("stageNote",log.linkedTargetsObserved!=null?"观察到 "+log.linkedTargetsObserved+" 个链接输出"+(log.prefixOmitted?"（仅日志尾部）":""):"阶段总量未知");
 const counts=batch.coverage.summary; text("testCount",counts?counts.executed+" / "+counts.selected:log.testTotal==null?"未记录":log.testCompleted+" / "+log.testTotal);text("coverage",counts?"已执行 / 已选 · "+label(batch.coverage.status):label(batch.coverage.status));
 text("freshness",batch.historical?"历史快照":batch.inputCheck==="stable"?"批次输入稳定":batch.inputCheck==="changed"?"输入已改变":batch.phase!=="editing"&&batch.inputStart?"已冻结输入":"未冻结");text("inputNote","当前源文件未持续重算哈希");
 text("owner",label(batch.ownerState));$("tasks").replaceChildren(...batch.tasks.map(task=>renderTask(task,batch.id,batch)));const evidence=$("evidence");evidence.replaceChildren();
 if(batch.repairOf){const previous=element("button","查看上次失败 "+batch.repairOf);previous.type="button";previous.addEventListener("click",()=>{selectBatch(batch.repairOf);});evidence.append(previous);detail(evidence,"续修说明","原请求保留；原就绪检查与测试结论不沿用，当前输入重新验证。");}
 if(batch.retry.status!=="not-recorded"){detail(evidence,"自动重试",label(batch.retry.status)+" · 原计划 "+batch.retry.selectedCount+" 项",tone(batch.retry.status));for(const item of batch.retry.tests){const box=element("details");retainFold(box,JSON.stringify(["retry",batch.id,item.name]));box.append(element("summary",item.name+" / "+label(item.status)+" / 失败 "+item.failureCount+" 次"));detail(box,"首次原因",item.firstReason);detail(box,"最终原因",item.finalReason);for(const attempt of item.attempts){detail(box,"尝试 "+attempt.attempt,label(attempt.status)+(attempt.reason?" · "+attempt.reason:""));if(attempt.logUrl){const button=element("button","查看本次日志"),output=element("pre","");button.type="button";button.addEventListener("click",()=>{remember("previews",attempt.logUrl,true,50);showPreview(attempt.logUrl,output);});if(view.previews[attempt.logUrl])showPreview(attempt.logUrl,output);box.append(button,output);}}evidence.append(box);}}
 if(batch.error)detail(evidence,"批次错误",batch.error,"bad");if(batch.planError)detail(evidence,"计划阻塞",batch.planError,"bad");
 if(batch.planStatus==="waiting-output-owner")detail(evidence,"输出被占用","正在等待应用释放输出；停止应用与重载 Shell 仅在执行者收到明确授权后发生。","warn");
 if(batch.preflight.status!=="not-observed")detail(evidence,"占用观察 · "+time(batch.preflight.observedUtc),batch.preflight.status+" / "+(batch.preflight.owners.map(x=>x.name+" PID "+x.pid).join(", ")||"未观察到占用者"));
 const source=batch.inputEnd||batch.inputStart;if(source)detail(evidence,"源码输入 SHA-256",source.digest+" / "+source.fileCount+" 文件"+(source.head?" · HEAD "+source.head.slice(0,10):""));
 for(const task of batch.coverage.tasks||[])detail(evidence,"测试覆盖 · "+task.participant,label(task.status)+" / 请求 "+(task.requested||[]).length+" 项"+((task.failed||[]).length?" · 失败："+task.failed.join(", "):""),tone(task.status));
 for(const item of batch.issues||[])detail(evidence,"交接 · "+(item.assignee||"未分配"),item.state+" · "+item.reason,item.state==="open"?"warn":"");
 if(batch.outcome==="failed"&&!batch.issues.length)detail(evidence,"责任待确认","测试覆盖说明哪些请求受影响；缺陷责任尚未分配，请用 issue 记录交接。","warn");
 for(const binary of batch.binaryEvidence.executables||[])detail(evidence,"输出 · "+binary.file,binary.sha256+" · "+binary.sourceAssociation);
 if(!evidence.childNodes.length)detail(evidence,"尚未冻结验证","完成轻量检查后，等待全部参与者关闭编辑阶段。");
 text("logHint",log.bytesTotal!=null?"已读取 "+log.bytesRead+" / "+log.bytesTotal+" 字节"+(log.prefixOmitted?" · 省略早期日志":""):"尚无日志");const logPosition=paused?(logView===batch.id?$("log").scrollTop:view.logs[batch.id]||0):null;text("log",log.lines.length?log.lines.join("\n"):"尚无本批日志。");logView=batch.id;$("log").scrollTop=paused?logPosition:$("log").scrollHeight;
}
function renderPresence(snapshot){
 const phases={investigating:"调查中",editing:"报告正在编辑",waiting:"等待中",awaiting_input:"等待用户",blocked:"有阻塞",validating:"验证中",completed:"任务结束",cancelled:"任务取消"};
 const tasks=snapshot.taskPresence||[];
 text("presenceCount",tasks.length+" / "+(snapshot.taskPresenceTotal||0)+" 项");
 $("taskPresence").replaceChildren(...tasks.map(task=>{
  const row=element("article",null,"presence-row");row.append(element("strong",task.displayName||task.id),element("span",phases[task.phase]||task.phase,"badge"));
  row.append(element("p",task.progress||"尚未报告进展"));
  if(task.blocker)row.append(element("p","阻塞："+task.blocker,"warn"));
  row.append(element("small",(task.adapter||"未记录宿主")+" · 最近进展 "+time(task.lastProgressUtc)+" · 心跳距今 "+duration(task.heartbeatAgeSeconds)));
  const more=element("details");more.append(element("summary","登记与宿主能力"));retainFold(more,JSON.stringify(["presence",task.id]));
  more.append(element("p",task.id+" · 任务修订 "+task.revision+" · 需求修订 "+task.requirementsRevision+" · 来源 "+task.registrationOrigin));
  more.append(element("p","写入拦截："+(task.capabilities.beforeWriteGate?"已接入":"未接入")+"；空闲唤醒："+(task.capabilities.idleWake?"已接入":"未接入")));
  row.append(more);return row;
 }));
 if(!tasks.length)$("taskPresence").append(element("p","暂无调查登记。调查登记不占编辑权，也不阻塞构建。","empty"));
 const events=snapshot.events||[];text("eventCount",events.length+" / "+(snapshot.eventTotal||0)+" 条");
 $("collaborationEvents").replaceChildren(...[...events].reverse().map(event=>{
  const row=element("article",null,"presence-row");row.append(element("strong",(event.importance==="urgent"?"需处理 · ":"")+event.sender+" → "+event.target),element("p",event.text));
  row.append(element("small","#"+event.sequence+" · "+time(event.createdUtc)+" · 已保存；适配器收到 "+event.adapterReceived+" / "+event.recipientCount+"；智能体确认 "+event.agentAcknowledged+" / "+event.recipientCount));return row;
 }));
 if(!events.length)$("collaborationEvents").append(element("p","暂无协作消息。","empty"));
}
Object.assign(names,{verified:"验证通过",exempt:"无需宿主验证",withdrawn:"已撤回",attention:"检查失败",alive:"执行者在线",none:"未记录执行者",notrun:"未运行",eligible:"等待条件已满足"});
let latestSnapshot=null, latestBatch=null;

function planTitle(plan){
 const suites=plan.suites||[],tests=plan.tests||[];
 if(suites.includes("none"))return "无需宿主验证";
 const groups=suites.filter(x=>x!=="selected").map(x=>({full:"应用全量",tools:"工具回归",core:"核心测试",fast:"快速测试"}[x]||x));
 if(tests.length)groups.push(tests.length+" 项定向测试");
 return groups.join(" + ")||"计划未记录";
}
function taskSituation(task,batch){
 if(task.state==="withdrawn")return task.withdrawalReason||"已撤回本轮登记";
 if(task.display==="attention")return task.check.reason||"检查失败，需要处理";
 if(task.display==="checking")return task.check.status==="pending"?"等待只读检查":"正在执行只读检查";
 if(task.state==="editing")return "正在编辑";
 if(task.display==="verified")return "本批验证通过";
 if(task.display==="exempt")return "本批无需宿主验证";
 if(batch?.outcome)return "本批已结束，查看验证明细";
 if(batch?.planStatus==="waiting-output-owner")return "等待释放构建产物";
 const editors=(batch?.tasks||[]).filter(x=>x.state==="editing");
 if(editors.length)return "等待 "+editors.length+" 项任务结束编辑";
 if(batch?.phase==="editing")return "已就绪，等待同批检查与执行";
 return "已加入本批验证";
}
function batchDescription(batch){
 if(batch.historical)return batch.outcome==="cancelled"&&!batch.buildStartedUtc?"编辑登记已撤回，没有启动构建。":"结果对应本记录的输入和产物。";
 if(batch.planError)return batch.planError;
 if(batch.phase==="editing"){
  const editing=batch.tasks.filter(x=>x.state==="editing").length,checking=batch.tasks.filter(x=>x.display==="checking").length;
  return editing?"等待 "+editing+" 项任务结束编辑"+(checking?"，另有 "+checking+" 项检查未完成":"")+"。":checking?"等待 "+checking+" 项只读检查完成。":"编辑已结束，等待执行条件满足。";
 }
 if(batch.phase==="interrupted")return "执行者已退出，运行需要核实与恢复。";
 return batch.buildStartedUtc?"执行开始于 "+time(batch.buildStartedUtc)+"。":"正在核对执行记录。";
}
function filterTasks(){
 const query=view.filters.task.trim().toLocaleLowerCase(),filter=view.filters.state;
 let visible=0;
 for(const row of $("tasks").children){
  const task=latestBatch?.tasks.find(x=>x.id===row.dataset.taskId);
  if(!task)continue;
  const search=[task.id,task.name,...task.ownedFiles,...(task.plan.tests||[]),task.plan.reason,task.check.reason].join(" ").toLocaleLowerCase();
  const done=["verified","exempt","batch-ended","withdrawn"].includes(task.display);
  const matches=filter==="all"||(filter==="done"?done:task.display===filter);
  row.hidden=!(matches&&search.includes(query));
  if(!row.hidden)visible++;
 }
 text("visibleTasks",visible+" / "+(latestBatch?.tasks.length||0)+" 项");
 $("taskEmpty").hidden=visible>0;
 text("taskEmpty",latestBatch?.tasks.length?"没有匹配的任务。":"没有登记的任务。");
}
function renderAttention(batch){
 const items=[];
 const add=(title,reason,state="warn",taskId=null,target="execution")=>items.push({title,reason,state,taskId,target});
 if(batch){
  if(batch.error)add("执行错误",batch.error,"bad",null,"logs");
  if(batch.planError)add("验证计划被阻塞",batch.planError,"bad");
  if(!batch.historical&&batch.phase==="interrupted")add("执行者已退出","需要核实进程与本轮日志后恢复。","bad");
  if(!batch.historical&&batch.planStatus==="waiting-output-owner")add("构建产物被占用",batch.preflight.owners.map(x=>x.name+" · PID "+x.pid).join("；")||"等待产物释放。");
  const editors=batch.tasks.filter(x=>x.state==="editing");
  if(!batch.historical&&editors.length)add("等待 "+editors.length+" 项任务结束编辑",editors.map(x=>x.name||x.id).join("、"),"",editors[0].id);
  for(const task of batch.tasks){
   if(task.display==="attention")add(task.name||task.id,task.check.reason||task.check.issues.join("；")||"检查失败。","bad",task.id);
   if(!batch.historical&&task.stale)add("登记时间较长",(task.name||task.id)+"：需要核实任务是否仍在进行。","warn",task.id);
  }
  for(const entry of batch.coverage.tasks||[]){if((entry.failed||[]).length)add("相关测试失败 · "+entry.participant,entry.failed.join("、"),"bad",entry.participant);}
  for(const entry of batch.issues||[]){if(entry.state==="open")add("待交接 · "+(entry.assignee||"未分配"),entry.reason,"warn",entry.assignee||entry.reportedBy);}
 }
 const nodes=items.slice(0,60).map(item=>{
  const box=element("article",null,"attention-item "+item.state);
  box.append(element("strong",item.title),element("p",item.reason));
  const button=element("button",item.taskId?"查看任务":"查看详情");button.type="button";button.dataset.focusKey=JSON.stringify(["attention",item.title,item.taskId,item.target]);
  button.addEventListener("click",()=>{
   if(item.taskId){
    view.filters.task="";view.filters.state="all";$("taskSearch").value="";$("taskFilter").value="all";saveView();filterTasks();
    const row=Array.from($("tasks").children).find(x=>x.dataset.taskId===item.taskId);
    if(row){row.querySelector("details").open=true;row.tabIndex=-1;row.focus();return;}
   }
   $(item.target).scrollIntoView({block:"start"});
  });box.append(button);return box;
 });
 if(!nodes.length)nodes.push(element("p",batch?"没有待处理记录。":"暂无活动批次。","empty"));
 $("attention").replaceChildren(...nodes);text("attentionCount",items.length?items.length+" 项":"—");
}
function renderExecution(batch){
 const facts=$("runFacts"),results=$("testResults"),summary=$("testSummary");facts.replaceChildren();results.replaceChildren();summary.replaceChildren();
 if(!batch){facts.append(element("p","执行记录将在批次开始后显示。","empty"));return;}
 const start=batch.buildStartedUtc&&new Date(batch.buildStartedUtc).getTime();
 const end=batch.completedUtc?new Date(batch.completedUtc).getTime():batch.historical?null:Date.now();
 const elapsed=start&&end&&end>=start?duration((end-start)/1000):"未记录";
 for(const [title,value] of [["执行开始",batch.buildStartedUtc?time(batch.buildStartedUtc):"未开始"],[batch.completedUtc?"执行总时长":"执行经过",elapsed],[batch.completedUtc?"执行结束":"最近日志",batch.completedUtc?time(batch.completedUtc):batch.log.modifiedUtc?time(batch.log.modifiedUtc):"未记录"],["阶段来源",batch.outcome?"结果记录":batch.phase==="editing"?"协调登记":"日志观察，未采集分段时长"]]){
  const box=element("div",null,"run-fact");box.append(element("span",title),element("strong",value));facts.append(box);
 }
 const counts=batch.coverage.summary,observed=batch.log.testCompleted;
 const liveLog=!batch.historical&&!batch.outcome&&batch.phase==="building"&&observed!=null&&observed>(counts?.executed||0);
 if(liveLog){
  text("testCount",observed+" / "+(counts?.selected||batch.log.testTotal||"未知"));
  text("coverage",counts?.selected?"日志已完成 / 已选":"日志已完成 / 日志总数");
  summary.append(element("span","日志报告完成 "+observed+" 项"));
 }
 if(!counts||(!counts.selected&&!counts.executed)){
  if(!liveLog)text("testCount",batch.coverage.mode==="skipped"?"无需宿主测试":"未记录");
  summary.append(element("p",batch.coverage.mode==="skipped"?"本批不要求宿主测试。":"测试计划与结果未记录。","muted"));return;
 }
 if(!liveLog)text("testCount",counts.executed+" / "+counts.selected);
 if(liveLog)summary.append(element("span","执行结果已记录 "+counts.executed+" 项"));
 for(const [title,key] of [["已选","selected"],["已执行","executed"],["通过","passed"],["失败","failed"],["未运行","notRun"],["跳过","skipped"],["结果未记录","unreported"]]){
  if(liveLog&&key==="executed")continue;
  if(counts[key]||key==="selected")summary.append(element("span",title+" "+counts[key],key==="failed"&&counts[key]?"failed":""));
 }
 if(counts.flaky)summary.append(element("span","重试后通过 "+counts.flaky));
 const completed=batch.coverage.completed||[];
 if(completed.length){
  const table=element("table",null,"result-table"),head=element("thead"),header=element("tr"),body=element("tbody");
  for(const title of ["测试","结果","耗时"])header.append(element("th",title));head.append(header);table.append(head);
  const order=item=>["failed","exhausted-failed","not-run","environment-blocked"].includes(item.status)?0:1;
  for(const item of [...completed].sort((a,b)=>order(a)-order(b))){const row=element("tr");row.append(element("td",item.name),element("td",label(item.status)),element("td",item.seconds==null?"未记录":item.seconds.toFixed(2)+" 秒"));body.append(row);}
  table.append(body);results.append(table);
  if(batch.coverage.completedTruncated)results.append(element("p","仅显示前 300 项结果。","muted"));
 }
 text("owner",batch.historical?"历史记录":label(batch.ownerState));
}
function filterHistory(){
 const filter=view.filters.history;let visible=0;
 for(const [index,row] of Array.from($("history").children).entries()){
  const item=latestSnapshot?.history[index];if(!item)continue;
  const noBuild=item.outcome==="skipped"||(item.outcome==="cancelled"&&!item.buildStartedUtc);
  const kind=noBuild?"skipped":item.outcome==="passed"?"passed":["failed","invalidated","interrupted","cancelled"].includes(item.outcome)?"failed":"unknown";
  row.hidden=filter!=="all"&&filter!==kind;if(!row.hidden)visible++;
  const names=(item.participants||[]).join("、");
  const button=row.querySelector("button");button.dataset.focusKey="history:"+item.batchId;button.textContent=(names?names+" · ":"")+item.batchId+" · 第 "+(item.attempt||1)+" 次";
  if(item.outcome==="cancelled"&&!item.buildStartedUtc){const badge=row.querySelector(".badge");badge.textContent="登记撤回 · 未构建";badge.className="badge";}
 }
 $("historyEmpty").hidden=visible>0;
}
function filterLog(){
 const lines=latestBatch?.log.lines||[],query=$("logSearch").value.trim().toLocaleLowerCase(),errorsOnly=$("errorsOnly").checked;
 const matches=lines.filter(line=>(!query||line.toLocaleLowerCase().includes(query))&&(!errorsOnly||/error|warning|failed|fatal|exception|错误|失败|警告/i.test(line)));
 const position=$("log").scrollTop;
 text("log",matches.length?matches.join("\n"):query||errorsOnly?"没有匹配的日志行。":"尚无本批日志。");
 text("logMatches",lines.length?"当前尾部 "+matches.length+" / "+lines.length+" 行":"");
 $("log").scrollTop=paused?position:$("log").scrollHeight;
}
function renderWorkbench(snapshot,batch){
 latestSnapshot=snapshot;latestBatch=batch;
 filterTasks();renderAttention(batch);renderExecution(batch);filterHistory();filterLog();
}
function retainFocus(){
 const active=document.activeElement;if(!active||active===document.body)return ()=>{};
 const key=active.dataset.focusKey,fold=active.closest("details"),foldKey=fold?.dataset.foldKey;
 const summary=active.tagName==="SUMMARY",buttonIndex=fold?Array.from(fold.querySelectorAll("button")).indexOf(active):-1;
 return ()=>{
  if(active.isConnected)return;
  let target=key?Array.from(document.querySelectorAll("[data-focus-key]")).find(node=>node.dataset.focusKey===key):null;
  if(!target&&foldKey){const parent=Array.from(document.querySelectorAll("details[data-fold-key]")).find(node=>node.dataset.foldKey===foldKey);target=summary?parent?.querySelector("summary"):buttonIndex>=0?parent?.querySelectorAll("button")[buttonIndex]:null;}
  if(target)target.focus({preventScroll:true});
 };
}
async function request(url){const response=await fetch(url,{cache:"no-store"});if(!response.ok)throw new Error("HTTP "+response.status);return response.json();}
async function poll(){if(inflight)return;inflight=true;const requested=selected;try{const data=await request("/api/status");let batch=data.current;if(requested)batch=await request("/api/batches/"+requested);if(requested!==selected)return;const restoreFocus=retainFocus();render(data,batch);renderWorkbench(data,batch);restoreFocus();text("connection","已连接");$("signal").classList.add("online");}catch(error){text("connection","读取失败 · "+error.message);$("signal").classList.remove("online");text("description","连接中断，当前显示上次读取的数据。");}finally{inflight=false;if(requested!==selected)poll();}}
text("pause",paused?"恢复跟随":"暂停跟随");
$("log").addEventListener("scroll",()=>{if(paused&&logView)remember("logs",logView,$("log").scrollTop,50);});
$("pause").addEventListener("click",()=>{paused=!paused;view.paused=paused;saveView();text("pause",paused?"恢复跟随":"暂停跟随");if(paused&&logView)remember("logs",logView,$("log").scrollTop,50);if(!paused)$("log").scrollTop=$("log").scrollHeight;});
$("current").addEventListener("click",()=>selectBatch(null));
window.addEventListener("hashchange",()=>{selected=historyBatch();poll();});
$("waitPanel").open=view.waits.open;
$("waitPanel").addEventListener("toggle",()=>{view.waits.open=$("waitPanel").open;saveView();text("waitCount",$("waits").children.length+" 条 · "+(view.waits.open?"点击收起":"点击展开"));if(view.waits.open)$("waits").scrollTop=view.waits.top;});
$("waits").addEventListener("scroll",()=>{if(view.waits.open){view.waits.top=$("waits").scrollTop;saveView();}});
$("taskSearch").value=view.filters.task;
$("taskFilter").value=["all","editing","checking","waiting","attention","done"].includes(view.filters.state)?view.filters.state:"all";
view.filters.state=$("taskFilter").value;
$("historyFilter").value=["all","passed","failed","skipped"].includes(view.filters.history)?view.filters.history:"all";
view.filters.history=$("historyFilter").value;
$("taskSearch").addEventListener("input",()=>{view.filters.task=$("taskSearch").value;saveView();filterTasks();});
$("taskFilter").addEventListener("change",()=>{view.filters.state=$("taskFilter").value;saveView();filterTasks();});
$("historyFilter").addEventListener("change",()=>{view.filters.history=$("historyFilter").value;saveView();filterHistory();});
$("logSearch").addEventListener("input",filterLog);$("errorsOnly").addEventListener("change",filterLog);
poll();setInterval(poll,2500);
