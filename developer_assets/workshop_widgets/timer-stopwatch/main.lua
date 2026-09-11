local logic = module.require("modules/timer.lua")
local function copy()
    return { name=l10n.tr("timer.name"), countdown=l10n.tr("timer.countdown"), stopwatch=l10n.tr("timer.stopwatch"),
        custom=l10n.tr("timer.custom"), start=l10n.tr("timer.start"), pause=l10n.tr("timer.pause"), resume=l10n.tr("timer.resume"),
        reset=l10n.tr("timer.reset"), done=l10n.tr("timer.done"), hint=l10n.tr("timer.hint"), invalid=l10n.tr("timer.invalid"),
        one=l10n.tr("timer.one"), five=l10n.tr("timer.five"), ten=l10n.tr("timer.ten") }
end
local function refresh(m)
    schedule.cancel("visual")
    schedule.cancel("deadline")
    if m.countdown.running or m.stopwatch.running then schedule.every("visual", 100, {whenHidden="pause"}) end
    if m.countdown.running then
        schedule.after("deadline", math.max(1, logic.value(m.countdown,time.monotonic(),true)), {whenHidden="continue"})
    end
end
local function setup()
    local m=logic.new()
    m.mode=storage.get("mode")=="stopwatch" and "stopwatch" or "countdown"
    m.draft=storage.get("duration") or "5:00"
    return m
end
local function button(id,label,r,selected)
    return view.button({key=id,label=label,width="fill",height=r,fontSize=r*0.43,textAlign="center",
        bold=selected==true,action={id=id},accessibility={label=label},
        style={foreground="textPrimary",cornerRadius=r*0.2,borderColor=selected and "textPrimary" or "border",borderWidth=selected and r*0.055 or 0},
        hoverStyle={opacity=0.8},pressedStyle={opacity=0.6}})
end
local function desktop(_,m)
    local c=copy()
    local r=ui.metrics().layoutRowHeight
    local t=m[m.mode]
    local cd=m.mode=="countdown"
    local actions={}
    if cd and t.value==0 and not t.running and not t.done then
        actions={button("preset.1",c.one,r),button("preset.5",c.five,r),button("preset.10",c.ten,r),button("custom",c.custom,r)}
    else
        local label=t.done and c.done or logic.format(logic.value(t,time.monotonic(),cd),cd)
        local value=view.text({key="value",text=label,width="auto",minWidth=r*3.9,flexShrink=0,height=r,fontSize=r*0.66,
            bold=true,verticalAlign="center",overflowText="clip",style={foreground="textPrimary"}})
        actions[#actions+1]=value
        if not t.done then actions[#actions+1]=button("toggle",t.running and c.pause or (t.value>0 and c.resume or c.start),r) end
        if t.running or t.value>0 or t.done then actions[#actions+1]=button("reset",c.reset,r) end
    end
    return view.column({key="timer",width="fill",height="fill",padding=r*0.3,gap=r*0.22,justifyContent="center",children={
        view.row({key="modes",width="fill",height=r,gap=r*0.2,children={button("mode.countdown",c.countdown,r,cd),button("mode.stopwatch",c.stopwatch,r,not cd)}}),
        view.row({key="actions",width="fill",height=r,gap=r*0.2,children=actions})}})
end
local function panel(_,m)
    local c=copy()
    local r=ui.metrics().layoutRowHeight
    return view.column({key="duration.panel",width="fill",height="fill",padding=r*0.6,gap=r*0.4,children={
        view.text({key="hint",text=c.hint,width="fill",height=r*2,fontSize=r*0.46,textWrap="wrap",style={foreground="textPrimary"}}),
        view.textInput({key="duration",value=m.draft,width="fill",height=r,fontSize=r*0.5,events={change={id="duration"},submit={id="custom.start"}},accessibility={label=c.hint}}),
        view.text({key="error",text=m.error and c.invalid or "",width="fill",height=r*2,fontSize=r*0.43,textWrap="wrap",style={foreground="textPrimary"}}),
        button("custom.start",c.start,r)}})
end
local function startCountdown(m,duration)
    m.countdown.value=duration
    logic.start(m.countdown,time.monotonic())
    m.mode="countdown"
    storage.set("mode",m.mode)
    refresh(m)
end
local function event(_,m,e)
    if logic.tick(m.countdown,time.monotonic()) then
        if widget.hasFeature("task.notification.show") and widget.hasFeature("task.start") and widget.hasPermission("notification.post") then
            task.start("notification.show",{title=copy().name,message=copy().done})
        end
        refresh(m)
    end
    if e.kind=="action" then
        local id=e.id
        if id=="mode.countdown" or id=="mode.stopwatch" then m.mode=id:sub(6);storage.set("mode",m.mode)
        elseif id=="custom" then
            m.error=false;widget.openPanel({title=copy().custom,width=420,height=260})
        elseif id=="duration" then m.draft=e.text or m.draft;m.error=false
        elseif id=="custom.start" then
            local duration=logic.parse(m.draft)
            if duration and not m.countdown.running then
                storage.set("duration",m.draft);startCountdown(m,duration);widget.closePanel()
            else m.error=true end
        elseif id=="preset.1" or id=="preset.5" or id=="preset.10" then
            if not m.countdown.running then startCountdown(m,tonumber(id:sub(8))*60000) end
        elseif id=="toggle" then
            local t=m[m.mode]
            if t.running then logic.pause(t,time.monotonic(),m.mode=="countdown")
            elseif not t.done then logic.start(t,time.monotonic()) end
            refresh(m)
        elseif id=="reset" then logic.reset(m[m.mode]);refresh(m) end
    end
    widget.invalidate()
end
return widget.define({name=l10n.tr("timer.name"),useCustomStyle=true,followPersonalizationDefault=true,
    bg=0x18202A,border=0xFFFFFF,alpha=0.42,borderAlpha=0.18,gradientEndA=0.28,
    setup=setup,view=desktop,panel=panel,event=event,
    dispose=function() schedule.cancel("visual");schedule.cancel("deadline") end})
