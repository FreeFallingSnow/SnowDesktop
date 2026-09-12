local album = module.require("modules/album.lua")
local sample = resource.image("sample")

local function copy()
    return {
        name=l10n.tr("album.name"), empty=l10n.tr("album.empty"),
        addImages=l10n.tr("album.add_images"), addFolders=l10n.tr("album.add_folders"),
        manage=l10n.tr("album.manage"), previous=l10n.tr("album.previous"), next=l10n.tr("album.next"),
        play=l10n.tr("album.play"), pause=l10n.tr("album.pause"), loading=l10n.tr("album.loading"),
        error=l10n.tr("album.error"), permission=l10n.tr("album.permission"),
        refresh=l10n.tr("album.refresh"), remove=l10n.tr("album.remove"),
        up=l10n.tr("album.up"), down=l10n.tr("album.down"),
        sources=l10n.tr("album.sources"), photos=l10n.tr("album.photos"),
        settings=l10n.tr("album.settings"), limit=l10n.tr("album.limit"),
        scanFailed=l10n.tr("album.scan_failed"), saveFailed=l10n.tr("album.save_failed"),
        sample=l10n.tr("album.sample"),
        importMode=l10n.tr("album.import_mode"), bindMode=l10n.tr("album.bind_mode"),
        removePhoto=l10n.tr("album.remove_photo"), busy=l10n.tr("album.busy"),
        importHint=l10n.tr("album.import_hint"),bindHint=l10n.tr("album.bind_hint"),
        modeLocked=l10n.tr("album.mode_locked"),foldersOnly=l10n.tr("album.folders_only"),
        clearAll=l10n.tr("album.clear_all"),clearConfirm=l10n.tr("album.clear_confirm"),
        cancel=l10n.tr("album.cancel"),
    }
end

local function enabled(key, fallback)
    local value=storage.get(key)
    if value==nil then return fallback end
    return value==true or value==1 or value=="1" or value=="true"
end

local function button(key, label, row, active, icon)
    local glyphs={chevron_left=0xF053,chevron_right=0xF054,play=0xF04B,pause=0xF04C,
        more_horizontal=0xF141,chevron_up=0xF077,chevron_down=0xF078,dismiss=0xF00D}
    local options={key=key,label=label,width="fill",height=row,
        fontSize=row*0.43,textAlign="center",enabled=active~=false,
        style={foreground="textPrimary",cornerRadius=row*0.20},
        action={id=key},accessibility={label=label},tooltip=label}
    if key=="previous" or key=="next" then options.events={doubleClick={id=key}} end
    if icon then options.label=nil;options.glyph=utf8.char(glyphs[icon]);options.iconFont="fa";return view.iconButton(options) end
    return view.button(options)
end

local function text(key, value, row, secondary)
    return view.text({key=key,text=value,width="fill",height=row,fontSize=row*0.43,
        verticalAlign="center",overflowText="ellipsis",
        style={foreground=secondary and "textSecondary" or "textPrimary"}})
end

local function setup(context)
    if storage.get("_albumPagingProbe")~=nil then storage.remove("_albumPagingProbe") end
    local sources=album.decodeSources(storage.get("sources"))
    local bindFolders=enabled("bindFolders",false)
    if #sources>0 then
        bindFolders=false
        for _,source in ipairs(sources) do if source.kind=="folder" then bindFolders=true;break end end
    end
    local a=album.new({
        start=function(name,args) return task.start(name,args) end,
        cancel=function(id) task.cancel(id) end,
        allowed=function() return widget.hasPermission("filesystem.userSelected.read") end,
        save=function(sources) return storage.set("sources",album.encodeSources(sources)) end,
        saveMode=function(bind) storage.set("bindFolders",bind) end,
        random=function(maximum) return math.random(1,maximum) end,
    },sources,bindFolders)
    local m={album=a,tab="sources",page=1,preview=context.preview==true}
    if m.preview and not enabled("previewEmpty",false) then
        a.photos={{name=copy().sample},{name=copy().sample},{name=copy().sample}}
        if enabled("previewSingle",false) then a.photos={a.photos[1]} end
        a.index=1; a.image=sample; a.loadedName=copy().sample
    else a:refresh() end
    schedule.every("album.tick",1000,{whenHidden="pause"})
    return m
