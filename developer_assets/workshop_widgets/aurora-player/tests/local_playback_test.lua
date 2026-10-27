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

-- Keep the actual panel/event/queue code; replace only host presentation and
-- system tasks so filtered rows must still commit to the real queue identity.
local function playlistFixture()
    local f={calls={},scrolls={}}
    local p=queue.new({start=function(name,args) f.calls[#f.calls+1]={name=name,args=args};return #f.calls end,
        cancel=function() end,save=function() return true end})
    p:ingest({{handle="a",kind="file",name="Alpha [live].mp3"},
        {handle="b",kind="file",name="Hidden.flac"},{handle="c",kind="file",name="ALPHA piano.flac"},
        {handle="d",kind="file",name="海边 散步.mp3"},{handle="e",kind="file",name="Alpha studio.mp3"}}) -- l10n-allow: Chinese filename fixture verifies Unicode search.
    f.model={player=p,source="local",panelKind="playlist"}
    widget={hasFeature=function() return true end,hasPermission=function() return true end,
        invalidate=function() end,theme=function() return {contentTheme=0} end}
    ui={metrics=function() return {layoutRowHeight=28} end}
    layout={contentHeight=function() return 360 end}
    l10n={tr=function(key) return key end}
    interaction={isPressed=function() return true end}
    view={}
    for _,name in ipairs({"row","column","spacer","button","iconButton","icon","text","listItem","stack","virtualList","searchBox"}) do
        local nodeType=name;view[name]=function(spec) spec.type=nodeType;return spec end
    end
    view.virtualRange=function(spec) return {offset=0,firstIndex=spec.itemCount>0 and 1 or 0,lastIndex=spec.itemCount} end
    view.scrollTo=function(key,offset) f.scrolls[#f.scrolls+1]={key=key,offset=offset} end
    function f:find(node,key)
        if node.key==key then return node end
        for _,child in ipairs(node.children or {}) do local found=self:find(child,key);if found then return found end end
    end
    function f:panel() return localPlayback.panel({},self.model) end
    function f:action(id,fields)
        local e=fields or {};e.kind="action";e.id="local."..id
        assert(localPlayback.event({},self.model,e))
    end
    return f
end

return {
    ["playlist search matches words literally and preserves the original queue"] = function()
        local f=playlistFixture();local p=f.model.player
        f:action("search",{text="  ALPHA  [live] "})
        local indices,active=localPlayback.filtered(f.model)
        assert(active and #indices==1 and indices[1]==1 and #p.tracks==5)
        f:action("search",{text="海边 散步"}) -- l10n-allow: Query matches the Chinese filename fixture.
        indices=localPlayback.filtered(f.model);assert(#indices==1 and indices[1]==4)
        f:action("search",{text="["});indices=localPlayback.filtered(f.model)
        assert(#indices==1 and indices[1]==1,"search punctuation must not become a Lua pattern")
        f:action("search",{text="not present"})
        local tree=f:panel();local list=f:find(tree,"playlist.list")
        assert(list.itemCount==0 and f:find(list.emptyContent,"playlist.empty").text=="music.no_matches")
        f:action("search.clear")
        assert(#localPlayback.filtered(f.model)==5 and f.model.searchText=="")
        assert(#f.scrolls==5 and f.scrolls[1].offset==0 and #f.calls==0)
    end,
    ["filtered panel row actions play and remove the actual matching song"] = function()
        local f=playlistFixture();local p=f.model.player
        local key=p.tracks[3].key
        f:action("search",{text="piano"})
        local tree=f:panel();local list=f:find(tree,"playlist.list")
        assert(list.itemCount==1 and list.children[1].key==key)
        local action=list.children[1].events.click
        f:action(action.id:sub(7),{value=action.value})
        assert(p.index==3 and f.calls[#f.calls].name=="audio.play" and f.calls[#f.calls].args.handle=="c")
        f:action("track.remove",{value=key})
        assert(#p.tracks==4 and p.tracks[1].name=="Alpha [live].mp3" and p.tracks[2].name=="Hidden.flac")
        assert(#localPlayback.filtered(f.model)==0)
        tree=f:panel();f:action("clear")
        assert(#p.tracks==0 and #p.sources==0 and f.model.searchText=="","clear always empties the full queue, including hidden songs")
    end,
    ["filtered dragging moves by visible anchors and retains playing identity"] = function()
        local f=playlistFixture();local p=f.model.player
        p:play(3);p:complete({taskId=p.playingTask,ok=true,value={playbackId="playing"}})
        local moved=p.tracks[1].key
        f:action("search",{text="alpha"});f:panel()
        f:action("drag.start",{value=moved,button=1,y=f.model.listTop+10})
        f:action("drag.move",{y=f.model.listTop+f.model.itemExtent*2.7})
        assert(f.model.drag.to==5 and f.model.drag.boundary==3)
        f:action("drag.end")
        assert(p.tracks[5].key==moved and p.tracks[1].name=="Hidden.flac" and p.tracks[3].name=="海边 散步.mp3") -- l10n-allow: Assert the Unicode fixture remains in queue order.
        assert(p.index==2 and p.playbackId=="playing" and f.model.suppressClick)
        f:action("track",{value=moved});assert(p.index==2,"a completed drag must not also play the clicked row")
        f:panel();f:action("drag.start",{value=moved,button=1,y=f.model.listTop+70})
        f:action("drag.move",{y=f.model.listTop-10});assert(f.model.drag.to==2 and f.model.drag.boundary==0)
        f:action("drag.end");assert(p.tracks[2].key==moved and p.index==3)
    end,
    ["search and library changes cancel stale drag commits"] = function()
        local f=playlistFixture();local p=f.model.player;local key=p.tracks[1].key
        f:panel();f:action("drag.start",{value=key,button=1,y=f.model.listTop+10})
        f:action("drag.move",{y=f.model.listTop+100})
        f:action("search",{text="piano"});f:action("drag.end")
        assert(p.tracks[1].key==key and f.model.drag==nil)
        f:action("search.clear");f:panel()
        f:action("drag.start",{value=key,button=1,y=f.model.listTop+10})
        p:append("new","New.mp3")
        f:action("drag.move",{y=f.model.listTop+100});f:action("drag.end")
        assert(p.tracks[1].key==key and #p.tracks==6 and f.model.drag==nil)
    end,
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
