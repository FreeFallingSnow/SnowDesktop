local options=module.require("modules/playback_options.lua")
local function session(controls)
    return {id="external",playbackStatus="playing",controls=controls or {}}
end
return {
    ["unavailable or unsupported system media hides all mode controls"]=function()
        for _,value in ipairs({{},session(),session({canChangePlaybackRate=false})}) do
            local state=options.state(value,true)
            assert(not state.rate and not state.shuffle and not state.repeatMode)
        end
        local state=options.state(nil,true)
        assert(not state.id)
        state=options.state(session({canToggleShuffle=true}),false)
        assert(not state.shuffle)
    end,
    ["partial SMTC support remains independent and unknown state stays unknown"]=function()
        local value=session({canToggleShuffle=true})
        local state=options.state(value,true)
        assert(state.shuffle and not state.rate and not state.repeatMode and state.shuffleValue==nil)
        value.shuffle=false;value.repeatMode="list";value.playbackRate=1.25
        state=options.state(value,true)
        assert(state.shuffleValue==false and state.repeatValue=="list" and state.rateValue==1.25)
        value.shuffle=true;value.playbackRate=0/0;value.repeatMode="unknown"
        state=options.state(value,true)
        assert(state.shuffleValue==true and state.rateValue==nil and state.repeatValue==nil)
    end,
    ["commands use the bound current session and recheck capabilities and permission"]=function()
        local value=session({canToggleShuffle=true,canChangeRepeatMode=true,canChangePlaybackRate=true})
        local name,args=options.command(value,true,{id="system.shuffle.off",value="external"})
        assert(name=="media.setShuffle" and args.shuffle==false and args.sessionId=="external")
        name,args=options.command(value,true,{id="system.repeat.track",value="external"})
        assert(name=="media.setRepeat" and args.mode=="track")
        name,args=options.command(value,true,{id="system.rate.1_25",value="external"})
        assert(name=="media.setRate" and args.rate==1.25)
        assert(not options.command(value,true,{id="system.rate.99",value="external"}))
        assert(not options.command(value,true,{id="system.repeat.other",value="external"}))
        assert(not options.command(value,true,{id="system.shuffle.on",value="retired"}))
        assert(not options.command(value,false,{id="system.shuffle.on",value="external"}))
        value.controls.canToggleShuffle=false
        assert(not options.command(value,true,{id="system.shuffle.on",value="external"}))
    end,
    ["local switching requests pause only for a playing pausable external session"]=function()
        local value=session({canPause=true})
        local name,args=options.pauseRequest(value,true)
        assert(name=="media.pause" and args.sessionId=="external")
        assert(not options.pauseRequest(value,false))
        for _,status in ipairs({"paused","stopped","closed","changing"}) do
            value.playbackStatus=status;assert(not options.pauseRequest(value,true))
        end
        value.playbackStatus="playing";value.controls={canPlayPause=true}
        assert(not options.pauseRequest(value,true))
        assert(not options.pauseRequest(nil,true))
    end,
}
