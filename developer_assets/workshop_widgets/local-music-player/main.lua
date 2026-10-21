local player = module.require("modules/player.lua")
local function setup(context)
    local p = player.new({
        start = function(name, args) return task.start(name, args) end,
        cancel = function(id) return task.cancel(id) end,
        save = function(sources) storage.set("sources", sources) end,
    })
    p.volume = math.max(0, math.min(1, tonumber(storage.get("volume")) or 0.65))
    local model = { player = p, preview = context.preview == true, page = 1 }
    if model.preview then
        p.tracks = {
            {name = l10n.tr("music.preview_track")},
            {name = l10n.tr("music.preview_second")},
            {name = l10n.tr("music.preview_third")},
        }
        p.index = 1; p.status = "paused"; p.position = 42000; p.duration = 214000
    else
        local sources = storage.get("sources")
        if type(sources) == "table" then p:ingest(sources, true) end
    end
    return model
end

local function clock(ms)
    local seconds = math.floor(math.max(0, ms or 0) / 1000)
    return string.format("%d:%02d", math.floor(seconds / 60), seconds % 60)
end

local function desktop(_context, model)
    local p = model.player
    local width, height = layout.contentWidth(), layout.contentHeight()
    local unit = math.min(width / 3, height / 2)
    local gap, font, control = unit * 0.075, unit * 0.105, unit * 0.29
    local theme = widget.theme()
    local dark = theme and theme.contentTheme == 1
    local primary = dark and 0x182B38 or 0xEDF8FC
    local secondary = dark and 0x496675 or 0xC3DAE7
    local accent = dark and 0x007E89 or 0x80E6D6
    local track = p.tracks[p.index]
    local allowed = widget.hasPermission("audio.playback") and widget.hasPermission("filesystem.userSelected.read")
    local function text(key, value, size, color, extra)
        local spec = {key=key,text=value,width="fill",height=size*1.55,fontSize=size,
            textAlign="center",textWrap="noWrap",style={foreground=color or primary}}
        for k,v in pairs(extra or {}) do spec[k]=v end
        return view.text(spec)
    end
    local function button(id, label, enabled, icon)
        return view.button({key="music."..id,text=label,height=control,width="fill",fontSize=font,
            enabled=enabled~=false,flexGrow=1,padding=gap*0.35,
            style={foreground=id=="toggle" and accent or primary,cornerRadius=control*0.35},
            icon=icon,iconFont=icon and "fluent" or nil,events={click={id=id}},accessibility={label=label}})
    end
    local children = {
        text("music.kicker",l10n.tr("music.name"),font*0.82,accent,{letterSpacing=font*0.16}),
        text("music.title",track and track.name or l10n.tr("music.empty"),font*1.45,primary,{bold=true}),
        text("music.subtitle",not allowed and l10n.tr("music.permissions") or p.error and l10n.tr("music.error") or
            p.scanning and l10n.tr("music.importing") or l10n.tr("music.drop"),font*0.8,secondary),
        view.slider({key="music.seek",width="fill",height=control*0.65,min=0,max=1,step=0.001,
            value=p.duration>0 and math.max(0,math.min(1,p.position/p.duration)) or 0,
            enabled=p.playbackId~=nil and p.duration>0,events={change={id="seek"}},
            style={foreground=accent},accessibility={label=l10n.tr("music.seek")}}),
        text("music.time",clock(p.position).." / "..clock(p.duration),font*0.78,secondary),
        view.row({key="music.controls",width="fill",height=control,gap=gap,children={
            button("previous",l10n.tr("music.previous"),#p.tracks>0,utf8.char(0xF8AD)),
            button("toggle",p.status=="playing" and l10n.tr("music.pause") or l10n.tr("music.play"),allowed and #p.tracks>0,
                utf8.char(p.status=="playing" and 0xF8AE or 0xF605)),
            button("next",l10n.tr("music.next"),#p.tracks>0,utf8.char(0xF569)),
        }}),
        view.row({key="music.volume.row",width="fill",height=control*0.65,gap=gap,alignItems="center",children={
            view.text({key="music.volume.label",text=l10n.tr("music.volume"),width=unit*0.5,height="fill",fontSize=font*0.8,
                style={foreground=secondary}}),
            view.slider({key="music.volume",width="fill",height="fill",min=0,max=1,step=0.01,value=p.volume,
                events={change={id="volume"}},style={foreground=accent},accessibility={label=l10n.tr("music.volume")}}),
        }}),
        view.row({key="music.import",width="fill",height=control,gap=gap,children={
            button("files",l10n.tr("music.files"),allowed),button("folder",l10n.tr("music.folder"),allowed),
            button("playlist",l10n.tr("music.playlist"),#p.tracks>0),
        }}),
    }
    return view.column({key="music.surface",width="fill",height="fill",padding=gap*1.5,gap=gap*0.4,
        alignItems="center",justifyContent="center",children=children,
        events={fileDrop={id="import"},contextMenu={id="music.menu",scope="component"}},
        accessibility={label=l10n.tr("music.name")}})
end

local function panel(_context, model)
    local p=model.player
    local row=ui.metrics().layoutRowHeight
    local children={}
    local first=(model.page-1)*12+1
    for i=first,math.min(first+11,#p.tracks) do
        children[#children+1]=view.button({key="track."..i,text=tostring(i).."  "..p.tracks[i].name,width="fill",
            height=row,fontSize=row*12/28,textAlign="start",events={click={id="track",value=i}}})
    end
    children[#children+1]=view.row({key="music.pages",width="fill",height=row,gap=row*0.2,children={
        view.button({key="page.back",text=l10n.tr("music.previous"),width="fill",height=row,enabled=model.page>1,events={click={id="page.back"}}}),
        view.button({key="page.next",text=l10n.tr("music.next"),width="fill",height=row,enabled=first+11<#p.tracks,events={click={id="page.next"}}}),
    }})
    return view.column({key="playlist.surface",width="fill",height="fill",padding=row*0.3,gap=row*0.15,children=children})
end

local function event(_context, model, e)
    local p=model.player
    if model.preview then return end
    if e.kind=="task.complete" then p:complete(e)
    elseif e.kind=="audio.state" then p:audio(e)
    elseif e.kind=="action" then
        if e.id=="import" and e.action=="fileDrop" then p:ingest(e.items)
        elseif e.id=="files" then p:pick(false)
        elseif e.id=="folder" then p:pick(true)
        elseif e.id=="toggle" then p:toggle()
        elseif e.id=="previous" then p:step(-1)
        elseif e.id=="next" then p:step(1)
        elseif e.id=="stop" then p:stop()
        elseif e.id=="seek" and tonumber(e.controlValue) then p:seek(e.controlValue)
        elseif e.id=="volume" and tonumber(e.controlValue) then p:setVolume(e.controlValue);storage.set("volume",p.volume)
        elseif e.id=="loop" then p:setLoop(not p.repeatTrack)
        elseif e.id=="repeat" then p.repeatList=not p.repeatList
        elseif e.id=="playlist" then widget.openPanel({title=l10n.tr("music.playlist"),width=540,height=520})
        elseif e.id=="track" then p:play(tonumber(e.value) or 1)
        elseif e.id=="page.back" then model.page=math.max(1,model.page-1)
        elseif e.id=="page.next" then model.page=model.page+1
        elseif e.id=="clear" then p:clear();model.page=1 end
    end
    widget.invalidate()
end

return widget.define({name=l10n.tr("music.name"),useCustomStyle=true,followPersonalizationDefault=true,
    bg=0x102D3B,border=0xB4ECF0,alpha=0.44,borderAlpha=0.18,gradientEndA=0.28,
    setup=setup,view=desktop,panel=panel,event=event,
    menu=function(_context,model,request)
        if request.id~="music.menu" then return nil end
        local p=model.player
        return ui.menu({
            {id="files",label=l10n.tr("music.files")},{id="folder",label=l10n.tr("music.folder")},
            {id="playlist",label=l10n.tr("music.playlist"),enabled=#p.tracks>0},
            {type="separator"},{id="loop",label=l10n.tr("music.loop"),checked=p.repeatTrack},
            {id="repeat",label=l10n.tr("music.repeat"),checked=p.repeatList},
            {id="stop",label=l10n.tr("music.stop"),enabled=p.playbackId~=nil},
            {id="clear",label=l10n.tr("music.clear"),enabled=#p.sources>0},
        })
    end})
