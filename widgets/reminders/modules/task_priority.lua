local taskPriority = { normal = 1 }

function taskPriority.decode(raw)
    local priorities = {}
    for entry in (raw or ""):gmatch("[^,]+") do
        local id, level = entry:match("^(%d+):([0-3])$")
        if id then priorities[id] = tonumber(level) end
    end
    return priorities
end

function taskPriority.encode(ids, priorities)
    local entries = {}
    for _, id in ipairs(ids) do
        local level = priorities[id]
        if level == 0 or level == 2 or level == 3 then
            entries[#entries + 1] = id .. ":" .. level
        end
    end
    return table.concat(entries, ",")
end

function taskPriority.sort(tasks)
    local sorted = {}
    -- Buckets preserve the saved order within each urgency/completion group.
    for _, done in ipairs({ false, true }) do
        for level = 3, 0, -1 do
            for _, task in ipairs(tasks) do
                if task.done == done and
                    (task.priority or taskPriority.normal) == level then
                    sorted[#sorted + 1] = task
                end
            end
        end
    end
    return sorted
end

return taskPriority
