local clock=module.require("modules/media_clock.lua")
local mediaSession=module.require("modules/media_session.lua")
local client=module.require("modules/client.lua")
local json=module.require("modules/json.lua")
local V=module.require("modules/visual.lua")
local strings=module.require("modules/strings.lua")
local Text=module.require("modules/text_layout.lua")
local FRAME="lyrics.frame"
local function previewPosition(lines)
    local position=tonumber(storage.get("previewPositionMs"))
    if position then return position end
    position=45500
    if storage.get("lyricMode")~="dynamic" then return position end
    local line=lines[clock.index(lines,position)]
    if not line then return position end
    local words=Text.estimateWords(line);local last=words[#words]
    if last then
        local count=math.max(1,#Text.clusters(last.text))
        return last.finish-(last.finish-last.start)/count/2
    end
    return line.finish-(line.finish-line.start)/math.max(1,#Text.clusters(line.text))/2
end
local function frames(m)
    local visual=not m.reducedMotion and m.status=="ready" and m.clock and m.clock.playing and #m.client.lines>0
    local decoding=m.client.decodeJob and m.client.enabled
    local run=not m.preview and m.mediaValid~=false and m.visible~=false and (visual or decoding)
    if run and not m.framePending then m.framePending=animation.requestFrame(FRAME)==true
    elseif not run and m.framePending then animation.cancelFrame(FRAME);m.framePending=false end
end
local function refresh(context,m)
    if m.preview then return end
    m.reducedMotion=context.accessibility and context.accessibility.reducedMotion==true
    mediaSession.update(m,m.media:value(),widget.hasPermission("media.read"),time.monotonic(),time.now())
    frames(m)
end
local function setup(context)
    local m={preview=context.preview==true,visible=context.visible~=false,status="waiting",reducedMotion=false}
    m.client=client.new({permission=function()return widget.hasPermission("network.internet")end,
        start=function(args)return task.start("network.request",args)end,cancel=task.cancel,monotonic=time.monotonic,
        decode=json.decode})
    if m.preview then
        local c=strings();local lines={};local values={c.preview_before,c.preview_previous,c.preview_current,c.preview_next,
            c.preview_after,c.preview_horizon,c.preview_song,c.preview_before,c.preview_previous,c.preview_current,c.preview_next,
            c.preview_after,c.preview_horizon,c.preview_song}
        for i,value in ipairs(values) do
            local words={};local count=utf8.len(value);local n=0
            for _,code in utf8.codes(value) do
                words[#words+1]={text=utf8.char(code),start=(i-1)*7000+n*7000/count,finish=(i-1)*7000+(n+1)*7000/count};n=n+1
            end
            lines[i]={text=value,start=(i-1)*7000,finish=i*7000,words=words}
        end
        m.client.lines=lines;m.client.state="ready";m.status="ready"
        m.clock={position=45500,duration=98000,playing=false,rate=1,at=time.monotonic()}
        local sample=storage.get("previewState");m.playbackActive=sample~="waiting"
        if sample=="estimated" then for _,line in ipairs(lines) do line.words={} end end
        m.clock.position=previewPosition(lines)
        if sample=="unsupported" then m.status="unsupported";m.clock=nil
        elseif sample=="waiting" then m.status="waiting";m.clock=nil end
    else
        m.media=data.subscribe("media.current",{maxAgeMs=1000,whenHidden="pause"})
        schedule.every("lyrics.refresh",500,{whenHidden="pause"});refresh(context,m)
    end
    return m
end
local function event(context,m,e)
    if e.kind=="frame" and e.id==FRAME then
        m.framePending=false
        if m.client.decodeJob then
            local previous=m.client.lines;m.client:advanceDecode()
            if previous~=m.client.lines then m.snap=true;m.focus=nil;m.textCache=nil end
            if m.mediaValid and m.clock then m.status=m.client.state end
        end
        frames(m);widget.invalidate();return
    end
    if e.kind=="task.complete" then
        refresh(context,m)
        local previous=m.client.lines;m.client:complete(e)
        if previous~=m.client.lines then m.snap=true;m.focus=nil;m.textCache=nil end
        if m.mediaValid and m.clock then m.status=m.client.state end;frames(m)
    elseif e.kind=="visibility" then m.visible=e.visible;if e.visible then refresh(context,m) else frames(m) end
    elseif e.kind=="data.change" or (e.kind=="schedule" and e.id=="lyrics.refresh") then refresh(context,m)
    elseif e.kind=="environment" then refresh(context,m)
    elseif e.kind=="settings.changed" then
        m.textCache=nil;m.snap=true
        if m.preview and m.clock then m.clock.position=previewPosition(m.client.lines) end
    elseif e.kind=="action" then
        if e.id=="mode.scroll" or e.id=="mode.dynamic" then
            local mode=e.id=="mode.dynamic" and "dynamic" or "scroll"
            if storage.get("lyricMode")~=mode then storage.set("lyricMode",mode) end
            m.textCache=nil;m.snap=true;m.scrollIndex=nil;m.shift=nil;frames(m)
            if m.preview and m.clock then m.clock.position=previewPosition(m.client.lines) end
        elseif e.id=="retry" and not m.preview then
            refresh(context,m)
            if m.mediaValid then m.client:retry(true);m.status=m.client.state;frames(m) end
        end
    end
    widget.invalidate()
end
local function settings()
    local c=strings()
    return {presets={{id="transparent",label=c.preset_transparent,default=true,
        values={followPersonalization=false,bg=0,border=0,alpha=0,borderAlpha=0,gradientEndA=0,
            glassEnabled=false,acrylicEnabled=false,__contentTheme=0}}},fields={
        {key="lyricMode",type="select",label=c.lyric_mode,default="scroll",options={"scroll","dynamic"},
            optionLabels={c.mode_scroll,c.mode_dynamic}},
        {key="textScale",type="int",label=c.text_scale,default=100,min=60,max=200,step=5},
        {key="lineSpacing",type="int",label=c.line_spacing,default=175,min=140,max=240,step=5},
        {key="offset",type="int",label=c.offset,default=0,min=-5000,max=5000,step=50},
        {key="estimateWordTiming",type="bool",label=c.estimate_words,default=true},
        {key="hideIdlePlaceholder",type="bool",label=c.hide_idle_placeholder,default=false},
        {key="translation",type="bool",label=c.translation,default=true},
        {key="romanization",type="bool",label=c.romanization,default=false}}}
end
return widget.define({name=strings().name,useCustomStyle=true,followPersonalizationDefault=false,showTitle=false,
    setup=setup,view=V.view,event=event,settings=settings(),
    menu=function()
        local c=strings();local mode=storage.get("lyricMode")=="dynamic" and "dynamic" or "scroll"
        return ui.menu({
            -- Fluent System Icons Regular: text_continuous_24, text_effects_20.
            {id="mode.scroll",label=c.mode_scroll,checked=mode=="scroll",icon=utf8.char(0xECFD),iconFont="fluent"},
            {id="mode.dynamic",label=c.mode_dynamic,checked=mode=="dynamic",icon=utf8.char(0xF7DB),iconFont="fluent"},
            {type="separator"},
            {id="retry",label=c.retry,icon=utf8.char(0xF13D),iconFont="fluent"}})
    end,
    dispose=function(_context,m)
        if m.media then m.media:unsubscribe() end
        m.client:dispose();animation.cancelFrame(FRAME);schedule.cancel("lyrics.refresh")
    end})
