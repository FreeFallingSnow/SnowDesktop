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
    ["default chooses one dedicated device and never sums other GPUs"] = function()
        local gib = 1024 * 1024 * 1024
        local value = monitorData.summarizeGpu({ adapters = {
            adapter("adapter-1", "A", 30, 4 * gib, 3 * gib),
            adapter("adapter-2", "B", 50, 8 * gib, 5 * gib),
        } }, nil, true)
        assert(value.dedicatedMemoryBytes == 8 * gib)
        assert(value.dedicatedUsedBytes == 5 * gib)
        assert(value.usagePercent == 50 and value.busiestName == "B")
        assert(value.count == 1 and value.name == "B")
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
        assert(monitorData.summarizeGpu({ adapters = { a, b } }, "", true).name == b.name)
    end,
    ["unknown usage does not hide independently valid memory or become zero"] = function()
        local a = adapter("adapter-1", "A", 0, 800, 400)
        local b = adapter("adapter-2", "B", 50, 800, 500)
        a.usageAvailable = false
        local value = monitorData.summarizeGpu({ adapters = { a, b } }, nil, true)
        assert(value.usagePercent == nil and value.busiestName == nil)
        assert(value.dedicatedUsedBytes == 400)
        a.usageAvailable, b.dedicatedUsageAvailable = true, false
        value = monitorData.summarizeGpu({ adapters = { a, b } }, nil, true)
        assert(value.usagePercent == 0 and value.dedicatedUsedBytes == 400)
        assert(value.sharedUsedBytes == 100)
        local selectedB = monitorData.summarizeGpu({ adapters = { a, b } }, b.id, true)
        assert(selectedB.usagePercent == 50 and selectedB.dedicatedUsedBytes == nil)
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
        assert(value.sharedMemoryBytes == 1000 and value.sharedUsedBytes == 100)
        assert(monitorData.summarizeGpu(nil, nil, true) == nil)
        assert(monitorData.summarizeGpu({ adapters = {} }, nil, true) == nil)
    end,
    ["default selection survives reorder and counter availability changes"] = function()
        local integrated = adapter("adapter-1", "Integrated", 90, 0, 0)
        local dedicated = adapter("adapter-9", "Dedicated", 0, 800, 100)
        local other = adapter("adapter-8", "Other", 99, 400, 200)
        for _, adapters in ipairs({ { integrated, dedicated, other },
                { other, dedicated, integrated } }) do
            assert(monitorData.resolveGpuChoice({ adapters = adapters }).id == dedicated.id)
            dedicated.usageAvailable = false
            assert(monitorData.resolveGpuChoice({ adapters = adapters }).id == dedicated.id)
        end
        assert(monitorData.resolveGpuChoice(nil) == nil)
        assert(monitorData.resolveGpuChoice({ adapters = {} }) == nil)
    end,
    ["saved alias resolves only with proven host identity and exact ID wins"] = function()
        local physical = adapter("adapter-9", "Same model", 25, 800, 300)
        physical.aliasIds = { "adapter-old" }
        local second = adapter("adapter-10", "Same model", 50, 800, 400)
        local value = { adapters = { physical, second } }
        assert(monitorData.resolveGpuChoice(value, "adapter-old").id == physical.id)
        assert(monitorData.summarizeGpu(value, "adapter-old", true).usagePercent == 25)
        assert(monitorData.resolveGpuChoice(value, "missing-adapter") == nil)
        physical.aliasIds = { second.id }
        assert(monitorData.resolveGpuChoice(value, second.id).id == second.id)
        physical.aliasIds = nil
        assert(monitorData.resolveGpuChoice(value, "adapter-old") == nil)
        assert(#monitorData.gpuChoices(value) == 2)
    end,
    ["an automatic choice remains selected through disappearance until settings change"] = function()
        local a = adapter("adapter-a", "Dedicated", 10, 800, 100)
        local b = adapter("adapter-b", "Integrated", 20, 0, 0)
        local state = {}
        assert(monitorData.rememberGpuChoice(state, { adapters = { a, b } }).id == a.id)
        assert(monitorData.rememberGpuChoice(state, { adapters = { b } }).id == a.id)
        assert(monitorData.rememberGpuChoice(state, nil).id == a.id)
        assert(monitorData.rememberGpuChoice(state, { adapters = { a, b } }, b.id).id == b.id)
        assert(monitorData.rememberGpuChoice(state, { adapters = { a, b } }, "unknown") == nil)
    end,
    ["proven alias migration stays canonical after the host discards alias history"] = function()
        local a = adapter("adapter-new", "Dedicated", 10, 800, 100)
        a.aliasIds = { "adapter-old" }
        local state, value = {}, { adapters = { a } }
        assert(monitorData.rememberGpuChoice(state, value, "adapter-old").id == a.id)
        a.aliasIds = nil
        assert(monitorData.rememberGpuChoice(state, value, "adapter-old").id == a.id)
        -- A lifecycle callback persisted the canonical ID; a new instance can
        -- now resolve it without the previous sampler's alias history.
        assert(monitorData.rememberGpuChoice({}, value, state.choice.id).id == a.id)
    end,
}
