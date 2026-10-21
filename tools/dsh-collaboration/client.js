window.__ModuleLoader__.load({id:'@local/snowdesktop-collaboration',factory(require){
 const React=require('react'),h=React.createElement,NS='snowdesktop-collaboration';
 const dictionaries={
  zh:{title:'SnowDesktop 协作',collapse:'收起',expand:'展开',position:'移动位置',connecting:'读取中',connected:'已连接',disconnected:'连接不可用',noBatch:'暂无活动批次',tasks:'任务',blockers:'阻塞',progress:'最近进展',tests:'测试',executed:'已执行',selected:'已选',passed:'通过',failed:'失败',notRun:'未运行',messages:'待确认消息',received:'适配器收到',acknowledged:'智能体确认',persisted:'已保存',emptyTasks:'暂无活动任务',emptyMessages:'暂无未确认的紧急消息',unknown:'未记录',source:'来自本机只读面板',more:'显示全部',less:'只看最近',investigating:'调查中',editing:'编辑中',waiting:'等待中',awaiting_input:'等待用户',blocked:'有阻塞',validating:'验证中',building:'构建 / 测试中',completed:'任务结束',cancelled:'任务取消',checking:'轻量检查中'},
  en:{title:'SnowDesktop collaboration',collapse:'Collapse',expand:'Expand',position:'Move panel',connecting:'Loading',connected:'Connected',disconnected:'Unavailable',noBatch:'No active batch',tasks:'Tasks',blockers:'Blockers',progress:'Latest progress',tests:'Tests',executed:'Executed',selected:'Selected',passed:'Passed',failed:'Failed',notRun:'Not run',messages:'Unacknowledged messages',received:'Adapter received',acknowledged:'Agent acknowledged',persisted:'Saved',emptyTasks:'No active tasks',emptyMessages:'No unacknowledged urgent messages',unknown:'Not recorded',source:'Local read-only dashboard',more:'Show all',less:'Show recent',investigating:'Investigating',editing:'Editing',waiting:'Waiting',awaiting_input:'Awaiting input',blocked:'Blocked',validating:'Validating',building:'Building / testing',completed:'Completed',cancelled:'Cancelled',checking:'Lightweight checks'}
 };
 const css=`.snow-collab{position:fixed;z-index:10;width:350px;max-width:calc(100vw - 24px);max-height:calc(100vh - 100px);pointer-events:auto;color:var(--dsw-alias-label-primary);background:var(--dsw-alias-bg-layer-1);border:1px solid var(--dsw-alias-border-l2);border-radius:12px;font-family:inherit;font-size:13px;line-height:1.5;overflow:hidden;box-shadow:0 4px 20px var(--dsw-alias-bg-mask-1)}.snow-collab header{display:flex;align-items:center;gap:8px;padding:10px 12px;border-bottom:1px solid var(--dsw-alias-border-l1)}.snow-collab header strong{flex:1;font-size:13px}.snow-collab button{font:inherit;color:inherit;border:1px solid var(--dsw-alias-border-l2);border-radius:6px;background:var(--dsw-alias-button-ghost-active-fill);padding:3px 7px;cursor:pointer}.snow-collab button:hover{background:var(--dsw-alias-interactive-bg-hover)}.snow-collab button:focus-visible,.snow-collab:focus-visible{outline:2px solid var(--dsw-alias-brand-primary);outline-offset:2px}.snow-collab-body{max-height:calc(100vh - 155px);overflow:auto;padding:10px 12px;overscroll-behavior:contain;overflow-wrap:anywhere}.snow-collab p{margin:4px 0}.snow-collab small,.snow-collab-time{color:var(--dsw-alias-label-secondary);font-size:11px}.snow-collab h3{font-size:12px;margin:12px 0 5px}.snow-collab-row{padding:7px 0;border-top:1px solid var(--dsw-alias-border-l1)}.snow-collab-row:first-child{border-top:0}.snow-collab-row>div{display:flex;gap:8px;justify-content:space-between}.snow-collab-row span{font-size:11px;flex-shrink:0;color:var(--dsw-alias-label-secondary)}.snow-collab-facts{display:flex;gap:6px;flex-wrap:wrap}.snow-collab-facts span{padding:2px 5px;background:var(--dsw-alias-bg-layer-2);border-radius:4px}.snow-collab-status{display:flex;justify-content:space-between;gap:8px}.snow-collab-placeholder{color:var(--dsw-alias-label-secondary)}@media(prefers-reduced-motion:reduce){.snow-collab{scroll-behavior:auto}}`;
 function loadView(){try{const value=JSON.parse(localStorage.getItem('snow-collaboration-view.v1'));if(value?.schema===1)return{collapsed:value.collapsed===true,corner:[0,1,2,3].includes(value.corner)?value.corner:0};}catch{}return{collapsed:false,corner:0};}
 function Panel({remote,t}){
  const [snapshot,setSnapshot]=React.useState(null),[error,setError]=React.useState(''),[view,setView]=React.useState(loadView),[all,setAll]=React.useState(false);
  React.useEffect(()=>{let alive=true,timer;
   const poll=async()=>{try{const data=JSON.parse(await remote.snapshot());if(alive){setSnapshot(data);setError('');}}catch(reason){if(alive)setError(String(reason.message||reason));}finally{if(alive)timer=setTimeout(poll,3000);}};
   void poll();return()=>{alive=false;clearTimeout(timer);};
  },[remote]);
  const update=next=>setView(current=>{const value={...current,...next};try{localStorage.setItem('snow-collaboration-view.v1',JSON.stringify({schema:1,...value}));}catch{}return value;});
  const date=value=>value?new Date(value).toLocaleTimeString():t('unknown');
  const phase=value=>dictionaries.en[value]?t(value):value||t('unknown');
  const tasks=(snapshot?.taskPresence||[]).filter(task=>!['completed','cancelled'].includes(task.phase));
  const messages=(snapshot?.events||[]).filter(event=>event.importance==='urgent'&&event.agentAcknowledged<event.recipientCount);
  const batch=snapshot?.current,counts=batch?.coverage?.summary;
  const position=[{top:72,right:12},{bottom:16,right:12},{bottom:16,left:12},{top:72,left:12}][view.corner];
  const rows=(all?tasks:tasks.slice(-6)).map(task=>h('article',{className:'snow-collab-row',key:task.id},h('div',null,h('strong',null,task.displayName||task.id),h('span',null,phase(task.phase))),h('p',null,task.progress||t('unknown')),task.blocker?h('p',null,t('blockers')+': '+task.blocker):null,h('small',null,t('progress')+' '+date(task.lastProgressUtc))));
  return h('section',{className:'snow-collab',style:position,'aria-label':t('title'),tabIndex:0,onKeyDown:event=>{if(event.key==='Escape'){update({collapsed:true});event.stopPropagation();}}},
   h('style',null,css),h('header',null,h('strong',null,t('title')),h('button',{type:'button',onClick:()=>update({corner:(view.corner+1)%4}),'aria-label':t('position')},'↗'),h('button',{type:'button',onClick:()=>update({collapsed:!view.collapsed}),'aria-expanded':!view.collapsed},t(view.collapsed?'expand':'collapse'))),
   view.collapsed?null:h('div',{className:'snow-collab-body'},
    h('div',{className:'snow-collab-status'},h('strong',null,batch?phase(batch.phase):t('noBatch')),h('span',{'aria-live':'polite'},t(error?'disconnected':snapshot?'connected':'connecting'))),
    error?h('p',{className:'snow-collab-placeholder',role:'status'},error):null,
    h('small',null,t('source')+' · '+date(snapshot?.updatedUtc)),
    h('h3',null,t('tasks')+' '+tasks.length+' · '+t('blockers')+' '+tasks.filter(task=>task.blocker).length),
    ...(rows.length?rows:[h('p',{key:'empty',className:'snow-collab-placeholder'},t('emptyTasks'))]),
    tasks.length>6?h('button',{type:'button',onClick:()=>setAll(!all)},t(all?'less':'more')):null,
    h('h3',null,t('tests')),counts?h('div',{className:'snow-collab-facts'},...['selected','executed','passed','failed','notRun'].map(key=>h('span',{key},t(key)+' '+counts[key]))):h('p',{className:'snow-collab-placeholder'},t('unknown')),
    h('h3',null,t('messages')+' '+messages.length),
    ...(messages.length?messages.slice(-10).reverse().map(event=>h('article',{className:'snow-collab-row',key:event.sequence},h('strong',null,'#'+event.sequence+' '+event.sender+' → '+event.target),h('p',null,event.text),h('small',null,t('persisted')+' · '+t('received')+' '+event.adapterReceived+'/'+event.recipientCount+' · '+t('acknowledged')+' '+event.agentAcknowledged+'/'+event.recipientCount))):[h('p',{key:'no-message',className:'snow-collab-placeholder'},t('emptyMessages'))])
   ));
 }
 return{inject:['slots','locale','remote.snowCollaboration'],apply(ctx){
  ctx.effect(()=>ctx.locale.register(NS,dictionaries),'snowdesktop collaboration dictionaries');const t=ctx.locale.bind(NS);
  ctx.slots.inject('shell.overlay',()=>ctx.slots.register({name:'shell.overlay',id:'snowdesktop-collaboration.panel',locale:NS,inject:()=>({remote:ctx.remote.snowCollaboration,t})},Panel));
 }};
}});
