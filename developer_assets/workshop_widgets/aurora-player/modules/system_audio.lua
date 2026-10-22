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
        local allowed=model.opened and model.api.hasFeature(spec[2]) and model.api.hasPermission(spec[3])
        if allowed and not model.subscriptions[spec[1]] then
            model.subscriptions[spec[1]]=model.api.subscribe(spec[1],{maxAgeMs=1000,whenHidden="pause"})
        elseif not allowed and model.subscriptions[spec[1]] then
            model.subscriptions[spec[1]]:unsubscribe()
            model.subscriptions[spec[1]]=nil
        end
    end
end

function M.new(context, api)
    local model={subscriptions={},pending={},preview=context.preview==true,
        api=api or {
            hasFeature=function(name) return widget.hasFeature(name) end,
            hasPermission=function(name) return widget.hasPermission(name) end,
            subscribe=function(name,options) return data.subscribe(name,options) end,
            start=function(name,args) return task.start(name,args) end,
            cancel=function(id) task.cancel(id) end,
            now=function() return time.now() end,
            invalidate=function() widget.invalidate() end,
            openSettings=function() widget.openSettings() end,
        }}
    if model.preview then M.sync(model,true) end
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
    local optimistic=model.optimistic
    if optimistic and (optimistic.endpoint~=id or optimistic.expires<api.now()) then
        model.optimistic=nil;optimistic=nil
    end
    local scalar=matched and clamp(volume.volume) or nil
    local muted=matched and volume.muted==true or false
    if optimistic and matched then
        if optimistic.volume~=nil then
            if math.abs(optimistic.volume-scalar)>0.005 then scalar=optimistic.volume
            else optimistic.volume=nil end
        end
        if optimistic.muted~=nil then
            if optimistic.muted~=muted then muted=optimistic.muted else optimistic.muted=nil end
        end
    end
    local busy=next(model.pending)~=nil
    return {
        id=id,volume=scalar,muted=muted,devices=devices,
        selected=model.expectedEndpoint or id or "",busy=busy,
        volumeSupported=api.hasFeature("data.audio.output.default") and api.hasFeature("data.audio.output.volume"),
        devicesSupported=api.hasFeature("data.audio.devices"),
        canVolume=matched and api.hasFeature("task.audio.output.control") and api.hasPermission("audio.output.control") and not busy,
        canChoose=api.hasFeature("task.audio.devices.control") and api.hasPermission("audio.devices.control") and #devices>0 and not busy,
        needsPermission=not api.hasPermission("audio.output.read") or not api.hasPermission("audio.output.control") or
            not api.hasPermission("audio.devices.read") or not api.hasPermission("audio.devices.control"),
    }
end

local function send(model, name, args, endpoint)
    if model.preview or next(model.pending) then return false end
    local id,err=model.api.start(name,args)
    if not id then
        if err~="rateLimited" then model.error=err or "failed" end
        return false
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

function M.event(model, event)
    if event.kind=="panel" then
        if event.action=="opened" then M.sync(model,true)
        elseif event.action=="closed" then M.sync(model,false);model.draft=nil;model.deviceOpen=false end
        return false
    elseif event.kind=="settings.changed" then
        M.sync(model,model.opened);model.deviceOpen=false;return false
    elseif event.kind=="task.complete" then
        local pending=model.pending[event.taskId]
        if not pending then return false end
        model.pending[event.taskId]=nil
        if event.ok~=true then
            model.optimistic=nil;model.expectedEndpoint=nil;model.expectedUntil=nil;model.error=event.error or "failed"
        end
        model.api.invalidate();return true
    elseif event.kind=="data.change" then
        if model.opened then model.api.invalidate() end
        return false
    end
    if event.kind~="action" or type(event.id)~="string" or event.id:sub(1,13)~="system.audio." then return false end
    if model.preview then return true end
    local state=M.state(model)
    if event.id=="system.audio.permissions" then model.api.openSettings()
    elseif event.id=="system.audio.volume" and state.canVolume and finite(event.controlValue) then
        local scalar=clamp(event.controlValue)
        if event.source=="pointer" then model.draft={volume=scalar,endpoint=state.id}
        else send(model,"audio.output.setVolume",{volume=scalar},state.id) end
    elseif event.id=="system.audio.volume.commit" then
        local draft=model.draft;model.draft=nil
        if draft and state.canVolume and draft.endpoint==state.id then
            send(model,"audio.output.setVolume",{volume=draft.volume},state.id)
        end
    elseif event.id=="system.audio.mute" and state.canVolume and type(event.checked)=="boolean" then
        send(model,"audio.output.setMute",{muted=event.checked},state.id)
    elseif event.id=="system.audio.device.open" then
        model.deviceOpen=state.canChoose and event.expanded==true
    elseif event.id=="system.audio.device" and state.canChoose and type(event.selection)=="string" then
        model.deviceOpen=false
        for _,device in ipairs(state.devices) do
            if device.id==event.selection and device.id~=state.selected then
                send(model,"audio.output.selectDevice",{endpointId=device.id},device.id);break
            end
        end
    end
    model.api.invalidate()
    return true
