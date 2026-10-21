window.__ModuleLoader__.load({id:'@local/snowdesktop-collaboration',factory(require){
 const React=require('react'),h=React.createElement,NS='snowdesktop-collaboration',z=require('zod').z;
 const contribution={package:'@local/snowdesktop-collaboration',descriptors:[{
  id:'@local/snowdesktop-collaboration#snowCollaboration/snapshot',namespace:'snowCollaboration',method:'snapshot',service:'snowCollaboration',
  description:'Read the identity-checked local collaboration dashboard snapshot.',invocation:{kind:'direct'},parameters:[],
  result:{mode:'strict',typeSymbol:'@local/snowdesktop-collaboration#snapshot:result',create:()=>z.string(),jsonSchema:{type:'string'}}
 }]};
 const dictionaries={
  zh:{title:'SnowDesktop 协作',collapse:'收起',expand:'展开',position:'位置',drag:'拖动标题栏移动；方向键微调位置',topLeft:'左上',topRight:'右上',bottomLeft:'左下',bottomRight:'右下',connecting:'正在读取协作状态',connected:'数据已连接',disconnected:'连接中断 · 以下为上次数据',noBatch:'暂无构建批次',batch:'当前批次',preparing:'尚未开始构建',waitingFor:'等待编辑完成',checkingFor:'等待检查完成',participants:'本批参与者',tasks:'任务进展',older:'较早登记',olderHint:'最近 5 分钟未更新，仅代表最后登记状态',noProgress:'尚未汇报进展',reported:'报告于',heartbeat:'最近活动',unknown:'未记录',blocker:'阻塞原因',tests:'本批测试',noPlan:'尚未冻结测试计划',planned:'计划待执行',notRequired:'本批无需测试',unreported:'结果待报告',executed:'已执行',passed:'通过',failed:'失败',notRun:'未运行',flaky:'重试后通过',messages:'待确认紧急消息',received:'适配器收到',acknowledged:'智能体确认',emptyMessages:'无待确认紧急消息',emptyTasks:'暂无任务登记',lastResult:'上批结果',snapshot:'数据时间',justNow:'刚刚',seconds:'秒前',minutes:'分钟前',hours:'小时前',days:'天前',investigating:'调查中',editing:'编辑中',waiting:'等待中',awaiting_input:'等待用户',blocked:'有阻塞',validating:'验证中',building:'构建 / 测试中',completed:'任务结束',cancelled:'已取消',checking:'轻量检查中',attention:'检查需处理',finished:'已就绪',passedResult:'通过',failedResult:'失败',interrupted:'已中断',invalidated:'输入已变化',skipped:'无需执行','start-failed':'启动失败','retirement-interrupted':'结果收尾中断'},
  en:{title:'SnowDesktop collaboration',collapse:'Collapse',expand:'Expand',position:'Position',drag:'Drag title to move; arrow keys adjust position',topLeft:'Top left',topRight:'Top right',bottomLeft:'Bottom left',bottomRight:'Bottom right',connecting:'Reading collaboration state',connected:'Data connected',disconnected:'Disconnected · showing last data',noBatch:'No build batch',batch:'Current batch',preparing:'Build has not started',waitingFor:'Waiting for edits',checkingFor:'Waiting for checks',participants:'Batch participants',tasks:'Task progress',older:'Older registrations',olderHint:'No update in the last 5 minutes; last reported state only',noProgress:'No progress reported yet',reported:'Reported',heartbeat:'Last activity',unknown:'Not recorded',blocker:'Blocker',tests:'Batch tests',noPlan:'Test plan is not frozen',planned:'Planned; awaiting execution',notRequired:'No tests required for this batch',unreported:'Awaiting results',executed:'Executed',passed:'Passed',failed:'Failed',notRun:'Not run',flaky:'Passed after retry',messages:'Urgent messages awaiting acknowledgment',received:'Adapter received',acknowledged:'Agent acknowledged',emptyMessages:'No urgent messages awaiting acknowledgment',emptyTasks:'No registered tasks',lastResult:'Previous batch',snapshot:'Data time',justNow:'Just now',seconds:'s ago',minutes:'m ago',hours:'h ago',days:'d ago',investigating:'Investigating',editing:'Editing',waiting:'Waiting',awaiting_input:'Awaiting input',blocked:'Blocked',validating:'Validating',building:'Building / testing',completed:'Completed',cancelled:'Cancelled',checking:'Lightweight checks',attention:'Checks need attention',finished:'Ready',passedResult:'Passed',failedResult:'Failed',interrupted:'Interrupted',invalidated:'Inputs changed',skipped:'Not required','start-failed':'Start failed','retirement-interrupted':'Result cleanup interrupted'}
 };
 const css=`.snow-collab{position:fixed;z-index:10;width:370px;max-width:calc(100vw - 24px);max-height:calc(100vh - 84px);display:flex;flex-direction:column;pointer-events:auto;color:var(--dsw-alias-label-primary);background:var(--dsw-alias-bg-layer-1);border:1px solid var(--dsw-alias-border-l2);border-radius:10px;font-family:inherit;font-size:12px;line-height:1.5;overflow:hidden;box-shadow:0 4px 20px var(--dsw-alias-bg-mask-1)}.snow-collab[data-collapsed=true]{width:300px}.snow-collab header{display:flex;align-items:center;gap:5px;padding:8px;border-bottom:1px solid var(--dsw-alias-border-l1);flex-shrink:0}.snow-collab-handle{flex:1;min-width:0;display:flex;align-items:center;gap:7px;cursor:grab;touch-action:none;user-select:none;padding:3px;border-radius:4px}.snow-collab-handle:active{cursor:grabbing}.snow-collab-grip{color:var(--dsw-alias-label-secondary);font-size:16px}.snow-collab header strong{font-size:12px}.snow-collab button{font:inherit;color:inherit;border:1px solid var(--dsw-alias-border-l2);border-radius:5px;background:var(--dsw-alias-button-ghost-active-fill);padding:4px 7px;cursor:pointer}.snow-collab button:hover{background:var(--dsw-alias-interactive-bg-hover)}.snow-collab button:focus-visible,.snow-collab [tabindex]:focus-visible,.snow-collab summary:focus-visible{outline:2px solid var(--dsw-alias-brand-primary);outline-offset:1px}.snow-collab-positions{display:grid;grid-template-columns:1fr 1fr;gap:5px;padding:8px;border-bottom:1px solid var(--dsw-alias-border-l1);flex-shrink:0}.snow-collab-body{overflow:auto;padding:0 12px 10px;overscroll-behavior:contain;overflow-wrap:anywhere}.snow-collab p{margin:4px 0}.snow-collab small,.snow-collab-time{color:var(--dsw-alias-label-secondary);font-size:11px}.snow-collab h3{font-size:12px;margin:12px 0 5px}.snow-collab-row{padding:8px 0;border-top:1px solid var(--dsw-alias-border-l1)}.snow-collab-row:first-child{border-top:0}.snow-collab-row-title{display:flex;gap:8px;align-items:baseline;justify-content:space-between}.snow-collab-row-title strong{font-size:12px}.snow-collab-phase{font-size:11px;flex-shrink:0;color:var(--dsw-alias-label-secondary)}.snow-collab-facts{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin:6px 0}.snow-collab-facts span{padding:5px 7px;background:var(--dsw-alias-bg-layer-2);border-radius:4px;color:var(--dsw-alias-label-secondary);font-size:11px}.snow-collab-facts b{display:block;color:var(--dsw-alias-label-primary);font-size:16px;font-variant-numeric:tabular-nums}.snow-collab-overview{padding:10px 0 8px;border-bottom:1px solid var(--dsw-alias-border-l1)}.snow-collab-overview>strong{font-size:14px}.snow-collab-status{display:flex;justify-content:space-between;gap:8px;font-size:11px;margin:8px 0 0;color:var(--dsw-alias-label-secondary)}.snow-collab-placeholder{color:var(--dsw-alias-label-secondary)}.snow-collab details{border-top:1px solid var(--dsw-alias-border-l1);margin-top:8px;padding-top:8px}.snow-collab summary{cursor:pointer;color:var(--dsw-alias-label-secondary)}.snow-collab-blocker{border-left:2px solid var(--dsw-alias-border-l2);padding-left:7px}.snow-collab-version{font-size:10px;color:var(--dsw-alias-label-secondary)}`;
 const key='snow-collaboration-view.v1',clamp=value=>Math.max(0,Math.min(1,value));
 function loadView(){
  try{const value=JSON.parse(localStorage.getItem(key));
   if(value?.schema===2&&Number.isFinite(value.x)&&Number.isFinite(value.y))return{collapsed:value.collapsed===true,x:clamp(value.x),y:clamp(value.y)};
   if(value?.schema===1){const [x,y]=[[1,0],[1,1],[0,1],[0,0]][value.corner]||[1,0];return{collapsed:value.collapsed===true,x,y};}
  }catch{}return{collapsed:false,x:1,y:0};
 }
 function Panel({remote,t}){
  const [snapshot,setSnapshot]=React.useState(null),[error,setError]=React.useState(''),[view,setView]=React.useState(loadView),[positions,setPositions]=React.useState(false),[placement,setPlacement]=React.useState({right:12,top:72});
  const panel=React.useRef(null),drag=React.useRef(null);
  const update=next=>setView(current=>{const value={...current,...next};try{localStorage.setItem(key,JSON.stringify({schema:2,...value}));}catch{}return value;});
  React.useEffect(()=>{let alive=true,timer;
   const poll=async()=>{try{const result=await remote.snapshot();if(!result?.ok)throw result?.error||new Error('Snapshot response is unavailable');const data=JSON.parse(result.value);if(alive){setSnapshot(data);setError('');}}catch(reason){if(alive)setError(String(reason.message||reason));}finally{if(alive)timer=setTimeout(poll,3000);}};
   void poll();return()=>{alive=false;clearTimeout(timer);};
  },[remote]);
  // Fractions of available travel keep the handle visible after resize or collapse.
  const bounds=()=>{const rect=panel.current?.getBoundingClientRect();return{width:Math.max(0,window.innerWidth-(rect?.width||370)-24),height:Math.max(0,window.innerHeight-(rect?.height||48)-84)};};
  React.useLayoutEffect(()=>{
   const arrange=()=>{if(!drag.current){const b=bounds();setPlacement({left:12+view.x*b.width,top:72+view.y*b.height});}};
   arrange();window.addEventListener('resize',arrange);
   const observer=typeof ResizeObserver==='function'?new ResizeObserver(arrange):null;if(panel.current)observer?.observe(panel.current);
   return()=>{window.removeEventListener('resize',arrange);observer?.disconnect();};
  },[view.x,view.y]);
  const startDrag=event=>{if(event.button!==0)return;const rect=panel.current.getBoundingClientRect();event.currentTarget.setPointerCapture(event.pointerId);drag.current={id:event.pointerId,dx:event.clientX-rect.left,dy:event.clientY-rect.top,x:view.x,y:view.y};event.preventDefault();};
  const moveDrag=event=>{const active=drag.current;if(!active||active.id!==event.pointerId)return;const b=bounds(),left=Math.max(12,Math.min(12+b.width,event.clientX-active.dx)),top=Math.max(72,Math.min(72+b.height,event.clientY-active.dy));active.x=b.width?(left-12)/b.width:0;active.y=b.height?(top-72)/b.height:0;setPlacement({left,top});};
  const finishDrag=event=>{const active=drag.current;if(!active||active.id!==event.pointerId)return;drag.current=null;update({x:active.x,y:active.y});if(event.currentTarget.hasPointerCapture(event.pointerId))event.currentTarget.releasePointerCapture(event.pointerId);};
  const moveKey=event=>{
   if(event.key==='Enter'||event.key===' '){setPositions(!positions);event.preventDefault();return;}
   if(!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(event.key))return;const b=bounds(),step=event.shiftKey?40:10;
   update({x:clamp(view.x+({'ArrowLeft':-step,'ArrowRight':step}[event.key]||0)/(b.width||1)),y:clamp(view.y+({'ArrowUp':-step,'ArrowDown':step}[event.key]||0)/(b.height||1))});event.preventDefault();event.stopPropagation();
  };
  const date=value=>{const time=new Date(value);return value&&Number.isFinite(time.getTime())?time.toLocaleTimeString():t('unknown');};
  const ago=value=>{const ms=Date.parse(value);if(!Number.isFinite(ms))return t('unknown');const seconds=Math.max(0,Math.floor((Date.now()-ms)/1000));if(seconds<10)return t('justNow');for(const [limit,unit] of [[86400,'days'],[3600,'hours'],[60,'minutes'],[1,'seconds']])if(seconds>=limit)return Math.floor(seconds/limit)+' '+t(unit);};
  const phase=value=>t(value==='passed'?'passedResult':value==='failed'?'failedResult':dictionaries.en[value]?value:'unknown');
  const batch=snapshot?.current,members=batch?.tasks||[],tasks=(snapshot?.taskPresence||[]).filter(task=>!['completed','cancelled'].includes(task.phase)),byId=new Map(tasks.map(task=>[task.id,task]));
  const names=members.map(member=>({...byId.get(member.id),...member,displayName:byId.get(member.id)?.displayName||member.name||member.id}));
  const old=task=>!Number.isFinite(Date.parse(task.lastHeartbeatUtc))||Date.now()-Date.parse(task.lastHeartbeatUtc)>300000;
  const extras=tasks.filter(task=>!members.some(member=>member.id===task.id)).sort((a,b)=>(Date.parse(b.lastProgressUtc)||0)-(Date.parse(a.lastProgressUtc)||0)),recent=extras.filter(task=>!old(task)),older=extras.filter(old);
  const editing=names.filter(member=>member.state==='editing'),checks=names.filter(member=>member.display==='checking'||member.display==='attention');
  const messages=(snapshot?.events||[]).filter(event=>event.importance==='urgent'&&event.agentAcknowledged<event.recipientCount),counts=batch?.coverage?.summary;
  const row=(task,member=false)=>h('article',{className:'snow-collab-row',key:task.id},
   h('div',{className:'snow-collab-row-title'},h('strong',null,task.displayName||task.id),h('span',{className:'snow-collab-phase'},phase(member?task.display:task.phase))),
   h('p',{className:task.progress?'':'snow-collab-placeholder'},task.progress||t('noProgress')),
   task.blocker?h('p',{className:'snow-collab-blocker'},t('blocker')+' · '+task.blocker):null,
   h('small',{title:date(task.lastProgressUtc)},(task.adapter?task.adapter.toUpperCase()+' · ':'')+t('reported')+' '+ago(task.lastProgressUtc)+(old(task)?' · '+t('heartbeat')+' '+ago(task.lastHeartbeatUtc):'')));
  let testView;
  if(!batch)testView=null;
  else if(batch.coverage?.mode==='skipped'||batch.coverage?.mode==='none')testView=h('p',{className:'snow-collab-placeholder'},t('notRequired'));
  else if(!batch.coverage?.mode||batch.coverage.status==='not-recorded')testView=h('p',{className:'snow-collab-placeholder'},t('noPlan'));
  else if(counts?.selected>0)testView=h(React.Fragment,null,
   h('p',null,t('executed')+' '+counts.executed+' / '+counts.selected+(counts.executed===0?' · '+t('planned'):'')),
   h('div',{className:'snow-collab-facts'},...['passed','failed','notRun'].map(name=>h('span',{key:name},t(name),h('b',null,counts[name]??'—')))),
   counts.unreported>0?h('small',null,t('unreported')+' '+counts.unreported):null,counts.flaky>0?h('p',null,t('flaky')+' '+counts.flaky):null);
  else testView=h('p',{className:'snow-collab-placeholder'},t('unknown'));
  return h('section',{ref:panel,className:'snow-collab',style:placement,'data-collapsed':view.collapsed,'aria-label':t('title'),onKeyDown:event=>{if(event.key==='Escape'){setPositions(false);update({collapsed:true});event.stopPropagation();}}},
   h('style',null,css),h('header',null,
    h('div',{className:'snow-collab-handle',role:'button',tabIndex:0,'aria-label':t('drag'),title:t('drag'),onPointerDown:startDrag,onPointerMove:moveDrag,onPointerUp:finishDrag,onPointerCancel:finishDrag,onLostPointerCapture:finishDrag,onKeyDown:moveKey},h('span',{className:'snow-collab-grip','aria-hidden':true},'⠿'),h('strong',null,t('title'))),
    h('button',{type:'button',onClick:()=>setPositions(!positions),'aria-expanded':positions},t('position')),
    h('button',{type:'button',onClick:()=>{setPositions(false);update({collapsed:!view.collapsed});},'aria-expanded':!view.collapsed},t(view.collapsed?'expand':'collapse'))),
   positions?h('div',{className:'snow-collab-positions','aria-label':t('position')},...[[0,0,'topLeft'],[1,0,'topRight'],[0,1,'bottomLeft'],[1,1,'bottomRight']].map(([x,y,name])=>h('button',{key:name,type:'button',onClick:()=>{update({x,y});setPositions(false);}},t(name)))):null,
   view.collapsed?null:h('div',{className:'snow-collab-body'},
    h('div',{className:'snow-collab-overview'},h('small',null,t('batch')+(batch?' · '+batch.id.slice(0,8):'')),h('br'),h('strong',null,batch?phase(batch.phase):t('noBatch')),
     batch?.phase==='editing'?h('p',null,t(editing.length?'waitingFor':checks.length?'checkingFor':'preparing')+(editing.length?' · '+editing.length:'')):null,
     !batch&&snapshot?.history?.length?h('p',{className:'snow-collab-placeholder'},t('lastResult')+' · '+phase(snapshot.history[0].outcome)+' · '+ago(snapshot.history[0].completedUtc)):null,
     h('div',{className:'snow-collab-status','aria-live':'polite'},h('span',{title:error||undefined},t(error?'disconnected':snapshot?'connected':'connecting')),h('time',{title:date(snapshot?.updatedUtc)},t('snapshot')+' '+date(snapshot?.updatedUtc)))),
    names.length?h(React.Fragment,null,h('h3',null,t('participants')+' · '+names.length),...names.map(task=>row(task,true))):null,
    recent.length?h(React.Fragment,null,h('h3',null,t('tasks')+' · '+recent.length),...recent.map(task=>row(task))):null,
    !names.length&&!recent.length&&!older.length?h('p',{className:'snow-collab-placeholder'},t('emptyTasks')):null,
    batch?h(React.Fragment,null,h('h3',null,t('tests')),testView):null,
    h('h3',null,t('messages')+' · '+messages.length),
    ...(messages.length?messages.slice(-10).reverse().map(event=>h('article',{className:'snow-collab-row',key:event.sequence},h('strong',null,'#'+event.sequence+' '+event.sender+' → '+event.target),h('p',null,event.text),h('small',null,t('received')+' '+event.adapterReceived+'/'+event.recipientCount+' · '+t('acknowledged')+' '+event.agentAcknowledged+'/'+event.recipientCount))):[h('p',{key:'no-message',className:'snow-collab-placeholder'},t('emptyMessages'))]),
    older.length?h('details',null,h('summary',null,t('older')+' · '+older.length),h('p',{className:'snow-collab-placeholder'},t('olderHint')),...older.map(task=>row(task))):null,
    h('span',{className:'snow-collab-version'},'0.1.2')
   ));
 }
 return{inject:['slots','locale','remote'],async apply(ctx){
  const unmount=await ctx.remote.$mount(contribution);
  try{
   ctx.effect(()=>ctx.locale.register(NS,dictionaries),'snowdesktop collaboration dictionaries');const t=ctx.locale.bind(NS);
   await ctx.inject(['slots','remote.snowCollaboration'],scope=>{
    scope.slots.inject('shell.overlay',()=>scope.slots.register({name:'shell.overlay',id:'snowdesktop-collaboration.panel',locale:NS,inject:()=>({remote:scope.remote.snowCollaboration,t})},Panel));
   });
  }catch(error){await unmount();throw error;}
  return unmount;
 }};
}});
