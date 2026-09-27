local monitorData = module.require("modules/monitor_data.lua")

local function adapter(id, name, usage, capacity, used)
    return {
        id = id, name = name, usagePercent = usage, usageAvailable = true,
        dedicatedMemoryBytes = capacity, dedicatedUsedBytes = used,
        dedicatedUsageAvailable = true,
        sharedMemoryBytes = 1000, sharedUsedBytes = 100,
        sharedUsageAvailable = true,
    }
end

return {
    ["all GPUs use busiest engine while dedicated memory is summed"] = function()
        local gib = 1024 * 1024 * 1024
        local value = monitorData.summarizeGpu({ adapters = {
            adapter("adapter-1", "A", 30, 8 * gib, 4 * gib),
            adapter("adapter-2", "B", 50, 8 * gib, 5 * gib),
        } }, nil, true)
        assert(value.dedicatedMemoryBytes == 16 * gib)
        assert(value.dedicatedUsedBytes == 9 * gib)
        assert(value.usagePercent == 50 and value.busiestName == "B")
        assert(value.count == 2)
    end,
    ["selection follows stable identity through reorder and disappearance"] = function()
        local a = adapter("adapter-123", "same model", 30, 800, 400)
        local b = adapter("adapter-456", "same model", 70, 1600, 1200)
        for _, adapters in ipairs({ { a, b }, { b, a } }) do
            local value = monitorData.summarizeGpu({ adapters = adapters }, a.id, true)
            assert(value.count == 1 and value.usagePercent == 30)
            assert(value.dedicatedMemoryBytes == 800 and value.dedicatedUsedBytes == 400)
            local choices = monitorData.gpuChoices({ adapters = adapters })
            assert(choices[1].id == a.id and choices[2].id == b.id)
            assert(choices[1].label ~= choices[2].label)
        end
        assert(monitorData.summarizeGpu({ adapters = { b } }, a.id, true) == nil)
        assert(monitorData.summarizeGpu({ adapters = { a, b } }, "", true) == nil)
    end,
    ["unknown usage does not hide independently valid memory or become zero"] = function()
        local a = adapter("adapter-1", "A", 0, 800, 400)
        local b = adapter("adapter-2", "B", 50, 800, 500)
        a.usageAvailable = false
        local value = monitorData.summarizeGpu({ adapters = { a, b } }, nil, true)
        assert(value.usagePercent == nil and value.busiestName == nil)
        assert(value.dedicatedUsedBytes == 900)
        a.usageAvailable, b.dedicatedUsageAvailable = true, false
        value = monitorData.summarizeGpu({ adapters = { a, b } }, nil, true)
        assert(value.usagePercent == 50 and value.dedicatedUsedBytes == nil)
        assert(value.sharedUsedBytes == 200)
        local idle = monitorData.summarizeGpu({ adapters = { a } }, a.id, true)
        assert(idle.usagePercent == 0)
    end,
    ["old hosts never advertise missing channel validity as idle"] = function()
        local a = adapter("adapter-1", "A", 0, 800, 0)
        local value = monitorData.summarizeGpu({ adapters = { a } }, nil, false)
        assert(value.name == "A" and value.dedicatedMemoryBytes == 800)
        assert(value.usagePercent == nil and value.dedicatedUsedBytes == nil)
        assert(value.sharedUsedBytes == nil)
        a.usageAvailable, a.dedicatedUsageAvailable, a.sharedUsageAvailable = nil, nil, nil
        value = monitorData.summarizeGpu({ adapters = { a } }, nil, true)
        assert(value.usagePercent == nil and value.dedicatedUsedBytes == nil)
    end,
    ["UMA does not invent dedicated memory or duplicate shared capacity"] = function()
        local a = adapter("adapter-1", "UMA", 20, 0, 0)
        a.dedicatedUsageAvailable = false
        local value = monitorData.summarizeGpu({ adapters = { a } }, nil, true)
        assert(value.dedicatedMemoryBytes == 0 and value.dedicatedUsedBytes == nil)
        local b = adapter("adapter-2", "dedicated", 40, 800, 200)
        value = monitorData.summarizeGpu({ adapters = { a, b } }, nil, true)
        assert(value.dedicatedMemoryBytes == 800 and value.dedicatedUsedBytes == 200)
        assert(value.sharedMemoryBytes == 1000 and value.sharedUsedBytes == 200)
        assert(monitorData.summarizeGpu(nil, nil, true) == nil)
        assert(monitorData.summarizeGpu({ adapters = {} }, nil, true) == nil)
    end,
}
