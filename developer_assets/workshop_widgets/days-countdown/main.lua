local logic=module.require("modules/countdown.lua")
local navigation=module.require("modules/navigation.lua")
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
        page_previous=l10n.tr("countdown.page_previous"),page_next=l10n.tr("countdown.page_next"),
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
    return view.button({key=key,label=s,height=r,width="fill",fontSize=r*0.46,textAlign="center",
        style={foreground="textPrimary",cornerRadius=r*0.12},
        action={id=key},enabled=enabled~=false,accessibility={label=s}})
end
local function browse(m,date)
    local first,last=logic.monthRange(date)
    if first~=m.first then
        m.listRevision=(m.listRevision or 0)+1
        m.page=1
    end
    if m.browse then m.browse:unsubscribe() end
    m.first,m.last=first,last
    m.browse=data.subscribe("calendar.events",{fromDate=m.first,toDate=m.last,whenHidden="pause",maxAgeMs=86400000})
end
local function leavePage(m)
    m.picker=nil
    if m.browse then m.browse:unsubscribe();m.browse=nil end
end
local function setup()
    local m={mode="manage",title="",date=today(),filter="",today=today(),alive=true}
    m.navigation=navigation.new({
        changePage=function(page)
            leavePage(m);m.mode=page;m.error=nil
            if page=="choose" then browse(m,m.first or today()) end
        end,
        openPanel=function() widget.openPanel({title=copyText().manage,width=520,height=640}) end,
        leavePage=function() leavePage(m) end})
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
            local state=text("event.status",delta>0 and c.remaining or c.elapsed,unit*0.20,true)
            state.textAlign="end"
            local date=text("event.date",item.date,unit*0.10,true);date.fontSize=unit*0.07;date.textAlign="center"
            local sideHeight=math.max(0,(h-pad*2-unit*0.38)/2)
            local heading=view.column({key="event.heading",height=sideHeight,gap=unit*0.015,
                justifyContent="center",children={title}})
            local footer=view.column({key="event.footer",height=sideHeight,
                justifyContent="center",children={date}})
            if delta~=0 then
                local suffix=text("event.unit",c.days,unit*0.20,true)
                local function labelUnits(s)
                    local characters=#(s:gsub("[\128-\191]",""))
                    local _,ascii=s:gsub("[\1-\127]","")
                    return math.max(1.1,ascii*0.65+(characters-ascii)*1.1)
                end
                local labelWidth=math.max(labelUnits(state.text),labelUnits(c.days))
                local labelSize=math.min(unit*0.08,(w-pad*2)*0.30/labelWidth)
                local sideWidth=labelSize*labelWidth
                state.fontSize=labelSize;state.width=sideWidth;state.flexShrink=0
                suffix.fontSize=labelSize;suffix.width=sideWidth;suffix.flexShrink=0
                number.fontSize=math.min(unit*0.34,(w-pad*2-sideWidth*2-unit*0.05)/(#value*0.72))
                number.width="auto";number.minWidth=number.fontSize*#value*0.72;number.flexShrink=0
                local countRow=view.row({key="event.count",height=unit*0.38,gap=unit*0.025,
                    justifyContent="center",alignItems="end",children={
                        state,number,suffix}})
                children={heading,countRow,footer}
            else children={heading,number,footer} end
            -- Balance the visible glyph, whose baseline sits below the text line center.
            local opticalOffset=number.fontSize*(delta==0 and 0.10 or 0.22)
            heading.height=math.max(0,sideHeight-opticalOffset)
            footer.height=sideHeight+opticalOffset
        end
    end
    if status~="ready" then
        local hint=text("status",c[status],unit*0.30,true)
        hint.textWrap="wrap";hint.maxLines=3;hint.textAlign="center";hint.fontSize=unit*0.085
        children={hint,button("choose",c.choose,unit*0.18,widget.hasPermission("calendar.read")),
            button("create",c.create,unit*0.18,widget.hasPermission("calendar.write"))}
    end
    return view.column({key="countdown.card",width="fill",height="fill",padding=pad,gap=status=="ready" and 0 or unit*0.015,
        justifyContent="center",children=children,events=status=="ready" and {click={id="manage"}} or nil,
        accessibility={role="group",label=c.manage}})
end
local function panel(context,m)
    local c=copyText();local r=ui.metrics().layoutRowHeight;local children={}
    if m.picker then children={m.picker:view({rowHeight=r})}
    elseif m.mode=="create" then
        local valid=m.title:match("%S")~=nil and #m.title<=512;local dateValid=logic.parts(m.date)~=nil
        children={text("label.title",c.title,r),view.textInput({key="input.title",value=m.title,
            height=r,fontSize=r*0.46,style={foreground="textPrimary"},maxBytes=512,action={id="title"},validationState=valid and "none" or "error",
            validationMessage=valid and "" or c.invalid_title,enabled=not m.link.pending,accessibility={label=c.title}})}
        if not valid then children[#children+1]=text("error.title",c.invalid_title,r,true) end
        children[#children+1]=text("label.date",c.date,r)
        children[#children+1]=view.textInput({key="input.date",value=m.date,height=r,fontSize=r*0.46,style={foreground="textPrimary"},maxBytes=10,
            action={id="date"},validationState=dateValid and "none" or "error",validationMessage=dateValid and "" or c.invalid_date,
            enabled=not m.link.pending,accessibility={label=c.date}})
        if not dateValid then children[#children+1]=text("error.date",c.invalid_date,r,true) end
        children[#children+1]=button("pick.create",c.pick,r,not m.link.pending)
        if not widget.hasPermission("calendar.write") then children[#children+1]=text("write.error",c.write_permission,r,true) end
        if m.error then children[#children+1]=text("save.error",c[m.error] or c.save_failed,r,true) end
        children[#children+1]=button("save",m.link.pending and c.saving or c.create,r,
            valid and dateValid and not m.link.pending and widget.hasPermission("calendar.write"))
    elseif m.mode=="choose" then
        children={view.row({key="month.nav",height=r,gap=r*0.25,children={button("previous",c.previous,r),
            button("pick.month",(m.first or today()):sub(1,7),r),button("next",c.next,r)}}),
            view.searchBox({key="filter",value=m.filter,height=r,fontSize=r*0.46,style={foreground="textPrimary"},maxBytes=512,
                placeholder=c.filter,action={id="filter"},accessibility={label=c.filter}})}
        local s=m.browse and m.browse:value()
        if not widget.hasPermission("calendar.read") then children[#children+1]=text("permission",c.permission,r,true)
        elseif not s or not s.available or not s.value then children[#children+1]=text("unavailable",c.unavailable,r,true)
        else
            local matches={}
            for _,item in ipairs(s.value.events or {}) do
                if m.filter=="" or item.title:lower():find(m.filter:lower(),1,true) then
                    matches[#matches+1]=item
                end
            end
            local pages=math.max(1,math.ceil(#matches/40))
            m.page=math.max(1,math.min(m.page or 1,pages))
            for i=(m.page-1)*40+1,math.min(m.page*40,#matches) do
                local item=matches[i]
                children[#children+1]=button("bind:"..item.id,item.date.."  "..item.title,r)
            end
            if #matches==0 then children[#children+1]=text("no.events",c.no_events,r,true) end
            if pages>1 then children[#children+1]=view.row({key="pages",height=r,gap=r*0.25,children={
                button("page.previous",c.page_previous,r,m.page>1),
                text("page.number",tostring(m.page).." / "..tostring(pages),r,true),
                button("page.next",c.page_next,r,m.page<pages)}}) end
            if s.value.truncated then children[#children+1]=text("truncated",c.truncated,r,true) end
        end
    else
        local status,item=m.link:current()
        local summary=text("linked.title",item and item.title or c[status],r)
        summary.textWrap="wrap";summary.maxLines=2;summary.height=r*1.5
        children={summary}
        if item then children[#children+1]=text("linked.date",item.date,r,true) end
        children[#children+1]=button("choose",c.choose,r,widget.hasPermission("calendar.read"))
        children[#children+1]=button("create",c.create,r,widget.hasPermission("calendar.write"))
        if m.link.id~="" then children[#children+1]=button("unlink",c.unlink,r) end
        if not widget.hasPermission("calendar.write") then children[#children+1]=text("write.error",c.write_permission,r,true) end
    end
    local page=m.picker and "picker" or m.mode
    local frame={}
    if page~="manage" then
        local back=button("panel.back",c.back,r,not m.link.pending);back.width=r*2
        local heading=text("panel.heading",page=="picker" and c.pick or c[page],r)
        heading.textWrap="wrap"
        frame[#frame+1]=view.row({key="panel.navigation",width="fill",height=r,flexShrink=0,gap=r*0.3,
            children={back,heading}})
    end
    if page=="choose" then
        for _=1,2 do
            local control=table.remove(children,1);control.flexShrink=0;frame[#frame+1]=control
        end
    end
    -- Each page owns its offset; only replacing search results resets the list.
    local scrollKey="countdown.scroll."..page
    if page=="choose" then scrollKey=scrollKey..":"..tostring(m.listRevision or 0) end
    frame[#frame+1]=view.scroll({key=scrollKey,width="fill",height="fill",children={
        view.column({key="countdown.body."..page,width="fill",height="auto",padding=r*0.10,gap=r*0.3,children=children})}})
    return view.column({key="countdown.panel",width="fill",height="fill",padding=r*0.65,gap=r*0.3,children=frame})
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
        if ok then
            if m.navigation.isOpen then m.navigation:show("manage") else m.mode="manage" end
            m.title="";m.error=nil
        elseif err then m.error=err end
    elseif e.kind=="schedule" or e.kind=="environment" or e.kind=="visibility" then
        m.today=today()
        if e.kind=="visibility" and e.visible then m.link:refresh() end
    elseif e.kind=="panel" and e.action=="closed" then
        m.navigation:closed()
    elseif e.kind=="action" then
        local id=e.id
        if m.link.pending then return end
        if id=="manage" or id=="choose" or id=="create" then m.navigation:show(id)
        elseif id=="panel.back" then
            if m.picker then m.picker=nil else m.navigation:show("manage") end
        elseif id=="unlink" then m.link:bind("")
        elseif id=="title" then m.title=e.text or m.title
        elseif id=="date" then m.date=e.text or m.date
        elseif id=="filter" then m.filter=e.text or m.filter;m.page=1;m.listRevision=(m.listRevision or 0)+1
        elseif id=="page.previous" then m.page=math.max(1,(m.page or 1)-1);m.listRevision=(m.listRevision or 0)+1
        elseif id=="page.next" then m.page=(m.page or 1)+1;m.listRevision=(m.listRevision or 0)+1
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
                    if item.id==target then m.link:bind(target);m.navigation:show("manage");break end
                end
            end
        end
    end
    widget.invalidate()
end
local function dispose(context,m)
    m.alive=false;m.navigation:dispose();m.link:dispose();schedule.cancel("countdown.day")
end
return widget.define({name=l10n.tr("countdown.name"),useCustomStyle=true,followPersonalizationDefault=true,
    bg=0x18202A,border=0xFFFFFF,alpha=0.42,borderAlpha=0.18,gradientEndA=0.28,
    setup=setup,view=desktop,panel=panel,event=event,dispose=dispose})
