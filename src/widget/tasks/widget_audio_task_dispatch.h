#pragma once

#include "widget_audio_playback.h"

#include <vector>

namespace snowdesktop::widget_runtime
{
class WidgetTaskBroker;

// Consume task results now and return playback events for dispatch after task completions.
std::vector<AudioPlaybackMessage> ConsumeAudioPlaybackMessages(
    WidgetAudioPlayback* playback, WidgetTaskBroker& broker);
}
