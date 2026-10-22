local player = module.require("modules/local_queue.lua")
local panel=module.require("modules/panel_ui.lua")
local M = {}
local function readChunks(prefix)
    local count = tonumber(storage.get(prefix.."Count"))
    if not count then return nil end
    local result = {}
    for i=1,math.min(count,128) do
        for _, item in ipairs(storage.get(prefix..i) or {}) do result[#result+1]=item end
    end
    return result
end
local function savePlaylist(sources, tracks)
    local previous={source=tonumber(storage.get("sourceCount")) or 0,playlist=tonumber(storage.get("playlistCount")) or 0}
    local ok=pcall(storage.transaction,function(tx)
        for prefix, items in pairs({source=sources, playlist=tracks}) do
            local count=math.ceil(#items/16)
            for i=1,count do
                local chunk={}
                for j=(i-1)*16+1,math.min(i*16,#items) do
                    local item=items[j]
                    chunk[#chunk+1]={handle=item.handle,name=item.name,child=item.child,kind=item.kind}
                end
                tx:set(prefix..i,chunk)
            end
            for i=count+1,previous[prefix] do tx:remove(prefix..i) end
            tx:set(prefix.."Count",count)
        end
        tx:remove("sources")
    end)
    return ok
end
local function preferences(p)
    storage.transaction(function(tx)
        tx:set("shuffle",p.shuffle);tx:set("repeatMode",p.repeatMode);tx:set("playbackRate",p.playbackRate)
    end)
end
function M.setup(context)
    local p=player.new({start=function(name,args) return task.start(name,args) end,
        cancel=function(id) return task.cancel(id) end,save=savePlaylist,preferences=preferences})
    p.volume=math.max(0,math.min(1,tonumber(storage.get("volume")) or 1))
    p.playbackRate=math.max(0.25,math.min(4,tonumber(storage.get("playbackRate")) or 1))
    if M.available() then p:setMode(storage.get("shuffle")==true,storage.get("repeatMode")) end
    local model={player=p,preview=context.preview==true,panelKind="playlist",source="system"}
    if M.available() and storage.get("playbackSource")=="local" then model.source="local" end
    if model.preview then
        for _, key in ipairs({"music.preview_track","music.preview_second","music.preview_third"}) do
            p:append("preview."..key,l10n.tr(key))
        end
        p.index=1;p.status="paused";p.position=42000;p.duration=214000
        p.title=p.tracks[1].name;p.artist=l10n.tr("music.preview_artist");p.album=l10n.tr("music.preview_album")
        if storage.get("previewEmpty")=="1" then p.tracks={};p.index=0;p.title=nil;p.artist=nil;p.album=nil;p.position=0;p.duration=0 end
    elseif M.available() then
        local sources=readChunks("source") or storage.get("sources")
        local tracks=readChunks("playlist")
        if type(sources)=="table" then
            if tracks then p:restore(sources,tracks) else p:ingest(sources,true) end
        end
    end
    return model
end
local function text(key,value,size,color,extra)
    local spec={key=key,text=value,width="fill",height=size*1.5,fontSize=size,
        textWrap="noWrap",style={foreground=color}}
    for k,v in pairs(extra or {}) do spec[k]=v end
    return view.text(spec)
end
local function modeKey(p)
    return p.shuffle and "shuffle" or p.repeatMode=="track" and "loop" or p.repeatMode=="list" and "repeat" or "sequential"
end
local rates={0.25,0.5,0.75,1,1.25,1.5,2,3,4}
function M.panel(_context,model)
    local p=model.player
    if model.drag and not model.preview and widget.hasFeature("interaction.pointerActions") then
        local track=p.tracks[model.drag.from]
        if not track or not interaction.isPressed(track.key) then model.drag=nil end
    end
    local row=ui.metrics().layoutRowHeight
    local c=panel.theme()
    local primary,secondary,accent,surface=c.primary,c.secondary,c.accent,c.surface
    local function label(key,value) return panel.text(key,value,row,{style={foreground=c.secondary}}) end
    local function command(id,key,enabled) return panel.choice("music."..id,l10n.tr(key),row,"local."..id,false,enabled) end
    local children={}
    if model.panelKind=="options" then
        children[#children+1]=label("source.label",l10n.tr("workshop.aurora_player.source"))
        children[#children+1]=view.row({key="source.selector",width="fill",height=row*1.12,gap=row*0.2,children={
            panel.choice("source.system",l10n.tr("workshop.aurora_player.source_system"),row,"local.source.system",model.source=="system"),
            panel.choice("source.local",l10n.tr("workshop.aurora_player.source_local"),row,"local.source.local",model.source=="local",M.available()),
        }})
        local modes={}
        local labels={sequential="music.sequential",["repeat"]="music.repeat",loop="music.loop",shuffle="music.shuffle"}
        for _, keys in ipairs({{"sequential","repeat"},{"loop","shuffle"}}) do
            local choices={}
            for _,key in ipairs(keys) do choices[#choices+1]=panel.choice("mode."..key,l10n.tr(labels[key]),row,
                "local.mode."..key,modeKey(p)==key,M.available()) end
            modes[#modes+1]=view.row({key="mode.row."..keys[1],width="fill",height=row*1.12,gap=row*0.2,children=choices})
        end
        children[#children+1]=view.spacer({key="options.section.gap",width="fill",height=row*0.16})
        children[#children+1]=label("local.preferences",l10n.tr("music.local_settings"))
        local localSettings={label("mode.label",l10n.tr("music.mode")),modes[1],modes[2],
            view.row({key="rate.row",width="fill",height=row*1.2,gap=row*0.25,alignItems="center",children={
                panel.text("rate.label",l10n.tr("music.rate"),row),
                panel.iconButton("rate.down",0xF068,l10n.tr("music.slower"),row,"local.rate.down",M.available() and p.playbackRate>rates[1]),
                panel.text("rate.value",p.playbackRate.."×",row,{width=row*1.5,textAlign="center",fontWeight=600}),
                panel.iconButton("rate.up",0xF067,l10n.tr("music.faster"),row,"local.rate.up",M.available() and p.playbackRate<rates[#rates]),
            }})}
        children[#children+1]=panel.well("local.preferences.card",row,localSettings)
        children[#children+1]=command("playlist","music.playlist",M.available())
    else
        children[#children+1]=view.row({key="playlist.toolbar",width="fill",height=row*1.12,gap=row*0.2,alignItems="center",children={
            panel.text("playlist.subtitle",tostring(#p.tracks).."  "..l10n.tr("music.tracks"),row,{style={foreground=c.secondary}}),
            panel.iconButton("playlist.add.files",0xF067,l10n.tr("music.files"),row*1.1,"local.files",M.available()),
            panel.iconButton("playlist.add.folder",0xF07B,l10n.tr("music.folder"),row*1.1,"local.folder",M.available()),
        }})
        local extent=row*1.25
        local viewport=math.max(row,layout.contentHeight()-row*3.28)
        local range=view.virtualRange({key="playlist.list",itemCount=#p.tracks,itemExtent=extent,viewportExtent=viewport,overscan=1})
        model.listOffset=range.offset;model.itemExtent=extent
        model.listTop=row*1.7;model.listViewport=viewport
        local items={}
        for i=math.max(1,range.firstIndex),range.lastIndex do
            local track=p.tracks[i]
            local color=i==p.index and accent or primary
            local drag=model.drag
            local dragging=drag and drag.moved and drag.from==i
            local boundary=drag and drag.moved and drag.boundary
            local layers={view.column({key=track.key..".padding",width="fill",height="fill",padding={vertical=row*0.07},children={
                view.row({key=track.key..".row",width="fill",height="fill",padding={horizontal=row*0.45},gap=row*0.25,
                    alignItems="center",style={background=i==p.index and c.selected or c.surface,cornerRadius=row*0.3,borderWidth=0,opacity=dragging and 0.45 or 1},children={
                        text(track.key..".index",tostring(i),row*0.4,color,{width=row*1.5,textAlign="end"}),
                        text(track.key..".title",track.name,row*0.43,color),
                        view.icon({key=track.key..".grip",glyph=utf8.char(0xF0C9),iconFont="fa",width=row*0.5,height=row*0.6,
                            fontSize=row*0.35,style={foreground=secondary}}),
                    }})}})
            }
            if boundary==i or (boundary==0 and i==1) then
                layers[#layers+1]=view.column({key=track.key..".drop",width="fill",height="fill",
                    justifyContent=boundary==0 and "start" or "end",children={
                        view.row({key=track.key..".drop.line",width="fill",height=row*0.12,alignItems="center",children={
                            view.shape({key=track.key..".drop.head",shape="circle",width=row*0.12,height=row*0.12,style={background=accent}}),
                            view.box({key=track.key..".drop.rule",width="fill",height=row*0.075,style={background=accent,cornerRadius=row*0.04}}),
                        }})}})
            end
            items[#items+1]=view.listItem({key=track.key,width="fill",height=extent,style={borderWidth=0},
                capturePointer=true,events={click={id="local.track",value=track.key},contextMenu={id="local.track.menu",value=track.key},
                    pointerDown={id="local.drag.start",value=track.key},pointerMove={id="local.drag.move"},pointerUp={id="local.drag.end"}},
                accessibility={label=track.name},children={
                    view.stack({key=track.key..".layers",width="fill",height="fill",children=layers})}})
        end
        children[#children+1]=view.virtualList({key="playlist.list",width="fill",height=viewport,itemCount=#p.tracks,itemExtent=extent,
            firstIndex=range.firstIndex,children=items,emptyContent=label("playlist.empty",l10n.tr("music.empty"))})
        children[#children+1]=view.row({key="playlist.footer",width="fill",height=row,gap=row*0.2,alignItems="center",children={
            view.spacer({key="playlist.footer.space",width="fill",height=row}),
            view.button({key="music.clear",label=l10n.tr("music.clear"),width="auto",height=row,
                fontSize=row*0.41,padding={horizontal=row*0.4},enabled=#p.sources>0,
                style={foreground=c.secondary,background=c.surface,borderWidth=0,cornerRadius=row*0.3},
                events={click={id="local.clear"}}})}})
    end
    local content=view.column({key="playlist.surface",width="fill",height=model.panelKind=="options" and "auto" or "fill",
        padding=row*0.3,gap=row*0.28,children=children})
    if model.panelKind=="options" then
        return view.scroll({key="options.scroll",width="fill",height="fill",children={content}})
    end
    return content
end
local function indexOf(p,key)
    for i,track in ipairs(p.tracks) do if track.key==key then return i end end
end
function M.playTrack(model,key,host)
    local index=indexOf(model.player,key)
    if not index or not M.select(model,"local",host) then return false end
    model.player:play(index)
    return true
end
function M.event(_context,model,e)
    local p=model.player
    if model.preview then return false end
    if e.kind=="task.complete" then
        if not p.pending[e.taskId] then return false end
        p:complete(e)
    elseif e.kind=="audio.state" then
        if e.playbackId~=p.playbackId then return false end
        p:audio(e);model.updatedAtMs=time.now()
    elseif e.kind=="audio.command" then
        if e.playbackId~=p.playbackId then return false end
        p:command(e)
    elseif e.kind=="panel" and e.action=="closed" then model.drag=nil
    elseif e.kind=="action" then
        if type(e.id)~="string" or e.id:sub(1,6)~="local." then return false end
        local id=e.id:sub(7)
        if id=="source.system" or id=="source.local" then
            M.select(model,id=="source.local" and "local" or "system")
            widget.invalidate();return true
        end
        if id=="options" and widget.hasFeature("widget.panel") then
            model.panelKind=id;model.drag=nil
            local row=ui.metrics().layoutRowHeight
            widget.openPanel({title=l10n.tr("music.options"),width=math.floor(row*12.3+36),height=math.floor(row*11.8+74)})
            return true
        end
        if not M.available() then return true end
        if id=="files" or id=="folder" or id=="import" then
            if not M.allowed() then widget.openSettings();return true end
            M.select(model,"local")
        end
        if id=="import" and e.action=="fileDrop" then p:ingest(e.items)
        elseif id=="files" then p:pick(false)
        elseif id=="folder" then p:pick(true)
        elseif id=="toggle" then p:toggle()
        elseif id=="previous" then p:step(-1)
        elseif id=="next" then p:step(1)
        elseif id=="stop" then p:stop()
        elseif id=="seek" and tonumber(e.controlValue) then
            model.seekPreview=e.controlValue
            if e.source~="pointer" then p:seek(e.controlValue);model.seekPreview=nil end
        elseif id=="seek.commit" and model.seekPreview then p:seek(model.seekPreview);model.seekPreview=nil
        elseif id=="volume" and tonumber(e.controlValue) then
            p.volume=math.max(0,math.min(1,e.controlValue))
            if e.source~="pointer" then p:setVolume(p.volume);storage.set("volume",p.volume) end
        elseif id=="volume.commit" then p:setVolume(p.volume);storage.set("volume",p.volume)
        elseif id=="playlist" or id=="options" then
            model.panelKind=id;model.drag=nil
            local row=ui.metrics().layoutRowHeight
            widget.openPanel({title=l10n.tr("music.playlist"),width=math.floor(row*13.7+36),
                height=math.floor(row*(3.28+math.max(1,math.min(#p.tracks,7))*1.25)+74)})
        elseif id=="mode.sequential" then p:setMode(false,"none")
        elseif id=="mode.repeat" then p:setMode(false,"list")
        elseif id=="mode.loop" then p:setMode(false,"track")
        elseif id=="mode.shuffle" then p:setMode(true,"list")
        elseif id=="rate.down" or id=="rate.up" then
            local target=id=="rate.up" and rates[#rates] or rates[1]
            if id=="rate.up" then
                for _,rate in ipairs(rates) do if rate>p.playbackRate+0.001 then target=rate;break end end
            else
                for i=#rates,1,-1 do if rates[i]<p.playbackRate-0.001 then target=rates[i];break end end
            end
            p:setRate(target)
        elseif id and id:sub(1,5)=="rate." then p:setRate(tonumber(id:sub(6)) or 1)
        elseif id=="track" then
            if not model.suppressClick then M.playTrack(model,e.value) end
            model.suppressClick=nil
        elseif id=="track.remove" then local i=indexOf(p,e.value);if i then p:remove(i) end
        elseif id=="track.top" then local i=indexOf(p,e.value);if i then p:move(i,1) end
        elseif id=="track.bottom" then local i=indexOf(p,e.value);if i then p:move(i,#p.tracks) end
        elseif id=="drag.start" and e.button==1 then
            local i=indexOf(p,e.value);model.suppressClick=nil
            if i then model.drag={from=i,to=i,y=e.y} end
        elseif id=="drag.move" and model.drag then
            local drag=model.drag
            drag.moved=drag.moved or math.abs(e.y-drag.y)>model.itemExtent*0.15
            local range=view.virtualRange({key="playlist.list",itemCount=#p.tracks,itemExtent=model.itemExtent,
                viewportExtent=model.listViewport,overscan=1})
            drag.to,drag.boundary=player.dropTarget(drag.from,#p.tracks,
                e.y-model.listTop+range.offset,model.itemExtent)
        elseif id=="drag.end" and model.drag then
            local drag=model.drag;model.drag=nil;model.suppressClick=drag.moved
            if drag.moved then p:move(drag.from,drag.to) end
        elseif id=="clear" then p:clear();model.drag=nil end
    else return false end
    widget.invalidate()
    return true
end

function M.available()
    for _, feature in ipairs({"task.audio.playback", "audio.playback.events",
        "audio.playback.metadata", "audio.playback.systemMediaControls",
        "audio.playback.rate", "audio.playback.mode", "audio.playback.formats.extended",
        "task.filesystem.access", "task.filesystem.list.names", "task.filesystem.picker",
        "task.filesystem.picker.multiple", "interaction.fileDrop.async", "storage.transaction",
        "widget.panel", "view.collection.virtual", "view.pointer.events", "interaction.pointerCapture"}) do
        if not widget.hasFeature(feature) then return false end
    end
    return true
end

function M.allowed()
    return M.available() and widget.hasPermission("audio.playback") and
        widget.hasPermission("filesystem.userSelected.read")
end

function M.isLocal(model)
    return model and model.source=="local" and M.available()
end

function M.select(model, source, host)
    source=source=="local" and "local" or "system"
    local available=host and host.available or M.available
    if source=="local" and not available() then return false end
    if model.source==source then return true end
    -- Remove our SMTC session before returning to system media control.
    if source=="system" then model.player:stop() end
    model.drag=nil
    model.source=source
    if host then host.persist(source) else storage.set("playbackSource",source) end
    return true
end

function M.session(model)
    local p=model.player
    local track=p.tracks[p.index]
    if not track then return nil end
    return {id="aurora.local:"..track.key, title=p.title or track.name,
        artist=p.artist or l10n.tr("music.unknown_artist"), album=p.album or "",
        sourceName=l10n.tr("workshop.aurora_player.source_local"), playbackStatus=p.status,
        controls={canPlayPause=true,canPrevious=true,canNext=true,canStop=p.playbackId~=nil,
            canSeek=p.playbackId~=nil and p.canSeek,canChangePlaybackRate=true,
            canToggleShuffle=true,canChangeRepeatMode=true},
        timeline={positionMs=p.position,durationMs=p.duration,minimumSeekMs=0,
            maximumSeekMs=p.duration,updatedAtMs=model.updatedAtMs or 0,
            playbackRate=p.playbackRate}, playbackRate=p.playbackRate}
end

function M.notice(model)
    local p=model.player
    if not M.allowed() then return l10n.tr("music.permissions") end
    if p.error then return l10n.tr(p.error=="audioDecodeFailed" and "music.codec_error" or "music.error") end
    if p.scanning then return l10n.tr("music.importing") end
    if #p.tracks==0 then return l10n.tr("music.drop") end
end

function M.control(model, name, arguments)
    if model.preview or not M.allowed() then return false end
    local p=model.player
    if name=="media.previous" then p:step(-1)
    elseif name=="media.next" then p:step(1)
    elseif name=="media.stop" then p:stop()
    elseif name=="media.toggle" then p:toggle()
    elseif name=="media.seek" and p.playbackId and p.canSeek then
        return p:start("audio.seek",{playbackId=p.playbackId,positionMs=arguments.positionMs})
    elseif name=="media.setRate" then p:setRate(arguments.rate)
    elseif name=="media.setShuffle" then p:setMode(arguments.shuffle,p.repeatMode)
    elseif name=="media.setRepeat" then p:setMode(p.shuffle,arguments.mode) end
    widget.invalidate()
    return true
end

function M.menu(_context,model,request)
    if request.id~="local.track.menu" then return nil end
    return ui.menu({{id="local.track",label=l10n.tr("music.play")},
        {id="local.track.remove",label=l10n.tr("music.remove")},{type="separator"},
        {id="local.track.top",label=l10n.tr("music.move_top")},
        {id="local.track.bottom",label=l10n.tr("music.move_bottom")}})
end

function M.menuItems(model)
    return {{label=l10n.tr("workshop.aurora_player.source"),children={
        {id="local.source.system",label=l10n.tr("workshop.aurora_player.source_system"),checked=model.source=="system"},
        {id="local.source.local",label=l10n.tr("workshop.aurora_player.source_local"),checked=model.source=="local",enabled=M.available()},
    }},{id="local.files",label=l10n.tr("music.files"),enabled=M.available()},
        {id="local.folder",label=l10n.tr("music.folder"),enabled=M.available()},
        {id="local.playlist",label=l10n.tr("music.playlist"),enabled=M.available()},
        {id="system.audio.menu",label=l10n.tr("shared.audio"),enabled=widget.hasFeature("widget.panel")},
        {id="local.options",label=l10n.tr("music.options"),enabled=widget.hasFeature("widget.panel")}}
end

return M
