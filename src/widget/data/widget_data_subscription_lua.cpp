#include "widget_data_subscription_lua.h"

#include "widget/data/widget_resource_lua.h"
#include "widget/data/widget_system_control_data.h"
#include "widget/data/widget_gpu_lua.h"
#include "widget/api/widget_calendar_query_options.h"
#include "widget/api/widget_api_registry.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/runtime/widget_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
namespace
{
const char kDataImageFunctions = 0;

const WidgetDataLuaImages* DataImages(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kDataImageFunctions);
    const auto* images = static_cast<const WidgetDataLuaImages*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return images;
}

std::string RegisterDataImage(lua_State* state, std::string_view token,
    const std::shared_ptr<const WidgetRuntimeImagePixels>& pixels,
    const char* source, bool preserveExisting)
{
    const auto* images = DataImages(state);
    return images ? images->registerImage(state, token, pixels, source, preserveExisting)
        : std::string{};
}
}

void BindWidgetDataLuaImages(lua_State* state, const WidgetDataLuaImages& images)
{
    lua_pushlightuserdata(state, const_cast<WidgetDataLuaImages*>(&images));
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kDataImageFunctions);
}

struct LuaDataSubscriptionHandle
{
    WidgetEngine* engine = nullptr;
    std::uint64_t id = 0;
    bool includeGpuDetails = false;
};

constexpr char kDataSubscriptionHandleMetatable[] =
    "SnowDesktop.DataSubscriptionHandle";

static LuaDataSubscriptionHandle* CheckDataSubscriptionHandle(
    lua_State* state, int index)
{
    return static_cast<LuaDataSubscriptionHandle*>(luaL_checkudata(
        state, index, kDataSubscriptionHandleMetatable));
}

static void PushDisplayDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetDisplayDataSnapshot& display)
{
    const auto pushRect = [state](
            const snowdesktop::widget_runtime::
                WidgetDisplayRectDataSnapshot& rect) {
        lua_createtable(state, 0, 4);
        lua_pushnumber(state, rect.x);
        lua_setfield(state, -2, "x");
        lua_pushnumber(state, rect.y);
        lua_setfield(state, -2, "y");
        lua_pushnumber(state, rect.width);
        lua_setfield(state, -2, "width");
        lua_pushnumber(state, rect.height);
        lua_setfield(state, -2, "height");
    };
    const auto pushPixelRect = [state](
            const snowdesktop::widget_runtime::
                WidgetDisplayPixelRectDataSnapshot& rect) {
        lua_createtable(state, 0, 4);
        lua_pushinteger(state, rect.x);
        lua_setfield(state, -2, "x");
        lua_pushinteger(state, rect.y);
        lua_setfield(state, -2, "y");
        lua_pushinteger(state, rect.width);
        lua_setfield(state, -2, "width");
        lua_pushinteger(state, rect.height);
        lua_setfield(state, -2, "height");
    };
    lua_createtable(state, 0, 15);
    lua_pushlstring(state, display.id.data(), display.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, display.name.data(), display.name.size());
    lua_setfield(state, -2, "name");
    lua_pushboolean(state, display.primary);
    lua_setfield(state, -2, "primary");
    pushRect(display.bounds);
    lua_setfield(state, -2, "bounds");
    pushRect(display.workArea);
    lua_setfield(state, -2, "workArea");
    pushPixelRect(display.pixelBounds);
    lua_setfield(state, -2, "pixelBounds");
    pushPixelRect(display.pixelWorkArea);
    lua_setfield(state, -2, "pixelWorkArea");
    lua_pushinteger(state, display.dpiX);
    lua_setfield(state, -2, "dpiX");
    lua_pushinteger(state, display.dpiY);
    lua_setfield(state, -2, "dpiY");
    lua_pushnumber(state, display.scale);
    lua_setfield(state, -2, "scale");
    lua_pushnumber(state, display.refreshHz);
    lua_setfield(state, -2, "refreshHz");
    lua_pushlstring(state, display.orientation.data(),
        display.orientation.size());
    lua_setfield(state, -2, "orientation");
    lua_pushboolean(state, display.hdrKnown);
    lua_setfield(state, -2, "hdrKnown");
    lua_pushboolean(state, display.hdrSupported);
    lua_setfield(state, -2, "hdrSupported");
    lua_pushboolean(state, display.hdrEnabled);
    lua_setfield(state, -2, "hdrEnabled");
}

