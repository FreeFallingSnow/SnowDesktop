local shared=module.require("modules/system_audio.lua")

local function fixture()
    local f={snapshots={},features={},permissions={},calls={},unsubscribed={},now=1000,opened=0}
    local api={
        hasFeature=function(k) return f.features[k]~=false end,
        hasPermission=function(k) return f.permissions[k]~=false end,
        subscribe=function(k,options)
            assert(options.maxAgeMs==1000 and options.whenHidden=="pause")
            return {value=function() return f.snapshots[k] end,
                unsubscribe=function() f.unsubscribed[k]=true end}
        end,
        start=function(k,args)
            local id=#f.calls+1;f.calls[id]={name=k,args=args};return id
        end,
        cancel=function(id) f.canceled=id end,
        now=function() return f.now end,
        invalidate=function() end,
        openSettings=function() f.opened=f.opened+1 end,
        openPopover=function(options) f.popover=options;return f.allowPopover~=false end,
        openPanel=function(options) f.panel=options end,
        closePopover=function() f.closed="popover" end,
        closePanel=function() f.closed="panel" end,
        translate=function(key) return key end,
        rowHeight=function() return 28 end,
    }
    f.model=shared.new({preview=false},api)
    function f:set(topic,data) self.snapshots[topic]={available=true,value=data} end
    f:set("audio.output.default",{id="speakers",name="Speakers"})
    f:set("audio.output.volume",{endpointId="speakers",volume=0.6,muted=false})
    f:set("audio.devices",{devices={
        {id="speakers",name="Speakers",direction="output",available=true,state="active"},
        {id="headphones",name="Headphones",direction="output",available=true,state="active"},
        {id="microphone",direction="input",available=true,state="active"},
        {id="gone",direction="output",available=false,state="unplugged"},
        {id="speakers",name="Duplicate",direction="output",available=true,state="active"},
    }})
    function f:open()
        shared.open(self.model,"system.audio.open",false)
        shared.event(self.model,{kind=self.model.surface,action="opened"})
    end
    function f:action(id,fields)
        local event=fields or {};event.kind="action";event.id="system.audio."..id
        assert(shared.event(self.model,event))
    end
    function f:complete(ok,id,error) shared.event(self.model,{kind="task.complete",taskId=id or #self.calls,ok=ok,error=error or "deviceGone"}) end
    return f
end

return {
    ["sound opens a titleless anchored popover with panel fallback and no playback changes"] = function()
        local f=fixture();f:open()
        assert(f.popover.anchorKey=="system.audio.open" and f.popover.title==nil and f.model.surface=="popover")
        assert(f.popover.width==364 and f.popover.height<400 and f.popover.dismissOnOutside and f.popover.dismissOnEscape)
        assert(#f.calls==0)
        f:action("close");assert(f.closed=="popover")
        shared.event(f.model,{kind="popover",action="closed"})
        f.features["widget.popover"]=false;f:open()
        assert(f.model.surface=="panel" and f.panel.width==372)
        f:action("close");assert(f.closed=="panel")
    end,
    ["volume stays ready for the exposed wheel while device discovery follows the sound popup"] = function()
        local f=fixture()
        shared.event(f.model,{kind="panel",action="opened"})
        assert(f.model.subscriptions["audio.output.volume"] and not f.model.subscriptions["audio.devices"])
        f:open();shared.event(f.model,{kind="panel",action="closed"})
        assert(f.model.opened and f.model.surface=="popover")
        f:action("open");assert(f.model.surface=="popover" and #f.calls==0)
        shared.event(f.model,{kind="popover",action="closed"})
        assert(f.model.subscriptions["audio.output.volume"] and not f.model.subscriptions["audio.devices"] and f.model.surface==nil)
    end,
    ["device rows use validated tokens and mute buttons toggle the current system state"] = function()
        local f=fixture();f:open();f:action("device",{value="headphones"})
        assert(f.calls[1].name=="audio.output.selectDevice" and f.calls[1].args.endpointId=="headphones")
        f:complete(false);f:action("mute.toggle")
        assert(f.calls[2].name=="audio.output.setMute" and f.calls[2].args.muted==true)
    end,
    ["visible widgets keep shared volume ready and release subscriptions when hidden"] = function()
        local f=fixture();assert(f.model.subscriptions["audio.output.volume"] and not f.model.subscriptions["audio.devices"])
        f:open();assert(f.model.subscriptions["audio.output.volume"])
        f.permissions["audio.devices.read"]=false
        shared.event(f.model,{kind="settings.changed"})
        assert(f.unsubscribed["audio.devices"] and not f.model.subscriptions["audio.devices"])
        shared.event(f.model,{kind="popover",action="closed"})
        shared.event(f.model,{kind="visibility",visible=false})
        assert(next(f.model.subscriptions)==nil and #f.calls==0)
        shared.event(f.model,{kind="visibility",visible=true})
        assert(f.model.subscriptions["audio.output.volume"] and not f.model.subscriptions["audio.devices"])
    end,
    ["system controls accept only matching endpoint volume and active output device tokens"] = function()
        local f=fixture();f:open()
        local state=shared.state(f.model)
        assert(state.canVolume and state.volume==0.6 and #state.devices==2)
        f:set("audio.output.volume",{endpointId="old",volume=0.9,muted=true})
        state=shared.state(f.model);assert(state.canVolume==false and state.volume==nil)
        f:action("volume",{controlValue=0.2});f:action("mute",{checked=true})
        f:action("device",{selection="gone"});f:action("device",{selection="microphone"})
        assert(#f.calls==0)
    end,
    ["legacy hosts commit pointer volume on release without sending the new parameter"] = function()
        local f=fixture();f:open()
        f.features["task.audio.output.coalescedVolume"]=false
        f:action("volume",{source="pointer",controlValue=0.4})
        f:action("volume",{source="pointer",controlValue=0.3});assert(#f.calls==0)
        f:action("volume.commit")
        assert(#f.calls==1 and f.calls[1].name=="audio.output.setVolume" and f.calls[1].args.volume==0.3)
        assert(f.calls[1].args.coalesce==nil)
        f:complete(true)
        f:action("volume",{source="pointer",controlValue=0.1})
        f:set("audio.output.default",{id="headphones"})
        f:set("audio.output.volume",{endpointId="headphones",volume=0.8})
        f:action("volume.commit");assert(#f.calls==1)
    end,
    ["keyboard volume mute and device actions use typed shared system tasks"] = function()
        local f=fixture();f:open();f:action("volume",{source="keyboard",controlValue=0.8})
        assert(f.calls[1].args.volume==0.8 and shared.state(f.model).busy)
        assert(shared.state(f.model).canVolume and shared.state(f.model).canChoose)
        f:action("mute",{checked=true});assert(#f.calls==2)
        f:complete(true,1)
        assert(f.calls[2].name=="audio.output.setMute" and f.calls[2].args.muted==true)
        f:complete(true);f:action("device",{selection="headphones"})
        assert(f.calls[3].name=="audio.output.selectDevice" and f.calls[3].args.endpointId=="headphones")
        f:complete(true);assert(shared.state(f.model).canVolume==false)
        f:set("audio.output.default",{id="headphones"})
        f:set("audio.output.volume",{endpointId="headphones",volume=0.2,muted=false})
        assert(shared.state(f.model).canVolume and shared.state(f.model).volume==0.2)
    end,
    ["the active output is first and other outputs retain provider order and stable keys"] = function()
        local f=fixture();f:open()
        local before=shared.state(f.model)
        local key=f.model.deviceKeys and f.model.deviceKeys.headphones
        f:set("audio.output.default",{id="headphones",name="Headphones"})
        f:set("audio.output.volume",{endpointId="headphones",volume=0.5})
        local state=shared.state(f.model)
        assert(state.devices[1].id=="headphones" and state.devices[2].id=="speakers")
        assert(state.selected=="headphones" and f.model.deviceKeys.headphones==key)
        assert(f.snapshots["audio.devices"].value.devices[1].id=="speakers")
        assert(before.devices[1].id=="speakers")
    end,
    ["dragging streams the latest volume without disabling controls or rolling back on superseded completions"] = function()
        local f=fixture();f:open()
        f:action("volume",{source="pointer",controlValue=0.4,trustedGesture=true})
        f:action("volume",{source="pointer",controlValue=0.3,trustedGesture=true})
        assert(#f.calls==2 and f.calls[1].args.coalesce and f.calls[2].args.volume==0.3)
        assert(shared.state(f.model).canVolume and shared.state(f.model).canChoose)
        f:complete(false,1,"canceled")
        assert(shared.state(f.model).volume==0.3 and f.model.error==nil)
        f:action("volume.commit")
        assert(f.calls[3].args.volume==0.3 and f.calls[3].args.coalesce)
        f:set("audio.output.default",{id="headphones"})
        f:set("audio.output.volume",{endpointId="headphones",volume=0.8})
        shared.event(f.model,{kind="data.change"})
        assert(f.canceled and next(f.model.pending)==nil)
    end,
    ["exposed wheel changes shared system volume while closed and accumulates rapid and fractional deltas"] = function()
        local f=fixture()
        f:action("volume.wheel",{delta=120})
        f:action("volume.wheel",{delta=120})
        assert(#f.calls==2,"each wheel gesture submits shared system volume while the popup is closed")
        assert(math.abs(f.calls[2].args.volume-0.64)<0.00001 and f.model.surface==nil)
        assert(not f.model.subscriptions["audio.devices"])
        f:action("volume.wheel",{delta=-30})
        assert(math.abs(f.calls[3].args.volume-0.635)<0.00001)
        f:action("volume.wheel",{delta=0});assert(#f.calls==3)
        f.permissions["audio.output.control"]=false
        f:action("volume.wheel",{delta=120});assert(#f.calls==3)
    end,
    ["small wheel increments accumulate before the shared output snapshot catches up"] = function()
        local f=fixture()
        f:action("volume.wheel",{delta=15})
        f:action("volume.wheel",{delta=15})
        assert(#f.calls==2 and math.abs(f.calls[1].args.volume-0.6025)<0.000001)
        assert(math.abs(f.calls[2].args.volume-0.605)<0.000001,"stale snapshots must not swallow fractional wheel increments")
        f:action("volume.wheel",{delta=-15})
        assert(math.abs(f.calls[3].args.volume-0.6025)<0.000001)
    end,
    ["untrusted pointer movement keeps a draft until release and a changed endpoint aborts that drag"] = function()
        local f=fixture();f:open()
        f:action("volume",{source="pointer",controlValue=0.2,trustedGesture=false})
        assert(#f.calls==0)
        f:set("audio.output.default",{id="headphones"})
        f:set("audio.output.volume",{endpointId="headphones",volume=0.8})
        f:action("volume",{source="pointer",controlValue=0.1,trustedGesture=true})
        f:action("volume.commit");assert(#f.calls==0)
    end,
    ["permission denial stays read only and opens component permission settings"] = function()
        local f=fixture();f:open()
        f.permissions["audio.output.control"]=false;f.permissions["audio.devices.control"]=false
        f:action("volume",{controlValue=0.1});f:action("mute",{checked=true})
        f:action("device",{selection="headphones"})
        assert(#f.calls==0 and shared.state(f.model).volume==0.6 and shared.state(f.model).needsPermission)
        f:action("permissions");assert(f.opened==1)
    end,
    ["failed shared actions roll back predictions without retrying or changing local playback"] = function()
        local f=fixture();f:open();f:action("volume",{source="keyboard",controlValue=0.2})
        assert(shared.state(f.model).volume==0.2)
        f:complete(false);assert(shared.state(f.model).volume==0.6 and not shared.state(f.model).busy)
        shared.event(f.model,{kind="data.change"});shared.event(f.model,{kind="schedule"})
        assert(#f.calls==1 and f.model.error=="deviceGone")
    end,
    ["external changes supersede acknowledged optimistic values and stale device selection expires"] = function()
        local f=fixture();f:open();f:action("mute",{checked=true});f:complete(true)
        f:set("audio.output.volume",{endpointId="speakers",volume=0.6,muted=true})
        assert(shared.state(f.model).muted)
        f:set("audio.output.volume",{endpointId="speakers",volume=0.6,muted=false})
        assert(not shared.state(f.model).muted)
        f:action("device",{selection="headphones"});f:complete(true)
        f.now=f.now+3000;assert(shared.state(f.model).canVolume)
    end,
    ["disposing shared controls cancels pending tasks and subscriptions"] = function()
        local f=fixture();f:open();f:action("mute",{checked=true})
        shared.dispose(f.model)
        assert(f.canceled==1 and next(f.model.pending)==nil and next(f.model.subscriptions)==nil)
    end,
}
