local player = module.require("modules/player.lua")

return {
    ["missing and closed media sessions stay inactive"] = function()
        assert(player.session({ available = false }) == nil)
        assert(player.session({ available = true, value = {
            session = { id = "s", playbackStatus = "closed" },
        } }) == nil)
        assert(player.session({ available = true, value = {
            session = { id = "s", playbackStatus = "playing" },
        } }).id == "s")
    end,

    ["artwork and timeline must match the active session"] = function()
        local handle = {}
        assert(player.artwork({ available = true, value = {
            sessionId = "other", image = handle,
        } }, "active") == nil)
        assert(player.artwork({ available = true, value = {
            sessionId = "active", image = handle,
        } }, "active") == handle)

        local session = { id = "active", timeline = { positionMs = 10 } }
        local mismatched = player.timeline({ available = true, value = {
            timeline = { sessionId = "other", positionMs = 20 },
        } }, session)
        assert(mismatched.positionMs == 10)
    end,

    ["progress and seek positions are clipped to the session range"] = function()
        local timeline = {
            positionMs = 12000,
            minimumSeekMs = 2000,
            maximumSeekMs = 10000,
            durationMs = 10000,
        }
        assert(player.progress(timeline) == 1)
        assert(player.seekPosition(timeline, -1) == 2000)
        assert(player.seekPosition(timeline, 0.5) == 6000)
        assert(player.seekPosition(timeline, 2) == 10000)
    end,

    ["media controls require permission and reported capability"] = function()
        local session = { controls = { canStop = true, canSeek = false } }
        assert(player.canControl(session, true, "canStop"))
        assert(not player.canControl(session, false, "canStop"))
        assert(not player.canControl(session, true, "canSeek"))
    end,

    ["visualizer honors every opt in gate and bounds samples"] = function()
        assert(player.shouldAnalyze(true, true, true, false))
        assert(not player.shouldAnalyze(false, true, true, false))
        assert(not player.shouldAnalyze(true, false, true, false))
        assert(not player.shouldAnalyze(true, true, false, false))
        assert(not player.shouldAnalyze(true, true, true, true))
        local values = player.spectrum({ available = true, value = {
            spectrum = { -1, 0.5, 2, math.huge },
        } }, 8, false)
        assert(#values == 8)
        assert(values[1] == 0 and values[3] == 0.5)
        assert(values[5] == 1 and values[7] == 0)
    end,

    ["failed tasks clear pending state and report their action"] = function()
        local tasks = { ["42"] = "media.seek" }
        local name, failed = player.finishTask(tasks, 42, false)
        assert(name == "media.seek" and failed)
        assert(tasks["42"] == nil)
        local missing, missingFailed = player.finishTask(tasks, 42, false)
        assert(missing == nil and not missingFailed)
    end,

    ["time labels use stable media notation"] = function()
        assert(player.formatTime(-1) == "0:00")
        assert(player.formatTime(65000) == "1:05")
        assert(player.formatTime(3661000) == "1:01:01")
    end,
}
