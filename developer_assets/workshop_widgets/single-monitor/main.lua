local metrics = module.require("modules/metrics.lua")
local instruments = module.require("modules/instruments.lua")

local function copy()
    return {
        name=l10n.tr("monitor.name"), cpu=l10n.tr("monitor.cpu"), gpu=l10n.tr("monitor.gpu"),
        memory=l10n.tr("monitor.memory"), vram=l10n.tr("monitor.vram"),
        battery=l10n.tr("monitor.battery"), disk=l10n.tr("monitor.disk"), network=l10n.tr("monitor.network"),
        choose=l10n.tr("monitor.choose"), device=l10n.tr("monitor.device"), automatic=l10n.tr("monitor.automatic"),
        history=l10n.tr("monitor.history"),
        used=l10n.tr("monitor.used"), available=l10n.tr("monitor.available"),
        dedicated=l10n.tr("monitor.dedicated"), gpu_load=l10n.tr("monitor.gpu_load"),
        charging=l10n.tr("monitor.charging"), charged=l10n.tr("monitor.charged"),
        plugged_in=l10n.tr("monitor.plugged_in"), on_battery=l10n.tr("monitor.on_battery"), saver=l10n.tr("monitor.saver"),
        download=l10n.tr("monitor.download"), upload=l10n.tr("monitor.upload"),
        loading=l10n.tr("monitor.loading"), permission=l10n.tr("monitor.permission"),
        unavailable=l10n.tr("monitor.unavailable"), not_present=l10n.tr("monitor.not_present"),
        device_missing=l10n.tr("monitor.device_missing"), no_vram=l10n.tr("monitor.no_vram"), stale=l10n.tr("monitor.stale"),
        cpu_source=l10n.tr("monitor.processor"), memory_source=l10n.tr("monitor.physical"),
        battery_source=l10n.tr("monitor.this_device"), network_source=l10n.tr("monitor.aggregate"),
    }
end

local function byteParts(value)
    local formatted=l10n.formatBytes(value,{base=1024,maximumFractionDigits=1})
    local amount,unit=formatted:match("^(.-)"..utf8.char(0x202F).."(.+)$")
    return amount or formatted,unit or ""
end
local function capacity(used,total,reference)
    local divisor=1
    while total/divisor>=1024 do divisor=divisor*1024 end
    local _,unit=byteParts(total)
    local options={maximumFractionDigits=1,minimumFractionDigits=reference and 1 or 0}
    local left=l10n.formatNumber((reference and total or used)/divisor,options)
    local right=l10n.formatNumber(total/divisor,options)
    if reference then left=left:gsub("%d","8");right=right:gsub("%d","8") end
    return left.." / "..right.." "..unit
end
local format = {
    percent=function(value) return l10n.formatNumber(value,{maximumFractionDigits=0}).."%" end,
    capacity=function(used,total) return capacity(used,total,false) end,
    capacityReference=function(total) return capacity(total,total,true) end,
    rateParts=function(value)
        local amount,unit=byteParts(value)
        return amount,unit.."/s"
    end,
    rateReference=function()
        return l10n.formatNumber(1023.9,{minimumFractionDigits=1,maximumFractionDigits=1}):gsub("%d","8")
    end,
    rate=function(value) return l10n.formatBytes(value,{base=1024,maximumFractionDigits=1}).."/s" end,
}
-- FluentSystemIcons-Regular.ttf: developer_board, desktop_pulse, ram,
-- battery_9, hard_drive and globe, all at the semantic 20-unit size.
local glyphs={cpu=0xE4AC,gpu=0xE49D,memory=0xE9F3,vram=0xE9F3,battery=0xF1CD,disk=0xE6E3,network=0xF45A}
local deviceGlyph,automaticGlyph=0xE4AA,0xF190 -- desktop_toolbox, arrow_sync
local function selected(mode)
    if mode=="gpu" or mode=="vram" then return storage.get("gpuId") or "" end
    if mode=="disk" then return storage.get("volumeId") or "" end
    return ""
end

