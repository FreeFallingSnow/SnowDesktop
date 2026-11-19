#pragma once

#include "widget_filesystem_task_executor.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace snowdesktop::widget_runtime
{
inline constexpr char kFilesystemReadPermission[] =
    "filesystem.userSelected.read";
inline constexpr char kFilesystemWritePermission[] =
    "filesystem.userSelected.write";
bool IsPreviewFilesystemHandle(std::string_view handle);

struct TaskCompletion;
class WidgetTaskBroker;

struct FilesystemCompletionOwner
{
    std::string packageId;
    bool canRead = false;
};

class FilesystemTaskPayload
{
public:
    using ImageRegistrar = std::function<std::string(const std::string&,
        const std::shared_ptr<const WidgetRuntimeImagePixels>&)>;

    bool HasValue() const noexcept;
    void PrepareValue(TaskCompletion& completion, const ImageRegistrar& registerImage);
    void PushValue(lua_State* state, std::string_view taskName) const;

private:
    friend class WidgetFilesystemTaskDispatch;

    std::vector<WidgetFilesystemHandleEntry> selections_;
    std::vector<std::string> selectionNames_;
    std::optional<WidgetFilesystemTaskCompletion> result_;
    std::string imageToken_;
};

// Runs on the host thread. Retains results until broker delivery and tracks
// submitted handles until the executor reports completion, including cancellation.
class WidgetFilesystemTaskDispatch
{
public:
    using ResolveOwner = std::function<std::optional<FilesystemCompletionOwner>(std::uint64_t)>;

    void Consume(std::vector<WidgetFilesystemTaskCompletion> messages,
        WidgetTaskBroker& broker, WidgetFilesystemHandleStore* store,
        const ResolveOwner& resolveOwner);
    void StorePreviewResult(std::uint64_t id, const WidgetFilesystemTaskRequest& request);
    void StorePreviewSelection(std::uint64_t id, WidgetFilesystemHandleOwner owner,
        WidgetFilesystemHandleKind kind, WidgetFilesystemHandleAccess access);
    std::string GrantSelection(std::uint64_t id, WidgetFilesystemHandleStore& store,
        const WidgetFilesystemHandleOwner& owner,
        const std::vector<std::filesystem::path>& paths,
        WidgetFilesystemHandleKind kind, WidgetFilesystemHandleAccess access);
    FilesystemTaskPayload TakeCompletion(TaskCompletion& completion);
    void DiscardCompletion(std::uint64_t id);
    void TrackHandle(std::uint64_t id, const std::string& handle);
    bool HasActiveHandle(std::string_view handle) const;
    std::vector<std::uint64_t> ActiveTasksForHandle(std::string_view handle) const;
    void Clear();

private:
    std::unordered_map<std::uint64_t, std::vector<WidgetFilesystemHandleEntry>> selections_;
    std::unordered_map<std::uint64_t, WidgetFilesystemTaskCompletion> results_;
    std::unordered_map<std::uint64_t, std::string> activeHandles_;
};
}
