#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace snowdesktop::popup_icon_load_rules
{
constexpr std::uint64_t NextGeneration(std::uint64_t current)
{
    ++current;
    return current == 0 ? 1 : current;
}

constexpr bool ShouldRejectResult(
    bool popupRequest,
    std::uint64_t resultGeneration,
    std::uint64_t currentGeneration)
{
    return popupRequest && resultGeneration != currentGeneration;
}

template <typename TaskQueue, typename PendingKeySet,
    typename IsPopupTask>
std::size_t CancelQueuedTasks(
    TaskQueue& queue,
    PendingKeySet& pendingKeys,
    IsPopupTask isPopupTask)
{
    std::size_t removed = 0;
    std::erase_if(queue, [&](const auto& task) {
        if (!isPopupTask(task))
            return false;
        pendingKeys.erase(task.requestKey);
        ++removed;
        return true;
    });
    return removed;
}
}
