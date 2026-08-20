local alignment = {}

function alignment.target(offset, maximum, step, wheelDelta)
    offset = math.max(0, offset or 0)
    maximum = math.max(0, maximum or 0)
    step = math.max(0, step or 0)
    if step <= 0 or maximum <= 0 or wheelDelta == 0 then
        return math.min(offset, maximum)
    end
    if wheelDelta < 0 then
        return math.min(maximum,
            (math.floor(offset / step) + 1) * step)
    end
    return math.max(0,
        (math.ceil(offset / step) - 1) * step)
end

return alignment
