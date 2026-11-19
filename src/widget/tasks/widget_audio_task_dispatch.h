#pragma once

#include "widget_audio_playback.h"

#include <functional>
#include <vector>

struct lua_State;

namespace snowdesktop::widget_runtime
{
class WidgetTaskBroker;

using AudioArtworkRegistrar = std::function<std::string(const std::string&,
    const std::shared_ptr<const WidgetRuntimeImagePixels>&)>;

// Consume task results now and return playback events for dispatch after task completions.
std::vector<AudioPlaybackMessage> ConsumeAudioPlaybackMessages(
    WidgetAudioPlayback* playback, WidgetTaskBroker& broker);
void CancelAudioPlaybackTask(WidgetAudioPlayback* playback,
    std::string_view taskName, std::uint64_t taskId);
void PushAudioPlaybackTaskValue(lua_State* state, std::uint64_t taskId);
void PushAudioPlaybackEvent(lua_State* state, const AudioPlaybackMessage& message,
    const AudioArtworkRegistrar& registerArtwork);
}
