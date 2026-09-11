local logic=module.require("modules/countdown.lua")
local function copyText()
    return {
        name=l10n.tr("countdown.name"),empty=l10n.tr("countdown.empty"),deleted=l10n.tr("countdown.deleted"),
        permission=l10n.tr("countdown.permission"),unavailable=l10n.tr("countdown.unavailable"),
        choose=l10n.tr("countdown.choose"),create=l10n.tr("countdown.create"),unlink=l10n.tr("countdown.unlink"),
        manage=l10n.tr("countdown.manage"),title=l10n.tr("countdown.title"),date=l10n.tr("countdown.date"),
        pick=l10n.tr("countdown.pick"),filter=l10n.tr("countdown.filter"),previous=l10n.tr("countdown.previous"),
        next=l10n.tr("countdown.next"),no_events=l10n.tr("countdown.no_events"),truncated=l10n.tr("countdown.truncated"),
        write_permission=l10n.tr("countdown.write_permission"),save_failed=l10n.tr("countdown.save_failed"),
        invalid_title=l10n.tr("countdown.invalid_title"),invalid_date=l10n.tr("countdown.invalid_date"),
        saving=l10n.tr("countdown.saving"),today=l10n.tr("countdown.today"),remaining=l10n.tr("countdown.remaining"),
        elapsed=l10n.tr("countdown.elapsed"),days=l10n.tr("countdown.days"),back=l10n.tr("countdown.back"),
    }
end
local function today()
    local p=time.parts(time.now(),"local")
    return string.format("%04d-%02d-%02d",p.year,p.month,p.day)
end
local function text(key,s,r,secondary)
    return view.text({key=key,text=s,width="fill",height=r,fontSize=r*0.46,
        verticalAlign="center",overflowText="ellipsis",style={foreground=secondary and "textSecondary" or "textPrimary"}})
end
local function button(key,s,r,enabled)
    return view.button({key=key,text=s,height=r,width="fill",fontSize=r*0.46,
        action={id=key},enabled=enabled~=false,accessibility={label=s}})
end
local function browse(m,date)
    if m.browse then m.browse:unsubscribe() end
    m.first,m.last=logic.monthRange(date)
    m.browse=data.subscribe("calendar.events",{fromDate=m.first,toDate=m.last,whenHidden="pause",maxAgeMs=86400000})
end
local function open(m,mode)
    m.mode=mode or "manage";m.error=nil
    if m.mode=="choose" then browse(m,m.first or today()) end
    widget.openPanel({title=copyText().manage,width=520,height=640})
end
local function setup()
    local m={mode="manage",title="",date=today(),filter="",today=today(),alive=true}
    m.link=logic.new({getId=function() return storage.get("eventId") end,
        setId=function(id) if id=="" then storage.remove("eventId") else storage.set("eventId",id) end end,
        canRead=function() return widget.hasPermission("calendar.read") end,
        canWrite=function() return widget.hasPermission("calendar.write") end,
        subscribe=function(options) return data.subscribe("calendar.events",options) end,
        start=function(args) return task.start("calendar.create",args) end,cancel=function(id) task.cancel(id) end})
    schedule.every("countdown.day",30000,{whenHidden="pause"})
    return m
