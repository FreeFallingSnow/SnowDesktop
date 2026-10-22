local panel=module.require("modules/panel_ui.lua")
local M = {}
local topics = {
    {"audio.output.default", "data.audio.output.default", "audio.output.read"},
    {"audio.output.volume", "data.audio.output.volume", "audio.output.read"},
    {"audio.devices", "data.audio.devices", "audio.devices.read"},
}

local function finite(value)
    return type(value)=="number" and value==value and value~=math.huge and value~=-math.huge
end
local function clamp(value)
    return math.max(0,math.min(1,value))
end
local function value(model, topic)
    local subscription=model.subscriptions[topic]
    local snapshot=subscription and subscription:value()
    if type(snapshot)=="table" and snapshot.available==true and type(snapshot.value)=="table" then
        return snapshot.value
    end
end

function M.sync(model, active)
    model.opened=active==true
    for _,spec in ipairs(topics) do
        local needed=model.opened or (spec[1]~="audio.devices" and model.visible)
        local allowed=needed and model.api.hasFeature(spec[2]) and model.api.hasPermission(spec[3])
        if allowed and not model.subscriptions[spec[1]] then
            model.subscriptions[spec[1]]=model.api.subscribe(spec[1],{maxAgeMs=1000,whenHidden="pause"})
        elseif not allowed and model.subscriptions[spec[1]] then
            model.subscriptions[spec[1]]:unsubscribe()
            model.subscriptions[spec[1]]=nil
        end
    end
end

function M.new(context, api)
    local model={subscriptions={},pending={},visible=true,deviceKeys={},nextDeviceKey=0,preview=context.preview==true,
        api=api or {
            hasFeature=function(name) return widget.hasFeature(name) end,
            hasPermission=function(name) return widget.hasPermission(name) end,
            subscribe=function(name,options) return data.subscribe(name,options) end,
            start=function(name,args) return task.start(name,args) end,
            cancel=function(id) task.cancel(id) end,
            now=function() return time.now() end,
            invalidate=function() widget.invalidate() end,
            openSettings=function() widget.openSettings() end,
            openPopover=function(options) return widget.openPopover(options) end,
            openPanel=function(options) widget.openPanel(options) end,
            closePopover=function() widget.closePopover() end,
            closePanel=function() widget.closePanel() end,
            translate=function(key) return l10n.tr(key) end,
            rowHeight=function() return ui.metrics().layoutRowHeight end,
        }}
    M.sync(model,model.preview)
    return model
end

