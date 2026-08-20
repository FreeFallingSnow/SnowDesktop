local marquee = {}

function marquee.shouldScroll(textWidth, viewportWidth)
    return math.max(0, textWidth or 0) >
        math.max(0, viewportWidth or 0)
end

function marquee.advance(travel, deltaMs, speed)
    return math.max(0, travel or 0) +
        math.max(0, deltaMs or 0) * math.max(0, speed or 0) / 1000
end

function marquee.position(travel, textWidth, gap)
    local cycle = math.max(1, textWidth or 0) + math.max(0, gap or 0)
    return math.max(0, travel or 0) % cycle, cycle
end

return marquee
