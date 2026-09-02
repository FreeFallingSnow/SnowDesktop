local M = {}

local function finite(value, fallback)
    local number = tonumber(value)
    if not number or number ~= number or number == math.huge or
        number == -math.huge then
        return fallback
    end
    return number
end

function M.clamp(value, minimum, maximum, fallback)
    return math.max(minimum, math.min(maximum,
        finite(value, fallback or minimum)))
end

function M.session(snapshot)
    if type(snapshot) ~= "table" or snapshot.available ~= true or
        type(snapshot.value) ~= "table" then
        return nil
    end
    local session = snapshot.value.session
    if type(session) ~= "table" or type(session.id) ~= "string" or
        session.id == "" or session.playbackStatus == "closed" then
        return nil
    end
    return session
end

function M.artwork(snapshot, sessionId)
    if type(snapshot) ~= "table" or snapshot.available ~= true or
        type(snapshot.value) ~= "table" then
        return nil
    end
    local value = snapshot.value
    if value.sessionId ~= sessionId or value.image == nil then return nil end
    return value.image
end

function M.timeline(snapshot, session)
    local fallback = type(session) == "table" and session.timeline or nil
    if type(snapshot) ~= "table" or snapshot.available ~= true or
        type(snapshot.value) ~= "table" or
        type(snapshot.value.timeline) ~= "table" then
        return fallback
    end
    local value = snapshot.value.timeline
    if type(session) ~= "table" or value.sessionId ~= session.id then
        return fallback
    end
    return value
end

local function timelineBounds(timeline)
    if type(timeline) ~= "table" then return 0, 0 end
    local minimum = math.max(0, finite(timeline.minimumSeekMs, 0))
    local duration = math.max(0, finite(timeline.durationMs, 0))
    local maximum = math.max(minimum,
        finite(timeline.maximumSeekMs, minimum))
    if maximum <= minimum and duration > minimum then
        maximum = duration
    end
    return minimum, maximum
end

function M.hasTimeline(timeline)
    if type(timeline) ~= "table" then return false end
    local minimum, maximum = timelineBounds(timeline)
    return math.max(0, finite(timeline.durationMs, 0)) > 0 or
        maximum > minimum
end

function M.duration(timeline)
    if type(timeline) ~= "table" then return 0 end
    local minimum, maximum = timelineBounds(timeline)
    local duration = math.max(0, finite(timeline.durationMs, 0))
    if duration > 0 then return duration end
    return math.max(0, maximum - minimum)
end

function M.progress(timeline)
    local minimum, maximum = timelineBounds(timeline)
    if maximum <= minimum then return 0 end
    local position = M.clamp(timeline.positionMs, minimum, maximum, minimum)
    return (position - minimum) / (maximum - minimum)
end

function M.seekPosition(timeline, fraction)
    local minimum, maximum = timelineBounds(timeline)
    local normalized = M.clamp(fraction, 0, 1, 0)
    return math.floor(minimum + (maximum - minimum) * normalized + 0.5)
end

function M.formatTime(milliseconds)
    local total = math.floor(math.max(0, finite(milliseconds, 0)) / 1000)
    local hours = math.floor(total / 3600)
    local minutes = math.floor((total % 3600) / 60)
    local seconds = total % 60
    if hours > 0 then
        return string.format("%d:%02d:%02d", hours, minutes, seconds)
    end
    return string.format("%d:%02d", minutes, seconds)
end

function M.canControl(session, permission, capability)
    if permission ~= true or type(session) ~= "table" or
        type(session.controls) ~= "table" then
        return false
    end
    return session.controls[capability] == true
end

function M.shouldAnalyze(setting, hasFeature, hasPermission, reducedMotion)
    return setting == true and hasFeature == true and hasPermission == true and
        reducedMotion ~= true
end

function M.spectrum(snapshot, count, preview)
    local result = {}
    local length = math.max(8, math.min(64,
        math.floor(finite(count, 36) + 0.5)))
    local source
    if preview then
        source = { 0.12, 0.31, 0.62, 0.28, 0.74, 0.43, 0.88, 0.51,
            0.68, 0.36, 0.79, 0.47, 0.58, 0.25, 0.44, 0.18 }
    elseif type(snapshot) == "table" and snapshot.available == true and
        type(snapshot.value) == "table" and
        type(snapshot.value.spectrum) == "table" then
        source = snapshot.value.spectrum
    else
        source = {}
    end
    for index = 1, length do
        local sourceIndex = #source == 0 and 0 or math.max(1,
            math.floor((index - 1) * #source / length) + 1)
        result[index] = M.clamp(source[sourceIndex], 0, 1, 0)
    end
    return result
end

function M.finishTask(tasks, taskId, ok)
    if type(tasks) ~= "table" then return nil, false end
    local key = tostring(taskId)
    local name = tasks[key]
    tasks[key] = nil
    return name, name ~= nil and ok ~= true
end

return M