function M.state(model)
    local api=model.api
    local endpoint=value(model,"audio.output.default")
    local volume=value(model,"audio.output.volume")
    local snapshot=value(model,"audio.devices")
    local id=endpoint and type(endpoint.id)=="string" and endpoint.id~="" and endpoint.id or nil
    if model.expectedEndpoint==id or (model.expectedUntil and model.expectedUntil<api.now()) then
        model.expectedEndpoint=nil;model.expectedUntil=nil
    end
    local matched=id~=nil and type(volume)=="table" and volume.endpointId==id and finite(volume.volume)
        and (not model.expectedEndpoint or model.expectedEndpoint==id)
    local devices,seen={},{}
    for _,device in ipairs(snapshot and snapshot.devices or {}) do
        if type(device.id)=="string" and device.id~="" and not seen[device.id] and
            device.direction=="output" and device.available==true and device.state=="active" and #devices<64 then
            seen[device.id]=true;devices[#devices+1]=device
        end
    end
    -- Partition a fresh array; never reorder the provider's shared snapshot.
    local ordered={}
    for _,device in ipairs(devices) do if device.id==id then ordered[#ordered+1]=device end end
    for _,device in ipairs(devices) do if device.id~=id then ordered[#ordered+1]=device end end
    devices=ordered
    for deviceId in pairs(model.deviceKeys) do if not seen[deviceId] then model.deviceKeys[deviceId]=nil end end
    for _,device in ipairs(devices) do
        if not model.deviceKeys[device.id] then
            model.nextDeviceKey=model.nextDeviceKey+1
            model.deviceKeys[device.id]="system.audio.device."..model.nextDeviceKey
        end
    end
    local optimistic=model.optimistic
    if optimistic and (optimistic.endpoint~=id or optimistic.expires<api.now()) then
        model.optimistic=nil;optimistic=nil
    end
    local scalar=matched and clamp(volume.volume) or nil
    local muted=matched and volume.muted==true or false
    if optimistic and matched then
        if optimistic.volume~=nil then
            if math.abs(optimistic.volume-scalar)>0.000001 then scalar=optimistic.volume
            else optimistic.volume=nil end
        end
        if optimistic.muted~=nil then
            if optimistic.muted~=muted then muted=optimistic.muted else optimistic.muted=nil end
        end
    end
    local busy=next(model.pending)~=nil
    local volumeBusy,muteBusy,deviceBusy=false,false,false
    for _,pending in pairs(model.pending) do
        if not pending.superseded then
            volumeBusy=volumeBusy or pending.name=="audio.output.setVolume"
            muteBusy=muteBusy or pending.name=="audio.output.setMute"
            deviceBusy=deviceBusy or pending.name=="audio.output.selectDevice"
        end
    end
    local coalesced=api.hasFeature("task.audio.output.coalescedVolume")
    local canVolume=matched and api.hasFeature("task.audio.output.control") and
        api.hasPermission("audio.output.control") and not deviceBusy and (coalesced or not volumeBusy)
    if #devices>0 then model.lastDeviceCount=#devices end
    return {
        id=id,name=endpoint and endpoint.name,volume=scalar,muted=muted,devices=devices,
        selected=id or "",busy=busy,coalesced=coalesced,canMute=canVolume and not muteBusy,
        volumeSupported=api.hasFeature("data.audio.output.default") and api.hasFeature("data.audio.output.volume"),
        devicesSupported=api.hasFeature("data.audio.devices"),
        canVolume=canVolume,
        canChoose=api.hasFeature("task.audio.devices.control") and api.hasPermission("audio.devices.control") and #devices>0 and not deviceBusy,
        needsPermission=not api.hasPermission("audio.output.read") or not api.hasPermission("audio.output.control") or
            not api.hasPermission("audio.devices.read") or not api.hasPermission("audio.devices.control"),
    }
end

local function send(model, name, args, endpoint)
    if model.preview then return false end
    if name=="audio.output.setVolume" and model.api.hasFeature("task.audio.output.coalescedVolume") then
        args.coalesce=true
    elseif name=="audio.output.selectDevice" then
        for id,pending in pairs(model.pending) do
            if pending.name~="audio.output.selectDevice" then model.api.cancel(id);model.pending[id]=nil end
        end
        model.draft=nil
    else
        for _,pending in pairs(model.pending) do if pending.name==name then return false end end
    end
    local id,err=model.api.start(name,args)
    if not id then
        if err~="rateLimited" then model.error=err or "failed" end
        return false
    end
    if args.coalesce then
        for _,pending in pairs(model.pending) do if pending.name==name then pending.superseded=true end end
    end
    model.pending[id]={name=name,endpoint=endpoint}
    model.error=nil
    if name=="audio.output.selectDevice" then
        model.expectedEndpoint=args.endpointId;model.expectedUntil=model.api.now()+2500;model.optimistic=nil
    else
        local optimistic=model.optimistic
        if not optimistic or optimistic.endpoint~=endpoint then optimistic={endpoint=endpoint} end
        optimistic.volume=args.volume~=nil and args.volume or optimistic.volume
        if args.muted~=nil then optimistic.muted=args.muted end
        optimistic.expires=model.api.now()+2500
        model.optimistic=optimistic
    end
    return true
end

function M.open(model,anchor,localActive)
    if model.preview then return false end
    M.sync(model,true)
    local row=model.api.rowHeight()
    local current=M.state(model)
    local count=math.max(1,math.min(4,model.lastDeviceCount or 2))
    local height=row*(7.68+count*1.85+(localActive and 3.9 or 0))+28
    if current.needsPermission then height=height+row*1.57 end
    local options={anchorKey=anchor,placement="bottomEnd",width=math.floor(row*12+28+0.5),height=math.floor(height+0.5),
        dismissOnOutside=true,dismissOnEscape=true}
    model.surface="popover"
    if not (model.api.hasFeature("widget.popover") and model.api.hasFeature("view.surface.popover") and
        model.api.openPopover(options)) then
        model.surface="panel"
        model.api.openPanel({title=model.api.translate("shared.audio"),width=options.width+8,
            height=math.floor(options.height+46-row*1.6)})
    end
    M.sync(model,true)
    return true
end

function M.event(model, event,localModel)
    if event.kind=="visibility" then
        model.visible=event.visible==true;M.sync(model,model.opened);return false
    elseif event.kind=="panel" or event.kind=="popover" then
        if event.kind~=model.surface then return false end
        if event.action=="opened" then M.sync(model,true)
        elseif event.action=="closed" then M.sync(model,false);model.draft=nil;model.dragAborted=nil;model.surface=nil end
        return false
    elseif event.kind=="settings.changed" then
        M.sync(model,model.opened);return false
    elseif event.kind=="task.complete" then
        local pending=model.pending[event.taskId]
        if not pending then return false end
        model.pending[event.taskId]=nil
        if event.ok~=true and not pending.superseded then
            model.optimistic=nil;model.expectedEndpoint=nil;model.expectedUntil=nil;model.error=event.error or "failed"
        end
        model.api.invalidate();return true
    elseif event.kind=="data.change" then
        local current=M.state(model)
        for id,pending in pairs(model.pending) do
            if pending.name=="audio.output.setVolume" and pending.endpoint~=current.id then
                model.api.cancel(id);model.pending[id]=nil
            end
        end
        if model.opened then model.api.invalidate() end
        return false
    end
    if event.kind~="action" or type(event.id)~="string" or event.id:sub(1,13)~="system.audio." then return false end
    if model.preview then return true end
    local state=M.state(model)
    if event.id=="system.audio.open" or event.id=="system.audio.menu" then
        return M.open(model,event.id=="system.audio.open" and "system.audio.open" or "aurora.surface",
            localModel and localModel.source=="local")
    elseif event.id=="system.audio.close" then
        if model.surface=="popover" then model.api.closePopover() else model.api.closePanel() end
    elseif event.id=="system.audio.permissions" then model.api.openSettings()
    elseif event.id=="system.audio.volume.wheel" and state.canVolume and finite(event.delta) then
        local scalar=clamp(state.volume+math.max(-1200,math.min(1200,event.delta))/120*0.02)
        if math.abs(scalar-state.volume)>0.000001 then send(model,"audio.output.setVolume",{volume=scalar},state.id) end
    elseif event.id=="system.audio.volume" and state.canVolume and finite(event.controlValue) then
        local scalar=clamp(event.controlValue)
        if event.source=="pointer" then
            if model.dragAborted then return true end
            if model.draft and model.draft.endpoint~=state.id then
                model.draft=nil;model.dragAborted=true;return true
            end
            model.draft={volume=scalar,endpoint=state.id}
            if state.coalesced and event.trustedGesture~=false then send(model,"audio.output.setVolume",{volume=scalar},state.id) end
        else send(model,"audio.output.setVolume",{volume=scalar},state.id) end
    elseif event.id=="system.audio.volume.commit" then
        local draft=model.draft;model.draft=nil
        model.dragAborted=nil
        if draft and state.canVolume and draft.endpoint==state.id then
            send(model,"audio.output.setVolume",{volume=draft.volume},state.id)
        end
    elseif event.id=="system.audio.mute" and state.canMute and type(event.checked)=="boolean" then
        send(model,"audio.output.setMute",{muted=event.checked},state.id)
    elseif event.id=="system.audio.mute.toggle" and state.canMute then
        send(model,"audio.output.setMute",{muted=not state.muted},state.id)
    elseif event.id=="system.audio.device" and state.canChoose then
        local selection=event.selection or event.value
        for _,device in ipairs(state.devices) do
            if device.id==selection and device.id~=state.selected then
                send(model,"audio.output.selectDevice",{endpointId=device.id},device.id);break
            end
        end
    end
    model.api.invalidate()
    return true
end

function M.view(model,localModel)
    local state=M.state(model)
    local row=ui.metrics().layoutRowHeight
    local c=panel.theme()
    local children={}
    if model.surface~="panel" then children[1]=view.row({key="system.audio.header",width="fill",height=row*1.15,
        alignItems="center",children={
            panel.text("system.audio.title",l10n.tr("shared.audio"),row,{fontSize=row*0.68,fontWeight=700,height=row}),
            panel.iconButton("system.audio.close",0xF00D,l10n.tr("shared.close"),row*0.85,"system.audio.close")}}) end
    if state.volumeSupported then
        local scalar=state.volume
        if model.draft and model.draft.endpoint==state.id then scalar=model.draft.volume end
        children[#children+1]=panel.volume("system.audio.volume",l10n.tr("shared.system_volume"),scalar,row,
            "system.audio.volume","system.audio.volume.commit",
            panel.iconButton("system.audio.mute",state.muted and 0xF6A9 or 0xF028,l10n.tr("shared.mute"),row*1.1,
                "system.audio.mute.toggle",state.canMute,state.muted),state.canVolume)
    end
    if localModel and localModel.source=="local" then
        children[#children+1]=panel.volume("music.volume",l10n.tr("music.volume"),localModel.player.volume,row,
            "local.volume","local.volume.commit",nil,widget.hasPermission("audio.playback"))
    end
    if state.devicesSupported then
        children[#children+1]=view.column({key="system.audio.output.heading",width="fill",height="auto",gap=row*0.08,children={
            panel.text("system.audio.output.label",l10n.tr("shared.output_devices"),row,{fontWeight=600,fontSize=row*0.49}),
            panel.text("system.audio.output.scope",l10n.tr("shared.output_scope"),row,{fontSize=row*0.36,
                height=row*0.55,style={foreground=c.secondary}})}})
        local devices={}
        for index,device in ipairs(state.devices) do
            local key=model.deviceKeys[device.id]
            local chosen=device.id==state.selected
            local label=device.name~="" and device.name or device.id
            if model.api.hasFeature("view.collection.basic") then
                devices[#devices+1]=view.listItem({key=key,width="fill",height=row*1.65,enabled=state.canChoose,
                    padding={horizontal=row*0.4},style={background=chosen and c.selected or c.surface,
                        cornerRadius=row*0.38,borderWidth=0},events={click={id="system.audio.device",value=device.id}},
                    accessibility={role="button",label=label..(chosen and " · "..l10n.tr("shared.current") or "")},children={
                        view.row({key=key..".row",width="fill",height="fill",gap=row*0.28,alignItems="center",children={
                            panel.icon(key..".icon",0xF028,row,chosen and c.accent or c.secondary),
                            panel.text(key..".name",label,row,{fontSize=row*0.46}),
                            chosen and panel.icon(key..".check",0xF00C,row*0.7,c.accent) or
                                view.spacer({key=key..".space",width=row*0.7,height=row*0.7}),
                        }})}})
            else
                devices[#devices+1]=panel.choice(key,label,row,"system.audio.device",chosen,state.canChoose,device.id)
            end
        end
        if #devices==0 then devices[1]=panel.text("system.audio.devices.empty",l10n.tr("shared.unavailable"),row,
            {height=row*1.65,style={foreground=c.secondary}}) end
        if #state.devices>0 and model.api.hasFeature("view.collection.basic") then
            children[#children+1]=view.list({key="system.audio.devices",width="fill",height=row*(#devices*1.85-0.2),
                gap=row*0.2,children=devices})
        else
            children[#children+1]=view.column({key="system.audio.devices.fallback",width="fill",height="auto",gap=row*0.2,children=devices})
        end
    end
    if state.needsPermission then
        children[#children+1]=panel.choice("system.audio.permissions",l10n.tr("shared.permissions"),row,
            "system.audio.permissions",false)
    elseif model.error then
        children[#children+1]=panel.text("system.audio.error",l10n.tr("shared.error"),row,{style={foreground=c.secondary}})
    end
    return view.scroll({key="system.audio.scroll",width="fill",height="fill",children={
        view.column({key="system.audio.settings",width="fill",height="auto",padding=row*0.3,gap=row*0.45,children=children})}})
end

function M.dispose(model)
    model.visible=false
    M.sync(model,false)
    for id in pairs(model.pending) do model.api.cancel(id) end
    model.pending={}
end

return M
