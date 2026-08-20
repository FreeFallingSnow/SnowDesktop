local marquee = {}

function marquee.shouldScroll(textWidth, viewportWidth)
    return math.max(0, textWidth or 0) >
        math.max(0, viewportWidth or 0)
end

function marquee.advance(offset, deltaMs, speed, cycle)
    local advanced = math.max(0, offset or 0) +
        math.max(0, deltaMs or 0) * math.max(0, speed or 0) / 1000
    if cycle and cycle > 0 then
        return advanced % cycle
    end
    return advanced
end

function marquee.position(travel, textWidth, gap)
    local cycle = math.max(1, textWidth or 0) + math.max(0, gap or 0)
    return math.max(0, travel or 0) % cycle, cycle
end

return marquee