end
local function desktop(context,m)
    local c=copyText();local w,h=layout.contentWidth(),layout.contentHeight()
    local unit=math.min(w,h);local pad=unit*0.07
    local status,item=m.link:current();local children={}
    if status=="ready" then
        local delta=logic.difference(item.date,m.today)
        if not delta then status="unavailable" else
            local title=text("event.title",item.title,unit*0.14,false)
            title.fontSize=unit*0.09;title.textAlign="center"
            local value=delta==0 and c.today or tostring(math.abs(delta))
            local number=text("event.days",value,unit*0.38,false)
            number.fontSize=delta==0 and unit*0.16 or math.min(unit*0.34,(w-pad*2)/math.max(1,#value)*1.3)
            number.bold=true;number.textAlign="center";number.overflowText="clip"
            local state=text("event.status",delta==0 and "" or ((delta>0 and c.remaining or c.elapsed).." · "..c.days),unit*0.09,true)
            state.fontSize=unit*0.07;state.textAlign="center"
            local date=text("event.date",item.date,unit*0.10,true);date.fontSize=unit*0.07;date.textAlign="center"
            children={title,number,state,date}
        end
    end
    if status~="ready" then
        local hint=text("status",c[status],unit*0.30,true)
        hint.textWrap="wrap";hint.maxLines=3;hint.textAlign="center";hint.fontSize=unit*0.085
        children={hint,button("choose",c.choose,unit*0.18,widget.hasPermission("calendar.read")),
            button("create",c.create,unit*0.18,widget.hasPermission("calendar.write"))}
    end
    return view.column({key="countdown.card",width="fill",height="fill",padding=pad,gap=unit*0.015,
        justifyContent="center",children=children,events=status=="ready" and {click={id="manage"}} or nil,
        accessibility={role="group",label=c.manage}})
end
local function panel(context,m)
    local c=copyText();local r=ui.metrics().layoutRowHeight;local children={}
    if m.picker then children={m.picker:view({rowHeight=r}),button("picker.back",c.back,r)}
    elseif m.mode=="create" then
        local valid=m.title:match("%S")~=nil and #m.title<=512;local dateValid=logic.parts(m.date)~=nil
        children={text("label.title",c.title,r),view.textInput({key="input.title",value=m.title,
            height=r,fontSize=r*0.46,maxBytes=512,action={id="title"},validationState=valid and "none" or "error",
            validationMessage=valid and "" or c.invalid_title,enabled=not m.link.pending,accessibility={label=c.title}})}
        if not valid then children[#children+1]=text("error.title",c.invalid_title,r,true) end
        children[#children+1]=text("label.date",c.date,r)
        children[#children+1]=view.textInput({key="input.date",value=m.date,height=r,fontSize=r*0.46,maxBytes=10,
            action={id="date"},validationState=dateValid and "none" or "error",validationMessage=dateValid and "" or c.invalid_date,
            enabled=not m.link.pending,accessibility={label=c.date}})
        if not dateValid then children[#children+1]=text("error.date",c.invalid_date,r,true) end
        children[#children+1]=button("pick.create",c.pick,r,not m.link.pending)
        if not widget.hasPermission("calendar.write") then children[#children+1]=text("write.error",c.write_permission,r,true) end
        if m.error then children[#children+1]=text("save.error",c[m.error] or c.save_failed,r,true) end
        children[#children+1]=button("save",m.link.pending and c.saving or c.create,r,
            valid and dateValid and not m.link.pending and widget.hasPermission("calendar.write"))
        children[#children+1]=button("manage",c.back,r,not m.link.pending)
    elseif m.mode=="choose" then
        children={view.row({key="month.nav",height=r,gap=r*0.25,children={button("previous",c.previous,r),
            button("pick.month",(m.first or today()):sub(1,7),r),button("next",c.next,r)}}),
            view.searchBox({key="filter",value=m.filter,height=r,fontSize=r*0.46,maxBytes=512,
                placeholder=c.filter,action={id="filter"},accessibility={label=c.filter}})}
        local s=m.browse and m.browse:value()
        if not widget.hasPermission("calendar.read") then children[#children+1]=text("permission",c.permission,r,true)
        elseif not s or not s.available or not s.value then children[#children+1]=text("unavailable",c.unavailable,r,true)
        else
            local count=0
            for _,item in ipairs(s.value.events or {}) do
                if m.filter=="" or item.title:lower():find(m.filter:lower(),1,true) then
                    count=count+1
                    if count<=40 then children[#children+1]=button("bind:"..item.id,item.date.."  "..item.title,r) end
                end
            end
            if count==0 then children[#children+1]=text("no.events",c.no_events,r,true) end
            if s.value.truncated or count>40 then children[#children+1]=text("truncated",c.truncated,r,true) end
        end
        children[#children+1]=button("manage",c.back,r)
    else
        local status,item=m.link:current()
        children={text("linked.title",item and item.title or c[status],r),text("linked.date",item and item.date or "",r,true),
            button("choose",c.choose,r,widget.hasPermission("calendar.read")),button("create",c.create,r,widget.hasPermission("calendar.write")),
            button("unlink",c.unlink,r,m.link.id~="")}
        if not widget.hasPermission("calendar.write") then children[#children+1]=text("write.error",c.write_permission,r,true) end
    end
    return view.scroll({key="countdown.panel.scroll",width="fill",height="fill",children={
        view.column({key="countdown.panel",width="fill",height="auto",padding=r*0.65,gap=r*0.3,children=children})}})
end
local function event(context,m,e)
    if not m.alive then return end
    if m.picker then
        local result=m.picker:handle(e)
        if result then
            if result.changed then
                if m.pickerTarget=="month" then browse(m,result.value) else m.date=result.value end
                m.picker=nil
            end
            widget.invalidate();return
        end
    end
    if e.kind=="task.complete" then
        local ok,err=m.link:complete(e)
        if ok then m.mode="manage";m.error=nil elseif err then m.error=err end
    elseif e.kind=="schedule" or e.kind=="environment" or e.kind=="visibility" then
        m.today=today()
        if e.kind=="visibility" and e.visible then m.link:refresh() end
    elseif e.kind=="panel" and e.action=="closed" then
        m.picker=nil
        if m.browse then m.browse:unsubscribe();m.browse=nil end
    elseif e.kind=="action" then
        local id=e.id
        if m.link.pending then return end
        if id=="manage" or id=="choose" or id=="create" then open(m,id)
        elseif id=="unlink" then m.link:bind("")
        elseif id=="title" then m.title=e.text or m.title
        elseif id=="date" then m.date=e.text or m.date
        elseif id=="filter" then m.filter=e.text or m.filter
        elseif id=="picker.back" then m.picker=nil
        elseif id=="pick.create" or id=="pick.month" then
            m.pickerTarget=id=="pick.month" and "month" or "create"
            m.picker=ui.datePicker({key="countdown.date",value=m.pickerTarget=="month" and m.first or m.date,todayDate=today(),allowClear=false})
        elseif id=="previous" or id=="next" then
            local y,month=logic.parts(m.first);local n=(y-1)*12+month-1+(id=="next" and 1 or -1)
            if n>=0 and n<9999*12 then browse(m,string.format("%04d-%02d-01",math.floor(n/12)+1,n%12+1)) end
        elseif id=="save" then local ok,err=m.link:create(m.title,m.date);m.error=not ok and err or nil
        elseif type(id)=="string" and id:sub(1,5)=="bind:" and widget.hasPermission("calendar.read") then
            local target=id:sub(6);local s=m.browse and m.browse:value()
            if s and s.available and s.value then
                for _,item in ipairs(s.value.events or {}) do
                    if item.id==target then m.link:bind(target);m.mode="manage";break end
                end
            end
        end
    end
    widget.invalidate()
end
local function dispose(context,m)
    m.alive=false;m.link:dispose();if m.browse then m.browse:unsubscribe() end;schedule.cancel("countdown.day")
end
return widget.define({name=l10n.tr("countdown.name"),useCustomStyle=true,followPersonalizationDefault=true,
    bg=0x18202A,border=0xFFFFFF,alpha=0.42,borderAlpha=0.18,gradientEndA=0.28,
    setup=setup,view=desktop,panel=panel,event=event,dispose=dispose})
