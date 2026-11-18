#include "widget_audio_task_dispatch.h"
#include "widget_task_broker.h"

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
        if (!snapshot || snapshot->cancelRequested)
        {
            playback->Cancel(message.taskId);
            continue;
        }

        (void)broker.Complete(message.taskId, message.ok, message.state.error);
    }
    return events;
}
}