static void PushMediaTimelineDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetMediaTimelineDataSnapshot& value)
{
    lua_createtable(state, 0, 7);
    lua_pushlstring(state, value.sessionId.data(), value.sessionId.size());
    lua_setfield(state, -2, "sessionId");
    lua_pushinteger(state, static_cast<lua_Integer>(value.positionMs));
    lua_setfield(state, -2, "positionMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.durationMs));
    lua_setfield(state, -2, "durationMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.minimumSeekMs));
    lua_setfield(state, -2, "minimumSeekMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.maximumSeekMs));
    lua_setfield(state, -2, "maximumSeekMs");
    lua_pushinteger(state, static_cast<lua_Integer>(value.updatedAtMs));
    lua_setfield(state, -2, "updatedAtMs");
    if (value.playbackRate)
    {
        lua_pushnumber(state, *value.playbackRate);
        lua_setfield(state, -2, "playbackRate");
    }
}

static void PushMediaControlsDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetMediaControlsDataSnapshot& value)
{
    lua_createtable(state, 0, 10);
    lua_pushboolean(state, value.canPlay);
    lua_setfield(state, -2, "canPlay");
    lua_pushboolean(state, value.canPause);
    lua_setfield(state, -2, "canPause");
    lua_pushboolean(state, value.canPlayPause);
    lua_setfield(state, -2, "canPlayPause");
    lua_pushboolean(state, value.canStop);
    lua_setfield(state, -2, "canStop");
    lua_pushboolean(state, value.canNext);
    lua_setfield(state, -2, "canNext");
    lua_pushboolean(state, value.canPrevious);
    lua_setfield(state, -2, "canPrevious");
    lua_pushboolean(state, value.canSeek);
    lua_setfield(state, -2, "canSeek");
    lua_pushboolean(state, value.canChangePlaybackRate);
    lua_setfield(state, -2, "canChangePlaybackRate");
    lua_pushboolean(state, value.canToggleShuffle);
    lua_setfield(state, -2, "canToggleShuffle");
    lua_pushboolean(state, value.canChangeRepeatMode);
    lua_setfield(state, -2, "canChangeRepeatMode");
}

static void PushMediaSessionDataValue(lua_State* state,
    const snowdesktop::widget_runtime::WidgetMediaSessionDataSnapshot& value)
{
    lua_createtable(state, 0, 12);
    lua_pushlstring(state, value.id.data(), value.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, value.sourceName.data(), value.sourceName.size());
    lua_setfield(state, -2, "sourceName");
    lua_pushlstring(state, value.title.data(), value.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, value.artist.data(), value.artist.size());
    lua_setfield(state, -2, "artist");
    lua_pushlstring(state, value.album.data(), value.album.size());
    lua_setfield(state, -2, "album");
    lua_pushlstring(state, value.playbackStatus.data(),
        value.playbackStatus.size());
    lua_setfield(state, -2, "playbackStatus");
    lua_pushboolean(state, value.current);
    lua_setfield(state, -2, "current");
    if (value.shuffle)
    {
        lua_pushboolean(state, *value.shuffle);
        lua_setfield(state, -2, "shuffle");
    }
    if (value.repeatMode)
    {
        lua_pushlstring(state, value.repeatMode->data(), value.repeatMode->size());
        lua_setfield(state, -2, "repeatMode");
    }
    if (value.playbackRate)
    {
        lua_pushnumber(state, *value.playbackRate);
        lua_setfield(state, -2, "playbackRate");
    }
    PushMediaControlsDataValue(state, value.controls);
    lua_setfield(state, -2, "controls");
    PushMediaTimelineDataValue(state, value.timeline);
    lua_setfield(state, -2, "timeline");
}

static void PushDesktopDataItemValue(
    lua_State* state, const LuaDesktopItemInfo& item)
{
    lua_createtable(state, 0, 6);
    lua_pushlstring(state, item.id.data(), item.id.size());
    lua_setfield(state, -2, "id");
    lua_pushlstring(state, item.title.data(), item.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, item.source.data(), item.source.size());
    lua_setfield(state, -2, "source");
    lua_pushlstring(state, item.type.data(), item.type.size());
    lua_setfield(state, -2, "type");
    lua_pushboolean(state, item.selected);
    lua_setfield(state, -2, "selected");
}

static void PushCalendarDataEventValue(lua_State* state,
    const snowdesktop::calendar::CalendarEvent& event)
{
    lua_createtable(state, 0, 12);
    lua_pushlstring(state, event.id.data(), event.id.size());
    lua_setfield(state, -2, "id");
    lua_pushinteger(state, event.revision);
    lua_setfield(state, -2, "revision");
    lua_pushlstring(state, event.title.data(), event.title.size());
    lua_setfield(state, -2, "title");
    lua_pushlstring(state, event.date.data(), event.date.size());
    lua_setfield(state, -2, "date");
    lua_pushboolean(state, event.allDay);
    lua_setfield(state, -2, "allDay");
    lua_pushinteger(state, event.startMinutes);
    lua_setfield(state, -2, "startMinutes");
    lua_pushinteger(state, event.endMinutes);
    lua_setfield(state, -2, "endMinutes");
    lua_pushlstring(state, event.notes.data(), event.notes.size());
    lua_setfield(state, -2, "notes");
    lua_pushinteger(state, event.reminderMinutes);
    lua_setfield(state, -2, "reminderMinutes");
    if (!event.seriesId.empty())
    {
        lua_pushlstring(state, event.seriesId.data(), event.seriesId.size());
        lua_setfield(state, -2, "seriesId");
        lua_pushlstring(state, event.occurrenceDate.data(), event.occurrenceDate.size());
        lua_setfield(state, -2, "occurrenceDate");
        lua_pushboolean(state, event.occurrenceOverride);
        lua_setfield(state, -2, "occurrenceOverride");
    }
}

static void PushDataSnapshotEnvelope(lua_State* state,
    const std::optional<LuaWidgetDataSnapshot>& snapshot,
    const char* missingError = "unsubscribed", bool includeGpuDetails = false)
{
    lua_createtable(state, 0, 7);
    const bool available = snapshot && (snapshot->available ||
        (includeGpuDetails && snapshot->topic == "system.gpu" &&
            snowdesktop::widget_runtime::WidgetGpuValueAvailable(snapshot->gpu, true)));
    lua_pushboolean(state, available);
    lua_setfield(state, -2, "available");
    lua_pushboolean(state, !snapshot || snapshot->stale);
    lua_setfield(state, -2, "stale");
    lua_pushboolean(state, snapshot && snapshot->warmingUp);
    lua_setfield(state, -2, "warmingUp");
    lua_pushinteger(state, snapshot
        ? static_cast<lua_Integer>(snapshot->timestampMs) : 0);
    lua_setfield(state, -2, "timestamp");

    const std::string error = snapshot
        ? snapshot->error : std::string(missingError);
    if (!error.empty())
    {
        lua_pushlstring(state, error.data(), error.size());
        lua_setfield(state, -2, "error");
    }
    if (!available) return;
    if (snowdesktop::widget_runtime::IsSystemControlDataTopic(snapshot->topic))
    {
        PushSystemControlJson(state, snapshot->systemControl);
        lua_setfield(state, -2, "value");
        return;
    }

    lua_createtable(state, 0, 6);
    if (snapshot->topic == "system.cpu")
    {
        lua_pushnumber(state, snapshot->cpu.usagePercent);
        lua_setfield(state, -2, "usagePercent");
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->cpu.logicalProcessors));
        lua_setfield(state, -2, "logicalProcessors");
        lua_pushlstring(state, snapshot->cpu.name.data(),
            snapshot->cpu.name.size());
        lua_setfield(state, -2, "name");
    }
    else if (snapshot->topic == "system.memory")
    {
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->memory.totalBytes));
        lua_setfield(state, -2, "totalBytes");
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->memory.usedBytes));
        lua_setfield(state, -2, "usedBytes");
        lua_pushinteger(state,
            static_cast<lua_Integer>(snapshot->memory.freeBytes));
        lua_setfield(state, -2, "freeBytes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->memory.commitLimitBytes));
        lua_setfield(state, -2, "commitLimitBytes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->memory.commitUsedBytes));
        lua_setfield(state, -2, "commitUsedBytes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->memory.commitAvailableBytes));
        lua_setfield(state, -2, "commitAvailableBytes");
        lua_pushnumber(state, snapshot->memory.usagePercent);
        lua_setfield(state, -2, "usagePercent");
    }
    else if (snapshot->topic == "process.summary")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->processSummary.processes.size()), 0);
        int processIndex = 1;
        for (const auto& process : snapshot->processSummary.processes)
        {
            lua_createtable(state, 0, 5);
            lua_pushlstring(state, process.id.data(), process.id.size());
            lua_setfield(state, -2, "id");
            lua_pushlstring(state, process.name.data(), process.name.size());
            lua_setfield(state, -2, "name");
            lua_pushnumber(state, process.cpuPercent);
            lua_setfield(state, -2, "cpuPercent");
            lua_pushinteger(state, static_cast<lua_Integer>(
                process.workingSetBytes));
            lua_setfield(state, -2, "workingSetBytes");
            lua_pushinteger(state, static_cast<lua_Integer>(
                process.privateBytes));
            lua_setfield(state, -2, "privateBytes");
            lua_rawseti(state, -2, processIndex++);
        }
        lua_setfield(state, -2, "processes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->processSummary.observedCount));
        lua_setfield(state, -2, "observedCount");
        lua_pushboolean(state, snapshot->processSummary.truncated);
        lua_setfield(state, -2, "truncated");
    }
    else if (snapshot->topic == "system.power")
    {
        lua_pushboolean(state, snapshot->power.acPower);
        lua_setfield(state, -2, "acPower");
        lua_pushboolean(state, snapshot->power.charging);
        lua_setfield(state, -2, "charging");
        lua_pushboolean(state, snapshot->power.saver);
        lua_setfield(state, -2, "saver");
        lua_pushnumber(state, snapshot->power.batteryPercent);
        lua_setfield(state, -2, "batteryPercent");
        if (snapshot->power.estimatedRemainingSeconds >= 0)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(
                snapshot->power.estimatedRemainingSeconds));
            lua_setfield(state, -2, "estimatedRemainingSeconds");
        }
    }
    else if (snapshot->topic == "system.screenTime")
    {
        if (const auto* images = DataImages(state))
            images->retainScreenTimeIcons(state, snapshot->screenTime);
        lua_pushboolean(state, snapshot->screenTime.enabled);
        lua_setfield(state, -2, "enabled");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->screenTime.totalSeconds));
        lua_setfield(state, -2, "totalSeconds");
        lua_createtable(state, 24, 0);
        for (int hour = 0; hour < 24; ++hour)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(
                snapshot->screenTime.hours[static_cast<std::size_t>(hour)]));
            lua_rawseti(state, -2, hour + 1);
        }
        lua_setfield(state, -2, "hours");
        lua_createtable(state,
            static_cast<int>(snapshot->screenTime.apps.size()), 0);
        int appIndex = 1;
        for (const auto& app : snapshot->screenTime.apps)
        {
            lua_createtable(state, 0, 3);
            lua_pushlstring(state, app.name.data(), app.name.size());
            lua_setfield(state, -2, "name");
            lua_pushinteger(state, static_cast<lua_Integer>(app.seconds));
            lua_setfield(state, -2, "seconds");
            const auto token = RegisterDataImage(state,
                app.iconToken, app.icon, "screen-time", true);
            if (!token.empty())
            {
                PushResourceHandle(state, LuaResourceType::Image, token);
                lua_setfield(state, -2, "icon");
            }
            lua_rawseti(state, -2, appIndex++);
        }
        lua_setfield(state, -2, "apps");
    }
    else if (snapshot->topic == "system.network.status")
    {
        lua_pushlstring(state, snapshot->networkStatus.connectivity.data(),
            snapshot->networkStatus.connectivity.size());
        lua_setfield(state, -2, "connectivity");
        lua_pushlstring(state, snapshot->networkStatus.transport.data(),
            snapshot->networkStatus.transport.size());
        lua_setfield(state, -2, "transport");
        lua_pushboolean(state, snapshot->networkStatus.costKnown);
        lua_setfield(state, -2, "costKnown");
        lua_pushboolean(state, snapshot->networkStatus.metered);
        lua_setfield(state, -2, "metered");
        lua_pushboolean(state, snapshot->networkStatus.roaming);
        lua_setfield(state, -2, "roaming");
        lua_pushboolean(state, snapshot->networkStatus.overLimit);
        lua_setfield(state, -2, "overLimit");
    }
    else if (snapshot->topic == "system.network.traffic")
    {
        lua_pushboolean(state, snapshot->networkTraffic.connected);
        lua_setfield(state, -2, "connected");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.receivedBytes));
        lua_setfield(state, -2, "receivedBytes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.sentBytes));
        lua_setfield(state, -2, "sentBytes");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.downloadBytesPerSecond));
        lua_setfield(state, -2, "downloadBytesPerSecond");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->networkTraffic.uploadBytesPerSecond));
        lua_setfield(state, -2, "uploadBytesPerSecond");
    }
    else if (snapshot->topic == "system.gpu")
    {
        snowdesktop::widget_runtime::PushWidgetGpuAdapters(state, snapshot->gpu, includeGpuDetails);
        lua_setfield(state, -2, "adapters");
    }
    else if (snapshot->topic == "system.storage.volumes")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->storageVolumes.volumes.size()), 0);
        int volumeIndex = 1;
        for (const auto& volume : snapshot->storageVolumes.volumes)
        {
            lua_createtable(state, 0, 9);
            lua_pushlstring(state, volume.id.data(), volume.id.size());
            lua_setfield(state, -2, "id");
            lua_pushlstring(state, volume.displayName.data(),
                volume.displayName.size());
            lua_setfield(state, -2, "displayName");
            lua_pushlstring(state, volume.mountPoint.data(),
                volume.mountPoint.size());
            lua_setfield(state, -2, "mountPoint");
            lua_pushlstring(state, volume.kind.data(), volume.kind.size());
            lua_setfield(state, -2, "kind");
            lua_pushinteger(state, static_cast<lua_Integer>(
                volume.capacityBytes));
            lua_setfield(state, -2, "capacityBytes");
            lua_pushinteger(state, static_cast<lua_Integer>(
                volume.freeBytes));
            lua_setfield(state, -2, "freeBytes");
            lua_pushboolean(state, volume.capacityAvailable);
            lua_setfield(state, -2, "capacityAvailable");
            lua_pushboolean(state, volume.removable);
            lua_setfield(state, -2, "removable");
            lua_pushboolean(state, volume.readOnly);
            lua_setfield(state, -2, "readOnly");
            lua_rawseti(state, -2, volumeIndex++);
        }
        lua_setfield(state, -2, "volumes");
    }
    else if (snapshot->topic == "system.storage.io")
    {
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->storageIo.readBytesPerSecond));
        lua_setfield(state, -2, "readBytesPerSecond");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->storageIo.writeBytesPerSecond));
        lua_setfield(state, -2, "writeBytesPerSecond");
        lua_pushnumber(state, snapshot->storageIo.busyPercent);
        lua_setfield(state, -2, "busyPercent");
    }
    else if (snapshot->topic == "system.display.topology")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->displayTopology.displays.size()), 0);
        int displayIndex = 1;
        for (const auto& display : snapshot->displayTopology.displays)
        {
            PushDisplayDataValue(state, display);
            lua_rawseti(state, -2, displayIndex++);
        }
        lua_setfield(state, -2, "displays");
    }
    else if (snapshot->topic == "system.display.current")
    {
        PushDisplayDataValue(state, snapshot->displayCurrent);
        lua_setfield(state, -2, "display");
    }
    else if (snapshot->topic == "audio.output.default")
    {
        lua_pushlstring(state, snapshot->audioOutputDefault.id.data(),
            snapshot->audioOutputDefault.id.size());
        lua_setfield(state, -2, "id");
        lua_pushlstring(state, snapshot->audioOutputDefault.name.data(),
            snapshot->audioOutputDefault.name.size());
        lua_setfield(state, -2, "name");
        lua_pushlstring(state, snapshot->audioOutputDefault.state.data(),
            snapshot->audioOutputDefault.state.size());
        lua_setfield(state, -2, "state");
    }
    else if (snapshot->topic == "audio.output.volume")
    {
        lua_pushlstring(state, snapshot->audioOutputVolume.endpointId.data(),
            snapshot->audioOutputVolume.endpointId.size());
        lua_setfield(state, -2, "endpointId");
        lua_pushnumber(state, snapshot->audioOutputVolume.volume);
        lua_setfield(state, -2, "volume");
        lua_pushboolean(state, snapshot->audioOutputVolume.muted);
        lua_setfield(state, -2, "muted");
        lua_pushnumber(state, 0.0);
        lua_setfield(state, -2, "minimum");
        lua_pushnumber(state, 1.0);
        lua_setfield(state, -2, "maximum");
    }
    else if (snapshot->topic == "audio.output.analysis")
    {
        if (snapshot->audioAnalysis.hasWaveform)
        {
            lua_createtable(state,
                static_cast<int>(snapshot->audioAnalysis.waveform.size()), 0);
            int waveformIndex = 1;
            for (const double value : snapshot->audioAnalysis.waveform)
            {
                lua_pushnumber(state, value);
                lua_rawseti(state, -2, waveformIndex++);
            }
            lua_setfield(state, -2, "waveform");
        }
        if (snapshot->audioAnalysis.hasSpectrum)
        {
            lua_createtable(state,
                static_cast<int>(snapshot->audioAnalysis.spectrum.size()), 0);
            int spectrumIndex = 1;
            for (const double value : snapshot->audioAnalysis.spectrum)
            {
                lua_pushnumber(state, value);
                lua_rawseti(state, -2, spectrumIndex++);
            }
            lua_setfield(state, -2, "spectrum");
        }
        if (snapshot->audioAnalysis.hasRms)
        {
            lua_pushnumber(state, snapshot->audioAnalysis.rms);
            lua_setfield(state, -2, "rms");
        }
        if (snapshot->audioAnalysis.hasPeak)
        {
            lua_pushnumber(state, snapshot->audioAnalysis.peak);
            lua_setfield(state, -2, "peak");
        }
        lua_pushboolean(state, snapshot->audioAnalysis.silent);
        lua_setfield(state, -2, "silent");
        lua_pushboolean(state, snapshot->audioAnalysis.deviceChanged);
        lua_setfield(state, -2, "deviceChanged");
        lua_pushlstring(state, snapshot->audioAnalysis.endpointId.data(),
            snapshot->audioAnalysis.endpointId.size());
        lua_setfield(state, -2, "endpointId");
        lua_pushinteger(state, snapshot->audioAnalysis.sampleRate);
        lua_setfield(state, -2, "sampleRate");
        lua_pushinteger(state, snapshot->audioAnalysis.channels);
        lua_setfield(state, -2, "channels");
    }
    else if (snapshot->topic == "media.sessions")
    {
        lua_pushlstring(state,
            snapshot->mediaSessions.currentSessionId.data(),
            snapshot->mediaSessions.currentSessionId.size());
        lua_setfield(state, -2, "currentSessionId");
        lua_createtable(state,
            static_cast<int>(snapshot->mediaSessions.sessions.size()), 0);
        int sessionIndex = 1;
        for (const auto& session : snapshot->mediaSessions.sessions)
        {
            PushMediaSessionDataValue(state, session);
            lua_rawseti(state, -2, sessionIndex++);
        }
        lua_setfield(state, -2, "sessions");
    }
    else if (snapshot->topic == "media.current")
    {
        PushMediaSessionDataValue(state, snapshot->mediaCurrent.session);
        lua_setfield(state, -2, "session");
    }
    else if (snapshot->topic == "media.timeline")
    {
        PushMediaTimelineDataValue(state, snapshot->mediaTimeline);
        lua_setfield(state, -2, "timeline");
    }
    else if (snapshot->topic == "media.artwork")
    {
        const std::string token = RegisterDataImage(state,
            snapshot->mediaArtwork.resourceToken, snapshot->mediaArtwork.pixels, "media", false);
        lua_pushlstring(state, snapshot->mediaArtwork.sessionId.data(),
            snapshot->mediaArtwork.sessionId.size());
        lua_setfield(state, -2, "sessionId");
        PushResourceHandle(state, LuaResourceType::Image,
            token);
        lua_setfield(state, -2, "image");
        if (snapshot->mediaArtwork.pixels)
        {
            lua_pushinteger(state, snapshot->mediaArtwork.pixels->width);
            lua_setfield(state, -2, "width");
            lua_pushinteger(state, snapshot->mediaArtwork.pixels->height);
            lua_setfield(state, -2, "height");
        }
    }
    else if (snapshot->topic == "filesystem.watch")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->filesystemWatchEvents.size()), 0);
        int eventIndex = 1;
        for (const auto& event : snapshot->filesystemWatchEvents)
        {
            lua_createtable(state, 0, 5);
            lua_pushlstring(state, event.kind.data(), event.kind.size());
            lua_setfield(state, -2, "kind");
            lua_pushlstring(state, event.name.data(), event.name.size());
            lua_setfield(state, -2, "name");
            if (!event.oldName.empty())
            {
                lua_pushlstring(state,
                    event.oldName.data(), event.oldName.size());
                lua_setfield(state, -2, "oldName");
            }
            if (!event.handle.empty())
            {
                lua_pushlstring(state,
                    event.handle.data(), event.handle.size());
                lua_setfield(state, -2, "handle");
            }
            if (!event.itemKind.empty())
            {
                lua_pushlstring(state,
                    event.itemKind.data(), event.itemKind.size());
                lua_setfield(state, -2, "itemKind");
            }
            lua_rawseti(state, -2, eventIndex++);
        }
        lua_setfield(state, -2, "events");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->filesystemWatchRevision));
        lua_setfield(state, -2, "revision");
        lua_pushboolean(state, snapshot->filesystemWatchOverflow);
        lua_setfield(state, -2, "overflow");
    }
    else if (snapshot->topic == "desktop.items" ||
        snapshot->topic == "desktop.selection")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->desktopItems.size()), 0);
        int itemIndex = 1;
        for (const auto& item : snapshot->desktopItems)
        {
            PushDesktopDataItemValue(state, item);
            lua_rawseti(state, -2, itemIndex++);
        }
        lua_setfield(state, -2, "items");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->desktopRevision));
        lua_setfield(state, -2, "revision");
    }
    else if (snapshot->topic == "desktop.changes")
    {
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->desktopRevision));
        lua_setfield(state, -2, "revision");
        lua_pushlstring(state, snapshot->desktopChangeReason.data(),
            snapshot->desktopChangeReason.size());
        lua_setfield(state, -2, "reason");
    }
    else if (snapshot->topic == "calendar.events")
    {
        lua_createtable(state,
            static_cast<int>(snapshot->calendarEvents.size()), 0);
        int eventIndex = 1;
        for (const auto& event : snapshot->calendarEvents)
        {
            PushCalendarDataEventValue(state, event);
            lua_rawseti(state, -2, eventIndex++);
        }
        lua_setfield(state, -2, "events");
        lua_pushlstring(state, snapshot->calendarRangeStart.data(),
            snapshot->calendarRangeStart.size());
        lua_setfield(state, -2, "fromDate");
        lua_pushlstring(state, snapshot->calendarRangeEnd.data(),
            snapshot->calendarRangeEnd.size());
        lua_setfield(state, -2, "toDate");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->calendarRevision));
        lua_setfield(state, -2, "revision");
        lua_pushboolean(state, snapshot->calendarTruncated);
        lua_setfield(state, -2, "truncated");
        if (snapshot->calendarTotal)
        {
            lua_pushinteger(state, static_cast<lua_Integer>(*snapshot->calendarTotal));
            lua_setfield(state, -2, "total");
            lua_pushboolean(state, snapshot->calendarHasMore);
            lua_setfield(state, -2, "hasMore");
            lua_pushinteger(state, static_cast<lua_Integer>(snapshot->calendarNextOffset));
            lua_setfield(state, -2, "nextOffset");
        }
    }
    else if (snapshot->topic == "calendar.selectedDate")
    {
        lua_pushlstring(state, snapshot->calendarSelectedDate.data(),
            snapshot->calendarSelectedDate.size());
        lua_setfield(state, -2, "date");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->calendarRevision));
        lua_setfield(state, -2, "revision");
    }
    else if (snapshot->topic == "app.indexStatus")
    {
        lua_pushlstring(state, snapshot->appIndexState.data(),
            snapshot->appIndexState.size());
        lua_setfield(state, -2, "state");
        lua_pushinteger(state, static_cast<lua_Integer>(
            snapshot->appIndexRevision));
        lua_setfield(state, -2, "revision");
    }
    lua_setfield(state, -2, "value");
}

