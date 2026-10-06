local M = {}

M.defaults = {
    cpu = true, memory = true, gpu = true, vram = true,
    network = true, battery = true, storage = true,
    disk_io = false, uptime = false,
}

function M.cardShown(name, value)
    if value == nil then return M.defaults[name] == true end
    if type(value) == "boolean" then return value end
    return value ~= "0" and value ~= "false"
end

local sources = {
    { key = "cpu", topic = "system.cpu", card = "cpu", age = 1000,
        permission = "system.performance.read" },
    { key = "memory", topic = "system.memory", card = "memory", age = 1000,
        permission = "system.performance.read" },
    { key = "gpu", topic = "system.gpu", card = "gpu", sharedCard = "vram",
        age = 1000, hidden = "pause", permission = "system.performance.read" },
    { key = "power", topic = "system.power", card = "battery", age = 2000,
        permission = "system.power.read" },
    { key = "network", topic = "system.network.traffic", card = "network",
        age = 1000, permission = "system.network.read" },
    { key = "networkStatus", topic = "system.network.status", card = "network",
        age = 2000, permission = "system.network.read" },
    { key = "storage", topic = "system.storage.volumes", card = "storage",
        age = 5000, permission = "system.storage.read" },
    { key = "diskIo", topic = "system.storage.io", card = "disk_io",
        age = 1000, hidden = "pause", permission = "system.storage.read" },
}

-- The caller owns this capability value alongside its handles. Keeping it out
-- of the handle table lets disposal iterate only subscriptions, and needs no
-- weak tables or metatable functions unavailable in the widget sandbox.
function M.reconcile(handles, cardShown, hasFeature, hasPermission, subscribe,
        previousGpuDetails)
    local details = hasFeature("data.system.gpu.details") == true
    if handles.gpu and previousGpuDetails ~= details then
        handles.gpu:unsubscribe()
        handles.gpu = nil
    end
    for _, source in ipairs(sources) do
        local wanted = (cardShown(source.card) or
            (source.sharedCard and cardShown(source.sharedCard))) and
            hasFeature("data." .. source.topic) and
            hasPermission(source.permission)
        if wanted and not handles[source.key] then
            local options = {
                maxAgeMs = source.age,
                whenHidden = source.hidden or "throttle",
            }
            -- Old hosts reject unknown subscription options. Only request
            -- per-channel validity and engine details after the feature probe.
            if source.key == "gpu" and details then
                options.includeDetails = true
            end
            handles[source.key] = subscribe(source.topic, options)
        elseif not wanted and handles[source.key] then
            handles[source.key]:unsubscribe()
            handles[source.key] = nil
        end
    end
    return details
end

return M
