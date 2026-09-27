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
            choices[#choices + 1] = {
                id = adapter.id, name = name, aliasIds = adapter.aliasIds,
                dedicatedMemoryBytes = adapter.dedicatedMemoryBytes,
            }
        end
    end
    table.sort(choices, function(a, b) return a.id < b.id end)
    for _, choice in ipairs(choices) do
        choice.label = names[choice.name] > 1 and
            (choice.name .. " (" .. choice.id .. ")") or choice.name
    end
    return choices
end

-- Only the host can prove that different LUIDs represent the same device.
-- A saved missing device stays missing; never remap it by a display name.
function M.resolveGpuChoice(value, selectedId)
    local choices = M.gpuChoices(value)
    if selectedId and selectedId ~= "" then
        for _, choice in ipairs(choices) do
            if choice.id == selectedId then return choice end
        end
        for _, choice in ipairs(choices) do
            for _, alias in ipairs(type(choice.aliasIds) == "table" and
                    choice.aliasIds or {}) do
                if alias == selectedId then return choice end
            end
        end
        return nil
    end
    -- Prefer a dedicated adapter for GPU/VRAM monitoring. Enumeration order,
    -- changing workload and temporarily missing counters never change the choice.
    local preferred, preferredCapacity
    for _, choice in ipairs(choices) do
        local capacity = nonnegative(choice.dedicatedMemoryBytes) and
            choice.dedicatedMemoryBytes or 0
        if not preferred or capacity > preferredCapacity then
            preferred, preferredCapacity = choice, capacity
        end
    end
    return preferred
end

-- Rendering may remember a proven choice, but persistence belongs to a
-- lifecycle/menu callback. Keep the chosen device through temporary loss.
function M.rememberGpuChoice(state, value, savedId)
    local sourceId = savedId or ""
    if state.sourceId ~= sourceId then
        state.sourceId, state.choice = sourceId, nil
    end
    local choice = M.resolveGpuChoice(value, state.choice and state.choice.id or savedId)
    if choice then state.choice = { id = choice.id, name = choice.name } end
    return state.choice
end

-- A card always describes one concrete device, including on legacy hosts.
function M.summarizeGpu(value, selectedId, detailsAvailable)
    if not value or not value.adapters or #value.adapters == 0 then
        return nil
    end
    local choice = M.resolveGpuChoice(value, selectedId)
    if not choice then return nil end
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
        if adapter.id == choice.id then
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
                -- UMA without dedicated memory has no dedicated channel.
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
                -- This is the selected adapter's system-RAM allowance.
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
            break
        end
    end
    if summary.count == 0 then return nil end
    -- Missing counters remain unknown, independently for each channel.
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
