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
        assert(player.artwork({ available = true, timestamp = 100, value = {
            sessionId = "other", image = handle,
        } }, "active") == nil)
        assert(player.artwork({ available = true, timestamp = 100, value = {
            sessionId = "active", image = handle,
        } }, "active") == handle)
        assert(player.artwork({ available = true, timestamp = 100, value = {
            sessionId = "active", image = handle,
        } }, "active", 101) == nil)

        local session = { id = "active", timeline = { positionMs = 10 } }
        local mismatched = player.timeline({ available = true, value = {
            timeline = { sessionId = "other", positionMs = 20 },
        } }, session)
        assert(mismatched.positionMs == 10)
    end,

    ["media identity changes with tracks but not playback position"] =
        function()
            local first = {
                id = "stable-session",
                title = "First",
                artist = "Artist",
                album = "Album",
                timeline = { positionMs = 1000, durationMs = 120000 },
            }
            local progressed = {
                id = "stable-session",
                title = "First",
                artist = "Artist",
                album = "Album",
                timeline = { positionMs = 90000, durationMs = 120000 },
            }
            local nextTrack = {
                id = "stable-session",
                title = "Second",
                artist = "Artist",
                album = "Album",
                timeline = { positionMs = 0, durationMs = 180000 },
            }
            assert(player.mediaIdentity(first) ==
                player.mediaIdentity(progressed))
            assert(player.mediaIdentity(first) ~=
                player.mediaIdentity(nextTrack))
            assert(player.mediaIdentity(nil) == nil)
            assert(not player.timelineIdentityConfirmed(100, 100, false))
            assert(player.timelineIdentityConfirmed(101, 100, false))
            assert(player.timelineIdentityConfirmed(100, 100, true))
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

    ["playing timelines advance from the Windows update timestamp"] = function()
        local timeline = {
            positionMs = 10000,
            minimumSeekMs = 0,
            maximumSeekMs = 120000,
            durationMs = 120000,
            updatedAtMs = 100000,
        }
        assert(player.position(timeline, true, 102500) == 12500)
        assert(player.progress(timeline,
            player.position(timeline, true, 102500)) == 12500 / 120000)
        assert(player.relativeSeekPosition(
            timeline, 10000, true, 102500) == 22500)
        assert(player.relativeSeekPosition(
            timeline, -10000, true, 102500) == 2500)
        assert(player.position(timeline, false, 102500) == 10000)
        assert(player.relativeSeekPosition(
            timeline, 10000, false, 102500) == 20000)
        assert(player.position(timeline, true, 99000) == 10000)
        assert(player.relativeSeekPosition(
            timeline, -20000, true, 102500) == 0)
        assert(player.position(timeline, true, 500000) == 120000)
    end,

    ["timelines without a usable update timestamp stay at the snapshot"] =
        function()
            local timeline = {
                positionMs = 42000,
                minimumSeekMs = 0,
                maximumSeekMs = 120000,
                durationMs = 120000,
                updatedAtMs = 0,
            }
            assert(player.position(timeline, true, 500000) == 42000)
            assert(player.position(timeline, true, math.huge) == 42000)
        end,

    ["optimistic seeks advance while playback continues"] = function()
        local timeline = {
            positionMs = 10000,
            minimumSeekMs = 0,
            maximumSeekMs = 120000,
            durationMs = 120000,
        }
        assert(player.optimisticSeekPosition(timeline, 60000, true,
            100000, 101500) == 61500)
        assert(player.optimisticSeekPosition(timeline, 60000, false,
            100000, 101500) == 60000)
        assert(player.optimisticSeekPosition(timeline, 119000, true,
            100000, 105000) == 120000)
    end,

    ["optimistic seek waits for a matching timeline update"] = function()
        local stale = {
            positionMs = 10000,
            minimumSeekMs = 0,
            maximumSeekMs = 120000,
            durationMs = 120000,
            updatedAtMs = 100000,
        }
        assert(not player.seekTimelineCaughtUp(stale, 100000, 10000,
            61500, true, 101500))

        local unrelated = {
            positionMs = 12000,
            minimumSeekMs = 0,
            maximumSeekMs = 120000,
            durationMs = 120000,
            updatedAtMs = 101000,
        }
        assert(not player.seekTimelineCaughtUp(unrelated, 100000, 10000,
            61500, true, 101500))

        local caughtUp = {
            positionMs = 60000,
            minimumSeekMs = 0,
            maximumSeekMs = 120000,
            durationMs = 120000,
            updatedAtMs = 100500,
        }
        assert(player.seekTimelineCaughtUp(caughtUp, 100000, 10000,
            61500, true, 101500))
    end,

    ["missing system timeline is not presented as zero duration media"] = function()
        local missing = {
            positionMs = 0,
            minimumSeekMs = 0,
            maximumSeekMs = 0,
            durationMs = 0,
        }
        assert(not player.hasTimeline(missing))
        assert(player.duration(missing) == 0)

        local durationOnly = {
            positionMs = 30000,
            minimumSeekMs = 0,
            maximumSeekMs = 0,
            durationMs = 120000,
        }
        assert(player.hasTimeline(durationOnly))
        assert(player.duration(durationOnly) == 120000)
        assert(player.progress(durationOnly) == 0.25)

        local seekRangeOnly = {
            positionMs = 45000,
            minimumSeekMs = 5000,
            maximumSeekMs = 85000,
            durationMs = 0,
        }
        assert(player.hasTimeline(seekRangeOnly))
        assert(player.duration(seekRangeOnly) == 80000)
        assert(player.progress(seekRangeOnly) == 0.5)
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
        assert(values[1] == 0 and values[3] > 0.8)
        assert(values[5] == 1 and values[7] == 0)
    end,

    ["artwork background requires its setting artwork and standard contrast"] =
        function()
            assert(player.shouldUseArtworkBackground(true, true, false))
            assert(not player.shouldUseArtworkBackground(false, true, false))
            assert(not player.shouldUseArtworkBackground(true, false, false))
            assert(not player.shouldUseArtworkBackground(true, true, true))
        end,

    ["record rotation follows artwork playback and motion gates"] = function()
        assert(player.shouldSpinRecord(true, "playing", false, true, false))
        assert(not player.shouldSpinRecord(false, "playing", false,
            true, false))
        assert(not player.shouldSpinRecord(true, "paused", false,
            true, false))
        assert(not player.shouldSpinRecord(true, "playing", true,
            true, false))
        assert(not player.shouldSpinRecord(true, "playing", false,
            false, false))
        assert(not player.shouldSpinRecord(true, "playing", false,
            true, true))
        assert(math.abs(player.advanceRecordRotation(359, 1000) - 23) <
            0.0001)
        assert(player.advanceRecordRotation(-1, -1) == 0)
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
