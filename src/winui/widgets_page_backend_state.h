#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace snowdesktop::winui::widgets_page_backend_detail
{

struct ReviewedPackageFileIdentity
{
    std::uint64_t volumeSerialNumber = 0;
    std::array<std::uint8_t, 16> fileId{};

    bool operator==(const ReviewedPackageFileIdentity&) const = default;
};

enum class OutstandingOperationKind : std::uint8_t
{
    Search,
    WorkshopUnsubscribe,
    SourceSynchronization,
};

struct OutstandingOperationIdentity
{
    std::uint64_t generation = 0;
    std::uint64_t activation = 0;
    std::uint64_t taskId = 0;
    OutstandingOperationKind kind = OutstandingOperationKind::Search;

    bool operator==(const OutstandingOperationIdentity&) const = default;
};

/**
 * Tracks host/worker operations independently from the currently visible page.
 * A partially matching callback cannot release the mutation gate. An exact
 * terminal callback does release its operation even when the visible page has
 * advanced to another activation. Deactivation intentionally has no operation
 * on this ledger.
 */
class OutstandingOperationLedger final
{
public:
    [[nodiscard]] bool Begin(OutstandingOperationIdentity identity)
    {
        if (identity.generation == 0 || identity.activation == 0 ||
            identity.taskId == 0)
        {
            return false;
        }
        return operations_.emplace(identity.taskId, identity).second;
    }

    [[nodiscard]] bool Complete(
        const OutstandingOperationIdentity& identity) noexcept
    {
        const auto found = operations_.find(identity.taskId);
        if (found == operations_.end() || found->second != identity)
            return false;
        operations_.erase(found);
        return true;
    }

    [[nodiscard]] bool Busy() const noexcept
    {
        return !operations_.empty();
    }

    [[nodiscard]] bool Contains(std::uint64_t taskId) const noexcept
    {
        return operations_.contains(taskId);
    }

    [[nodiscard]] std::vector<std::uint64_t> Tasks(
        OutstandingOperationKind kind) const
    {
        std::vector<std::uint64_t> result;
        result.reserve(operations_.size());
        for (const auto& [taskId, identity] : operations_)
        {
            if (identity.kind == kind) result.push_back(taskId);
        }
        return result;
    }

    void Clear() noexcept
    {
        operations_.clear();
    }

private:
    std::unordered_map<std::uint64_t, OutstandingOperationIdentity>
        operations_;
};

} // namespace snowdesktop::winui::widgets_page_backend_detail