static int lua_DataSubscriptionValue(lua_State* state)
{
    auto* handle = CheckDataSubscriptionHandle(state, 1);
    if (!handle->engine || handle->id == 0)
    {
        PushDataSnapshotEnvelope(state, std::nullopt);
        return 1;
    }
    PushDataSnapshotEnvelope(
        state, handle->engine->RuntimeGetDataSnapshot(handle->id, handle->includeGpuDetails), "unsubscribed", handle->includeGpuDetails);
    return 1;
}

static int lua_DataSubscriptionHistory(lua_State* state)
{
    auto* handle = CheckDataSubscriptionHandle(state, 1);
    std::string error;
    std::string adapterId;
    if (!lua_isnoneornil(state, 2))
    {
        if (!lua_istable(state, 2)) error = "invalidHistoryOptions";
        else
        {
            lua_getfield(state, 2, "adapterId");
            if (!lua_isnil(state, -1))
            {
                if (lua_type(state, -1) != LUA_TSTRING) error = "invalidHistoryOptions";
                else
                {
                    std::size_t length = 0;
                    const char* id = lua_tolstring(state, -1, &length);
                    if (length == 0 || length > 1024) error = "invalidHistoryOptions";
                    else adapterId.assign(id, length);
                }
            }
            lua_pop(state, 1);
        }
    }
    std::vector<snowdesktop::widget_runtime::WidgetResourcePoint> points;
    if (error.empty())
    {
        if (handle->engine && handle->id != 0)
            points = handle->engine->RuntimeGetDataHistory(
                handle->id, error, adapterId, handle->includeGpuDetails);
        else error = "unsubscribed";
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    lua_createtable(state, static_cast<int>(points.size()), 0);
    lua_Integer index = 0;
    for (const auto& point : points)
    {
        if (point.timestampMs > now || point.timestampMs < now - 60000) continue;
        lua_createtable(state, 0, 4);
        lua_pushinteger(state, static_cast<lua_Integer>(point.timestampMs));
        lua_setfield(state, -2, "timestamp");
        lua_pushinteger(state, static_cast<lua_Integer>(now - point.timestampMs));
        lua_setfield(state, -2, "ageMs");
        if (point.primary)
        {
            lua_pushnumber(state, *point.primary);
            lua_setfield(state, -2, "value");
        }
        if (point.secondary)
        {
            lua_pushnumber(state, *point.secondary);
            lua_setfield(state, -2, "secondary");
        }
        lua_rawseti(state, -2, ++index);
    }
    if (error.empty()) lua_pushnil(state);
    else lua_pushlstring(state, error.data(), error.size());
    return 2;
}

static int lua_DataSubscriptionUnsubscribe(lua_State* state)
{
    auto* handle = CheckDataSubscriptionHandle(state, 1);
    bool removed = false;
    if (handle->engine && handle->id != 0)
        removed = handle->engine->RuntimeUnsubscribeData(handle->id);
    handle->id = 0;
    lua_pushboolean(state, removed);
    return 1;
}

static int lua_DataSubscriptionGc(lua_State* state)
{
    auto* handle = static_cast<LuaDataSubscriptionHandle*>(
        luaL_testudata(state, 1, kDataSubscriptionHandleMetatable));
    if (handle && handle->engine && handle->id != 0)
        (void)handle->engine->RuntimeUnsubscribeData(handle->id);
    if (handle) handle->id = 0;
    return 0;
}

void RegisterDataSubscriptionHandle(lua_State* state)
{
    if (luaL_newmetatable(state, kDataSubscriptionHandleMetatable))
    {
        lua_newtable(state);
        lua_pushcfunction(state, lua_DataSubscriptionValue);
        lua_setfield(state, -2, "value");
        lua_pushcfunction(state, lua_DataSubscriptionHistory);
        lua_setfield(state, -2, "history");
        lua_pushcfunction(state, lua_DataSubscriptionUnsubscribe);
        lua_setfield(state, -2, "unsubscribe");
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, lua_DataSubscriptionGc);
        lua_setfield(state, -2, "__gc");
        lua_pushliteral(state, "data subscription handle");
        lua_setfield(state, -2, "__metatable");
    }
    lua_pop(state, 1);
}

