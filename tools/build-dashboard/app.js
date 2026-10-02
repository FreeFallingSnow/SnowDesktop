"use strict";
const $ = id => document.getElementById(id);
const names = {verified:"完成 · 共享验证通过",exempt:"完成 · 宿主验证豁免","batch-ended":"本批已结束",editing:"编辑中",checking:"轻量检查中",waiting:"已就绪 · 等待",attention:"检查需处理",withdrawn:"已显式退出",building:"构建 / 测试中",passed:"验证通过",failed:"验证失败",invalidated:"输入已改变",interrupted:"运行已中断",cancelled:"已取消",skipped:"豁免宿主验证",none:"无所有者",alive:"执行者存活",exited:"执行者已退出",unknown:"未知",pending:"排队",running:"执行中",notrun:"未运行","not-run":"未运行","not-required":"不要求宿主测试","not-recorded":"尚无记录","retirement-interrupted":"结果已保存 · 待恢复登记",configure:"配置",compile:"编译",link:"链接",test:"测试"};
let selected = null, paused = false, inflight = false;
const text = (id, value) => { $(id).textContent = value == null ? "—" : String(value); };
function element(tag, content, klass){const node=document.createElement(tag);if(content!=null)node.textContent=String(content);if(klass)node.className=klass;return node;}
function label(value){return names[value] || value || "未知";}
function tone(state){return ["passed","waiting","verified"].includes(state)?"good":["failed","invalidated","interrupted","attention"].includes(state)?"bad":["editing","checking","pending","skipped"].includes(state)?"warn":"";}
function detail(parent,title,value,klass=""){const row=element("div",null,"detail "+klass);row.append(element("span",title,"label"),element("div",value));parent.append(row);}
function time(value){if(!value)return "—";const date=new Date(value);return Number.isNaN(date.getTime())?"—":date.toLocaleString();}
function duration(seconds){if(seconds==null)return "时长未知";return seconds<60?seconds+" 秒":seconds<3600?Math.floor(seconds/60)+" 分钟":Math.floor(seconds/3600)+" 小时";}
function renderTask(task){
 const row=element("article",null,"task-row"),top=element("div",null,"task-top");
 top.append(element("span",task.id,"task-name"),element("span",label(task.display),"badge "+tone(task.display)));row.append(top);
 row.append(element("p","修订 r"+task.editRevision+" · 已登记 "+duration(task.ageSeconds)+(task.reopenedUtc?" · 曾重开编辑":""),"task-meta"));
 if(task.stale)detail(row,"陈旧登记","超过 24 小时，仅供诊断；需要核实编辑者并显式恢复。","warn");
 if(task.check.status!=="not-recorded")detail(row,"只读检查 / "+(task.check.source||"来源未知"),label(task.check.status)+(task.check.reason?" · "+task.check.reason:""),tone(task.check.status));
 for(const issue of task.check.issues||[])detail(row,"检查发现",issue,"bad");
 const more=element("details"),summary=element("summary","查看文件声明、测试计划与证据");more.append(summary);
 more.append(element("p","文件："+(task.ownedFiles.join(", ")||"未声明，无法自动判定其他会话的同文件冲突")));
 const plan=task.plan;more.append(element("p","测试计划："+(plan.suites||[]).join(", ")+((plan.tests||[]).length?" / "+plan.tests.join(", "):"")+(plan.requiredFull?" · 要求全量自动测试":"")));
 more.append(element("p","依据："+(plan.reason||"旧协议 / 未声明影响范围")));
 const identity=task.check.inputEnd||task.check.inputStart;if(identity)more.append(element("p","检查输入："+identity.digest));
 if(task.withdrawalReason)more.append(element("p","退出原因："+task.withdrawalReason));row.append(more);return row;
}
function render(snapshot,batch){
 $("fixture").hidden=!snapshot.fixture;text("updated","更新时间 "+time(snapshot.updatedUtc));
 $("history").replaceChildren(...snapshot.history.map(item=>{const row=element("div",null,"history-row"),btn=element("button",item.batchId);btn.type="button";btn.addEventListener("click",()=>{selected=item.batchId;poll();});row.append(btn,element("span",label(item.outcome),"badge "+tone(item.outcome)),element("time",time(item.completedUtc)));return row;}));
 if(!batch){text("headline","暂无活动批次");text("description","开始修改前调用 begin。历史结果可在下方查看。只读查询不会创建编辑登记。");text("batch","—");text("stamp","—");text("taskCount",0);text("readyCount","暂无登记");text("stage","未知");text("stageNote","尚无本批日志");text("testCount","未知");text("coverage","没有冻结计划");text("freshness","未冻结");text("inputNote","尚无输入记录");text("owner","—");text("logHint","暂无日志");text("log","尚无本批日志。");$("tasks").replaceChildren();$("evidence").replaceChildren();return;}
 text("headline",(batch.historical?"历史 · ":"")+label(batch.phase));
 text("description",batch.historical?"此结果仅验证记录的输入快照。当前源码的有效性尚未重新核验。":batch.phase==="editing"?"所有参与者完成编辑与检查后，同批改动共同构建与验证。":"批次成员与测试计划已冻结。新的 begin 等待下一批编辑窗口。");
 text("batch",batch.id);text("stamp","协议 v"+batch.protocolVersion+" · "+time(batch.completedUtc||batch.createdUtc));
 text("taskCount",batch.tasks.length);text("readyCount",batch.tasks.filter(x=>x.display==="waiting").length+" 已就绪 / "+batch.tasks.filter(x=>x.state==="editing").length+" 编辑中");
 const log=batch.log;text("stage",batch.phase==="editing"?"等待就绪":batch.outcome?label(batch.outcome):label(log.stage));text("stageNote",log.linkedTargetsObserved!=null?"观察到 "+log.linkedTargetsObserved+" 个链接输出"+(log.prefixOmitted?"（仅日志尾部）":""):"阶段总量未知");
 text("testCount",batch.phase==="editing"||log.testTotal==null?"未知":log.testCompleted+" / "+log.testTotal);text("coverage",label(batch.coverage.status)+(batch.coverage.mode?" · "+batch.coverage.mode:""));
 text("freshness",batch.historical?"历史快照":batch.inputCheck==="stable"?"批次输入稳定":batch.inputCheck==="changed"?"输入已改变":batch.phase!=="editing"&&batch.inputStart?"已冻结输入":"未冻结");text("inputNote","当前源文件未持续重算哈希");
 text("owner",label(batch.ownerState));$("tasks").replaceChildren(...batch.tasks.map(renderTask));const evidence=$("evidence");evidence.replaceChildren();
 if(batch.error)detail(evidence,"批次错误",batch.error,"bad");if(batch.planError)detail(evidence,"计划阻塞",batch.planError,"bad");
 if(batch.planStatus==="waiting-output-owner")detail(evidence,"输出被占用","正在等待应用释放输出；停止应用与重载 Shell 仅在执行者收到明确授权后发生。","warn");
 if(batch.preflight.status!=="not-observed")detail(evidence,"占用观察 · "+time(batch.preflight.observedUtc),batch.preflight.status+" / "+(batch.preflight.owners.map(x=>x.name+" PID "+x.pid).join(", ")||"未观察到占用者"));
 const source=batch.inputEnd||batch.inputStart;if(source)detail(evidence,"源码输入 SHA-256",source.digest+" / "+source.fileCount+" 文件"+(source.head?" · HEAD "+source.head.slice(0,10):""));
 for(const task of batch.coverage.tasks||[])detail(evidence,"测试覆盖 · "+task.participant,label(task.status)+" / 请求 "+(task.requested||[]).length+" 项"+((task.failed||[]).length?" · 失败："+task.failed.join(", "):""),tone(task.status));
 for(const item of batch.issues||[])detail(evidence,"交接 · "+(item.assignee||"未分配"),item.state+" · "+item.reason,item.state==="open"?"warn":"");
 if(batch.outcome==="failed"&&!batch.issues.length)detail(evidence,"责任待确认","测试覆盖说明哪些请求受影响；缺陷责任尚未分配，请用 issue 记录交接。","warn");
 for(const binary of batch.binaryEvidence.executables||[])detail(evidence,"输出 · "+binary.file,binary.sha256+" · "+binary.sourceAssociation);
 if(!evidence.childNodes.length)detail(evidence,"尚未冻结验证","完成轻量检查后，等待全部参与者关闭编辑阶段。");
 text("logHint",log.bytesTotal!=null?"已读取 "+log.bytesRead+" / "+log.bytesTotal+" 字节"+(log.prefixOmitted?" · 省略早期日志":""):"尚无日志");text("log",log.lines.length?log.lines.join("\n"):"尚无本批日志。");if(!paused)$("log").scrollTop=$("log").scrollHeight;
}
async function request(url){const response=await fetch(url,{cache:"no-store"});if(!response.ok)throw new Error("HTTP "+response.status);return response.json();}
async function poll(){if(inflight)return;inflight=true;try{const data=await request("/api/status");let batch=data.current;if(selected)batch=await request("/api/batches/"+selected);render(data,batch);text("connection","已连接");$("signal").classList.add("online");}catch(error){text("connection","读取失败 · "+error.message);$("signal").classList.remove("online");text("description","状态读取失败，页面保留上次数据；它不能代表新的验证结果。");}finally{inflight=false;}}
$("pause").addEventListener("click",()=>{paused=!paused;text("pause",paused?"恢复滚动":"暂停滚动");});$("current").addEventListener("click",()=>{selected=null;poll();});poll();setInterval(poll,2500);
