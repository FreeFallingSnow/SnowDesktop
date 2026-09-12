local M = {}
local extensions = {jpg=true, jpeg=true, png=true, bmp=true, gif=true, tif=true, tiff=true, webp=true}
M.extensions = {"jpg", "jpeg", "png", "bmp", "gif", "tif", "tiff", "webp"}
M.maximumSources, M.maximumPhotos = 128, 10000

local function field(value) return tostring(#value)..":"..value end
function M.encodeSources(sources)
    -- One string per source stays within the host's 256-node storage limit.
    local result={}
    for _,s in ipairs(sources) do
        local names={};for name in pairs(s.excluded or {}) do names[#names+1]=name end;table.sort(names)
        local value=(s.kind=="folder" and "d" or "f")..field(s.handle)..field(s.name)
        for _,name in ipairs(names) do value=value..field(name) end
        result[#result+1]=value
    end
    return result
end
function M.decodeSources(values)
    local result={}
    for _,value in ipairs(type(values)=="table" and values or {}) do
        if type(value)=="table" then result[#result+1]=value
        elseif type(value)=="string" and (value:sub(1,1)=="d" or value:sub(1,1)=="f") then
            local position=2
            local function read()
                local first,last,length=value:find("(%d+):",position)
                if first~=position then return nil end
                length=tonumber(length);position=last+1
                if length>#value-position+1 then return nil end
                local part=value:sub(position,position+length-1);position=position+length;return part
            end
            local handle,name=read(),read()
            if handle and name then
                local s={handle=handle,name=name,kind=value:sub(1,1)=="d" and "folder" or "file",excluded={}}
                while position<=#value do local name=read();if not name then break end;s.excluded[name]=true end
                result[#result+1]=s
            end
        end
    end
    return result
end

function M.isImage(name)
    return type(name) == "string" and extensions[name:lower():match("%.([^.]+)$")] == true
end

function M.new(ports, sources, bindFolders)
    local a = {ports=ports, sources={}, photos={}, pending={}, releases={}, index=0,
        generation=0, alive=true, elapsed=0, failures=0, deferredId=0,
        bindFolders=bindFolders==true, dropRevision=0}
    local seen = {}
    for _, source in ipairs(type(sources)=="table" and sources or {}) do
        if type(source)=="table" and type(source.handle)=="string" and
            type(source.name)=="string" and (source.kind=="file" or source.kind=="folder") and
            not seen[source.handle] and #a.sources<M.maximumSources then
            seen[source.handle]=true
            a.sources[#a.sources+1]={handle=source.handle,name=source.name,kind=source.kind,excluded=source.excluded}
        end
    end

    function a:canChangeMode()
        return #self.sources==0 and not self.importing and not self.picking and not self.scanning
    end

    function a:isClearing()
        if not self.clearing then return false end
        if self.releasing or next(self.releases) then return true end
        for _,p in pairs(self.pending) do if p.discard then return true end end
        return false
    end

    function a:setMode(bind)
        if not self:canChangeMode() then return false end
        bind=bind==true
        if self.ports.saveMode and not pcall(self.ports.saveMode,bind) then
            self.error="saveFailed";return false
        end
        self.bindFolders=bind;self.dropRevision=self.dropRevision+1
        self.warning=nil;return true
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
            if p.kind=="image" or (p.kind=="list" and not p.importing) then
                if id>0 then self.ports.cancel(id) end
                self.pending[id]=nil
            end
        end
        self.loading=nil; self.scanning=false;self.scanPhotos=nil
    end

    function a:release(handle)
        if type(handle)~="string" then return end
        for _, source in ipairs(self.sources) do if source.handle==handle then return end end
        self.releases[handle]=true
    end

    function a:discard(items)
        local retained={}
        if self.importing then
            for _,item in ipairs(self.importing.queue) do retained[item.handle]=true end
            for _,item in ipairs(self.importing.photos) do retained[item.handle]=true end
        end
        for _,item in ipairs(items or {}) do
            if item.handle and not retained[item.handle] then self:release(item.handle) end
        end
    end

    function a:flushReleases()
        if self.releasing then return end
        for handle in pairs(self.releases) do
            self.releasing=self:start("release",{handle=handle},{handle=handle})
            return
        end
        if self.importWaiting then self:importNext() end
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
        if #sources==#self.sources then
            for _,handle in ipairs(excess) do self:release(handle) end
            return
        end
        if not self:save(sources) then
            for _, item in ipairs(items or {}) do self:release(item.handle) end
            return
        end
        for _, handle in ipairs(excess) do self:release(handle) end
        self:refresh()
    end

    function a:ingest(items, bindFolders)
        if self:isClearing() then self:discard(items);self.warning="busy";return end
        self.clearing=false
        if bindFolders~=nil and bindFolders~=self.bindFolders then self:discard(items);return end
        self.warning=nil
        if self.bindFolders then
            local folders={}
            for _,item in ipairs(items or {}) do
                if item.kind=="folder" then folders[#folders+1]=item
                else self:release(item.handle);self.warning="foldersOnly" end
            end
            if #folders>0 then self:add(folders) end
            return
        end
        if self.importing then
            local retained={}
            for _,item in ipairs(self.importing.queue) do retained[item.handle]=true end
            for handle in pairs(self.importing.seen) do retained[handle]=true end
            for _,item in ipairs(items or {}) do
                if not retained[item.handle] then
                    retained[item.handle]=true
                    if #self.importing.queue<M.maximumSources then
                        self.importing.queue[#self.importing.queue+1]=item
                    else self:release(item.handle);self.warning="limit" end
                end
            end
            return
        end
        self.importing={queue=items or {},index=1,photos={},seen={}}
        self:importNext()
    end

    function a:clear()
        local previous=self.sources
        if not self:save({}) then return false end
        self.clearing=true
        local batch=self.importing
        self.importing=nil;self.importWaiting=nil
        self:cancelWork()
        for id,p in pairs(self.pending) do
            if p.importing or p.kind=="pickOpen" or p.kind=="pickFolder" then
                if id>0 then p.discard=true;self.ports.cancel(id)
                else self.pending[id]=nil end
            end
        end
        self.picking=nil
        self:discard(previous)
        if batch then self:discard(batch.queue);self:discard(batch.photos) end
        self.photos={};self.index=0;self.image=nil;self.loadedPhoto=nil
        self.loadedIndex=nil;self.loadedName=nil;self.resumePhoto=nil
        self.error=nil;self.warning=nil;self.elapsed=0;self.failures=0
        -- Invalidate already captured OLE actions even if the mode is unchanged.
        self.dropRevision=self.dropRevision+1
        return true
    end

    function a:importNext()
        local batch=self.importing;if not batch then return end
        self.importWaiting=nil
        if self.releasing or next(self.releases) then
            self.importWaiting=true;self:flushReleases();return
        end
        while batch.index<=#batch.queue do
            local item=batch.queue[batch.index]
            if item.kind=="folder" then
                if self:start("list",{handle=item.handle,offset=batch.offset or 0,limit=16},
                    {importing=true,source=item,offset=batch.offset or 0}) then return end
                self.warning="scanFailed";self:release(item.handle)
            elseif M.isImage(item.name) then
                if not batch.seen[item.handle] and #batch.photos<M.maximumSources then
                    batch.seen[item.handle]=true;batch.photos[#batch.photos+1]=item
                elseif not batch.seen[item.handle] then self:release(item.handle);self.warning="limit" end
            else self:release(item.handle) end
            batch.index=batch.index+1;batch.offset=nil
        end
        self.importing=nil;self.importWaiting=nil
        self:add(batch.photos)
    end

    function a:removePhoto(index)
        local photo=self.photos[index];if not photo then return end
        local sources={}
        for _,source in ipairs(self.sources) do
            if source.handle~=photo.handle then sources[#sources+1]=source
            elseif photo.child then
                local updated={handle=source.handle,name=source.name,kind=source.kind,excluded={}}
                for name in pairs(source.excluded or {}) do updated.excluded[name]=true end
                updated.excluded[photo.child]=true;sources[#sources+1]=updated
            end
        end
        if self:save(sources) then
            self:refresh(index)
            if not photo.child then self:release(photo.handle) end
        end
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
        if source.excluded and source.excluded[name or source.name] then return true end
        local photos=self.scanPhotos or self.photos
        if #photos>=M.maximumPhotos then self.warning="limit"; return false end
        photos[#photos+1]={handle=source.handle,name=name or source.name,
            child=source.kind=="folder" and name or nil}
        return true
    end

    function a:scanNext()
        while self.sourceIndex<=#self.sources do
            local source=self.sources[self.sourceIndex]
            if #self.scanPhotos>=M.maximumPhotos then self.warning="limit"; break end
            if source.kind=="file" then self:append(source); self.sourceIndex=self.sourceIndex+1
            else
                if self:start("list", {handle=source.handle,offset=0,limit=100,grantHandles=false},
                    {source=source,offset=0}) then return end
                self.warning="scanFailed"; self.sourceIndex=self.sourceIndex+1
            end
        end
        self.scanning=false
        self.photos=self.scanPhotos;self.scanPhotos=nil
        local function find(photo)
            if not photo then return nil end
            for i,item in ipairs(self.photos) do
                if item.handle==photo.handle and item.child==photo.child then return i end
            end
        end
        self.loadedIndex=find(self.loadedPhoto)
        if not self.loadedIndex then
            self.image=nil;self.loadedName=nil;self.loadedPhoto=nil
        end
        if #self.photos==0 then self.index=0;self.elapsed=0;return end
        local index=find(self.resumePhoto) or math.min(self.resumeIndex or 1,#self.photos)
        if self.image and index==self.loadedIndex and not self.reloadImage then self.index=index
        else self:show(index) end
    end

    function a:refresh(resumeIndex, reloadImage)
        self.reloadImage=reloadImage==true
        self.resumePhoto=not resumeIndex and self.photos[self.index] or nil
        self.resumeIndex=resumeIndex or math.max(1,self.index)
        self:cancelWork()
        self.error=nil; self.failures=0;self.sourceIndex=1
        if not self.ports.allowed() then
            self.image=nil;self.loadedPhoto=nil;self.loadedIndex=nil;self.loadedName=nil
            self.error="permission";return
        end
        self.scanPhotos={}
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
        if self.picking or self.importing or self:isClearing() or not self.ports.allowed() then return end
        if self.bindFolders and not folder then self.warning="foldersOnly";return end
        self.picking=self:start(folder and "pickFolder" or "pickOpen", folder and
            {access="read",multiple=true} or {extensions=M.extensions,multiple=true},{bindFolders=self.bindFolders})
    end

    function a:complete(e)
        if not self.alive then return end
        local p=self.pending[e.taskId]
        if not p then
            -- Canceled loads no longer have a pending entry. Their completion
            -- releases the host slot needed by the latest navigation request.
            self:retryDeferred(); return
        end
        self.pending[e.taskId]=nil
        if p.discard then
            if e.ok then self:discard(e.value.items or {e.value}) end
            self:retryDeferred();return
        end
        if p.kind=="release" then
            self.releasing=nil
            if e.ok or e.error=="invalidReference" then self.releases[p.handle]=nil end
            if self.importWaiting and (e.ok or e.error=="invalidReference") then self:flushReleases() end
            return
        elseif p.kind=="pickOpen" or p.kind=="pickFolder" then
            self.picking=nil
            if e.ok then self:ingest(e.value.items or {e.value},p.bindFolders)
            elseif e.error~="userCanceled" and e.error~="canceled" then self.error=e.error end
            return
        end
        if p.importing then
            local batch=self.importing;if not batch then return end
            if e.ok then
                for _,item in ipairs(e.value.items or {}) do
                    if item.kind=="file" and M.isImage(item.name) and #batch.photos<M.maximumSources then
                        if not batch.seen[item.handle] then batch.seen[item.handle]=true;batch.photos[#batch.photos+1]=item end
                    else self:release(item.handle) end
                end
                if e.value.hasMore and #batch.photos<M.maximumSources and e.value.nextOffset>p.offset then
                    batch.offset=e.value.nextOffset;self:importNext();return
                end
                if e.value.hasMore then self.warning="limit" end
            else self.warning="scanFailed" end
            self:release(p.source.handle)
            batch.index=batch.index+1;batch.offset=nil;self:importNext();return
        end
        if p.generation~=self.generation then return end
        if p.kind=="list" then
            if e.ok then
                for _, item in ipairs(e.value.items or {}) do
                    if item.kind=="file" and M.isImage(item.name) then
                        if not self:append(p.source,item.name) then break end
                    end
                end
                if e.value.hasMore and #self.scanPhotos<M.maximumPhotos and
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
                self.loadedPhoto=p.photo
                self.failures=0; self.error=nil; self.elapsed=0
            else
                self.error=e.error or "imageFailed"; self.failures=self.failures+1
                if self.failures<#self.photos and self.ports.allowed() then self:show(self.index+1)
                else self.image=nil; self.loadedName=nil;self.loadedPhoto=nil;self.loadedIndex=nil end
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
