#include "widget_native_task_dispatch.h"
#include "widget_native_task_executor.h"
#include "widget_task_broker.h"

#include <utility>

namespace snowdesktop::widget_runtime
{
const JsonValue* NativeTaskEvent::ReadValue(JsonValue& frame) const
{
    return ParseJson(json, frame) ? frame.Find("value") : nullptr;
}

const JsonValue* NativeTaskCompletionPayload::Value() const
{
    return kind == Kind::Value ? frame.Find("value") : nullptr;
}

const JsonValue* NativeTaskCompletionPayload::Diagnostic() const
{
    return kind == Kind::Diagnostic ? &frame : nullptr;
}

void WidgetNativeTaskDispatch::Consume(
    std::vector<NativeTaskMessage> messages, WidgetTaskBroker& broker,
    const EventSink& onEvent)
{
    for (auto& message : messages)
    {
        const auto snapshot = broker.Snapshot(message.id);
        if (!snapshot) continue;

        if (message.final)
        {
            if (!snapshot->cancelRequested)
            {
                if (message.ok)
                    completions_.insert_or_assign(message.id, std::move(message.json));
                else if (!message.diagnostic.empty())
                    completions_.insert_or_assign(message.id, std::move(message.diagnostic));
            }
            (void)broker.Complete(message.id, message.ok, std::move(message.error));
            continue;
        }

        if (!snapshot->cancelRequested)
            onEvent({message.id, snapshot->ownerToken, message.json});
    }
}

NativeTaskCompletionPayload WidgetNativeTaskDispatch::TakeCompletion(
    TaskCompletion& completion, const std::function<bool()>& canDeliverValue)
{
    using Kind = NativeTaskCompletionPayload::Kind;
    const bool native = completion.name == "native.request" || completion.name == "native.watch";
    const auto stored = completions_.find(completion.id);
    JsonValue frame;
    const bool parsed = native && stored != completions_.end() &&
        ParseJson(stored->second, frame);
    if (stored != completions_.end())
        completions_.erase(stored);

    if (!native) return {};
    if (completion.ok)
    {
        if (parsed && frame.Find("value") && canDeliverValue())
            return {Kind::Value, std::move(frame)};
        completion.ok = false;
        completion.error = "taskResultUnavailable";
    }
    else if (parsed && frame.Find("reason"))
        return {Kind::Diagnostic, std::move(frame)};
    return {};
}

void WidgetNativeTaskDispatch::DiscardCompletion(std::uint64_t id)
{
    completions_.erase(id);
}

void WidgetNativeTaskDispatch::Clear()
{
    completions_.clear();
}
}
