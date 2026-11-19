#pragma once

#include "widget_clipboard_task_executor.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace snowdesktop::widget_runtime
{
struct TaskBrokerAction;
struct TaskCompletion;
class WidgetTaskBroker;

struct ClipboardWriteSources
{
    std::function<void(const std::string&, WidgetClipboardTaskRequest&)> resolveImage;
    std::function<const char*(const std::string&, std::filesystem::path&)> resolveFile;
};

const char* PrepareClipboardTaskRequest(const TaskBrokerAction& action,
    const ClipboardWriteSources& sources, WidgetClipboardTaskRequest& request);

struct ClipboardFileItem
{
    std::string reference;
    std::string target;
    std::string name;
    bool folder = false;
};

class ClipboardTaskPayload
{
public:
    using ImageRegistrar = std::function<std::string(const std::string&,
        const std::shared_ptr<const WidgetRuntimeImagePixels>&)>;
    // An empty span with a nonzero count clears prior clipboard references on failure.
    using FilePublisher = std::function<void(std::size_t, std::span<const ClipboardFileItem>)>;

    bool HasValue() const noexcept;
    void PrepareValue(TaskCompletion& completion, const ImageRegistrar& registerImage,
        const FilePublisher& publishFiles);
    void PushValue(lua_State* state) const;

private:
    friend class WidgetClipboardTaskDispatch;

    std::optional<WidgetClipboardTaskCompletion> result_;
    std::vector<ClipboardFileItem> files_;
    std::string imageToken_;
};

// Host-thread completion dispatch keeps the payload alive until Lua delivery.
class WidgetClipboardTaskDispatch
{
public:
    void Consume(std::vector<WidgetClipboardTaskCompletion> messages, WidgetTaskBroker& broker);
    void StorePreviewRead(std::uint64_t id, const std::string& format);
    ClipboardTaskPayload TakeCompletion(TaskCompletion& completion);
    void DiscardCompletion(std::uint64_t id);
    void Clear();

private:
    std::unordered_map<std::uint64_t, WidgetClipboardTaskCompletion> results_;
};
}