local function reconcile(m)
    local mode=metrics.mode(storage.get("metric"))
    local source,deviceId=metrics.sources[mode],selected(mode)
    local details=widget.hasFeature("data.system.gpu.details")
    local permitted=widget.hasPermission(source.permission)
    local available=widget.hasFeature("data."..source.topic)
    local identity=mode..":"..deviceId
    if m.identity~=identity then m.history,m.identity={},identity end
    m.mode=mode
    if m.topic~=source.topic or m.details~=details or not permitted or not available then
        if m.handle then m.handle:unsubscribe();m.handle=nil end
    end
    m.topic,m.details=source.topic,details
    m.blocked=not permitted and "permission" or (not available and "unavailable" or nil)
    if not m.blocked and not m.handle then
        local options={maxAgeMs=source.age,whenHidden="pause"}
        if source.topic=="system.gpu" and details then options.includeDetails=true end
        m.handle=data.subscribe(source.topic,options)
    end
end

local function sample(m)
    m.now=time.monotonic()
    m.snapshot=m.handle and m.handle:value() or nil
    m.current=m.blocked and {status=m.blocked} or metrics.read(m.mode,m.snapshot,selected(m.mode),m.details)
    metrics.append(m.history,m.now,m.current)
    m.choices={}
    if m.snapshot and m.snapshot.available then
        local c,value=copy(),m.snapshot.value or {}
        if m.mode=="gpu" or m.mode=="vram" then
            local names={}
            for _,item in ipairs(value.adapters or {}) do names[item.name]=(names[item.name] or 0)+1 end
            for i,item in ipairs(value.adapters or {}) do
                if i>40 then break end
                m.choices[#m.choices+1]={id=item.id,label=item.name..
                    (names[item.name]>1 and (" · "..l10n.formatNumber(i)) or "")}
            end
        elseif m.mode=="disk" then
            for i,item in ipairs(value.volumes or {}) do
                if i>40 then break end
                m.choices[#m.choices+1]={id=item.id,label=item.mountPoint..
                    (item.displayName~="" and (" "..item.displayName) or "")}
            end
        end
        if #m.choices>0 then table.insert(m.choices,1,{id="",label=c.automatic}) end
    end
end

-- Only deterministic preview fixtures receive a representative past.
-- Live instances start with real samples; there is no invented live history.
local function previewHistory(m)
    if m.current.status~="ready" or (m.mode~="cpu" and m.mode~="network") then return end
    m.history={}
    local pattern={0.6,0.9,0.72,1.4,0.98,1.7,1.1,0.82,0.45,0.65,0.52,0.9,0.4,0.62,0.94,
        0.72,1.25,1.65,1.05,1.4,0.95,0.7,0.55,0.3,0.65,0.9,0.76,0.6,0.9,1}
    for i,factor in ipairs(pattern) do
        m.history[i]={time=m.now-(#pattern-i)*2000,timestamp=i,
            value=m.current.percent and math.min(100,m.current.percent*factor),
            download=m.current.download and m.current.download*factor,
            upload=m.current.upload and m.current.upload*pattern[#pattern-i+1]}
    end
end

local function setup(context)
    local m={history={},choices={},menuTargets={}}
    reconcile(m);sample(m)
    if context.preview then previewHistory(m) end
    schedule.every("monitor.sample",1000,{whenHidden="pause"})
    return m
end
local function render(_context,m)
    instruments.render(m.mode,m.current,m.history,m.now,copy(),format)
end
local function chooseMode(m,mode)
    if not metrics.sources[mode] then return end
    if storage.get("metric")~=mode then storage.set("metric",mode) end
    reconcile(m);sample(m);widget.closePopover()
end
local function chooseDevice(m,id)
    if type(id)~="string" or (m.mode~="gpu" and m.mode~="vram" and m.mode~="disk") then return end
    local key=m.mode=="disk" and "volumeId" or "gpuId"
    if storage.get(key)~=id then storage.set(key,id) end
    reconcile(m);sample(m);widget.closePanel()
end

local function popover(_context,m)
    local c,r,children=copy(),ui.metrics().layoutRowHeight,{}
    for _,mode in ipairs(metrics.order) do
        children[#children+1]=view.button({key="mode."..mode,label=c[mode],
            height=r,width="fill",fontSize=r*0.46,textAlign="start",
            padding={horizontal=r*0.38},bold=mode==m.mode,
            style={foreground="textPrimary",cornerRadius=r*0.16},
            action={id="mode."..mode},accessibility={label=c[mode]}})
        if mode==m.mode and (mode=="gpu" or mode=="vram" or mode=="disk") then
            children[#children+1]=view.button({key="device.open",label=c.device,height=r,width="fill",
                textAlign="start",padding={left=r*0.95,right=r*0.38},fontSize=r*0.46,
                style={foreground="textPrimary",cornerRadius=r*0.16},
                action={id="device.open"},accessibility={label=c.device}})
        end
    end
    return view.column({key="selector.root",width="fill",height="fill",padding=r*0.32,gap=r*0.13,children=children})
end
local function panel(_context,m)
    local c,r,children=copy(),ui.metrics().layoutRowHeight,{}
    if #m.choices==0 then
        children[1]=view.text({key="devices.empty",text=c[m.current.status] or c.unavailable,
            width="fill",height=r*2,textWrap="wrap",fontSize=r*0.46,style={foreground="textSecondary"}})
    end
    for i,choice in ipairs(m.choices) do
        children[#children+1]=view.button({key="device."..i,label=choice.label,width="fill",height=r,
            fontSize=r*0.46,textAlign="start",bold=choice.id==selected(m.mode),padding={horizontal=r*0.35},
            style={foreground="textPrimary",cornerRadius=r*0.16},
            action={id="device.choose",value={id=choice.id,mode=m.mode}},accessibility={label=choice.label}})
    end
    return view.scroll({key="devices.scroll",width="fill",height="fill",children={
        view.column({key="devices.list",width="fill",padding=r*0.4,gap=r*0.15,children=children})}})
