local monitorData = module.require("modules/monitor_data.lua")

return {
    ["two dedicated GPUs sum both memory use and capacity"] = function()
        local gib = 1024 * 1024 * 1024
        local value = monitorData.summarizeGpu({ adapters = {
            { name = "A", usagePercent = 30, dedicatedMemoryBytes = 8 * gib,
                dedicatedUsedBytes = 4 * gib },
            { name = "B", usagePercent = 50, dedicatedMemoryBytes = 8 * gib,
                dedicatedUsedBytes = 5 * gib },
        } })
        assert(value.dedicatedMemoryBytes == 16 * gib)
        assert(value.dedicatedUsedBytes == 9 * gib)
        assert(value.usagePercent == 50)
    end,
    ["UMA and absent GPU states do not invent dedicated memory"] = function()
        local value = monitorData.summarizeGpu({ adapters = {
            { dedicatedMemoryBytes = 0, dedicatedUsedBytes = 0,
                sharedMemoryBytes = 1000, sharedUsedBytes = 200 },
            { sharedMemoryBytes = 1000, sharedUsedBytes = 100 },
        } })
        assert(value.dedicatedMemoryBytes == 0)
        assert(value.sharedMemoryBytes == 1000)
        assert(value.sharedUsedBytes == 300)
        assert(monitorData.summarizeGpu(nil) == nil)
        assert(monitorData.summarizeGpu({ adapters = {} }) == nil)
    end,
}
