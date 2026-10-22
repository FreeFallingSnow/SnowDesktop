local localPlayback=module.require("modules/local_playback.lua")
local queue=module.require("modules/local_queue.lua")
local timeline=module.require("modules/player.lua")

local function fixture()
    local calls, canceled, saved={}, {}, 0
    local p=queue.new({
        start=function(name,args)
            local id=#calls+1
            calls[id]={name=name,args=args}
            return id
        end,
        cancel=function(id) canceled[#canceled+1]=id end,
        save=function() saved=saved+1 end,
    })
    p:ingest({{handle="a",kind="file",name="First.flac"},
        {handle="b",kind="file",name="Second.opus"}})
    return p,calls,canceled,function() return saved end
end

return {
    ["switching to local attempts system pause before playback and does not block on failure"]=function()
        local p,calls=fixture()
        local attempted=0
        local model={player=p,source="system",beforeLocal=function()
            assert(#calls==0);attempted=attempted+1;error("pause rejected")
        end}
        local host={available=function() return true end,persist=function() end}
        assert(localPlayback.playTrack(model,p.tracks[1].key,host))
        assert(model.source=="local" and attempted==1 and #calls==1)
        assert(localPlayback.select(model,"local",host) and attempted==1)
    end,
    ["playing a library item selects local ownership before starting audio"] = function()
        local p,calls=fixture()
        local model={player=p,source="system"}
        local persisted
        local host={available=function() return true end,persist=function(source)
            assert(#calls==0);persisted=source
        end}
        assert(localPlayback.playTrack(model,p.tracks[2].key,host))
        assert(model.source=="local" and persisted=="local" and p.index==2 and p.playingTask)
        assert(#calls==1)
    end,
    ["stale library keys and missing capabilities preserve the system source without starting audio"] = function()
        local p,calls=fixture()
        local model={player=p,source="system"}
        local host={available=function() return false end,persist=function() error("must not persist") end}
        assert(not localPlayback.playTrack(model,"removed",host))
        assert(not localPlayback.playTrack(model,p.tracks[1].key,host))
        assert(model.source=="system" and #calls==0 and not p.playingTask)
    end,
    ["playlist insertion uses item midpoints and matches final move indices"] = function()
        local target,boundary=queue.dropTarget(1,4,124,50)
        assert(target==2 and boundary==2)
        target,boundary=queue.dropTarget(1,4,126,50)
        assert(target==3 and boundary==3)
        target,boundary=queue.dropTarget(4,4,74,50)
        assert(target==2 and boundary==1)
        target,boundary=queue.dropTarget(4,4,-100,50)
        assert(target==1 and boundary==0)
        target,boundary=queue.dropTarget(1,4,9999,50)
        assert(target==4 and boundary==4)
        target,boundary=queue.dropTarget(2,4,60,50)
        assert(target==2 and boundary==nil)
        local p=fixture()
        p:append("c","Third.mp3");p:append("d","Fourth.wav")
        local moved=p.tracks[4]
        target=queue.dropTarget(4,4,74,50)
        p:move(4,target)
        assert(p.tracks[2]==moved and p.tracks[1].name=="First.flac")
    end,
    ["switching back to system stops owned audio before saving the source"] = function()
        local p,calls=fixture()
        p:play(1)
        local playId=p.playingTask
        p:complete({taskId=playId,ok=true,value={playbackId="owned"}})
        local model={player=p,source="local"}
        local persisted
        assert(localPlayback.select(model,"system",{available=function() return true end,
            persist=function(source)
                assert(p.playbackId==nil and calls[#calls].name=="audio.stop")
                assert(calls[#calls].args.playbackId=="owned")
                persisted=source
            end}))
        assert(model.source=="system" and persisted=="system" and #p.tracks==2)
    end,
    ["late play completions cannot restart local audio after switching sources"] = function()
        local p,calls,canceled=fixture()
        p:play(1)
        local playId=p.playingTask
        local model={player=p,source="local"}
        localPlayback.select(model,"system",{available=function() return true end,persist=function() end})
        assert(canceled[1]==playId)
        p:complete({taskId=playId,ok=true,value={playbackId="late"}})
        assert(p.playbackId==nil and calls[#calls].name=="audio.stop")
        assert(calls[#calls].args.playbackId=="late" and model.source=="system")
    end,
    ["unavailable local capabilities preserve the existing system source"] = function()
        local stopped,persisted=0,0
        local model={source="system",player={stop=function() stopped=stopped+1 end}}
        local host={available=function() return false end,persist=function() persisted=persisted+1 end}
        assert(not localPlayback.select(model,"local",host))
        assert(localPlayback.select(model,"system",host))
        assert(model.source=="system" and stopped==0 and persisted==0)
    end,
    ["reordering or removing another item keeps the playing local track"] = function()
        local p,calls,savedCanceled,saved=fixture()
        p:play(1)
        p:complete({taskId=p.playingTask,ok=true,value={playbackId="current"}})
        local before=#calls
        p:move(1,2)
        assert(p.index==2 and p.tracks[2].name=="First.flac" and p.playbackId=="current")
        p:remove(1)
        assert(p.index==1 and p.tracks[1].name=="First.flac" and p.playbackId=="current")
        assert(saved()>=3 and #savedCanceled==0)
        for i=before+1,#calls do assert(calls[i].name~="audio.stop") end
    end,
    ["local rate changes advance both playback and optimistic seek at that rate"] = function()
        local value={positionMs=5000,durationMs=60000,maximumSeekMs=60000,
            updatedAtMs=1000,playbackRate=2}
        assert(timeline.position(value,true,2500)==8000)
        assert(timeline.optimisticSeekPosition(value,10000,true,1000,2500)==13000)
        value.playbackRate=0.5
        assert(timeline.position(value,true,3000)==6000)
        value.playbackRate=nil
        assert(timeline.position(value,true,3000)==7000)
    end,
}
