local M = {}
local function audioFile(name)
    local extension = type(name) == "string" and name:lower():match("%.([^%.]+)$")
    return extension == "mp3" or extension == "wav"
end
M.audioFile = audioFile

function M.new(api)
    local p = { tracks = {}, sources = {}, pending = {}, scans = {}, releases = {}, index = 0,
        status = "stopped", position = 0, duration = 0, volume = 0.65,
        repeatTrack = false, repeatList = false, generation = 0, error = nil }
    function p:start(name, args, context)
        local id, err = api.start(name, args)
        if id then self.pending[id] = context or { kind = "control" }
        elseif err ~= "userCanceled" then self.error = err end
        return id
    end
    function p:save()
        api.save(self.sources)
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
        self.tracks[#self.tracks + 1] = { handle = handle, name = name, child = child }
        if self.index == 0 then self.index = 1 end
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
            folder and { access = "read", multiple = true } or { extensions = { "mp3", "wav" }, multiple = true },
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
        self.index = index; self.duration = 0; self.error = nil; self.status = "loading"
        self.playingTask = self:start("audio.play", { handle = track.handle, name = track.child,
            volume = self.volume, loop = self.repeatTrack }, { kind = "play", generation = self.generation })
        if not self.playingTask then self.status = "error" end
    end
    function p:toggle()
        if self.playbackId then
            self:start(self.status == "paused" and "audio.resume" or "audio.pause", { playbackId = self.playbackId })
        elseif self.status ~= "loading" then self:play(math.max(1, self.index)) end
    end
    function p:step(delta)
        if #self.tracks == 0 then return end
        self:play((math.max(1, self.index) - 1 + delta) % #self.tracks + 1)
    end
    function p:setVolume(value)
        self.volume = math.max(0, math.min(1, value))
        if self.playbackId then self:start("audio.setVolume", { playbackId = self.playbackId, volume = self.volume }) end
    end
    function p:setLoop(value)
        self.repeatTrack = value
        if self.playbackId then self:start("audio.setLoop", { playbackId = self.playbackId, loop = value }) end
    end
    function p:seek(fraction)
        if self.playbackId and self.duration > 0 then
            self:start("audio.seek", { playbackId = self.playbackId,
                positionMs = math.floor(self.duration * math.max(0, math.min(1, fraction))) })
        end
    end
    function p:clear()
        self:stop(); self.generation = self.generation + 1
        local sources = self.sources; self.sources = {}; self.tracks = {}; self.scans = {}; self.scanning = nil
        self.index = 0; self.duration = 0; self.error = nil; self:save()
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
            self:pump()
        elseif context.kind == "pick" and event.ok then self:ingest(event.value.items or {event.value}) end
        if not event.ok and event.error ~= "canceled" and event.error ~= "userCanceled" and context.kind ~= "release" then
            self.error = event.error
        end
        self:pumpRelease()
    end
    function p:audio(event)
        if event.playbackId ~= self.playbackId then return end
        self.status = event.status; self.position = event.positionMs; self.duration = event.durationMs
        if event.status == "ended" then
            self.playbackId = nil
            if self.index < #self.tracks then self:play(self.index + 1)
            elseif self.repeatList and #self.tracks > 0 then self:play(1) end
        elseif event.status == "error" then self.playbackId = nil; self.error = event.error
        elseif event.status == "stopped" then self.playbackId = nil end
    end
    return p
end
return M
