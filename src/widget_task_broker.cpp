#include "widget_task_broker.h"

#include <algorithm>
#include <utility>

namespace snowdesktop::widget_runtime
{
bool WidgetTaskBroker::RegisterTask(
    TaskDescriptor descriptor, std::string& error)
{
    error.clear();
    if (descriptor.name.empty())
    {
        error = "task name is required";
        return false;
    }
    if (descriptor.maximumPerInstance == 0 ||
        descriptor.maximumPerInstance > MaximumTasksPerInstance)
    {
        error = "task per-instance limit is invalid";
        return false;
    }
    if (descriptors_.contains(descriptor.name))
    {
        error = "task is already registered";
        return false;
    }
    const std::string name = descriptor.name;
    descriptors_.emplace(name, std::move(descriptor));
    return true;
}

TaskStartResult WidgetTaskBroker::Start(
    std::string instanceId, std::string_view name,
    TaskStartOptions options)
{
    if (instanceId.empty()) return { 0, "instance ID is required" };
    const auto descriptor = descriptors_.find(std::string(name));
    if (descriptor == descriptors_.end())
        return { 0, "task is not registered" };
    if (!descriptor->second.requiredPermission.empty() &&
        !options.permissionGranted)
        return { 0, "permissionDenied" };
    if (descriptor->second.requiresTrustedGesture &&
        !options.trustedGesture)
        return { 0, "userGestureRequired" };
    if (tasks_.size() >= MaximumTasks)
        return { 0, "global task limit exceeded" };

    std::size_t instanceCount = 0;
    std::size_t descriptorCount = 0;
    for (const auto& [_, task] : tasks_)
    {
        if (task.instanceId != instanceId) continue;
        ++instanceCount;
        if (task.name == name) ++descriptorCount;
    }
    if (instanceCount >= MaximumTasksPerInstance)
        return { 0, "per-instance task limit exceeded" };
    if (descriptorCount >= descriptor->second.maximumPerInstance)
        return { 0, "task concurrency limit exceeded" };

    std::uint64_t id = ++nextId_;
    if (id == 0) id = ++nextId_;
    Task task{ id, std::move(instanceId), std::string(name),
        options.preview };
    actions_.push_back({ TaskBrokerActionType::Start,
        TaskBrokerCancelReason::Requested, id, task.instanceId,
        task.name, task.preview });
    tasks_.emplace(id, std::move(task));
    return { id, {} };
}

bool WidgetTaskBroker::Cancel(
    std::uint64_t id, TaskBrokerCancelReason reason)
{
    const auto found = tasks_.find(id);
    if (found == tasks_.end() || found->second.cancelRequested)
        return false;
    found->second.cancelRequested = true;
    found->second.cancelReason = reason;
    actions_.push_back({ TaskBrokerActionType::Cancel, reason, id,
        found->second.instanceId, found->second.name,
        found->second.preview });
    return true;
}

bool WidgetTaskBroker::Complete(
    std::uint64_t id, bool ok, std::string error)
{
    const auto found = tasks_.find(id);
    if (found == tasks_.end()) return false;
    if (found->second.cancelRequested)
    {
        ok = false;
        switch (found->second.cancelReason)
        {
        case TaskBrokerCancelReason::PermissionRevoked:
            error = "permissionRevoked";
            break;
        case TaskBrokerCancelReason::InstanceDisposed:
            error = "instanceDisposed";
            break;
        case TaskBrokerCancelReason::Shutdown:
            error = "shutdown";
            break;
        default:
            error = "canceled";
            break;
        }
    }
    else if (ok)
    {
        error.clear();
    }
    else if (error.empty())
    {
        error = "taskFailed";
    }
    completions_.push_back({ id, found->second.instanceId,
        found->second.name, ok, std::move(error) });
    tasks_.erase(found);
    return true;
}

std::size_t WidgetTaskBroker::SetPermission(
    std::string_view instanceId, std::string_view permission, bool granted)
{
    if (granted) return 0;
    std::vector<std::uint64_t> affected;
    for (const auto& [id, task] : tasks_)
    {
        const auto descriptor = descriptors_.find(task.name);
        if (task.instanceId == instanceId &&
            descriptor != descriptors_.end() &&
            descriptor->second.requiredPermission == permission &&
            !task.cancelRequested)
            affected.push_back(id);
    }
    for (const std::uint64_t id : affected)
        (void)Cancel(id, TaskBrokerCancelReason::PermissionRevoked);
    return affected.size();
}

std::size_t WidgetTaskBroker::CancelInstance(std::string_view instanceId)
{
    std::vector<std::uint64_t> affected;
    for (const auto& [id, task] : tasks_)
    {
        if (task.instanceId == instanceId && !task.cancelRequested)
            affected.push_back(id);
    }
    for (const std::uint64_t id : affected)
        (void)Cancel(id, TaskBrokerCancelReason::InstanceDisposed);
    return affected.size();
}

void WidgetTaskBroker::Shutdown()
{
    std::vector<std::uint64_t> affected;
    affected.reserve(tasks_.size());
    for (const auto& [id, task] : tasks_)
    {
        if (!task.cancelRequested) affected.push_back(id);
    }
    for (const std::uint64_t id : affected)
        (void)Cancel(id, TaskBrokerCancelReason::Shutdown);
}

std::optional<TaskSnapshot>
WidgetTaskBroker::Snapshot(std::uint64_t id) const
{
    const auto found = tasks_.find(id);
    if (found == tasks_.end()) return std::nullopt;
    return TaskSnapshot{ found->second.id, found->second.instanceId,
        found->second.name, found->second.preview,
        found->second.cancelRequested };
}

std::vector<TaskBrokerAction> WidgetTaskBroker::DrainActions()
{
    return std::exchange(actions_, {});
}

std::vector<TaskCompletion> WidgetTaskBroker::DrainCompletions()
{
    return std::exchange(completions_, {});
}

std::size_t WidgetTaskBroker::ActiveCount() const noexcept
{
    return tasks_.size();
}
}
