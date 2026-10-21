local player = module.require("modules/player.lua")
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
local function setup(context)
    local p=player.new({start=function(name,args) return task.start(name,args) end,
        cancel=function(id) return task.cancel(id) end,save=savePlaylist,preferences=preferences})
    p.volume=math.max(0,math.min(1,tonumber(storage.get("volume")) or 1))
    p.playbackRate=math.max(0.25,math.min(4,tonumber(storage.get("playbackRate")) or 1))
    p:setMode(storage.get("shuffle")==true,storage.get("repeatMode"))
    local model={player=p,preview=context.preview==true,panelKind="playlist"}
    if model.preview then
        for _, key in ipairs({"music.preview_track","music.preview_second","music.preview_third"}) do
            p:append("preview."..key,l10n.tr(key))
        end
        p.index=1;p.status="paused";p.position=42000;p.duration=214000
        p.title=p.tracks[1].name;p.artist=l10n.tr("music.preview_artist");p.album=l10n.tr("music.preview_album")
        if storage.get("previewEmpty")=="1" then p.tracks={};p.index=0;p.title=nil;p.artist=nil;p.album=nil;p.position=0;p.duration=0 end
    else
        local sources=readChunks("source") or storage.get("sources")
        local tracks=readChunks("playlist")
        if type(sources)=="table" then
            if tracks then p:restore(sources,tracks) else p:ingest(sources,true) end
        end
    end
    return model
end
local function palette()
    local theme=widget.theme()
    local dark=theme and theme.contentTheme==1
    return dark and 0x182B38 or 0xEDF8FC,dark and 0x496675 or 0xC3DAE7,
        dark and 0x007E89 or 0x80E6D6,dark and 0xE3EBED or 0x263B46
end
local function text(key,value,size,color,extra)
    local spec={key=key,text=value,width="fill",height=size*1.5,fontSize=size,
        textWrap="noWrap",style={foreground=color}}
    for k,v in pairs(extra or {}) do spec[k]=v end
    return view.text(spec)
end
local function button(id,label,height,color,enabled,icon)
    local spec={key="music."..id,width="fill",height=height,fontSize=height*0.42,
        enabled=enabled~=false,padding=height*0.2,textAlign="center",style={foreground=color,cornerRadius=height*0.25},
        events={click={id=id}},accessibility={label=label}}
    if icon then spec.glyph=icon;spec.iconFont="fa";return view.iconButton(spec) end
    spec.label=label;return view.button(spec)
end
local function clock(ms)
    local seconds=math.floor(math.max(0,ms or 0)/1000)
    return string.format("%d:%02d",math.floor(seconds/60),seconds%60)
end
local function modeKey(p)
    return p.shuffle and "shuffle" or p.repeatMode=="track" and "loop" or p.repeatMode=="list" and "repeat" or "sequential"