end

local function modeButtons(a,c,row)
    local import=button("mode.import",(a.bindFolders and "" or "✓ ")..c.importMode,row,a.bindFolders)
    local bind=button("mode.bind",(a.bindFolders and "✓ " or "")..c.bindMode,row,not a.bindFolders)
    return {import,bind}
end

local function desktop(context,m)
    local a,c=m.album,copy()
    local w,h=layout.contentWidth(),layout.contentHeight()
    local unit=math.min(w,h); local gap=unit*0.025; local row=unit*0.14
    local content
    if a.image then
        content=view.image({key="photo",source=a.image,width="fill",height="fill",fit=enabled("fill",true) and "cover" or "contain",
            alt=a.loadedName or c.name,style={cornerRadius=unit*0.025}})
    else
        local message=a.error and (c[a.error] or c.error) or
            (a.warning and (c[a.warning] or c.error)) or
            ((a.scanning or a.loading or a.importing or a.picking or a:isClearing()) and c.loading or
                (a.bindFolders and c.bindHint or c.importHint))
        local heading=text("empty.title",c.name,row); heading.textAlign="center"; heading.bold=true
        local hint=text("empty.hint",message,row*1.5,true)
        hint.fontSize=row*0.34; hint.textAlign="center"; hint.textWrap="wrap"; hint.maxLines=3
        local children={heading,hint}
        if a:canChangeMode() then
            for _,control in ipairs(modeButtons(a,c,row)) do children[#children+1]=control end
        end
        children[#children+1]=button("manage",c.manage,row,true)
        content=view.column({key="empty",width="fill",height="fill",justifyContent="center",gap=gap,
            children=children})
    end
    local controls={}
    if #a.photos>1 then
        controls[#controls+1]=button("previous",c.previous,row,true,"chevron_left")
        controls[#controls+1]=button("toggle",enabled("paused",false) and c.play or c.pause,row,true,
            enabled("paused",false) and "play" or "pause")
        controls[#controls+1]=button("next",c.next,row,true,"chevron_right")
    end
    if a.image then
        local count=text("count",tostring(a.loadedIndex or a.index).." / "..tostring(#a.photos),row,true)
        if a.warning then count.text=c[a.warning] or c.error;count.tooltip=count.text end
        count.textAlign="center"; count.fontSize=row*0.34
        controls[#controls+1]=count
        controls[#controls+1]=button("manage",c.manage,row,true,"more_horizontal")
    end
    local children={content}
    if #controls>0 then
        children[#children+1]=view.row({key="controls",width="fill",height=row,flexShrink=0,gap=gap,children=controls})
    end
    local events={contextMenu={id="album.menu",scope="component"}}
    if not a:isClearing() then events.fileDrop={id="import",value=a.dropRevision} end
    return view.column({key="album",width="fill",height="fill",padding=unit*0.03,gap=gap,children=children,events=events})
end

local function panel(context,m)
    local a,c=m.album,copy(); local row=ui.metrics().layoutRowHeight
    local canRead=widget.hasPermission("filesystem.userSelected.read")
    if m.confirmClear then
        local hint=text("clear.hint",c.clearConfirm,row*3,true)
        hint.textWrap="wrap";hint.maxLines=4
        local children={text("clear.title",c.clearAll,row),hint}
        if a.error then children[#children+1]=text("clear.error",c[a.error] or c.error,row,true) end
        children[#children+1]=view.row({key="clear.actions",width="fill",height=row,gap=row*0.25,
            children={button("clear.cancel",c.cancel,row),button("clear.confirm",c.clearAll,row,not m.preview)}})
        return view.column({key="clear",width="fill",height="fill",padding=row*0.6,gap=row*0.4,children=children})
    end
    local header={}
    if a:canChangeMode() then
        header[#header+1]=view.row({key="mode",width="fill",height=row,gap=row*0.25,children=modeButtons(a,c,row)})
    else
        header[#header+1]=text("mode.current",a.bindFolders and c.bindMode or c.importMode,row)
        local hint=text("mode.locked",c.modeLocked,row*1.4,true);hint.textWrap="wrap";hint.maxLines=2
        header[#header+1]=hint
    end
    local add={}
    local canAdd=canRead and not a.picking and not a.importing and not a:isClearing()
    if not a.bindFolders then add[#add+1]=button("addImages",c.addImages,row,canAdd) end
    add[#add+1]=button("addFolders",c.addFolders,row,canAdd)
    header[#header+1]=view.row({key="add",width="fill",height=row,gap=row*0.25,children=add})
    header[#header+1]=view.row({key="tabs",width="fill",height=row,gap=row*0.25,children={
            button("sources",c.sources,row,m.tab~="sources"),button("photos",c.photos,row,m.tab~="photos"),
            button("refresh",c.refresh,row,canRead and not a.scanning)}})
    local status=not canRead and c.permission or (a.error and (c[a.error] or c.error)) or
        ((a.scanning or a.importing) and c.loading) or (a.warning and (c[a.warning] or c.error))
    if status then header[#header+1]=text("status",status,row,true) end
    local items={}
    local total=m.tab=="sources" and #a.sources or #a.photos
    local pageSize=m.tab=="sources" and 20 or 30
    local page=math.max(1,math.min(m.page,math.max(1,math.ceil(total/pageSize))))
    if m.tab=="sources" then
        local hint=text("folder.hint",a.bindFolders and c.bindHint or c.importHint,row*1.5,true)
        hint.fontSize=row*0.34
        hint.textWrap="wrap"; hint.maxLines=2; items[#items+1]=hint
        for i=(page-1)*pageSize+1,math.min(page*pageSize,#a.sources) do
            local source=a.sources[i]
            local up=button("up:"..i,c.up,row,i>1,"chevron_up"); up.width=row
            local down=button("down:"..i,c.down,row,i<#a.sources,"chevron_down"); down.width=row
            local remove=button("remove:"..i,c.remove,row,true,"dismiss"); remove.width=row
            items[#items+1]=view.row({key="source:"..source.handle,width="fill",height=row,gap=row*0.2,
                children={text("name:"..i,source.name,row),up,down,remove}})
        end
        if #a.sources==0 then items[#items+1]=text("no.sources",c.empty,row,true) end
    else
        for i=(page-1)*30+1,math.min(page*30,#a.photos) do
            items[#items+1]=button("photo:"..i,tostring(i).."  "..a.photos[i].name,row,not a.scanning)
        end
        if #a.photos==0 then items[#items+1]=text("no.photos",c.empty,row,true) end
    end
    if total>pageSize then
        header[#header+1]=view.row({key="paging",width="fill",height=row,gap=row*0.25,children={
            button("page.previous",c.previous,row,page>1),text("page",tostring(page).." / "..math.ceil(total/pageSize),row),
            button("page.next",c.next,row,page*pageSize<total)}})
    end
    header[#header+1]=view.scroll({key="list."..m.tab..":"..m.page,width="fill",height="fill",children={
        view.column({key="items",width="fill",height="auto",gap=row*0.25,children=items})}})
    local footer={button("settings",c.settings,row,true)}
    if #a.sources>0 or a.importing or a.picking then footer[#footer+1]=button("clear",c.clearAll,row,not m.preview) end
    header[#header+1]=view.row({key="footer",width="fill",height=row,gap=row*0.25,children=footer})
    return view.column({key="album.panel",width="fill",height="fill",padding=row*0.6,gap=row*0.4,children=header})
end

local function event(context,m,e)
    local a=m.album
    if e.kind=="task.complete" then a:complete(e)
    elseif e.kind=="schedule" and not m.preview then
        local interval=math.max(3,math.min(300,tonumber(storage.get("interval")) or 5))
        a:tick(interval,enabled("paused",false),enabled("shuffle",false))
        if a.image and not a.loading and resource.status(a.image).state~="ready" then a:show(a.index) end
    elseif e.kind=="settings.changed" then a.elapsed=0
    elseif e.kind=="action" then
        local id=e.id or ""
        if id=="manage" then m.confirmClear=false;widget.openPanel({title=copy().manage,width=560,height=600})
        elseif id=="addImages" then a:pick(false)
        elseif id=="addFolders" then a:pick(true)
        elseif id=="import" and e.action=="fileDrop" then
            if e.value==a.dropRevision then a:ingest(e.items) else a:discard(e.items) end
        elseif id=="mode.import" then a:setMode(false)
        elseif id=="mode.bind" then a:setMode(true)
        elseif id=="clear" then m.confirmClear=true;widget.openPanel({title=copy().manage,width=560,height=600})
        elseif id=="clear.cancel" then m.confirmClear=false
        elseif id=="clear.confirm" and m.confirmClear and not m.preview then
            if a:clear() then m.confirmClear=false;m.menuPhoto=nil;m.tab="sources";m.page=1 end
        elseif id=="removePhoto" and m.menuPhoto then
            for index,photo in ipairs(a.photos) do
                if photo.handle==m.menuPhoto.handle and photo.child==m.menuPhoto.child then a:removePhoto(index);break end
            end
            m.menuPhoto=nil
        elseif id=="refresh" then a.warning=nil; a:refresh(nil,true)
        elseif id=="previous" then a:step(-1,false)
        elseif id=="next" then a:step(1,false)
        elseif id=="toggle" then storage.set("paused",not enabled("paused",false)); a.elapsed=0
        elseif id=="settings" then widget.openSettings()
        elseif id=="sources" or id=="photos" then m.tab=id; m.page=1
        elseif id=="page.previous" then m.page=math.max(1,m.page-1)
        elseif id=="page.next" then m.page=m.page+1
        else
            local action,index=id:match("^([a-z]+):(%d+)$"); index=tonumber(index)
            if action=="remove" then a:remove(index)
            elseif action=="up" then a:move(index,-1)
            elseif action=="down" then a:move(index,1)
            elseif action=="photo" then a:show(index); storage.set("paused",true) end
        end
    end
    local total=m.tab=="sources" and #a.sources or #a.photos
    local pageSize=m.tab=="sources" and 20 or 30
    m.page=math.max(1,math.min(m.page,math.max(1,math.ceil(total/pageSize))))
    widget.invalidate()
end

return widget.define({name=l10n.tr("album.name"),useCustomStyle=true,followPersonalizationDefault=true,
    bg=0x18202A,border=0xFFFFFF,alpha=0.42,borderAlpha=0.18,gradientEndA=0.28,
    settings={fields={
        {key="interval",label=l10n.tr("album.interval"),type="range",min=3,max=300,step=1,default=5},
        {key="shuffle",label=l10n.tr("album.shuffle"),type="bool",default=false},
        {key="fill",label=l10n.tr("album.fill"),type="bool",default=true},
    }},
    setup=setup,view=desktop,panel=panel,event=event,
    menu=function(context,m,request)
        if request.id~="album.menu" then return nil end
        local a=m.album;local photo=a.photos[a.loadedIndex or a.index]
        m.menuPhoto=photo and {handle=photo.handle,child=photo.child} or nil
        return ui.menu({{id="removePhoto",label=copy().removePhoto,enabled=photo~=nil and not m.preview},
            {id="clear",label=copy().clearAll,enabled=not m.preview and (#a.sources>0 or a.importing~=nil or a.picking~=nil)},
            {id="manage",label=copy().manage}})
    end,
    -- The host retires this VM's timers. A named cancel here can target the
    -- replacement VM, which is already set up when hotReload disposes us.
    dispose=function(context,m) m.album:dispose() end})