end

function M.view(model)
    local state=M.state(model)
    if not state.volumeSupported and not state.devicesSupported then return nil end
    local row=ui.metrics().layoutRowHeight
    local function label(key,text)
        return view.text({key=key,text=text,width="fill",height=row*0.8,fontSize=row*0.43,
            style={foreground="textSecondary"},textWrap="noWrap"})
    end
    local children={}
    if state.volumeSupported then
        local scalar=state.volume
        if model.draft and model.draft.endpoint==state.id then scalar=model.draft.volume end
        children[#children+1]=label("system.audio.volume.label",l10n.tr("shared.system_volume")..
            (scalar~=nil and "  "..math.floor(scalar*100+0.5).."%" or ""))
        children[#children+1]=view.slider({key="system.audio.volume",width="fill",height=row,min=0,max=1,step=0.01,
            value=scalar or 0,enabled=state.canVolume,style={foreground="textPrimary"},
            events={change={id="system.audio.volume"},pointerUp={id="system.audio.volume.commit"}},
            accessibility={label=l10n.tr("shared.system_volume")}})
        if model.api.hasFeature("view.selectionControls") then
            children[#children+1]=view.checkbox({key="system.audio.mute",width="fill",height=row,
                label=l10n.tr("shared.mute"),checked=state.muted,enabled=state.canVolume,
                events={change={id="system.audio.mute"}},style={foreground="textPrimary"}})
        end
    end
    if state.devicesSupported then
        children[#children+1]=label("system.audio.device.label",l10n.tr("shared.output_device"))
        local options={}
        for index,device in ipairs(state.devices) do
            options[#options+1]={key="output."..index,value=device.id,label=device.name~="" and device.name or device.id}
        end
        local selected=""
        for _,option in ipairs(options) do if option.value==state.selected then selected=state.selected;break end end
        if #options==0 then
            options={{key="none",value="none",label=l10n.tr("shared.unavailable"),enabled=false}};selected="none"
        end
        children[#children+1]=view.select({key="system.audio.device",width="fill",height=row,
            selectedValue=selected,options=options,enabled=state.canChoose,expanded=model.deviceOpen==true and state.canChoose,
            events={click={id="system.audio.device.open"},change={id="system.audio.device"}},style={foreground="textPrimary"},
            accessibility={label=l10n.tr("shared.output_device")}})
    end
    if state.needsPermission then
        children[#children+1]=view.button({key="system.audio.permissions",width="fill",height=row,
            label=l10n.tr("shared.permissions"),events={click={id="system.audio.permissions"}}})
    elseif model.error then
        children[#children+1]=label("system.audio.error",l10n.tr("shared.error"))
    end
    return view.column({key="system.audio.settings",width="fill",height="auto",gap=row*0.12,children=children})
end

function M.dispose(model)
    M.sync(model,false)
    for id in pairs(model.pending) do model.api.cancel(id) end
    model.pending={}
end

return M