end
local function menu(_context,m,request)
    if request.id~="monitor.menu" then return nil end
    local c,items=copy(),{}
    for _,mode in ipairs(metrics.order) do
        items[#items+1]={id="mode."..mode,label=c[mode],checked=m.mode==mode,
            icon=utf8.char(glyphs[mode]),iconFont="fluent"}
    end
    if m.mode=="gpu" or m.mode=="vram" or m.mode=="disk" then
        local choices={}
        m.menuTargets={}
        for i,choice in ipairs(m.choices) do
            local action="device.menu."..i
            m.menuTargets[action]={id=choice.id,mode=m.mode}
            choices[#choices+1]={id=action,label=choice.label,checked=choice.id==selected(m.mode),
                icon=utf8.char(choice.id=="" and automaticGlyph or glyphs[m.mode]),iconFont="fluent"}
        end
        items[#items+1]={type="separator"}
        if #choices>0 then
            items[#items+1]={label=c.device,children=choices,icon=utf8.char(deviceGlyph),iconFont="fluent"}
        else
            items[#items+1]={id="device.open",label=c.device,icon=utf8.char(deviceGlyph),iconFont="fluent"}
        end
    end
    return ui.menu(items)
end

local function event(context,m,value)
    if value.kind=="schedule" and value.id=="monitor.sample" then reconcile(m);sample(m)
    elseif value.kind=="settings.changed" or value.kind=="environment" or value.kind=="data.change" or
            (value.kind=="visibility" and value.visible) then reconcile(m);sample(m)
    elseif value.kind=="action" then
        if value.id=="selector.open" then
            local r=ui.metrics().layoutRowHeight
            local rows=(m.mode=="gpu" or m.mode=="vram" or m.mode=="disk") and 8 or 7
            widget.openPopover({anchorKey="monitor.selector",width=math.ceil(r*10),
                height=math.ceil(r*(rows*1.13+0.8)),placement="bottomEnd"})
        elseif value.id:sub(1,5)=="mode." then chooseMode(m,value.id:sub(6))
        elseif value.id=="device.open" then
            widget.closePopover();widget.openPanel({title=copy().device,width=440,height=360})
        elseif value.id=="device.choose" and type(value.value)=="table" and value.value.mode==m.mode then
            chooseDevice(m,value.value.id)
        elseif value.id:sub(1,12)=="device.menu." and m.menuTargets[value.id]~=nil then
            local target=m.menuTargets[value.id]
            if target.mode==m.mode then chooseDevice(m,target.id) end
        end
        if context.preview then previewHistory(m) end
    end
end

local c=copy()
local labels={c.cpu,c.gpu,c.memory,c.vram,c.battery,c.disk,c.network}
return widget.define({
    name=c.name,useCustomStyle=true,followPersonalizationDefault=true,showTitle=false,
    bg=0x142234,border=0xDFEBFF,alpha=0.34,borderAlpha=0.18,gradientEndA=0.28,
    settings={fields={{key="metric",label=c.choose,type="select",default="cpu",
        options=metrics.order,optionLabels=labels}}},
    setup=setup,render=render,popover=popover,panel=panel,menu=menu,event=event,
    dispose=function(_context,m)
        if m.handle then m.handle:unsubscribe() end
        schedule.cancel("monitor.sample")
    end,
})
