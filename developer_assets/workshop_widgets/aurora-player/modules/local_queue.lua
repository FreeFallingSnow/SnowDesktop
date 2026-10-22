local M = {}
M.extensions = {"mp3","wav","flac","m4a","m4b","aac","adts","wma","ogg","oga","opus"}
local formats = {}
for _, extension in ipairs(M.extensions) do formats[extension] = true end
local function audioFile(name)
    local extension = type(name) == "string" and name:lower():match("%.([^%.]+)$")
    return formats[extension] == true
end
M.audioFile = audioFile

-- Pointer position is in list-content coordinates, including scroll offset.
-- Each item's midpoint divides the two insertion gaps around that item.
function M.dropTarget(from, count, position, extent)
    if count<1 or extent<=0 then return from,nil end
    local boundary=math.max(0,math.min(count,math.floor(position/extent+0.5)))
    local target=boundary<from and boundary+1 or boundary
    target=math.max(1,math.min(count,target))
    if target==from then return from,nil end
    return target,boundary
end

function M.new(api)
    local p = { tracks = {}, sources = {}, pending = {}, scans = {}, releases = {}, index = 0,
        status = "stopped", position = 0, duration = 0, canSeek = false, volume = 1, playbackRate = 1, shuffle = false, repeatMode = "none", nextKey = 0, bag = {}, history = {},
        repeatTrack = false, repeatList = false, generation = 0, error = nil }
    function p:start(name, args, context)
        local id, err = api.start(name, args)
        if id then self.pending[id] = context or { kind = "control" }
        elseif err ~= "userCanceled" then self.error = err end
        return id
    end
    function p:save()
        if api.save(self.sources, self.tracks) == false then self.error = "playlistSaveFailed" end
    end
    function p:pumpRelease()
        for _, context in pairs(self.pending) do
            if context.kind == "release" or context.kind == "scan" then return end
        end
        if #self.releases > 0 then
            local handle = table.remove(self.releases, 1)
            self:start("filesystem.release", { handle = handle }, { kind = "release" })
        end
    end
    function p:release(handle)
        if not handle then return end
        for _, source in ipairs(self.sources) do if source.handle == handle then return end end
        for _, queued in ipairs(self.releases) do if queued == handle then return end end
        self.releases[#self.releases + 1] = handle
        self:pumpRelease()
    end
    function p:append(handle, name, child)
        if #self.tracks >= 2000 then self.error = "playlistLimit"; return end
        for _, track in ipairs(self.tracks) do
            if track.handle == handle and track.child == child then return end
        end
        self.nextKey = self.nextKey + 1
        self.tracks[#self.tracks + 1] = { handle = handle, name = name, child = child, key = "track."..self.nextKey }
        self.bag = {}; self.shuffleExhausted = false
        if self.index == 0 then self.index = 1 end
    end
    function p:restore(sources, tracks)
        self.sources = sources
        for _, track in ipairs(tracks) do self:append(track.handle, track.name, track.child) end
    end
    function p:pump()
        if self.scanning or #self.scans == 0 then return end
        local scan = table.remove(self.scans, 1)
        self.scanning = self:start("filesystem.list", { handle = scan.handle, offset = scan.offset,
            limit = 100, grantHandles = false }, { kind = "scan", scan = scan, generation = self.generation })
    end
    function p:ingest(items, restored)
        for _, item in ipairs(items or {}) do
            local duplicate = false
            for _, source in ipairs(self.sources) do if source.handle == item.handle then duplicate = true end end
            if not duplicate then
                if #self.sources >= 64 then self.error = "playlistLimit"; self:release(item.handle)
                elseif item.kind == "file" and audioFile(item.name) then
                    self.sources[#self.sources + 1] = { handle = item.handle, kind = "file", name = item.name }
                    self:append(item.handle, item.name)
                elseif item.kind == "folder" then
                    self.sources[#self.sources + 1] = { handle = item.handle, kind = "folder", name = item.name }
                    self.scans[#self.scans + 1] = { handle = item.handle, offset = 0 }
                else self:release(item.handle) end
            end
        end
        if not restored then self:save() end
        self:pump()
    end
    function p:pick(folder)
        self:start(folder and "filesystem.pickFolder" or "filesystem.pickOpen",
            folder and { access = "read", multiple = true } or { extensions = M.extensions, multiple = true },
            { kind = "pick", generation = self.generation })
    end
    function p:stop()
        if self.playingTask then api.cancel(self.playingTask); self.playingTask = nil end
        if self.playbackId then self:start("audio.stop", { playbackId = self.playbackId }); self.playbackId = nil end
        self.status = "stopped"; self.position = 0
    end
    function p:play(index)
        self:stop()
        local track = self.tracks[index]
        if not track then return end
        for i=#self.bag,1,-1 do if self.bag[i] == track then table.remove(self.bag,i) end end
        self.index = index; self.duration = 0; self.canSeek = false; self.error = nil; self.status = "loading"
        self.title = nil; self.artist = nil; self.album = nil; self.artwork = nil
        if self.history[#self.history] ~= track then self.history[#self.history + 1] = track end
        if #self.history > 2000 then table.remove(self.history, 1) end
        self.playingTask = self:start("audio.play", { handle = track.handle, name = track.child,
            volume = self.volume, playbackRate = self.playbackRate, shuffle = self.shuffle, repeatMode = self.repeatMode,
            readMetadata = true, systemMediaControls = true }, { kind = "play", generation = self.generation })
        if not self.playingTask then self.status = "error" end
    end
    function p:toggle()
        if self.playbackId then
            self:start(self.status == "paused" and "audio.resume" or "audio.pause", { playbackId = self.playbackId })
        elseif self.status ~= "loading" then self:play(math.max(1, self.index)) end
    end
    function p:step(delta, natural)
        if #self.tracks == 0 then return end
        if self.shuffle then
            local nextTrack
            if delta < 0 and #self.history > 1 then
                table.remove(self.history)
                nextTrack = table.remove(self.history)
            else
                if #self.bag == 0 then
                    if natural and self.shuffleExhausted and self.repeatMode == "none" then return end
                    for _, track in ipairs(self.tracks) do
                        if track ~= self.tracks[self.index] or #self.tracks == 1 then self.bag[#self.bag+1] = track end
                    end
                    self.shuffleExhausted = true
                end
                nextTrack = table.remove(self.bag, math.random(#self.bag))
            end
            for index, track in ipairs(self.tracks) do if track == nextTrack then self:play(index); return end end
        else
            local index = self.index + delta
            if natural and index > #self.tracks and self.repeatMode == "none" then return end
            self:play((index - 1) % #self.tracks + 1)
        end
    end
    function p:setMode(shuffle, repeatMode)
        self.shuffle = shuffle == true
        self.repeatMode = repeatMode == "track" and "track" or repeatMode == "list" and "list" or "none"
        self.repeatTrack = self.repeatMode == "track"; self.repeatList = self.repeatMode == "list"
        self.bag = {}; self.shuffleExhausted = false
        if self.playbackId then self:start("audio.setPlaybackMode", {playbackId=self.playbackId,
            shuffle=self.shuffle, repeatMode=self.repeatMode}) end
        if api.preferences then api.preferences(self) end
    end
    function p:setRate(value)
        self.playbackRate = math.max(0.25, math.min(4, value))
        if self.playbackId then self:start("audio.setPlaybackRate", {playbackId=self.playbackId, playbackRate=self.playbackRate}) end
        if api.preferences then api.preferences(self) end
    end
    function p:command(event)
        if event.playbackId ~= self.playbackId then return end
        if event.command == "next" then self:step(1)
        elseif event.command == "previous" then self:step(-1)
        elseif event.command == "setShuffle" then self:setMode(event.shuffle, self.repeatMode)
        elseif event.command == "setRepeatMode" then self:setMode(self.shuffle, event.repeatMode)
        elseif event.command == "setPlaybackRate" then self:setRate(event.playbackRate) end
    end
    function p:move(from, to)
        from = math.floor(from); to = math.floor(to)
        if not self.tracks[from] or not self.tracks[to] or from == to then return end
        local current = self.tracks[self.index]
        table.insert(self.tracks, to, table.remove(self.tracks, from))
        for index, track in ipairs(self.tracks) do if track == current then self.index = index end end
        self:save()
    end
    function p:remove(index)
        local track = self.tracks[index]
        if not track then return end
        local active = index == self.index and (self.playbackId ~= nil or self.playingTask ~= nil)
        local current = self.tracks[self.index]
        if index == self.index then self:stop(); self.title=nil; self.artist=nil; self.album=nil; self.artwork=nil; self.duration=0 end
        table.remove(self.tracks, index)
        self.index = #self.tracks > 0 and math.min(index, #self.tracks) or 0
        for i, item in ipairs(self.tracks) do if item == current then self.index = i end end
        self.bag = {}; self.history = {}; self.shuffleExhausted = false
        local used = false
        for _, item in ipairs(self.tracks) do if item.handle == track.handle then used = true end end
        if not used and not self.scanning then
            for i=#self.sources,1,-1 do
                if self.sources[i].handle == track.handle then table.remove(self.sources, i) end
            end
            self:release(track.handle)
        end
        self:save()
        if active and self.index > 0 then self:play(self.index) end
    end
    function p:setVolume(value)
        self.volume = math.max(0, math.min(1, value))
        if self.playbackId then self:start("audio.setVolume", { playbackId = self.playbackId, volume = self.volume }) end
    end
    function p:setLoop(value)
        self:setMode(self.shuffle, value and "track" or "none")
    end
    function p:seek(fraction)
        if self.playbackId and self.duration > 0 and self.canSeek then
            self:start("audio.seek", { playbackId = self.playbackId,
                positionMs = math.floor(self.duration * math.max(0, math.min(1, fraction))) })
        end
    end
    function p:clear()
        self:stop(); self.generation = self.generation + 1
        local sources = self.sources; self.sources = {}; self.tracks = {}; self.scans = {}; self.scanning = nil
        self.index = 0; self.duration = 0; self.error = nil; self.title=nil; self.artist=nil; self.album=nil; self.artwork=nil
        self.bag={}; self.history={}; self.shuffleExhausted=false; self:save()
        for _, source in ipairs(sources) do self:release(source.handle) end
    end
    function p:complete(event)
        local context = self.pending[event.taskId]; self.pending[event.taskId] = nil
        if not context then return end
        if context.generation and context.generation ~= self.generation then
            if event.ok and context.kind == "pick" then
                for _, item in ipairs(event.value.items or {event.value}) do self:release(item.handle) end
            elseif event.ok and context.kind == "play" then self:start("audio.stop", {playbackId = event.value.playbackId}) end
            self:pumpRelease()
            return
        end
        if context.kind == "play" then
            if self.playingTask ~= event.taskId then
                if event.ok then self:start("audio.stop", {playbackId = event.value.playbackId}) end
                return
            end
            self.playingTask = nil
            if event.ok then self.playbackId = event.value.playbackId else self.status = "error" end
        elseif context.kind == "scan" then
            self.scanning = nil
            if event.ok then
                local scan = context.scan
                for _, item in ipairs(event.value.items or {}) do
                    if item.kind == "file" and audioFile(item.name) then self:append(scan.handle, item.name, item.name) end
                end
                if event.value.hasMore and #self.tracks < 2000 then
                    self.scans[#self.scans + 1] = { handle = scan.handle, offset = event.value.nextOffset }
                end
            end
            self:pump(); self:save()
        elseif context.kind == "pick" and event.ok then self:ingest(event.value.items or {event.value}) end
        if not event.ok and event.error ~= "canceled" and event.error ~= "userCanceled" and context.kind ~= "release" then
            self.error = event.error
        end
        self:pumpRelease()
    end
    function p:audio(event)
        if event.playbackId ~= self.playbackId then return end
        self.status = event.status; self.position = event.positionMs; self.duration = event.durationMs
        self.canSeek = event.canSeek ~= false
        self.title = event.title ~= "" and event.title or nil
        self.artist = event.artist ~= "" and event.artist or nil
        self.album = event.album ~= "" and event.album or nil
        self.artwork = event.artwork
        if event.volume then self.volume = event.volume end
        if event.playbackRate then self.playbackRate = event.playbackRate end
        if event.status == "ended" then
            self.playbackId = nil
            self:step(1, true)
        elseif event.status == "error" then self.playbackId = nil; self.error = event.error
        elseif event.status == "stopped" then self.playbackId = nil end
    end
    return p
end
return M
