local taskOrder = {}

-- Reorder the complete displayed list, retaining hidden tasks and metadata.
function taskOrder.move(tasks, sourceId, beforeId)
    if sourceId == beforeId then return nil end
    local ids, sourceIndex, targetIndex = {}, nil, nil
    for index, task in ipairs(tasks) do
        ids[index] = task.id
        if task.id == sourceId then sourceIndex = index end
        if task.id == beforeId then targetIndex = index end
    end
    if not sourceIndex or (beforeId and not targetIndex) then return nil end
    table.remove(ids, sourceIndex)
    if targetIndex and sourceIndex < targetIndex then targetIndex = targetIndex - 1 end
    table.insert(ids, targetIndex or (#ids + 1), sourceId)
    for index, task in ipairs(tasks) do
        if ids[index] ~= task.id then return ids end
    end
    return nil
end

-- Midpoints handle wrapped rows and gaps without relying on fixed row heights.
function taskOrder.target(rows, sourceId, contentY)
    local last
    for _, row in ipairs(rows) do
        if row.task.id ~= sourceId then
            if contentY < row.top + row.height / 2 then
                return row.task.id, row.top
            end
            last = row
        end
    end
    return nil, last and (last.top + last.height) or nil
end

function taskOrder.contains(shape, x, y)
    return shape and type(x) == "number" and type(y) == "number" and
        x >= shape.x and x <= shape.x + shape.width and
        y >= shape.y and y <= shape.y + shape.height
end

return taskOrder
