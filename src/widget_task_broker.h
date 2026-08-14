#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class TaskBrokerActionType
{
    Start,
    Cancel,
};

enum class TaskBrokerCancelReason
{
    Requested,
    PermissionRevoked,
    InstanceDisposed,
    Shutdown,
};

struct TaskDescriptor
{
    std::string name;
    std::string requiredPermission;
    bool requiresTrustedGesture = false;
    std::size_t maximumPerInstance = 4;
};

struct TaskStartOptions
{
    std::uint64_t ownerToken = 0;
    bool permissionGranted = false;
    bool trustedGesture = false;
    bool preview = false;
};

struct TaskStartResult
{
    std::uint64_t id = 0;
    std::string error;

    explicit operator bool() const noexcept
    {
        return id != 0 && error.empty();
    }
};

struct TaskBrokerAction
{
    TaskBrokerActionType type = TaskBrokerActionType::Start;
    TaskBrokerCancelReason cancelReason =
        TaskBrokerCancelReason::Requested;
    std::uint64_t id = 0;
    std::uint64_t ownerToken = 0;
    std::string instanceId;
    std::string name;
    bool preview = false;
};

struct TaskCompletion
{
    std::uint64_t id = 0;
    std::uint64_t ownerToken = 0;
    std::string instanceId;
    std::string name;
    bool ok = false;
    std::string error;
};

struct TaskSnapshot
{
    std::uint64_t id = 0;
    std::uint64_t ownerToken = 0;
    std::string instanceId;
    std::string name;
    bool preview = false;
    bool cancelRequested = false;
};

class WidgetTaskBroker
{
public:
    static constexpr std::size_t MaximumTasks = 512;
    static constexpr std::size_t MaximumTasksPerInstance = 16;

    bool RegisterTask(TaskDescriptor descriptor, std::string& error);
    TaskStartResult Start(std::string instanceId, std::string_view name,
        TaskStartOptions options);
    bool Cancel(std::uint64_t id,
        TaskBrokerCancelReason reason = TaskBrokerCancelReason::Requested);
    bool Complete(std::uint64_t id, bool ok, std::string error = {});
    std::size_t SetPermission(std::string_view instanceId,
        std::string_view permission, bool granted);
    std::size_t CancelInstance(std::string_view instanceId);
    void Shutdown();

    std::optional<TaskSnapshot> Snapshot(std::uint64_t id) const;
    std::vector<TaskBrokerAction> DrainActions();
    std::vector<TaskCompletion> DrainCompletions();
    std::size_t ActiveCount() const noexcept;

private:
    struct Task
    {
        std::uint64_t id = 0;
        std::uint64_t ownerToken = 0;
        std::string instanceId;
        std::string name;
        bool preview = false;
        bool cancelRequested = false;
        TaskBrokerCancelReason cancelReason =
            TaskBrokerCancelReason::Requested;
    };

    std::unordered_map<std::string, TaskDescriptor> descriptors_;
    std::unordered_map<std::uint64_t, Task> tasks_;
    std::vector<TaskBrokerAction> actions_;
    std::vector<TaskCompletion> completions_;
    std::uint64_t nextId_ = 0;
    bool shuttingDown_ = false;
};
}
