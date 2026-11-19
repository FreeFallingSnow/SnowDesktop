#include "widget_data_snapshot.h"

#include "common/l10n.h"
#include "common/utils.h"
#include "widget/preview/widget_preview_context.h"
#include "widget_data_broker.h"
#include "widget_system_control_data.h"

#include <algorithm>
#include <array>
#include <utility>

namespace snowdesktop::widget_runtime
{
LuaWidgetDataSnapshot BuildPreviewDataSnapshot(const DataSubscriptionSnapshot& binding,
    LuaWidgetPreviewDataState previewState, const WidgetDesktopDataReader& readDesktop)
{
    LuaWidgetDataSnapshot result;
    result.topic = binding.topic;
    const auto timestampNow = PreviewWallClockMilliseconds;
    if (previewState == LuaWidgetPreviewDataState::PermissionDenied)
    {
        result.error = "permissionDenied";
        return result;
    }
    if (previewState == LuaWidgetPreviewDataState::Loading)
    {
        result.warmingUp = true;
        return result;
    }
    if (previewState == LuaWidgetPreviewDataState::Error)
    {
        result.error = "providerUnavailable";
        return result;
    }
    result.available = true;
    result.stale = false;
    result.timestampMs = timestampNow;
    if (snowdesktop::widget_runtime::IsSystemControlDataTopic(result.topic))
        result.systemControl = snowdesktop::widget_runtime::PreviewSystemControlData(
            result.topic, previewState == LuaWidgetPreviewDataState::Empty);
    if (previewState == LuaWidgetPreviewDataState::Empty &&
        result.topic != "calendar.events")
    {
        if (result.topic == "audio.input.volume") result.available = false;
        return result;
    }
    if (result.topic == "system.cpu")
    {
        result.cpu.available = true;
        result.cpu.warmingUp = false;
        result.cpu.usagePercent = 42.0;
        result.cpu.logicalProcessors = 12;
        result.cpu.name = _L("app.widget_preview.api.cpu");
        result.cpu.timestampMs = timestampNow;
    }
    else if (result.topic == "system.memory")
    {
        result.memory.available = true;
        result.memory.totalBytes = 16ull * 1024 * 1024 * 1024;
        result.memory.usedBytes = 9ull * 1024 * 1024 * 1024;
        result.memory.freeBytes = 7ull * 1024 * 1024 * 1024;
        result.memory.commitLimitBytes = 32ull * 1024 * 1024 * 1024;
        result.memory.commitUsedBytes = 13ull * 1024 * 1024 * 1024;
        result.memory.commitAvailableBytes =
            19ull * 1024 * 1024 * 1024;
        result.memory.usagePercent = 56.25;
        result.memory.timestampMs = timestampNow;
    }
    else if (result.topic == "process.summary")
    {
        result.processSummary.available = true;
        result.processSummary.warmingUp = false;
        result.processSummary.processes = {
            { "process-preview-editor", "Editor.exe", 18.5,
                640ull * 1024 * 1024, 512ull * 1024 * 1024 },
            { "process-preview-player", "Player.exe", 7.25,
                384ull * 1024 * 1024, 320ull * 1024 * 1024 },
        };
        result.processSummary.observedCount = 24;
        result.processSummary.truncated = true;
        result.processSummary.timestampMs = timestampNow;
    }
    else if (result.topic == "system.power")
    {
        result.power.available = true;
        result.power.acPower = false;
        result.power.charging = false;
        result.power.saver = false;
        result.power.batteryPercent = 73.0;
        result.power.estimatedRemainingSeconds = 10800;
        result.power.timestampMs = timestampNow;
    }
    else if (result.topic == "system.screenTime")
    {
        result.screenTime.available = true;
        result.screenTime.enabled = true;
        result.screenTime.totalSeconds = 5400;
        result.screenTime.hours[9] = 1800;
        result.screenTime.hours[10] = 3600;
        wchar_t windows[MAX_PATH]{};
        const UINT windowsLength = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
        const auto directory = windowsLength && windowsLength < ARRAYSIZE(windows)
            ? WideToUtf8(std::wstring(windows, windowsLength)) : std::string{};
        result.screenTime.apps.push_back(snowdesktop::widget_runtime::MakeScreenTimeAppSnapshot("File Explorer", 3600,
            directory.empty() ? std::string{} : "path:" + directory + "\\explorer.exe"));
        result.screenTime.apps.push_back(snowdesktop::widget_runtime::MakeScreenTimeAppSnapshot("Notepad", 1800,
            directory.empty() ? std::string{} : "path:" + directory + "\\System32\\notepad.exe"));
        result.screenTime.timestampMs = timestampNow;
    }
    else if (result.topic == "system.network.status")
    {
        result.networkStatus.available = true;
        result.networkStatus.connectivity = "internet";
        result.networkStatus.transport = "wifi";
        result.networkStatus.costKnown = true;
        result.networkStatus.timestampMs = timestampNow;
    }
    else if (result.topic == "system.network.traffic")
    {
        result.networkTraffic.available = true;
        result.networkTraffic.connected = true;
        result.networkTraffic.warmingUp = false;
        result.networkTraffic.receivedBytes = 987654321;
        result.networkTraffic.sentBytes = 123456789;
        result.networkTraffic.downloadBytesPerSecond = 245760;
        result.networkTraffic.uploadBytesPerSecond = 32768;
        result.networkTraffic.timestampMs = timestampNow;
    }
    else if (result.topic == "system.gpu")
    {
        result.gpu.available = true;
        result.gpu.warmingUp = false;
        result.gpu.timestampMs = timestampNow;
        result.gpu.adapters = {
            { "adapter-1", _L("app.widget_preview.api.gpu"), 38.0,
                8ull * 1024 * 1024 * 1024,
                3ull * 1024 * 1024 * 1024,
                8ull * 1024 * 1024 * 1024,
                1ull * 1024 * 1024 * 1024 }
        };
        auto& adapter = result.gpu.adapters.front();
        adapter.usageAvailable = true;
        adapter.dedicatedUsageAvailable = true;
        adapter.sharedUsageAvailable = true;
        adapter.engines = { { 0, 0, "3D", 38.0, 38.0, 2 }, { 0, 1, "Copy", 4.0, 4.0, 1 } };
    }
    else if (result.topic == "system.storage.volumes")
    {
        result.storageVolumes.available = true;
        result.storageVolumes.timestampMs = timestampNow;
        result.storageVolumes.volumes = {
            { "volume-preview-system", "System", "C:\\", "fixed",
                512ull * 1024 * 1024 * 1024,
                214ull * 1024 * 1024 * 1024, true, false, false },
            { "volume-preview-removable", "Portable", "E:\\",
                "removable", 64ull * 1024 * 1024 * 1024,
                48ull * 1024 * 1024 * 1024, true, true, false }
        };
    }
    else if (result.topic == "system.storage.io")
    {
        result.storageIo.available = true;
        result.storageIo.warmingUp = false;
        result.storageIo.readBytesPerSecond = 18ull * 1024 * 1024;
        result.storageIo.writeBytesPerSecond = 4ull * 1024 * 1024;
        result.storageIo.busyPercent = 27.5;
        result.storageIo.timestampMs = timestampNow;
    }
    else if (result.topic == "system.display.topology")
    {
        result.displayTopology.available = true;
        result.displayTopology.timestampMs = timestampNow;
        result.displayTopology.displays = {
            { "display-preview-primary", "Preview Display", true,
                { 0.0, 0.0, 1280.0, 720.0 },
                { 0.0, 0.0, 1280.0, 680.0 },
                { 0, 0, 1920, 1080 }, { 0, 0, 1920, 1020 },
                144, 144, 1.5, 60.0, "landscape",
                true, true, false }
        };
    }
    else if (result.topic == "system.display.current")
    {
        result.displayCurrent = {
            "display-preview-primary", "Preview Display", true,
            { 0.0, 0.0, 1280.0, 720.0 },
            { 0.0, 0.0, 1280.0, 680.0 },
            { 0, 0, 1920, 1080 }, { 0, 0, 1920, 1020 },
            144, 144, 1.5, 60.0, "landscape",
            true, true, false };
    }
    else if (result.topic == "audio.output.default")
    {
        result.audioOutputDefault.available = true;
        result.audioOutputDefault.id = "audio-output-preview";
        result.audioOutputDefault.name = "Preview Speakers";
        result.audioOutputDefault.state = "active";
        result.audioOutputDefault.timestampMs = timestampNow;
    }
    else if (result.topic == "audio.output.volume")
    {
        result.audioOutputVolume.available = true;
        result.audioOutputVolume.endpointId = "audio-output-preview";
        result.audioOutputVolume.volume = 0.68;
        result.audioOutputVolume.muted = false;
        result.audioOutputVolume.timestampMs = timestampNow;
    }
    else if (result.topic == "audio.output.analysis")
    {
        result.audioAnalysis.available = true;
        result.audioAnalysis.warmingUp = false;
        result.audioAnalysis.silent = false;
        result.audioAnalysis.endpointId = "audio-output-preview";
        result.audioAnalysis.sampleRate = 48000;
        result.audioAnalysis.channels = 2;
        result.audioAnalysis.rms = 0.42;
        result.audioAnalysis.peak = 0.78;
        result.audioAnalysis.timestampMs = timestampNow;
        result.audioAnalysis.waveform.resize(
            snowdesktop::widget_runtime::
                WidgetAudioAnalysisProvider::MaximumWaveformPoints);
        for (std::size_t index = 0;
            index < result.audioAnalysis.waveform.size(); ++index)
        {
            result.audioAnalysis.waveform[index] =
                0.62 * std::sin(
                    static_cast<double>(index) * 0.22);
        }
        result.audioAnalysis.spectrum.resize(
            snowdesktop::widget_runtime::
                WidgetAudioAnalysisProvider::MaximumSpectrumBins);
        for (std::size_t index = 0;
            index < result.audioAnalysis.spectrum.size(); ++index)
        {
            result.audioAnalysis.spectrum[index] =
                std::exp(-static_cast<double>(index) / 14.0);
        }
    }
    else if (result.topic == "media.sessions" ||
        result.topic == "media.current" ||
        result.topic == "media.timeline")
    {
        snowdesktop::widget_runtime::WidgetMediaSessionDataSnapshot
            session;
        session.id = "media-session-preview";
        session.sourceName = "SnowDesktop";
        session.title = _L("app.widget_preview.api.media_title");
        session.artist = _L("app.widget_preview.api.media_artist");
        session.album = _L("app.widget_preview.api.media_album");
        session.playbackStatus = "playing";
        session.current = true;
        session.shuffle = false;
        session.repeatMode = "list";
        session.playbackRate = 1.25;
        session.controls = { true, true, true, true, true, true,
            true, true, true, true };
        session.timeline.available = true;
        session.timeline.sessionId = session.id;
        session.timeline.positionMs = 62000;
        session.timeline.durationMs = 215000;
        session.timeline.minimumSeekMs = 0;
        session.timeline.maximumSeekMs = 215000;
        session.timeline.updatedAtMs = timestampNow;
        session.timeline.timestampMs = timestampNow;
        session.timeline.playbackRate = session.playbackRate;
        if (result.topic == "media.sessions")
        {
            result.mediaSessions.available = true;
            result.mediaSessions.currentSessionId = session.id;
            result.mediaSessions.sessions = { session };
            result.mediaSessions.timestampMs = timestampNow;
        }
        else if (result.topic == "media.current")
        {
            result.mediaCurrent.available = true;
            result.mediaCurrent.session = session;
            result.mediaCurrent.timestampMs = timestampNow;
        }
        else
        {
            result.mediaTimeline = session.timeline;
        }
    }
    else if (result.topic == "media.artwork")
    {
        auto pixels = std::make_shared<snowdesktop::widget_runtime::
            WidgetRuntimeImagePixels>();
        pixels->width = 64;
        pixels->height = 64;
        pixels->stride = pixels->width * 4;
        pixels->bgraPremultiplied.resize(
            pixels->stride * pixels->height);
        for (std::uint32_t y = 0; y < pixels->height; ++y)
        {
            for (std::uint32_t x = 0; x < pixels->width; ++x)
            {
                const std::size_t offset =
                    static_cast<std::size_t>(y * pixels->stride + x * 4);
                pixels->bgraPremultiplied[offset] =
                    static_cast<std::uint8_t>(96 + x * 2);
                pixels->bgraPremultiplied[offset + 1] =
                    static_cast<std::uint8_t>(64 + y * 2);
                pixels->bgraPremultiplied[offset + 2] = 224;
                pixels->bgraPremultiplied[offset + 3] = 255;
            }
        }
        result.mediaArtwork.available = true;
        result.mediaArtwork.sessionId = "media-session-preview";
        result.mediaArtwork.resourceToken = "@media:preview";
        result.mediaArtwork.pixels = std::move(pixels);
        result.mediaArtwork.timestampMs = timestampNow;
        result.mediaArtwork.revision = 1;
    }
    else if (result.topic == "desktop.items" ||
        result.topic == "desktop.selection")
    {
        result.desktopItems = readDesktop(result.topic == "desktop.selection");
        const std::size_t maximum = result.topic == "desktop.items"
            ? 2048u : 512u;
        if (result.desktopItems.size() > maximum)
            result.desktopItems.resize(maximum);
        result.desktopRevision = 1;
    }
    else if (result.topic == "desktop.changes")
    {
        result.desktopRevision = 1;
        result.desktopChangeReason = "preview";
    }
    else if (result.topic == "calendar.events")
    {
        result.calendarRangeStart = binding.options.rangeStart.empty()
            ? "2026-06-01" : binding.options.rangeStart;
        result.calendarRangeEnd = binding.options.rangeEnd.empty()
            ? "2026-10-03" : binding.options.rangeEnd;
        const std::vector<snowdesktop::calendar::CalendarEvent> samples = {
            { "preview-1", 1,
                _L("app.widget_preview.api.calendar_review"),
                "2026-08-02", false, 600, 660,
                _L("app.widget_preview.api.calendar_review_notes"),
                15, {} },
            { "preview-2", 1,
                _L("app.widget_preview.api.calendar_publish"),
                "2026-08-03",
                true, 0, 0, {}, -1, {} },
        };
        for (const auto& event : samples)
        {
            if (previewState != LuaWidgetPreviewDataState::Empty &&
                ((!binding.options.eventId.empty() &&
                    event.id == binding.options.eventId) ||
                (binding.options.eventId.empty() &&
                    event.date >= result.calendarRangeStart &&
                    event.date <= result.calendarRangeEnd)))
            {
                result.calendarEvents.push_back(event);
            }
        }
        if (!binding.options.eventId.empty())
        {
            result.calendarRangeStart.clear();
            result.calendarRangeEnd.clear();
        }
        result.calendarRevision = 1;
        if (binding.options.calendarQuery)
        {
            auto page = snowdesktop::widget_runtime::QueryCalendarEvents(
                result.calendarEvents, *binding.options.calendarQuery);
            result.calendarEvents = std::move(page.events);
            result.calendarTotal = page.total;
            result.calendarHasMore = page.hasMore;
            result.calendarNextOffset = page.nextOffset;
            result.calendarTruncated = page.total > result.calendarEvents.size();
        }
    }
    else if (result.topic == "calendar.selectedDate")
    {
        result.calendarSelectedDate = "2026-08-02";
        result.calendarRevision = 1;
    }
    else if (result.topic == "app.indexStatus")
    {
        result.appIndexState = "ready";
        result.appIndexRevision = 1;
    }
    else if (result.topic == "filesystem.watch")
    {
        result.filesystemWatchEvents.push_back({
            "added", "Preview.txt", {},
            "filesystem:11111111111111111111111111111111",
            "file" });
        result.filesystemWatchRevision = 1;
        result.filesystemWatchOverflow = false;
    }
    if (previewState == LuaWidgetPreviewDataState::Stale)
    {
        result.stale = true;
        result.timestampMs = timestampNow -
            binding.options.requestedInterval.count() *
                snowdesktop::widget_runtime::
                    DataSnapshotStaleIntervalCount - 1;
    }
    return result;
}

bool PopulatePlatformDataSnapshot(LuaWidgetDataSnapshot& result,
    const DataSubscriptionSnapshot& binding, const WidgetSystemDataProvider& system,
    const WidgetAudioAnalysisProvider* audio, bool includeGpuDetails,
    std::int64_t timestampNow, const WidgetDisplayBoundsReader& readDisplayBounds)
{
    const auto setFreshness = [&](std::int64_t timestamp) {
        result.timestampMs = timestamp;
        result.stale = snowdesktop::widget_runtime::IsDataSnapshotStale(
            timestampNow, timestamp, binding.options.requestedInterval);
    };
    if (result.topic == "system.cpu")
    {
        const auto snapshot = system.Cpu();
        if (snapshot)
        {
            result.cpu = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.memory")
    {
        const auto snapshot = system.Memory();
        if (snapshot)
        {
            result.memory = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "process.summary")
    {
        const auto snapshot = system.ProcessSummary();
        if (snapshot)
        {
            result.processSummary = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.power")
    {
        const auto snapshot = system.Power();
        if (snapshot)
        {
            result.power = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.screenTime")
    {
        const auto snapshot = system.ScreenTime();
        if (snapshot)
        {
            result.screenTime = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.network.status")
    {
        const auto snapshot = system.NetworkStatus();
        if (snapshot)
        {
            result.networkStatus = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.network.traffic")
    {
        const auto snapshot = system.NetworkTraffic();
        if (snapshot)
        {
            result.networkTraffic = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.gpu")
    {
        const auto snapshot = system.Gpu(includeGpuDetails);
        if (snapshot)
        {
            result.gpu = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.storage.volumes")
    {
        const auto snapshot =
            system.StorageVolumes();
        if (snapshot)
        {
            result.storageVolumes = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.storage.io")
    {
        const auto snapshot = system.StorageIo();
        if (snapshot)
        {
            result.storageIo = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "system.display.topology")
    {
        const auto snapshot =
            system.DisplayTopology();
        if (snapshot)
        {
            result.displayTopology = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "system.display.current")
    {
        const auto snapshot = system.DisplayCurrent();
        if (snapshot)
        {
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
            const auto bounds = snapshot->available ? readDisplayBounds() : std::nullopt;
            if (bounds)
            {
                const auto display = MatchDisplayByPixelBounds(*snapshot, *bounds);
                if (display)
                {
                    result.displayCurrent = *display;
                    result.available = true;
                }
                else
                {
                    result.error = "currentDisplayUnavailable";
                }
            }
        }
    }
    else if (result.topic == "audio.output.default")
    {
        const auto snapshot =
            system.AudioOutputDefault();
        if (snapshot)
        {
            result.audioOutputDefault = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "audio.output.volume")
    {
        const auto snapshot =
            system.AudioOutputVolume();
        if (snapshot)
        {
            result.audioOutputVolume = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (snowdesktop::widget_runtime::IsSystemControlDataTopic(result.topic))
    {
        const auto snapshot = system.Controls()->Current(result.topic);
        if (snapshot)
        {
            result.systemControl = snapshot->value;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
            if (result.topic == "audio.input.volume" && result.available)
            {
                result.systemControl.object["minimum"] = snowdesktop::system_control::json::Number(0);
                result.systemControl.object["maximum"] = snowdesktop::system_control::json::Number(1);
            }
        }
        else result.warmingUp = true;
    }
    else if (result.topic == "audio.output.analysis")
    {
        const auto snapshot = audio
            ? audio->Snapshot()
            : std::nullopt;
        if (snapshot)
        {
            result.audioAnalysis = *snapshot;
            result.available = snapshot->available;
            result.warmingUp = snapshot->warmingUp;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
        else
        {
            result.warmingUp = true;
        }
    }
    else if (result.topic == "media.sessions")
    {
        const auto snapshot = system.MediaSessions();
        if (snapshot)
        {
            result.mediaSessions = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "media.current")
    {
        const auto snapshot = system.MediaCurrent();
        if (snapshot)
        {
            result.mediaCurrent = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "media.timeline")
    {
        const auto snapshot = system.MediaTimeline();
        if (snapshot)
        {
            result.mediaTimeline = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else if (result.topic == "media.artwork")
    {
        const auto snapshot = system.MediaArtwork();
        if (snapshot)
        {
            result.mediaArtwork = *snapshot;
            result.available = snapshot->available;
            result.error = snapshot->error;
            setFreshness(snapshot->timestampMs);
        }
    }
    else return false;

    return true;
}
}
