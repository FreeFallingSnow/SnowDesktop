local M = {}

M.order = { "cpu", "gpu", "memory", "vram", "battery", "disk", "network" }
M.sources = {
    cpu = { topic = "system.cpu", permission = "system.performance.read", age = 1000 },
    gpu = { topic = "system.gpu", permission = "system.performance.read", age = 1000 },
    memory = { topic = "system.memory", permission = "system.performance.read", age = 1000 },
    vram = { topic = "system.gpu", permission = "system.performance.read", age = 1000 },
    battery = { topic = "system.power", permission = "system.power.read", age = 2000 },
    disk = { topic = "system.storage.volumes", permission = "system.storage.read", age = 5000 },
    network = { topic = "system.network.traffic", permission = "system.network.read", age = 1000 },
}

function M.mode(value)
    return M.sources[value] and value or "cpu"
end

function M.number(value)
    return type(value) == "number" and value == value and value >= 0 and value < math.huge
end

function M.percent(value)
    return M.number(value) and math.min(100, value) or nil
end

function M.ratio(used, total)
    if not M.number(used) or not M.number(total) or total <= 0 then return nil end
    return math.min(100, used / total * 100)
end

function M.gpu(value, selected)
    local adapters = value and value.adapters or {}
    if selected and selected ~= "" then
        for _, item in ipairs(adapters) do
            if item.id == selected then return item end
        end
        for _, item in ipairs(adapters) do
            for _, alias in ipairs(item.aliasIds or {}) do
                if alias == selected then return item end
            end
        end
        return nil
    end
    local best
    for _, item in ipairs(adapters) do
        local capacity = M.number(item.dedicatedMemoryBytes) and item.dedicatedMemoryBytes or 0
        local previous = best and M.number(best.dedicatedMemoryBytes) and best.dedicatedMemoryBytes or 0
        if not best or capacity > previous or
                (capacity == previous and item.id < best.id) then best = item end
    end
    return best
end

function M.volume(value, selected)
    local volumes = value and value.volumes or {}
    if selected and selected ~= "" then
        for _, item in ipairs(volumes) do
            if item.id == selected then return item end
        end
        return nil
    end
    local best
    for _, item in ipairs(volumes) do
        if item.capacityAvailable and M.number(item.capacityBytes) and item.capacityBytes > 0 and
                item.kind == "fixed" and (not best or item.mountPoint < best.mountPoint) then
            best = item
        end
    end
    return best
end

function M.read(mode, snapshot, selected, details)
    selected = selected or ""
    if not snapshot then return { status = "unavailable" } end
    if not snapshot.available then
        local status = snapshot.error == "permissionDenied" and "permission" or
            (snapshot.error == "notPresent" and "not_present" or
            (snapshot.warmingUp and "loading" or "unavailable"))
        return { status = status }
    end
    local value = snapshot.value or {}
    local out = { status = "ready", stale = snapshot.stale == true, timestamp = snapshot.timestamp }
    if mode == "cpu" then
        out.percent, out.source = M.percent(value.usagePercent), value.name
    elseif mode == "memory" then
        out.percent = M.ratio(value.usedBytes, value.totalBytes)
        out.used, out.total = value.usedBytes, value.totalBytes
    elseif mode == "gpu" or mode == "vram" then
        local item = M.gpu(value, selected)
        if not item then return { status = selected ~= "" and "device_missing" or "not_present" } end
        out.source, out.deviceId = item.name, item.id
        if mode == "gpu" then
            if details and item.usageAvailable == true then out.percent = M.percent(item.usagePercent) end
        else
            if M.number(item.dedicatedMemoryBytes) and item.dedicatedMemoryBytes == 0 then
                return { status = "no_vram", source = item.name }
            end
            if details and item.dedicatedUsageAvailable == true then
                out.percent = M.ratio(item.dedicatedUsedBytes, item.dedicatedMemoryBytes)
                out.used, out.total = item.dedicatedUsedBytes, item.dedicatedMemoryBytes
            end
        end
    elseif mode == "battery" then
        out.percent = M.percent(value.batteryPercent)
        out.charging, out.acPower, out.saver = value.charging, value.acPower, value.saver
    elseif mode == "disk" then
        local item = M.volume(value, selected)
        if not item then return { status = selected ~= "" and "device_missing" or "not_present" } end
        out.source = item.mountPoint ~= "" and item.mountPoint or item.displayName
        out.deviceId = item.id
        if item.capacityAvailable and M.number(item.capacityBytes) and M.number(item.freeBytes) then
            out.used, out.total = math.max(0, item.capacityBytes - item.freeBytes), item.capacityBytes
            out.percent = M.ratio(out.used, out.total)
        end
    elseif mode == "network" then
        out.download, out.upload = value.downloadBytesPerSecond, value.uploadBytesPerSecond
        out.connected = value.connected
        if not M.number(out.download) or not M.number(out.upload) then out.status = "unavailable" end
    end
    if mode ~= "network" and out.percent == nil then
        out.status = snapshot.warmingUp and "loading" or "unavailable"
    end
    return out
end

function M.append(history, now, reading)
    while #history > 0 and history[1].time < now - 60000 do table.remove(history, 1) end
    if reading.status ~= "ready" or reading.stale then return end
    local last = history[#history]
    if last and last.timestamp == reading.timestamp then return end
    history[#history + 1] = { time = now, timestamp = reading.timestamp,
        value = reading.percent, download = reading.download, upload = reading.upload }
    while #history > 61 do table.remove(history, 1) end
end

function M.mergeHistory(history, now, mode, points)
    local byTimestamp={}
    for _,point in ipairs(history) do
        if point.time>=now-60000 then byTimestamp[math.floor(point.timestamp/1000)]=point end
    end
    for _,point in ipairs(points) do
        if M.number(point.timestamp) and M.number(point.ageMs) and point.ageMs<=60000 then
            local entry={time=now-point.ageMs,timestamp=point.timestamp}
            if mode=="cpu" then entry.value=M.percent(point.value)
            else
                entry.download=M.number(point.value) and point.value or nil
                entry.upload=M.number(point.secondary) and point.secondary or nil
            end
            byTimestamp[math.floor(point.timestamp/1000)]=entry
        end
    end
    for i=#history,1,-1 do history[i]=nil end
    for _,point in pairs(byTimestamp) do history[#history+1]=point end
    table.sort(history,function(a,b) return a.timestamp<b.timestamp end)
    while #history>61 do table.remove(history,1) end
end

return M
