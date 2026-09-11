local logic = module.require("modules/timer.lua")
local function copy()
    return { name=l10n.tr("timer.name"), countdown=l10n.tr("timer.countdown"), stopwatch=l10n.tr("timer.stopwatch"),
        custom=l10n.tr("timer.custom"), start=l10n.tr("timer.start"), pause=l10n.tr("timer.pause"), resume=l10n.tr("timer.resume"),
        reset=l10n.tr("timer.reset"), done=l10n.tr("timer.done"), hint=l10n.tr("timer.hint"), invalid=l10n.tr("timer.invalid"),
        unit=l10n.tr("timer.unit"), settings=l10n.tr("timer.settings"), one=l10n.tr("timer.one"), five=l10n.tr("timer.five"), ten=l10n.tr("timer.ten"), fifteen=l10n.tr("timer.fifteen"), twentyfive=l10n.tr("timer.twentyfive"), thirty=l10n.tr("timer.thirty") }
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
-- Canvas-like controls follow the proportions of the phone reference.
local function desktop(_,m)
    local c=copy()
    local w,h=layout.contentWidth(),layout.contentHeight()
    local side=math.min(w,h)
    local pad=side*0.075
    local topPad=side*0.04
    local gap=side*0.05
    local width=w-pad*2
    local header=side*0.16
    local tileH=(h-topPad-pad-header-gap*2)/2
    local tileW=(width-gap*2)/3
    local dark=widget.theme().contentTheme==1
    local surface=dark and 0xDEE3EC or 0x242B3F
    local accent=0x438BF5
    local cd=m.mode=="countdown"
    local t=m[m.mode]
    local function icon(id,glyph,label,size,primary,headerIcon)
        return (primary and view.iconButton or view.icon)({key=id,glyph=utf8.char(glyph),iconFont="fa",width=size,height=size,
            fontSize=size*0.43,flexShrink=0,textAlign="center",verticalAlign="center",
            padding={left=glyph==0xF04B and size*0.05 or 0,top=headerIcon and size*0.08 or 0},events={click={id=id}},accessibility={label=label},
            style={background=primary and accent or nil,
                foreground=primary and 0xFFFFFF or "textPrimary",cornerRadius=size/2},
            hoverStyle={opacity=0.82},pressedStyle={opacity=0.65}})
    end
    local function preset(n)
        return view.column({key="preset."..n,width=tileW,height=tileH,gap=tileH*0.04,justifyContent="center",alignItems="center",
            events={click={id="preset."..n}},accessibility={role="button",label=n.." "..c.unit},
            style={background=surface,cornerRadius=side*0.075},hoverStyle={opacity=0.85},pressedStyle={opacity=0.65},children={
                view.text({key="number."..n,text=tostring(n),width="fill",height=tileH*0.49,fontSize=math.min(tileH*0.36,tileW*0.58),flexShrink=0,
                    textAlign="center",verticalAlign="center",style={foreground="textPrimary"}}),
                view.text({key="unit."..n,text=c.unit,width="fill",height=tileH*0.24,fontSize=math.min(tileH*0.16,tileW*0.24),flexShrink=0,
                    textAlign="center",verticalAlign="center",style={foreground="textPrimary"}})}})
    end
    local children={view.row({key="header",width="fill",height=header,flexShrink=0,alignItems="center",children={
        icon("settings",0xF013,c.settings,header,false,true),
        view.text({key="title",text=cd and c.countdown or c.stopwatch,width="fill",height=header,fontSize=side*0.10,
            bold=true,textAlign="center",verticalAlign="center",style={foreground="textPrimary"}}),
        icon(cd and "mode.stopwatch" or "mode.countdown",0xF0EC,cd and c.stopwatch or c.countdown,header,false,true)}})}
    if cd and t.value==0 and not t.running and not t.done then
        children[#children+1]=view.row({key="presets.top",width="fill",height=tileH,gap=gap,children={preset(1),preset(2),preset(3)}})
        children[#children+1]=view.row({key="presets.bottom",width="fill",height=tileH,gap=gap,children={preset(5),
            view.button({key="custom",label=c.custom,width=tileW*2+gap,height=tileH,fontSize=side*0.09,bold=true,padding={horizontal=tileW*0.12,vertical=0},
                textAlign="center",action={id="custom"},style={background=accent,foreground=0xFFFFFF,cornerRadius=side*0.075},
                hoverStyle={opacity=0.85},pressedStyle={opacity=0.65}})}})
    elseif not cd and t.value==0 and not t.running then
        local size=math.min(tileH*1.8,width*0.61)
        children[#children+1]=view.column({key="start.area",width="fill",height=tileH*2+gap,justifyContent="center",alignItems="center",
            children={icon("toggle",0xF04B,c.start,size,true)}})
    else
        local valueHeight=side*0.30
        -- Explicit spacers keep the header and time fixed without flex compression.
        local actionGap=gap
        gap=0
        children[#children+1]=view.spacer({key="time.top",width="fill",height=h/2-topPad-header-valueHeight/2,flexShrink=0})
        children[#children+1]=view.text({key="value",text=t.done and c.done or logic.format(logic.value(t,time.monotonic(),cd),cd),
            width="fill",height=valueHeight,flexShrink=0,fontSize=math.min(side*0.19,width/5.1),textAlign="center",verticalAlign="center",
            overflowText="clip",style={foreground="textPrimary"}})
        local actions={}
        local size=side*0.25
        children[#children+1]=view.spacer({key="time.bottom",width="fill",height=h/2-valueHeight/2-pad-size,flexShrink=0})
        if not t.done then actions[#actions+1]=icon("toggle",t.running and 0xF04C or 0xF04B,t.running and c.pause or c.resume,size,true) end
        actions[#actions+1]=icon("reset",0xF0E2,c.reset,size,false)
        children[#children+1]=view.row({key="actions",width="fill",height=size,flexShrink=0,gap=actionGap,justifyContent="center",alignItems="center",children=actions})
    end
    return view.column({key="timer",width="fill",height="fill",padding={left=pad,right=pad,top=topPad,bottom=pad},gap=gap,children=children})
end
local function panel(_,m)
    local c=copy()
    local r=ui.metrics().layoutRowHeight
    return view.column({key="duration.panel",width="fill",height="fill",padding=r*0.6,gap=r*0.4,children={
        view.text({key="hint",text=c.hint,width="fill",height=r*2,fontSize=r*0.46,textWrap="wrap",style={foreground="textPrimary"}}),
        view.textInput({key="duration",value=m.draft,width="fill",height=r,fontSize=r*0.43,padding={horizontal=r*0.25,vertical=0},events={change={id="duration"},submit={id="custom.start"}},accessibility={label=c.hint}}),
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
        elseif id=="settings" then widget.openSettings()
        elseif id=="custom" then
            m.error=false;widget.openPanel({title=copy().custom,width=420,height=260})
        elseif id=="duration" then m.draft=e.text or m.draft;m.error=false
        elseif id=="custom.start" then
            local duration=logic.parse(m.draft)
            if duration and not m.countdown.running then
                storage.set("duration",m.draft);startCountdown(m,duration);widget.closePanel()
            else m.error=true end
        elseif id=="preset.1" or id=="preset.2" or id=="preset.3" or id=="preset.5" then
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
