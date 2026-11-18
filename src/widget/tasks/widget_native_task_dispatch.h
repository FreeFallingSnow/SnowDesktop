#pragma once

#include "common/json_value.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
class WidgetTaskBroker;
struct NativeTaskMessage;
struct TaskCompletion;

struct NativeTaskEvent
{
    std::uint64_t taskId;
    std::uint64_t ownerToken;
    std::string_view json;

    const JsonValue* ReadValue(JsonValue& frame) const;
};

struct NativeTaskCompletionPayload
{
    enum class Kind { None, Value, Diagnostic };
    Kind kind = Kind::None;
    JsonValue frame;

    const JsonValue* Value() const;
    const JsonValue* Diagnostic() const;
};

// Host-thread dispatch and the result payloads that accompany broker completions.
class WidgetNativeTaskDispatch
{
public:
    using EventSink = std::function<void(const NativeTaskEvent&)>;

    // Deliver each event synchronously so cancellation in a callback takes
    // effect before the next message is checked against the broker.
    void Consume(std::vector<NativeTaskMessage> messages, WidgetTaskBroker& broker,
        const EventSink& onEvent);
    NativeTaskCompletionPayload TakeCompletion(TaskCompletion& completion,
        const std::function<bool()>& canDeliverValue);
    void DiscardCompletion(std::uint64_t id);
    void Clear();

private:
    std::unordered_map<std::uint64_t, std::string> completions_;
};
}
