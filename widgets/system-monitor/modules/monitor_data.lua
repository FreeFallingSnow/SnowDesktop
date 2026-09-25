local M = {}

function M.summarizeGpu(value)
    if not value or not value.adapters or #value.adapters == 0 then
        return nil
    end
    local summary = {
        usagePercent = 0,
        dedicatedMemoryBytes = 0,
        dedicatedUsedBytes = 0,
        sharedMemoryBytes = 0,
        sharedUsedBytes = 0,
        names = {},
    }
    for _, adapter in ipairs(value.adapters) do
        summary.usagePercent = math.max(summary.usagePercent,
            adapter.usagePercent or 0)
        summary.dedicatedMemoryBytes = summary.dedicatedMemoryBytes +
            math.max(0, adapter.dedicatedMemoryBytes or 0)
        summary.dedicatedUsedBytes = summary.dedicatedUsedBytes +
            math.max(0, adapter.dedicatedUsedBytes or 0)
        -- Shared capacity is a system-RAM allowance, not independent VRAM
        -- added for every adapter. Keep the largest reported allowance.
        summary.sharedMemoryBytes = math.max(summary.sharedMemoryBytes,
            math.max(0, adapter.sharedMemoryBytes or 0))
        summary.sharedUsedBytes = summary.sharedUsedBytes +
            math.max(0, adapter.sharedUsedBytes or 0)
        if adapter.name and adapter.name ~= "" then
            summary.names[#summary.names + 1] = adapter.name
        end
    end
    summary.name = table.concat(summary.names, " · ")
    return summary
end

return M