end
local function desktop(_context,model)
    local p=model.player
    local unit=math.min(layout.contentWidth()/3,layout.contentHeight()/2)
    local primary,secondary,accent,surface=palette()
    local font,gap,control=unit*0.105,unit*0.09,unit*0.32
    local track=p.tracks[p.index]
    local allowed=widget.hasPermission("audio.playback") and widget.hasPermission("filesystem.userSelected.read")
    local cover=p.artwork and view.image({key="music.cover",source=p.artwork,width=unit*0.86,height=unit*0.86,
        fit="cover",alt=p.album or p.title or "",style={cornerRadius=unit*0.1}}) or
        view.box({key="music.cover.placeholder",width=unit*0.86,height=unit*0.86,style={background=surface,cornerRadius=unit*0.1},children={
            view.icon({key="music.cover.note",glyph=utf8.char(0xF001),iconFont="fa",width="fill",height="fill",fontSize=unit*0.33,
                style={foreground=accent}})}})
    local info={
        text("music.kicker",l10n.tr("music.name"),font*0.8,accent),
        text("music.title",p.title or (track and track.name) or l10n.tr("music.empty"),font*1.4,primary,
            {bold=true,height=font*3.1,textWrap="wrap"}),
        text("music.artist",p.artist or (track and l10n.tr("music.unknown_artist")) or "",font*0.9,secondary),
        text("music.album",p.album or "",font*0.78,secondary),
    }
    local notice=not allowed and l10n.tr("music.permissions") or p.error and l10n.tr(p.error=="audioDecodeFailed" and "music.codec_error" or "music.error") or
        p.scanning and l10n.tr("music.importing") or not track and l10n.tr("music.drop") or ""
    return view.column({key="music.surface",width="fill",height="fill",padding=unit*0.13,gap=gap*0.35,
        justifyContent="center",children={
            view.row({key="music.heading",width="fill",height=unit*0.9,gap=gap,alignItems="center",children={cover,
                view.column({key="music.info",width="fill",height="fill",justifyContent="center",children=info})}}),
            text("music.notice",notice,font*0.75,secondary),
            view.slider({key="music.seek",width="fill",height=control*0.55,min=0,max=1,step=0.001,
                value=model.seekPreview or (p.duration>0 and math.max(0,math.min(1,p.position/p.duration)) or 0),
                enabled=p.playbackId~=nil and p.duration>0 and p.canSeek,events={change={id="seek"},pointerUp={id="seek.commit"}},
                style={foreground=accent},accessibility={label=l10n.tr("music.seek")}}),
            text("music.time",clock(p.position).." / "..clock(p.duration),font*0.78,secondary,{textAlign="center"}),
            view.row({key="music.controls",width="fill",height=control,gap=gap,children={
                button("playlist",l10n.tr("music.playlist"),control,primary,true,utf8.char(0xF03A)),
                button("previous",l10n.tr("music.previous"),control,primary,#p.tracks>0,utf8.char(0xF048)),
                button("toggle",p.status=="playing" and l10n.tr("music.pause") or l10n.tr("music.play"),control,accent,allowed and #p.tracks>0,
                    utf8.char(p.status=="playing" and 0xF04C or 0xF04B)),
                button("next",l10n.tr("music.next"),control,primary,#p.tracks>0,utf8.char(0xF051)),
                button("options",l10n.tr("music.options"),control,primary,true,utf8.char(0xF141)),
            }}),
        },events={fileDrop={id="import"},contextMenu={id="music.menu",scope="component"}},
        accessibility={label=l10n.tr("music.name")}})
end
local rates={0.25,0.5,0.75,1,1.25,1.5,2,3,4}
local function panel(_context,model)
    local p=model.player
    local row=ui.metrics().layoutRowHeight
    local primary,secondary,accent,surface=palette()
    local function label(key,value) return text(key,value,row*0.43,secondary) end
    local function command(id,key,enabled) return button(id,l10n.tr(key),row,primary,enabled) end
    local children={view.row({key="panel.toolbar",width="fill",height=row,gap=row*0.2,children={
        command("files","music.files"),command("folder","music.folder"),
        command(model.panelKind=="playlist" and "options" or "playlist",model.panelKind=="playlist" and "music.options" or "music.playlist"),
    }})}
    if model.panelKind=="options" then
        children[#children+1]=label("volume.label",l10n.tr("music.volume").."  "..math.floor(p.volume*100+0.5).."%")
        children[#children+1]=view.slider({key="music.volume",width="fill",height=row,min=0,max=1,step=0.01,value=p.volume,
            events={change={id="volume"},pointerUp={id="volume.commit"}},style={foreground=accent},accessibility={label=l10n.tr("music.volume")}})
        children[#children+1]=label("mode.label",l10n.tr("music.mode"))
        for _, keys in ipairs({{"sequential","repeat"},{"loop","shuffle"}}) do
            local buttons={}
            for _, key in ipairs(keys) do buttons[#buttons+1]=button("mode."..key,l10n.tr("music."..key),row,
                modeKey(p)==key and accent or primary) end
            children[#children+1]=view.row({key="mode."..keys[1],width="fill",height=row,gap=row*0.2,children=buttons})
        end
        children[#children+1]=label("rate.label",l10n.tr("music.rate").."  "..p.playbackRate.."×")
        for first=1,#rates,3 do
            local buttons={}
            for i=first,math.min(first+2,#rates) do
                local rate=rates[i]
                buttons[#buttons+1]=button("rate."..rate,rate.."×",row,p.playbackRate==rate and accent or primary)
            end
            children[#children+1]=view.row({key="rates."..first,width="fill",height=row,gap=row*0.2,children=buttons})
        end
        children[#children+1]=command("stop","music.stop",p.playbackId~=nil)
    else
        local extent=row*1.25
        local viewport=math.max(row,layout.contentHeight()-row*3)
        local range=view.virtualRange({key="playlist.list",itemCount=#p.tracks,itemExtent=extent,viewportExtent=viewport,overscan=1})
        model.listOffset=range.offset;model.itemExtent=extent
        local items={}
        for i=math.max(1,range.firstIndex),range.lastIndex do
            local track=p.tracks[i]
            local color=i==p.index and accent or primary
            local target=model.drag and model.drag.to==i and model.drag.from~=i
            items[#items+1]=view.listItem({key=track.key,width="fill",height=extent,padding={vertical=row*0.06},
                capturePointer=true,events={click={id="track",value=track.key},contextMenu={id="track.menu",value=track.key},
                    pointerDown={id="drag.start",value=track.key},pointerMove={id="drag.move"},pointerUp={id="drag.end"}},
                accessibility={label=track.name},children={
                    view.row({key=track.key..".row",width="fill",height="fill",padding={horizontal=row*0.45},gap=row*0.25,
                        alignItems="center",style={background=target and accent or surface,cornerRadius=row*0.2},children={
                            text(track.key..".index",tostring(i),row*0.4,color,{width=row*1.5,textAlign="end"}),
                            text(track.key..".title",track.name,row*0.43,color),
                            view.icon({key=track.key..".grip",glyph=utf8.char(0xF0C9),iconFont="fa",width=row*0.5,height=row*0.6,
                                fontSize=row*0.35,style={foreground=secondary}}),
                        }})}})
        end
        children[#children+1]=view.virtualList({key="playlist.list",width="fill",height=viewport,itemCount=#p.tracks,itemExtent=extent,
            firstIndex=range.firstIndex,children=items,emptyContent=label("playlist.empty",l10n.tr("music.empty"))})
        children[#children+1]=view.row({key="playlist.footer",width="fill",height=row,gap=row*0.2,alignItems="center",children={
            label("playlist.count",tostring(#p.tracks).."  "..l10n.tr("music.tracks")),command("clear","music.clear",#p.sources>0)}})
    end
    local content=view.column({key="playlist.surface",width="fill",height=model.panelKind=="options" and "auto" or "fill",
        padding=row*0.3,gap=row*0.2,children=children})
    if model.panelKind=="options" then
        return view.scroll({key="options.scroll",width="fill",height="fill",children={content}})
    end
    return content
end
local function indexOf(p,key)
    for i,track in ipairs(p.tracks) do if track.key==key then return i end end
end
local function event(_context,model,e)
    local p=model.player
    if model.preview then return end
    if e.kind=="task.complete" then p:complete(e)
    elseif e.kind=="audio.state" then p:audio(e)
    elseif e.kind=="audio.command" then p:command(e)
    elseif e.kind=="panel" and e.action=="closed" then model.drag=nil
    elseif e.kind=="action" then
        if e.id=="import" and e.action=="fileDrop" then p:ingest(e.items)
        elseif e.id=="files" then p:pick(false)
        elseif e.id=="folder" then p:pick(true)
        elseif e.id=="toggle" then p:toggle()
        elseif e.id=="previous" then p:step(-1)
        elseif e.id=="next" then p:step(1)
        elseif e.id=="stop" then p:stop()
        elseif e.id=="seek" and tonumber(e.controlValue) then
            model.seekPreview=e.controlValue
            if e.source~="pointer" then p:seek(e.controlValue);model.seekPreview=nil end
        elseif e.id=="seek.commit" and model.seekPreview then p:seek(model.seekPreview);model.seekPreview=nil
        elseif e.id=="volume" and tonumber(e.controlValue) then
            p.volume=math.max(0,math.min(1,e.controlValue))
            if e.source~="pointer" then p:setVolume(p.volume);storage.set("volume",p.volume) end
        elseif e.id=="volume.commit" then p:setVolume(p.volume);storage.set("volume",p.volume)
        elseif e.id=="playlist" or e.id=="options" then
            model.panelKind=e.id;model.drag=nil
            widget.openPanel({title=l10n.tr("music."..e.id),width=540,height=520})
        elseif e.id=="mode.sequential" then p:setMode(false,"none")
        elseif e.id=="mode.repeat" then p:setMode(false,"list")
        elseif e.id=="mode.loop" then p:setMode(false,"track")
        elseif e.id=="mode.shuffle" then p:setMode(true,"list")
        elseif e.id and e.id:sub(1,5)=="rate." then p:setRate(tonumber(e.id:sub(6)) or 1)
        elseif e.id=="track" then
            if not model.suppressClick then local i=indexOf(p,e.value);if i then p:play(i) end end
            model.suppressClick=nil
        elseif e.id=="track.remove" then local i=indexOf(p,e.value);if i then p:remove(i) end
        elseif e.id=="track.top" then local i=indexOf(p,e.value);if i then p:move(i,1) end
        elseif e.id=="track.bottom" then local i=indexOf(p,e.value);if i then p:move(i,#p.tracks) end
        elseif e.id=="drag.start" and e.button==1 then
            local i=indexOf(p,e.value);model.suppressClick=nil
            if i then model.drag={from=i,to=i,y=e.y,offset=model.listOffset or 0} end
        elseif e.id=="drag.move" and model.drag then
            local drag=model.drag
            drag.moved=drag.moved or math.abs(e.y-drag.y)>model.itemExtent*0.15
            drag.to=math.max(1,math.min(#p.tracks,drag.from+math.floor((e.y-drag.y+(model.listOffset or 0)-drag.offset)/model.itemExtent+0.5)))
        elseif e.id=="drag.end" and model.drag then
            local drag=model.drag;model.drag=nil;model.suppressClick=drag.moved
            if drag.moved then p:move(drag.from,drag.to) end
        elseif e.id=="clear" then p:clear();model.drag=nil end
    end
    widget.invalidate()
end
return widget.define({name=l10n.tr("music.name"),useCustomStyle=true,followPersonalizationDefault=true,
    bg=0x102D3B,border=0xB4ECF0,alpha=0.44,borderAlpha=0.18,gradientEndA=0.28,
    setup=setup,view=desktop,panel=panel,event=event,
    menu=function(_context,model,request)
        if request.id=="track.menu" then
            return ui.menu({{id="track",label=l10n.tr("music.play")},{id="track.remove",label=l10n.tr("music.remove")},
                {type="separator"},{id="track.top",label=l10n.tr("music.move_top")},{id="track.bottom",label=l10n.tr("music.move_bottom")}})
        end
        if request.id~="music.menu" then return nil end
        local p=model.player
        return ui.menu({{id="files",label=l10n.tr("music.files")},{id="folder",label=l10n.tr("music.folder")},
            {id="playlist",label=l10n.tr("music.playlist")},{id="options",label=l10n.tr("music.options")},
            {type="separator"},{id="stop",label=l10n.tr("music.stop"),enabled=p.playbackId~=nil},
            {id="clear",label=l10n.tr("music.clear"),enabled=#p.sources>0}})
    end})
