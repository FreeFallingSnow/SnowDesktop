local M = {}
local extensions = {jpg=true, jpeg=true, png=true, bmp=true, gif=true, tif=true, tiff=true, webp=true}
M.extensions = {"jpg", "jpeg", "png", "bmp", "gif", "tif", "tiff", "webp"}
M.maximumSources, M.maximumPhotos = 128, 10000

function M.isImage(name)
    return type(name) == "string" and extensions[name:lower():match("%.([^.]+)$")] == true
end

function M.new(ports, sources)
    local a = {ports=ports, sources={}, photos={}, pending={}, releases={}, index=0,
        generation=0, alive=true, elapsed=0, failures=0, deferredId=0}
    local seen = {}
    for _, source in ipairs(type(sources)=="table" and sources or {}) do
        if type(source)=="table" and type(source.handle)=="string" and
            type(source.name)=="string" and (source.kind=="file" or source.kind=="folder") and
            not seen[source.handle] and #a.sources<M.maximumSources then
            seen[source.handle]=true
            a.sources[#a.sources+1]={handle=source.handle,name=source.name,kind=source.kind}
        end
    end

    function a:start(kind, args, context)
        local id, err = self.ports.start("filesystem."..kind, args)
        context=context or {}; context.kind=kind; context.generation=self.generation
        if not id and (kind=="image" or kind=="list" or kind=="release") and
            (err=="task concurrency limit exceeded" or err=="per-instance task limit exceeded") then
            self.deferredId=self.deferredId-1; id=self.deferredId
            context.args=args
        elseif not id then
            if kind=="release" and err=="invalidReference" then
                self.releases[args.handle]=nil
            else self.error=err or "unavailable" end
            return nil
        end
        self.pending[id]=context
        return id
    end

    function a:retryDeferred()
        local queued={}
        for id,p in pairs(self.pending) do if id<0 then queued[#queued+1]={id=id,p=p} end end
        for _,item in ipairs(queued) do
            local id,err=self.ports.start("filesystem."..item.p.kind,item.p.args)
            if id then
                self.pending[item.id]=nil; self.pending[id]=item.p
                for _,field in ipairs({"loading","picking","releasing"}) do
                    if self[field]==item.id then self[field]=id end
                end
            elseif err~="task concurrency limit exceeded" and err~="per-instance task limit exceeded" then
                self:complete({taskId=item.id,ok=false,error=err or "unavailable"})
            end
        end
    end

    function a:cancelWork()
        self.generation=self.generation+1
        for id, p in pairs(self.pending) do
            if p.kind=="image" or p.kind=="list" then
                if id>0 then self.ports.cancel(id) end
                self.pending[id]=nil
            end
        end
        self.loading=nil; self.scanning=false
    end

    function a:release(handle)
        for _, source in ipairs(self.sources) do if source.handle==handle then return end end
        self.releases[handle]=true
    end

    function a:flushReleases()
        if self.releasing then return end
        for handle in pairs(self.releases) do
            self.releasing=self:start("release",{handle=handle},{handle=handle})
            return
        end
    end

    function a:save(sources)
        -- storage.set succeeds without returning a value and raises on failure.
        local ok=pcall(self.ports.save,sources)
        if not ok then self.error="saveFailed"; return false end
        self.sources=sources; return true
    end

    function a:add(items)
        local sources, seen = {}, {}
        for _, source in ipairs(self.sources) do sources[#sources+1]=source; seen[source.handle]=true end
        local excess={}
        for _, item in ipairs(items or {}) do
            if not seen[item.handle] then
                seen[item.handle]=true
                if #sources<M.maximumSources and (item.kind=="folder" or M.isImage(item.name)) then
                    sources[#sources+1]={handle=item.handle,kind=item.kind,name=item.name}
                else excess[#excess+1]=item.handle; self.warning="limit" end
            end
        end
        if not self:save(sources) then
            for _, item in ipairs(items or {}) do self:release(item.handle) end
            return
        end
        for _, handle in ipairs(excess) do self:release(handle) end
        self:refresh()
    end

    function a:remove(index)
        local removed=self.sources[index]; if not removed then return end
        local sources={}
        for i, source in ipairs(self.sources) do if i~=index then sources[#sources+1]=source end end
        if self:save(sources) then self:refresh(); self:release(removed.handle) end
    end

    function a:move(index, delta)
        local target=index+delta
        if not self.sources[index] or not self.sources[target] then return end
        local sources={}; for i, source in ipairs(self.sources) do sources[i]=source end
        sources[index],sources[target]=sources[target],sources[index]
        if self:save(sources) then self:refresh() end
    end

    function a:append(source, name)
        if #self.photos>=M.maximumPhotos then self.warning="limit"; return false end
        self.photos[#self.photos+1]={handle=source.handle,name=name or source.name,
            child=source.kind=="folder" and name or nil}
        return true
    end

    function a:scanNext()
        while self.sourceIndex<=#self.sources do
            local source=self.sources[self.sourceIndex]
            if #self.photos>=M.maximumPhotos then self.warning="limit"; break end
            if source.kind=="file" then self:append(source); self.sourceIndex=self.sourceIndex+1
            else
                if self:start("list", {handle=source.handle,offset=0,limit=100,grantHandles=false},
                    {source=source,offset=0}) then return end
                self.warning="scanFailed"; self.sourceIndex=self.sourceIndex+1
            end
        end
        self.scanning=false
        if #self.photos>0 then self:show(1) end
    end

    function a:refresh()
        self:cancelWork()
        self.photos={}; self.index=0; self.image=nil; self.loadedName=nil
        self.error=nil; self.failures=0; self.elapsed=0; self.sourceIndex=1
        if not self.ports.allowed() then self.error="permission"; return end
        self.scanning=true; self:scanNext()
    end

    function a:show(index)
        if not self.alive or not self.ports.allowed() or #self.photos==0 then return end
        if self.loading then
            if self.loading>0 then self.ports.cancel(self.loading) end
            self.pending[self.loading]=nil
        end
        self.index=(index-1)%#self.photos+1; self.elapsed=0
        local photo=self.photos[self.index]
        self.loading=self:start("image", {handle=photo.handle,name=photo.child,maxDimension=2048},
            {index=self.index,photo=photo})
    end

    function a:step(delta, shuffle)
        if #self.photos==0 or self.scanning then return end
        self.failures=0; self.error=nil
        local nextIndex=self.index+delta
        if shuffle and #self.photos>1 then nextIndex=self.index+self.ports.random(#self.photos-1) end
        self:show(nextIndex)
    end

    function a:pick(folder)
        if self.picking or not self.ports.allowed() then return end
        self.picking=self:start(folder and "pickFolder" or "pickOpen", folder and
            {access="read",multiple=true} or {extensions=M.extensions,multiple=true})
    end

    function a:complete(e)
        if not self.alive then return end
        local p=self.pending[e.taskId]; if not p then return end
        self.pending[e.taskId]=nil
        if p.kind=="release" then
            self.releasing=nil
            if e.ok or e.error=="invalidReference" then self.releases[p.handle]=nil end
            return
        elseif p.kind=="pickOpen" or p.kind=="pickFolder" then
            self.picking=nil
            if e.ok then self:add(e.value.items or {e.value})
            elseif e.error~="userCanceled" and e.error~="canceled" then self.error=e.error end
            return
        end
        if p.generation~=self.generation then return end
        if p.kind=="list" then
            if e.ok then
                for _, item in ipairs(e.value.items or {}) do
                    if item.kind=="file" and M.isImage(item.name) then
                        if not self:append(p.source,item.name) then break end
                    end
                end
                if e.value.hasMore and #self.photos<M.maximumPhotos and
                    e.value.nextOffset>p.offset then
                    if self:start("list",{handle=p.source.handle,offset=e.value.nextOffset,limit=100,grantHandles=false},
                        {source=p.source,offset=e.value.nextOffset}) then return end
                    self.warning="scanFailed"
                end
            else self.warning="scanFailed" end
            self.sourceIndex=self.sourceIndex+1; self:scanNext()
        elseif p.kind=="image" then
            self.loading=nil
            if e.ok and self.ports.allowed() then
                self.image=e.value.image; self.loadedName=p.photo.name; self.loadedIndex=p.index
                self.failures=0; self.error=nil; self.elapsed=0
            else
                self.error=e.error or "imageFailed"; self.failures=self.failures+1
                if self.failures<#self.photos and self.ports.allowed() then self:show(self.index+1)
                else self.image=nil; self.loadedName=nil end
            end
        end
    end

    function a:tick(interval, paused, shuffle)
        self:flushReleases()
        if not self.ports.allowed() then
            self:cancelWork(); self.image=nil; self.error="permission"; return
        end
        if self.error=="permission" then self:refresh(); return end
        self:retryDeferred()
        if not paused and not self.loading and not self.scanning and self.image and #self.photos>1 then
            self.elapsed=self.elapsed+1
            if self.elapsed>=interval then self:step(1,shuffle) end
        end
    end

    function a:dispose()
        self.alive=false
        for id in pairs(self.pending) do if id>0 then self.ports.cancel(id) end end
        self.pending={}; self.image=nil
    end
    return a
end
return M
