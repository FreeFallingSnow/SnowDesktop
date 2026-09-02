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

function M.artwork(snapshot, sessionId, minimumTimestampMs)
    if type(snapshot) ~= "table" or snapshot.available ~= true or
        type(snapshot.value) ~= "table" then
        return nil
    end
    local minimumTimestamp = math.max(0, finite(minimumTimestampMs, 0))
    if finite(snapshot.timestamp, 0) < minimumTimestamp then return nil end
    local value = snapshot.value
    if value.sessionId ~= sessionId or value.image == nil then return nil end
    return value.image
end

function M.mediaIdentity(session)
    if type(session) ~= "table" or type(session.id) ~= "string" or
        session.id == "" then
        return nil
    end
    local timeline = type(session.timeline) == "table" and
        session.timeline or {}
    return table.concat({
        session.id,
        tostring(session.title or ""),
        tostring(session.artist or ""),
        tostring(session.album or ""),
        tostring(math.max(0, finite(timeline.durationMs, 0))),
    }, "\31")
end

function M.timelineIdentityConfirmed(snapshotTimestampMs, changedAtMs,
    preview)
    if preview == true or changedAtMs == nil then return true end
    return finite(snapshotTimestampMs, 0) >
        math.max(0, finite(changedAtMs, 0))
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

function M.position(timeline, playing, nowMs)
    local minimum, maximum = timelineBounds(timeline)
    if maximum <= minimum then return minimum end
    local position = M.clamp(timeline.positionMs, minimum, maximum, minimum)
    local updatedAt = math.max(0, finite(timeline.updatedAtMs, 0))
    local now = finite(nowMs, updatedAt)
    if playing == true and updatedAt > 0 and now > updatedAt then
        position = M.clamp(position + now - updatedAt,
            minimum, maximum, minimum)
    end
    return position
end

function M.relativeSeekPosition(timeline, deltaMs, playing, nowMs)
    local minimum, maximum = timelineBounds(timeline)
    local current = M.position(timeline, playing, nowMs)
    return math.floor(M.clamp(current + finite(deltaMs, 0),
        minimum, maximum, current) + 0.5)
end

function M.progress(timeline, positionMs)
    local minimum, maximum = timelineBounds(timeline)
    if maximum <= minimum then return 0 end
    local position = M.clamp(positionMs == nil and timeline.positionMs or
        positionMs, minimum, maximum, minimum)
    return (position - minimum) / (maximum - minimum)
end

function M.optimisticSeekPosition(timeline, targetMs, playing,
    committedAtMs, nowMs)
    local minimum, maximum = timelineBounds(timeline)
    if maximum <= minimum then return minimum end
    local position = M.clamp(targetMs, minimum, maximum, minimum)
    local committedAt = math.max(0, finite(committedAtMs, 0))
    local now = finite(nowMs, committedAt)
    if playing == true and committedAt > 0 and now > committedAt then
        position = M.clamp(position + now - committedAt,
            minimum, maximum, minimum)
    end
    return position
end

function M.seekTimelineCaughtUp(timeline, baselineUpdatedAtMs,
    baselinePositionMs, optimisticPositionMs, playing, nowMs)
    if type(timeline) ~= "table" then return false end
    local updatedAt = math.max(0, finite(timeline.updatedAtMs, 0))
    local baselineUpdatedAt = math.max(0,
        finite(baselineUpdatedAtMs, 0))
    local rawPosition = finite(timeline.positionMs, 0)
    local baselinePosition = finite(baselinePositionMs, rawPosition)
    local timelineChanged = updatedAt > baselineUpdatedAt or
        math.abs(rawPosition - baselinePosition) > 250
    if not timelineChanged then return false end
    local reportedPosition = M.position(timeline, playing, nowMs)
    return math.abs(reportedPosition - finite(optimisticPositionMs,
        reportedPosition)) <= 2000
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

function M.shouldUseArtworkBackground(setting, hasArtwork, highContrast)
    return setting == true and hasArtwork == true and highContrast ~= true
end

function M.shouldSpinRecord(hasArtwork, playbackStatus, reducedMotion,
    visible, preview)
    return hasArtwork == true and playbackStatus == "playing" and
        reducedMotion ~= true and visible ~= false and preview ~= true
end

function M.advanceRecordRotation(rotation, deltaMs)
    local current = M.clamp(rotation, 0, 360, 0) % 360
    local elapsed = M.clamp(deltaMs, 0, 1000, 0)
    return (current + elapsed * 0.024) % 360
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
        local value = M.clamp(source[sourceIndex], 0, 1, 0)
        if not preview and value > 0 then
            value = M.clamp(math.sqrt(value) * 1.15, 0, 1, 0)
        end
        result[index] = value
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
