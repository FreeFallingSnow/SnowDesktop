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
                available = (adapter.usageAvailable == nil and
                    adapter.dedicatedUsageAvailable == nil and adapter.sharedUsageAvailable == nil) or
                    adapter.usageAvailable == true or adapter.dedicatedUsageAvailable == true or
                    adapter.sharedUsageAvailable == true,
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
-- This resolver does not infer identity from a display name; recovery is separate.
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

-- Identity resolution stays strict. Recovery is a separate, explicit fallback
-- policy, not evidence that two equally named GPUs are the same hardware.
function M.rememberGpuChoice(state, value, savedId, savedName, timestamp)
    local sourceId = savedId or ""
    if state.sourceId ~= sourceId then
        state.sourceId, state.choice = sourceId, nil
        state.failedSamples, state.lastTimestamp = 0, nil
    end
    local choice = M.resolveGpuChoice(value, state.choice and state.choice.id or savedId)
    local choices = M.gpuChoices(value)
    if #choices == 0 then return state.choice end
    if choice and choice.available then
        state.failedSamples = 0
    elseif choice then
        -- Count samples, never frames or menu openings. A warming counter or
        -- one missing reading must not make the selected device oscillate.
        if timestamp ~= nil and timestamp ~= state.lastTimestamp then
            state.failedSamples = (state.failedSamples or 0) + 1
        end
    end
    state.lastTimestamp = timestamp
    if not choice or (not choice.available and (sourceId == "" or (state.failedSamples or 0) >= 3)) then
        local preferred, matching, matchingCount = nil, nil, 0
        local name = state.choice and state.choice.name or savedName
        for _, candidate in ipairs(choices) do
            if candidate.available then
                if candidate.name == name then matching, matchingCount = candidate, matchingCount + 1 end
                if not preferred or (nonnegative(candidate.dedicatedMemoryBytes) and candidate.dedicatedMemoryBytes or 0) >
                        (nonnegative(preferred.dedicatedMemoryBytes) and preferred.dedicatedMemoryBytes or 0) then
                    preferred = candidate
                end
            end
        end
        local fallback = matchingCount == 1 and matching or preferred
        if fallback then choice, state.failedSamples = fallback, 0 end
    end
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
