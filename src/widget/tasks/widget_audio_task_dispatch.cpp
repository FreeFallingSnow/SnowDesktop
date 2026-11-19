#include "widget_audio_task_dispatch.h"
#include "widget_task_broker.h"

#include "widget/data/widget_resource_lua.h"

#include <utility>

namespace snowdesktop::widget_runtime
{
std::vector<AudioPlaybackMessage> ConsumeAudioPlaybackMessages(
    WidgetAudioPlayback* playback, WidgetTaskBroker& broker)
{
    if (!playback) return {};

    std::vector<AudioPlaybackMessage> events;
    for (auto& message : playback->Drain())
    {
        if (!message.taskId)
        {
            events.push_back(std::move(message));
            continue;
        }

        const auto snapshot = broker.Snapshot(message.taskId);
        if (snapshot && (snapshot->ownerToken != message.owner ||
                !WidgetAudioPlayback::SupportsAction(snapshot->name)))
            continue;
        if (!snapshot || snapshot->cancelRequested)
        {
            playback->Cancel(message.taskId);
            continue;
        }

        (void)broker.Complete(message.taskId, message.ok, message.state.error);
    }
    return events;
}

void CancelAudioPlaybackTask(WidgetAudioPlayback* playback,
    std::string_view taskName, std::uint64_t taskId)
{
    if (playback && WidgetAudioPlayback::SupportsAction(taskName))
        playback->Cancel(taskId);
}

void PushAudioPlaybackTaskValue(lua_State* state, std::uint64_t taskId)
{
    lua_createtable(state, 0, 1);
    const std::string playbackId = "audio-" + std::to_string(taskId);
    lua_pushstring(state, playbackId.c_str());
    lua_setfield(state, -2, "playbackId");
}

void PushAudioPlaybackEvent(lua_State* state, const AudioPlaybackMessage& message,
    const AudioArtworkRegistrar& registerArtwork)
{
    lua_pushstring(state, message.playbackId.c_str());
    lua_setfield(state, -2, "playbackId");
    lua_pushstring(state, message.state.status.c_str());
    lua_setfield(state, -2, "status");
    lua_pushstring(state, message.state.error.c_str());
    lua_setfield(state, -2, "error");
    lua_pushinteger(state, message.state.positionMs);
    lua_setfield(state, -2, "positionMs");
    lua_pushinteger(state, message.state.durationMs);
    lua_setfield(state, -2, "durationMs");
    lua_pushnumber(state, message.state.volume);
    lua_setfield(state, -2, "volume");
    lua_pushboolean(state, message.state.loop);
    lua_setfield(state, -2, "loop");
    lua_pushboolean(state, message.state.canSeek);
    lua_setfield(state, -2, "canSeek");
    lua_pushnumber(state, message.state.playbackRate);
    lua_setfield(state, -2, "playbackRate");
    lua_pushboolean(state, message.state.shuffle);
    lua_setfield(state, -2, "shuffle");
    lua_pushstring(state, message.state.repeatMode.c_str());
    lua_setfield(state, -2, "repeatMode");
    lua_pushboolean(state, message.state.systemMediaControls);
    lua_setfield(state, -2, "systemMediaControls");
    lua_pushstring(state, message.state.title.c_str());
    lua_setfield(state, -2, "title");
    lua_pushstring(state, message.state.artist.c_str());
    lua_setfield(state, -2, "artist");
    lua_pushstring(state, message.state.album.c_str());
    lua_setfield(state, -2, "album");
    if (message.state.artwork)
    {
        const auto token = registerArtwork(message.state.artworkToken, message.state.artwork);
        if (!token.empty())
        {
            PushResourceHandle(state, LuaResourceType::Image, token);
            lua_setfield(state, -2, "artwork");
        }
    }

    const auto& command = message.command;
    if (command.command.empty()) return;
    lua_pushstring(state, command.command.c_str());
    lua_setfield(state, -2, "command");
    if (command.command == "setPlaybackRate")
    {
        lua_pushnumber(state, command.value);
        lua_setfield(state, -2, "playbackRate");
    }
    else if (command.command == "setShuffle")
    {
        lua_pushboolean(state, command.value != 0);
        lua_setfield(state, -2, "shuffle");
    }
    else if (command.command == "setRepeatMode")
    {
        lua_pushstring(state, command.repeatMode.c_str());
        lua_setfield(state, -2, "repeatMode");
    }
}
}