// Parser-owned values live outside the inner pcall, including error paths.
struct LuaDataSubscribeContext
{
    std::string topicValue, rangeStart, rangeEnd, eventId, scopeHandle;
    std::string calendarQueryError, feature;
    std::unordered_set<std::string> seen;
    std::optional<std::string> maximumEnd;
    std::optional<snowdesktop::widget_runtime::CalendarEventQueryOptions> calendarQuery;
    snowdesktop::widget_runtime::DataSubscriptionResult result;
    bool nativeFailure = false;
};

// Lua errors use longjmp. Keep nontrivial C++ temporaries out of the parsing
// frame: MSVC /EHsc can otherwise run their cleanup before construction when
// unwinding an extern-C Lua error. These helpers must not inline into it.
static __declspec(noinline) void LuaDataSubscribeMaximumEnd(LuaDataSubscribeContext& context)
{
    context.maximumEnd = snowdesktop::calendar::CalendarService::
        AddDays(context.rangeStart, 365);
}

static __declspec(noinline) void LuaDataSubscribeRegister(lua_State* state,
    WidgetEngine& engine, LuaDataSubscribeContext& context, lua_Integer maxAgeMs,
    snowdesktop::widget_runtime::DataHiddenPolicy hiddenPolicy,
    snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration audioAnalysis)
{
    // Resolve Lua state before constructing the native call's by-value
    // arguments. No Lua API operation follows the owning widget ID here.
    const auto widgetId = BoundWidgetId(state);
    context.result = engine.RuntimeSubscribeData(
        widgetId, context.topicValue, std::chrono::milliseconds(maxAgeMs), hiddenPolicy,
        std::move(context.rangeStart), std::move(context.rangeEnd),
        std::move(context.scopeHandle), audioAnalysis, std::move(context.eventId),
        std::move(context.calendarQuery));
}

