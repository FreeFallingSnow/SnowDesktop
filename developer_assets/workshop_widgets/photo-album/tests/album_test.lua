local album=module.require("modules/album.lua")
local function fixture(sources)
    local f={calls={},canceled={},saved={},allowed=true,saveOk=true}
    f.a=album.new({start=function(name,args)
        local id=#f.calls+1;f.calls[id]={name=name,args=args};return id end,
        cancel=function(id) f.canceled[id]=true end,
        allowed=function() return f.allowed end,
        save=function(value) if not f.saveOk then error("storage.set: failed to persist storage") end; f.saved=value end,
        random=function(maximum) return maximum end},sources)
    function f:complete(id,value,err) self.a:complete({taskId=id,ok=not err,value=value,error=err}) end
    return f
end
local file={handle="file",name="one.jpg",kind="file"}
local folder={handle="folder",name="Pictures",kind="folder"}
return {
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
    ["rapid changes wait for canceled host tasks to release concurrency slots"]=function()
        local f=fixture({file});local start=f.a.ports.start;local busy=false
        f.a.ports.start=function(name,args)
            if busy and name=="filesystem.image" then return nil,"task concurrency limit exceeded" end
            return start(name,args)
        end
        f.a:refresh();busy=true;f.a:show(1)
        assert(f.a.loading<0 and not f.a.error and f.canceled[1])
        f.a:tick(3,false,false);assert(#f.calls==1)
        busy=false;f:complete(1,{image="old"});f.a:tick(3,false,false)
        assert(#f.calls==2 and f.a.loading==2 and not f.a.image)
        f:complete(2,{image="new"});assert(f.a.image=="new")
    end,
}
