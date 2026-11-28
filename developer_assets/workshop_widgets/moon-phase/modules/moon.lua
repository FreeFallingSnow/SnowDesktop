local data = module.require("modules/ephemeris_data.lua")
local samples = data.samples:gsub("%s", "")
local sampleCount = #samples // 2
local digits = {}
local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
for index = 1, #alphabet do
    digits[alphabet:byte(index)] = index - 1
end

local function angleSample(index)
    local high, low = samples:byte(index * 2 + 1, index * 2 + 2)
    return digits[high] * 64 + digits[low]
end

local moon = { frameCount = 32 }

function moon.at(now)
    local position = (now - data.start) / data.step
    if position < 0 or position >= sampleCount - 1 then
        return nil
    end

    local index = math.floor(position)
    local angle = angleSample(index)
    local nextAngle = angleSample(index + 1)
    -- Use the short arc when adjacent samples straddle the new moon.
    local difference = (nextAngle - angle + 1800) % 3600 - 1800
    local interpolated = (angle + (position - index) * difference) % 3600
    local frame = math.floor(interpolated * moon.frameCount / 3600 + 0.5) % moon.frameCount
    -- Center the eight named phases on multiples of 45 degrees, starting at new moon.
    local phase = math.floor(interpolated / 450 + 0.5) % 8
    return frame, phase
end

return moon
