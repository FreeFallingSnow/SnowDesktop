#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class WidgetNotificationHostOperation
{
    Show,
    Update,
    Dismiss,
};

struct WidgetNotificationHostRequest
{
    WidgetNotificationHostOperation operation =
        WidgetNotificationHostOperation::Show;
    std::string id;
    std::wstring title;
    std::wstring message;
};

struct WidgetNotificationOperationResult
{
    bool ok = false;
    std::string id;
    std::string error;
};

struct WidgetNotificationDelivery
{
    std::uint64_t ownerToken = 0;
    std::string id;
    bool ok = false;
    std::string error;
};

/**
 * Owns opaque notification IDs and their instance-scoped lifecycle.
 * Host presentation remains synchronous and replaceable so the engine can
 * use a tray balloon today without exposing that provider to Lua.
 */
class WidgetNotificationCenter
{
public:
    using Clock = std::chrono::system_clock;
    using HostCallback =
        std::function<bool(const WidgetNotificationHostRequest&)>;
    using DeliveryAdmission = std::function<std::string(std::uint64_t)>;

    static constexpr std::size_t MaximumRecordsPerOwner = 64;
    static constexpr std::size_t MaximumScheduledPerOwner = 32;
    static constexpr auto MaximumScheduleDelay = std::chrono::hours(24 * 366);
    static constexpr auto DeliveredRecordLifetime = std::chrono::hours(24);

    WidgetNotificationOperationResult Show(std::uint64_t ownerToken,
        std::wstring title, std::wstring message, Clock::time_point now,
        const HostCallback& host);
    WidgetNotificationOperationResult Schedule(std::uint64_t ownerToken,
        std::wstring title, std::wstring message, Clock::time_point due,
        Clock::time_point now);
    WidgetNotificationOperationResult RestoreScheduled(
        std::uint64_t ownerToken, std::string id,
        std::wstring title, std::wstring message, Clock::time_point due,
        Clock::time_point now);
    WidgetNotificationOperationResult Update(std::uint64_t ownerToken,
        std::string_view id, std::optional<std::wstring> title,
        std::optional<std::wstring> message, const HostCallback& host);
    WidgetNotificationOperationResult Dismiss(std::uint64_t ownerToken,
        std::string_view id, const HostCallback& host);
    WidgetNotificationOperationResult Cancel(std::uint64_t ownerToken,
        std::string_view id);

    std::vector<WidgetNotificationDelivery> DispatchDue(
        Clock::time_point now, const DeliveryAdmission& admit,
        const HostCallback& host);
    void RemoveOwner(std::uint64_t ownerToken, const HostCallback& host);
    void Clear(const HostCallback& host);
    std::size_t CountForOwner(std::uint64_t ownerToken) const;

private:
    enum class State
    {
        Scheduled,
        Delivered,
    };

    struct Record
    {
        std::uint64_t ownerToken = 0;
        std::string id;
        std::wstring title;
        std::wstring message;
        State state = State::Scheduled;
        Clock::time_point due{};
        Clock::time_point updated{};
    };

    std::string AllocateId(std::uint64_t ownerToken);
    void PruneExpired(Clock::time_point now);
    std::vector<Record>::iterator FindOwned(
        std::uint64_t ownerToken, std::string_view id);
    std::vector<Record>::const_iterator FindOwned(
        std::uint64_t ownerToken, std::string_view id) const;

    std::vector<Record> records_;
    std::uint64_t nextId_ = 0;
};
}