static int LuaDataSubscribeBody(lua_State* state, LuaDataSubscribeContext& context)
{
    size_t topicLength = 0;
    const char* topic = luaL_checklstring(state, 1, &topicLength);
    if (topicLength == 0 || topicLength > 128)
        return luaL_error(state,
            "data.subscribe: topic must contain 1 to 128 bytes");
    if (lua_gettop(state) > 2)
        return luaL_error(state,
            "data.subscribe: expected topic and optional options");

    context.topicValue.assign(topic, topicLength);
    const auto& topicValue = context.topicValue;
    const bool audioAnalysisTopic =
        topicValue == "audio.output.analysis";
    lua_Integer maxAgeMs = audioAnalysisTopic ? 33 : 1000;
    bool maxAgeSpecified = false;
    bool includeGpuDetails = false;
    bool updateHzSpecified = false;
    bool waveformPointsSpecified = false;
    bool spectrumBinsSpecified = false;
    snowdesktop::widget_runtime::WidgetAudioAnalysisConfiguration
        audioAnalysis;
    auto& rangeStart = context.rangeStart;
    auto& rangeEnd = context.rangeEnd;
    auto& eventId = context.eventId;
    auto& scopeHandle = context.scopeHandle;
    auto& calendarQuery = context.calendarQuery;
    auto hiddenPolicy = topicValue == "filesystem.watch" ||
            audioAnalysisTopic
        ? snowdesktop::widget_runtime::DataHiddenPolicy::Pause
        : snowdesktop::widget_runtime::DataHiddenPolicy::Throttle;
    if (!lua_isnoneornil(state, 2))
    {
        luaL_checktype(state, 2, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, 2) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
                return luaL_error(state,
                    "data.subscribe: option keys must be strings");
            size_t keyLength = 0;
            const char* keyRaw = lua_tolstring(state, -2, &keyLength);
            const std::string_view key(keyRaw, keyLength);
            const bool common = key == "maxAgeMs" ||
                key == "whenHidden";
            const bool calendar = topicValue == "calendar.events" &&
                (key == "fromDate" || key == "toDate" || key == "eventId" ||
                    key == "query" || key == "offset" || key == "limit");
            const bool filesystem = topicValue == "filesystem.watch" &&
                key == "handle";
            const bool audio = audioAnalysisTopic &&
                (key == "features" || key == "updateHz" ||
                    key == "waveformPoints" || key == "spectrumBins");
            const bool gpu = topicValue == "system.gpu" && key == "includeDetails";
            if (!common && !calendar && !filesystem && !audio && !gpu)
            {
                return luaL_error(state,
                    "data.subscribe: option '%.*s' is not valid for topic '%s'",
                    static_cast<int>(keyLength), keyRaw,
                    topicValue.c_str());
            }
            lua_pop(state, 1);
        }
        lua_getfield(state, 2, "includeDetails");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isboolean(state, -1))
                return luaL_error(state, "data.subscribe: includeDetails must be a boolean");
            includeGpuDetails = lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);
        lua_getfield(state, 2, "maxAgeMs");
        if (!lua_isnil(state, -1))
        {
            maxAgeSpecified = true;
            int isInteger = 0;
            maxAgeMs = lua_tointegerx(state, -1, &isInteger);
            if (!isInteger || maxAgeMs <= 0 || maxAgeMs > 86400000)
                return luaL_error(state,
                    "data.subscribe: maxAgeMs must be an integer from 1 to 86400000");
        }
        lua_pop(state, 1);

        lua_getfield(state, 2, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            size_t policyLength = 0;
            const char* policy = luaL_checklstring(
                state, -1, &policyLength);
            const std::string_view value(policy, policyLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::DataHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "data.subscribe: whenHidden must be 'pause', 'throttle', or 'continue'");
            if (audioAnalysisTopic && value != "pause")
                return luaL_error(state,
                    "data.subscribe: audio.output.analysis requires whenHidden='pause'");
        }
        lua_pop(state, 1);

        const auto readDateOption = [&](const char* name,
                                        std::string& output) {
            lua_getfield(state, 2, name);
            if (!lua_isnil(state, -1))
            {
                size_t length = 0;
                const char* value = luaL_checklstring(
                    state, -1, &length);
                output.assign(value, length);
            }
            lua_pop(state, 1);
        };
        readDateOption("fromDate", rangeStart);
        readDateOption("toDate", rangeEnd);
        lua_getfield(state, 2, "eventId");
        if (!lua_isnil(state, -1))
        {
            if (lua_type(state, -1) != LUA_TSTRING)
                return luaL_error(state, "data.subscribe: eventId must be a string");
            size_t length = 0;
            const char* raw = lua_tolstring(state, -1, &length);
            eventId.assign(raw, length);
            if (length == 0 || length > 128 || eventId.find('\0') != std::string::npos)
                return luaL_error(state, "data.subscribe: eventId must contain 1 to 128 bytes without NUL");
            lua_getfield(state, 2, "fromDate");
            const bool hasStart = !lua_isnil(state, -1);
            lua_pop(state, 1);
            lua_getfield(state, 2, "toDate");
            const bool hasEnd = !lua_isnil(state, -1);
            lua_pop(state, 1);
            if (hasStart || hasEnd)
                return luaL_error(state, "data.subscribe: eventId and date range are mutually exclusive");
        }
        lua_pop(state, 1);

        lua_getfield(state, 2, "handle");
        if (!lua_isnil(state, -1))
        {
            size_t handleLength = 0;
            const char* handle = luaL_checklstring(
                state, -1, &handleLength);
            if (handleLength == 0 || handleLength > 128)
                return luaL_error(state,
                    "data.subscribe: handle must contain 1 to 128 bytes");
            scopeHandle.assign(handle, handleLength);
        }
        lua_pop(state, 1);

        if (audioAnalysisTopic)
        {
            lua_getfield(state, 2, "features");
            if (!lua_isnil(state, -1))
            {
                luaL_checktype(state, -1, LUA_TTABLE);
                const int featuresTable = lua_absindex(state, -1);
                if (lua_getmetatable(state, featuresTable) != 0)
                {
                    lua_pop(state, 1);
                    return luaL_error(state,
                        "data.subscribe: audio features must be a plain array");
                }
                const size_t featureCount = lua_rawlen(
                    state, featuresTable);
                if (featureCount == 0 || featureCount > 4)
                    return luaL_error(state,
                        "data.subscribe: audio features must contain 1 to 4 entries");
                size_t actualCount = 0;
                lua_pushnil(state);
                while (lua_next(state, featuresTable) != 0)
                {
                    if (!lua_isinteger(state, -2))
                        return luaL_error(state,
                            "data.subscribe: audio features must be a dense array");
                    const lua_Integer key = lua_tointeger(state, -2);
                    if (key < 1 || static_cast<size_t>(key) > featureCount)
                        return luaL_error(state,
                            "data.subscribe: audio features must be a dense array");
                    ++actualCount;
                    lua_pop(state, 1);
                }
                if (actualCount != featureCount)
                    return luaL_error(state,
                        "data.subscribe: audio features must be a dense array");

                audioAnalysis.waveform = false;
                audioAnalysis.spectrum = false;
                audioAnalysis.rms = false;
                audioAnalysis.peak = false;
                auto& seen = context.seen;
                for (size_t index = 1; index <= featureCount; ++index)
                {
                    lua_rawgeti(state, featuresTable,
                        static_cast<lua_Integer>(index));
                    size_t featureLength = 0;
                    const char* featureRaw = luaL_checklstring(
                        state, -1, &featureLength);
                    context.feature.assign(featureRaw, featureLength);
                    const auto& feature = context.feature;
                    lua_pop(state, 1);
                    if (!seen.insert(feature).second)
                        return luaL_error(state,
                            "data.subscribe: audio features must be unique");
                    if (feature == "waveform")
                        audioAnalysis.waveform = true;
                    else if (feature == "spectrum")
                        audioAnalysis.spectrum = true;
                    else if (feature == "rms")
                        audioAnalysis.rms = true;
                    else if (feature == "peak")
                        audioAnalysis.peak = true;
                    else
                        return luaL_error(state,
                            "data.subscribe: unsupported audio feature '%s'",
                            feature.c_str());
                }
            }
            lua_pop(state, 1);

            const auto readAudioInteger = [&](const char* field,
                lua_Integer minimum, lua_Integer maximum,
                std::size_t& output, bool& specified) {
                lua_getfield(state, 2, field);
                if (!lua_isnil(state, -1))
                {
                    specified = true;
                    if (!lua_isinteger(state, -1))
                        luaL_error(state,
                            "data.subscribe: %s must be an integer",
                            field);
                    const lua_Integer value = lua_tointeger(state, -1);
                    if (value < minimum || value > maximum)
                        luaL_error(state,
                            "data.subscribe: %s must be between %lld and %lld",
                            field, static_cast<long long>(minimum),
                            static_cast<long long>(maximum));
                    output = static_cast<std::size_t>(value);
                }
                lua_pop(state, 1);
            };
            readAudioInteger("waveformPoints", 16, 256,
                audioAnalysis.waveformPoints, waveformPointsSpecified);
            readAudioInteger("spectrumBins", 16, 128,
                audioAnalysis.spectrumBins, spectrumBinsSpecified);

            lua_getfield(state, 2, "updateHz");
            if (!lua_isnil(state, -1))
            {
                updateHzSpecified = true;
                if (!lua_isinteger(state, -1))
                    return luaL_error(state,
                        "data.subscribe: updateHz must be an integer");
                const lua_Integer updateHz = lua_tointeger(state, -1);
                if (updateHz < 1 || updateHz > 60)
                    return luaL_error(state,
                        "data.subscribe: updateHz must be between 1 and 60");
                maxAgeMs = (1000 + updateHz - 1) / updateHz;
            }
            lua_pop(state, 1);

            if (maxAgeSpecified && updateHzSpecified)
                return luaL_error(state,
                    "data.subscribe: maxAgeMs and updateHz are mutually exclusive");
            if (waveformPointsSpecified && !audioAnalysis.waveform)
                return luaL_error(state,
                    "data.subscribe: waveformPoints requires the waveform feature");
            if (spectrumBinsSpecified && !audioAnalysis.spectrum)
                return luaL_error(state,
                    "data.subscribe: spectrumBins requires the spectrum feature");
        }
    }

    if (topicValue == "filesystem.watch")
    {
        if (scopeHandle.empty())
            return luaL_error(state,
                "data.subscribe: filesystem.watch requires a folder handle");
    }
    else if (!scopeHandle.empty())
    {
        return luaL_error(state,
            "data.subscribe: handle is only valid for filesystem.watch");
    }

    if (!rangeStart.empty() || !rangeEnd.empty())
    {
        if (topicValue != "calendar.events" || rangeStart.empty() ||
            rangeEnd.empty() || rangeEnd < rangeStart ||
            !snowdesktop::calendar::CalendarService::
                GetDateInfo(rangeStart) ||
            !snowdesktop::calendar::CalendarService::
                GetDateInfo(rangeEnd))
        {
            return luaL_error(state,
                "data.subscribe: fromDate/toDate require a valid calendar.events range");
        }
        LuaDataSubscribeMaximumEnd(context);
        if (context.maximumEnd && rangeEnd > *context.maximumEnd)
        {
            return luaL_error(state,
                "data.subscribe: calendar.events range cannot exceed 366 days");
        }
    }

    auto& calendarQueryError = context.calendarQueryError;
    if (!snowdesktop::widget_api::ParseCalendarEventQueryOptions(
            state, 2, calendarQuery, calendarQueryError))
        return luaL_error(state, "data.subscribe: %s", calendarQueryError.c_str());

    auto* host = BoundWidgetEngine(state);
    if (!host)
        return luaL_error(state, "data.subscribe: host is unavailable");
    // Allocate the complete Lua handle before creating the native subscription.
    auto* handle = static_cast<LuaDataSubscriptionHandle*>(
        lua_newuserdata(state, sizeof(LuaDataSubscriptionHandle)));
    *handle = {};
    luaL_getmetatable(state, kDataSubscriptionHandleMetatable);
    lua_setmetatable(state, -2);
    auto& result = context.result;
    LuaDataSubscribeRegister(state, *host, context,
        maxAgeMs, hiddenPolicy, audioAnalysis);
    if (!result)
        return luaL_error(state, "data.subscribe: %s", result.error.c_str());

    // No allocating Lua operation follows native registration.
    *handle = { host, result.id, includeGpuDetails };
    return 1;
}

static int LuaDataSubscribeBridge(lua_State* state)
{
    auto& context = *static_cast<LuaDataSubscribeContext*>(lua_touserdata(state, 1));
    lua_remove(state, 1);
    try { return LuaDataSubscribeBody(state, context); }
    catch (...) { context.nativeFailure = true; return 0; }
}

int lua_DataSubscribe(lua_State* state)
{
    const int arguments = lua_gettop(state);
    if (arguments < 1 || arguments > 2)
        return luaL_error(state, "data.subscribe: expected topic and optional options");
    if (!lua_checkstack(state, arguments + 2))
        return luaL_error(state, "data.subscribe: not enough Lua stack space");
    int status = LUA_OK;
    bool nativeFailure = false;
    {
        LuaDataSubscribeContext context;
        lua_pushcfunction(state, LuaDataSubscribeBridge);
        lua_pushlightuserdata(state, &context);
        for (int index = 1; index <= arguments; ++index) lua_pushvalue(state, index);
        status = lua_pcall(state, arguments + 1, 1, 0);
        nativeFailure = context.nativeFailure;
    }
    if (nativeFailure)
        return luaL_error(state, "data.subscribe: native host failure");
    if (status != LUA_OK) return lua_error(state);
    return 1;
}
}
