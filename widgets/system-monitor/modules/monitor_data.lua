local M = {}

local function nonnegative(value)
    return type(value) == "number" and value == value and
        value >= 0 and value < math.huge
end

function M.gpuChoices(value)
    local choices, seen, names = {}, {}, {}
    for _, adapter in ipairs(value and value.adapters or {}) do
        if type(adapter.id) == "string" and adapter.id ~= "" and
                not seen[adapter.id] then
            seen[adapter.id] = true
            local name = adapter.name and adapter.name ~= "" and
                adapter.name or adapter.id
            names[name] = (names[name] or 0) + 1
            choices[#choices + 1] = { id = adapter.id, name = name }
        end
    end
    table.sort(choices, function(a, b) return a.id < b.id end)
    for _, choice in ipairs(choices) do
        choice.label = names[choice.name] > 1 and
            (choice.name .. " (" .. choice.id .. ")") or choice.name
    end
    return choices
end

-- nil selects all adapters; an empty or missing stable ID never selects the
-- first adapter. Persist the host's LUID-derived ID, not enumeration order.
function M.summarizeGpu(value, selectedId, detailsAvailable)
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
        count = 0,
    }
    local usageKnown = detailsAvailable == true
    local dedicatedKnown = detailsAvailable == true
    local sharedKnown = detailsAvailable == true
    for _, adapter in ipairs(value.adapters) do
        if selectedId == nil or adapter.id == selectedId then
            summary.count = summary.count + 1
            local name = adapter.name and adapter.name ~= "" and
                adapter.name or adapter.id or "GPU"
            summary.names[#summary.names + 1] = name
            if adapter.usageAvailable == true and
                    nonnegative(adapter.usagePercent) then
                -- The host reports the busiest engine, not a sum of engines.
                if not summary.busiestName or
                        adapter.usagePercent > summary.usagePercent then
                    summary.usagePercent = adapter.usagePercent
                    summary.busiestName = name
                end
            else
                usageKnown = false
            end
            local capacity = adapter.dedicatedMemoryBytes
            if nonnegative(capacity) then
                summary.dedicatedMemoryBytes =
                    summary.dedicatedMemoryBytes + capacity
                -- An integrated adapter with no dedicated memory contributes
                -- no dedicated channel; it must not invalidate other GPUs.
                if capacity > 0 then
                    if adapter.dedicatedUsageAvailable == true and
                            nonnegative(adapter.dedicatedUsedBytes) then
                        summary.dedicatedUsedBytes = summary.dedicatedUsedBytes +
                            adapter.dedicatedUsedBytes
                    else
                        dedicatedKnown = false
                    end
                end
            else
                dedicatedKnown = false
            end
            local sharedCapacity = adapter.sharedMemoryBytes
            if nonnegative(sharedCapacity) then
                -- Shared capacity is the same system-RAM allowance, not
                -- independent VRAM that can be added for every adapter.
                summary.sharedMemoryBytes = math.max(summary.sharedMemoryBytes,
                    sharedCapacity)
                if sharedCapacity > 0 then
                    if adapter.sharedUsageAvailable == true and
                            nonnegative(adapter.sharedUsedBytes) then
                        summary.sharedUsedBytes = summary.sharedUsedBytes +
                            adapter.sharedUsedBytes
                    else
                        sharedKnown = false
                    end
                end
            else
                sharedKnown = false
            end
        end
    end
    if summary.count == 0 then return nil end
    -- An aggregate is unknown when any contributing channel is unknown.
    -- A partial sum or maximum would understate the advertised all-GPU value.
    if not usageKnown then
        summary.usagePercent, summary.busiestName = nil, nil
    end
    if not dedicatedKnown or summary.dedicatedMemoryBytes == 0 then
        summary.dedicatedUsedBytes = nil
    end
    if not sharedKnown or summary.sharedMemoryBytes == 0 then
        summary.sharedUsedBytes = nil
    end
    summary.name = table.concat(summary.names, " · ")
    return summary
end

return M
