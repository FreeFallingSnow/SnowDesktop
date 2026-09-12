local album=module.require("modules/album.lua")
local function fixture(sources, bindFolders)
    local f={calls={},canceled={},saved={},allowed=true,saveOk=true}
    f.a=album.new({start=function(name,args)
        local id=#f.calls+1;f.calls[id]={name=name,args=args};return id end,
        cancel=function(id) f.canceled[id]=true end,
        allowed=function() return f.allowed end,
        save=function(value) if not f.saveOk then error("storage.set: failed to persist storage") end; f.saved=value end,
        random=function(maximum) return maximum end},sources,bindFolders)
    function f:complete(id,value,err) self.a:complete({taskId=id,ok=not err,value=value,error=err}) end
    return f
end
local file={handle="file",name="one.jpg",kind="file"}
local folder={handle="folder",name="Pictures",kind="folder"}
return {
    ["clearing a legacy bound album preserves its mode across restart"]=function()
        local f=fixture({folder},true);local savedMode=false
        f.a.ports.saveMode=function(bind) savedMode=bind end
        assert(f.a:clear() and savedMode)
        local restored=fixture(f.saved,savedMode)
        assert(restored.a:canChangeMode() and restored.a.bindFolders)
        local failed=fixture({folder},true)
        failed.a.ports.saveMode=function() error("mode write failed") end
        assert(not failed.a:clear() and #failed.a.sources==1 and #failed.saved==0 and failed.a.error=="saveFailed")
    end,
    ["adding photos keeps the displayed image position and autoplay progress"]=function()
        local second={handle="two",name="two.jpg",kind="file"}
        local f=fixture({file,second});f.a:refresh();f:complete(1,{image="first"})
        f.a:show(2);f:complete(2,{image="second"});f.a:tick(5,false,false)
        f.a:ingest({{handle="three",name="three.jpg",kind="file"}},false)
        assert(f.a.image=="second" and f.a.loadedIndex==2 and f.a.index==2,
            "appending must never blank the displayed photo or jump to the first image")
        assert(f.a.elapsed==1 and #f.calls==2 and #f.a.photos==3,
            "appending retains playback progress without decoding the same image again")
    end,
    ["binding mode rejects individual images without saving or starting a decode"]=function()
        local f=fixture({},true);f.a:ingest({file},true)
        assert(#f.a.sources==0 and #f.calls==0 and f.a.releases.file,
            "binding mode must discard a dropped image grant without admitting it to the album")
    end,
    ["a second drop during folder import queues new photos without revoking shared grants"]=function()
        local f=fixture({});f.a:ingest({folder},false)
        f.a:ingest({folder,file},false)
        assert(#f.a.importing.queue==2 and not f.a.releases.folder and not f.a.releases.file)
        f:complete(1,{items={file},hasMore=false})
        for _=1,3 do if f.a.releasing then f:complete(f.a.releasing,{}) end end
        assert(not f.a.importing and #f.saved==1 and f.saved[1].handle=="file" and not next(f.a.releases))
    end,
    ["folder import retains individual images and releases the directory"]=function()
        local f=fixture({});f.a:ingest({folder},false)
        assert(f.calls[1].name=="filesystem.list" and f.calls[1].args.grantHandles~=false)
        f:complete(1,{items={file,{handle="txt",name="note.txt",kind="file"}},hasMore=false,nextOffset=2})
        for _=1,3 do if f.a.releasing then f:complete(f.a.releasing,{}) end end
        assert(not f.a.importing and #f.a.sources==1 and f.saved[1].handle=="file")
        assert(f.a.sources[1].kind=="file" and not f.a.photos[1].child and not next(f.a.releases))
        for _,source in ipairs(f.saved) do assert(source.handle~="folder") end
        local restored=fixture(album.decodeSources(album.encodeSources(f.saved)));restored.a:refresh()
        assert(restored.calls[1].name=="filesystem.image" and restored.calls[1].args.handle=="file")
    end,
    ["binding mode keeps folders and removed photos stay excluded after refresh"]=function()
        local f=fixture({},true);f.a:ingest({folder},true)
        assert(f.calls[1].args.grantHandles==false and f.a.sources[1].kind=="folder")
        f:complete(1,{items={{name="one.jpg",kind="file"},{name="two.jpg",kind="file"}},hasMore=false})
        f:complete(2,{image="one"});f.a:removePhoto(1)
        assert(f.saved[1].excluded["one.jpg"] and not f.a.releases.folder)
        f:complete(3,{items={{name="one.jpg",kind="file"},{name="two.jpg",kind="file"}},hasMore=false})
        assert(#f.a.photos==1 and f.a.photos[1].name=="two.jpg")
        local restored=album.decodeSources(album.encodeSources(f.saved))
        assert(restored[1].excluded["one.jpg"])
    end,
    ["modes can change only in an empty idle album and save failures preserve the mode"]=function()
        local f=fixture({});assert(f.a:canChangeMode() and f.a:setMode(true))
        local revision=f.a.dropRevision
        f.a:pick(true);assert(not f.a:setMode(false) and f.a.bindFolders)
        f:complete(1,{items={folder}})
        assert(#f.a.sources==1 and not f.a:setMode(false) and f.a.dropRevision==revision)
        assert(f.a:clear() and f.a:canChangeMode() and f.a:setMode(false))
        assert(not f.a.bindFolders and f.a.dropRevision>revision)
        f.a.ports.saveMode=function() error("save failed") end
        assert(not f.a:setMode(true) and not f.a.bindFolders and f.a.error=="saveFailed")
    end,
    ["clearing cancels an import and releases late grants without restoring photos"]=function()
        local f=fixture({file});f.a:refresh();f:complete(1,{image="current"})
        f.a:ingest({folder},false);local pending=f.a.pending[2]
        assert(pending and pending.importing)
        local revision=f.a.dropRevision
        assert(f.a:clear() and f.canceled[2] and f.a.dropRevision>revision)
        assert(#f.saved==0 and #f.a.photos==0 and not f.a.image and f.a:isClearing())
        f:complete(2,{items={{handle="late",name="late.jpg",kind="file"}},hasMore=false})
        assert(#f.saved==0 and not f.a.importing and f.a.releases.late and f.a.releases.folder and f.a.releases.file)
        for _=1,5 do f.a:flushReleases();if f.a.releasing then f:complete(f.a.releasing,{}) end end
        assert(not f.a:isClearing() and not next(f.a.releases) and not next(f.a.pending))
    end,
    ["clear failure leaves saved sources current pixels and active imports unchanged"]=function()
        local f=fixture({file});f.a:refresh();f:complete(1,{image="current"})
        f.a:ingest({folder},false);local revision=f.a.dropRevision
        f.saveOk=false;assert(not f.a:clear())
        assert(#f.a.sources==1 and f.a.image=="current" and f.a.importing and not f.canceled[2])
        assert(f.a.dropRevision==revision and not f.a:isClearing() and not next(f.a.releases))
    end,
    ["clearing a pending picker discards its result before another mode accepts input"]=function()
        local f=fixture({});f.a:pick(false);assert(f.a:clear() and f.a:setMode(true))
        f.a:pick(true);assert(#f.calls==1)
        f:complete(1,{items={file}})
        assert(not f.a.picking and #f.a.sources==0 and f.a.releases.file)
        f.a:flushReleases();f:complete(f.a.releasing,{})
        f.a:pick(true);assert(f.calls[3].name=="filesystem.pickFolder")
    end,
    ["binding another folder keeps current pixels and list until scanning finishes"]=function()
        local f=fixture({folder},true);f.a:refresh()
        f:complete(1,{items={{kind="file",name="one.jpg"}},hasMore=false});f:complete(2,{image="current"})
        f.a:tick(5,false,false)
        f.a:ingest({{handle="other",kind="folder",name="Other"}},true)
        assert(f.a.scanning and f.a.image=="current" and #f.a.photos==1)
        f:complete(3,{items={{kind="file",name="before.jpg"},{kind="file",name="one.jpg"}},hasMore=false})
        assert(f.a.scanning and f.a.image=="current" and #f.a.photos==1)
        f:complete(4,{items={{kind="file",name="last.jpg"}},hasMore=false})
        assert(not f.a.scanning and f.a.image=="current" and f.a.loadedIndex==2 and f.a.index==2 and #f.a.photos==3)
        assert(#f.calls==4)
        f.a:refresh(nil,true)
        f:complete(5,{items={{kind="file",name="one.jpg"}},hasMore=false})
        f:complete(6,{items={},hasMore=false})
        assert(f.calls[7].name=="filesystem.image" and f.a.image=="current",
            "an explicit refresh reloads changed pixels while keeping the old image visible")
    end,
    ["import failure preserves existing photos and releases unsaved grants"]=function()
        local f=fixture({file});f.saveOk=false
        f.a:ingest({{handle="two",kind="file",name="two.jpg"}},false)
        assert(#f.a.sources==1 and f.a.sources[1].handle=="file" and f.a.releases.two and not f.a.releases.file)
        assert(f.a.error=="saveFailed")
    end,
    ["two rapid directional steps advance twice and autoplay remains available"]=function()
        local sources={};for i=1,5 do sources[i]={handle=tostring(i),name=i..".jpg",kind="file"} end
        local f=fixture(sources);f.a:refresh();f:complete(1,{image="first"})
        f.a:step(1,false);local canceled=f.a.loading;f.a:step(1,false)
        assert(f.a.index==3 and f.canceled[canceled]);f:complete(f.a.loading,{image="third"})
        f.a:tick(1,false,false);assert(f.a.index==4)
        f.a:step(-1,false);f.a:step(-1,false);assert(f.a.index==2)
    end,
    ["compact source persistence round trips Unicode names and existing saves"]=function()
        local sources={};for i=1,128 do sources[i]={handle="h:"..i,name="猫:"..i..".png",kind="file"} end -- l10n-allow: Unicode filename fixture, not UI text
        local encoded=album.encodeSources(sources);assert(#encoded==128 and type(encoded[1])=="string")
        local decoded=album.decodeSources(encoded);assert(#decoded==128 and decoded[128].name=="猫:128.png") -- l10n-allow: Unicode filename fixture, not UI text
        assert(album.decodeSources({folder})[1].kind=="folder")
    end,
    ["single photo stays static and preserves source across restart"]=function()
        local f=fixture({file});f.a:refresh();assert(f.calls[1].name=="filesystem.image")
        f:complete(1,{image="pixels"});for _=1,20 do f.a:tick(3,false,false) end
        assert(#f.calls==1 and f.a.image=="pixels" and #f.a.sources==1)
        local next=fixture(f.a.sources);next.a:refresh();assert(next.calls[1].args.handle=="file")
    end,
    ["mixed folders paginate without allocating child grants"]=function()
        local f=fixture({folder,file});f.a:refresh()
        assert(f.calls[1].args.grantHandles==false)
        f:complete(1,{items={{kind="file",name="A.PNG"},{kind="folder",name="sub"},{kind="file",name="note.txt"}},hasMore=true,nextOffset=100})
        assert(f.calls[2].args.offset==100)
        f:complete(2,{items={{kind="file",name="B.jpg"}},hasMore=false,nextOffset=101})
        assert(#f.a.photos==3 and f.a.photos[3].name=="one.jpg")
        assert(f.calls[3].args.handle=="folder" and f.calls[3].args.name=="A.PNG")
        f:complete(3,{image="A"});f.a:tick(1,true,false);assert(#f.calls==3)
        f.a:tick(1,false,false);assert(f.calls[4].args.name=="B.jpg")
        f:complete(4,{image="B"});f.a:step(1,true);assert(f.a.index~=2)
    end,
    ["canceled refresh and rapid navigation ignore stale completions"]=function()
        local f=fixture({folder,file});f.a:refresh();f.a:remove(1)
        assert(f.canceled[1] and f.calls[2].args.handle=="file")
        f:complete(1,{items={{kind="file",name="ghost.jpg"}},hasMore=false,nextOffset=1})
        assert(#f.a.photos==1)
        f.a:show(1);f:complete(2,{image="stale"});assert(f.a.image==nil)
        f:complete(3,{image="current"});assert(f.a.image=="current")
    end,
    ["broken photos skip once and stop when all are unreadable"]=function()
        local f=fixture({file,{handle="two",name="two.jpg",kind="file"}});f.a:refresh()
        f:complete(1,nil,"imageDecodeFailed");assert(f.calls[2].args.handle=="two")
        f:complete(2,nil,"notFound")
        for _=1,10 do f.a:tick(1,false,false) end
        assert(#f.calls==2 and not f.a.image and f.a.error=="notFound")
    end,
    ["source removal is transactional and releases only unreferenced handles"]=function()
        local f=fixture({folder,file});f.saveOk=false;f.a:remove(1)
        assert(#f.a.sources==2 and not next(f.a.releases))
        f.saveOk=true;f.a:remove(1);f.a:flushReleases()
        assert(#f.a.sources==1 and f.calls[2].name=="filesystem.release")
        f:complete(2,nil,"handleBusy");f.a:flushReleases();assert(f.calls[3].args.handle=="folder")
        f:complete(3,{});assert(not next(f.a.releases))
    end,
    ["multi-select deduplicates and cancel keeps sources"]=function()
        local f=fixture({file});f.a:pick(true);f.a:pick(true);assert(#f.calls==1 and f.calls[1].args.multiple)
        f:complete(1,nil,"userCanceled");assert(#f.a.sources==1 and not f.a.error)
        f.a:add({file,folder,folder});assert(#f.a.sources==2)
    end,
    ["host storage saves return no value and throw on persistence failure"]=function()
        local f=fixture({})
        -- Mirrors lua_StorageSet: success returns zero values; failure raises.
        f.a.ports.save=function(value) f.saved=value end
        f.a:add({file})
        assert(not f.a.error and #f.a.sources==1 and f.saved[1].handle=="file",
            "a successful void storage write must update the album without an error")
        assert(not next(f.a.releases) and f.calls[1].name=="filesystem.image",
            "a successfully saved selection must retain its grant and start decoding")
        f.a.ports.save=function() error("storage.set: failed to persist storage") end
        local ok=pcall(function() f.a:remove(1) end)
        assert(ok and f.a.error=="saveFailed" and #f.a.sources==1 and not next(f.a.releases),
            "a thrown storage failure must preserve the previous album and its grants")
    end,
    ["removing an already revoked source completes without repeated cleanup errors"]=function()
        local f=fixture({file});local start=f.a.ports.start
        f.a.ports.start=function(name,args)
            if name=="filesystem.release" then return nil,"invalidReference" end
            return start(name,args)
        end
        f.a:remove(1);f.a:flushReleases()
        assert(#f.a.sources==0 and not next(f.a.releases) and not f.a.error,
            "an already absent grant must finish cleanup without retrying or showing an error")
        f.a:add({folder})
        assert(#f.a.sources==1 and not f.a.error and f.calls[1].name=="filesystem.list",
            "the user must be able to add another source after removing a revoked one")
    end,
    ["permission loss clears image and late results cannot revive disposed albums"]=function()
        local f=fixture({file});f.a:refresh();f:complete(1,{image="private"})
        f.allowed=false;f.a:tick(5,false,false);assert(not f.a.image and f.a.error=="permission")
        f.allowed=true;f.a:tick(5,false,false);assert(f.calls[2].name=="filesystem.image")
        f.a:dispose();f:complete(2,{image="late"});assert(not f.a.image and f.canceled[2])
    end,
    ["rapid navigation resumes on cancellation completion without waiting for a timer"]=function()
        local f=fixture({file});local start=f.a.ports.start;local busy=false
        f.a.ports.start=function(name,args)
            if busy and name=="filesystem.image" then return nil,"task concurrency limit exceeded" end
            return start(name,args)
        end
        f.a:refresh();busy=true;f.a:show(1)
        assert(f.a.loading<0 and not f.a.error and f.canceled[1])
        f.a:tick(3,false,false);assert(#f.calls==1)
        busy=false;f:complete(1,nil,"canceled")
        assert(#f.calls==2 and f.a.loading==2 and not f.a.image,
            "cancellation completion must immediately start the latest queued photo")
        f:complete(2,{image="new"});assert(f.a.image=="new")
    end,
}
