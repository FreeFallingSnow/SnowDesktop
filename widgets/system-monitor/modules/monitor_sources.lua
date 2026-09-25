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

function M.reconcile(handles, cardShown, hasFeature, hasPermission, subscribe)
    for _, source in ipairs(sources) do
        local wanted = (cardShown(source.card) or
            (source.sharedCard and cardShown(source.sharedCard))) and
            hasFeature("data." .. source.topic) and
            hasPermission(source.permission)
        if wanted and not handles[source.key] then
            handles[source.key] = subscribe(source.topic, {
                maxAgeMs = source.age,
                whenHidden = source.hidden or "throttle",
            })
        elseif not wanted and handles[source.key] then
            handles[source.key]:unsubscribe()
            handles[source.key] = nil
        end
    end
end

return M
