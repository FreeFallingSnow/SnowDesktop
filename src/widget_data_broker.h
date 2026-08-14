#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class DataProviderState
{
    Stopped,
    Starting,
    Active,
    IdleGrace,
};

enum class DataHiddenPolicy
{
    Pause,
    Throttle,
    Continue,
};

enum class DataBrokerActionType
{
    Start,
    Stop,
    Reconfigure,
};

enum class DataBrokerReason
{
    FirstSubscriber,
    SubscriptionChanged,
    VisibilityChanged,
    LastSubscriber,
    IdleGraceExpired,
    PermissionRevoked,
    Shutdown,
};

struct DataProviderDescriptor
{
    std::string topic;
    std::string requiredPermission;
    std::chrono::milliseconds minimumInterval{ 1000 };
    std::chrono::milliseconds hiddenInterval{ 5000 };
    std::chrono::milliseconds idleGrace{ 2000 };
    bool highRisk = false;
    bool supportsHiddenContinue = false;
};

struct DataSubscriptionOptions
{
    std::chrono::milliseconds requestedInterval{ 1000 };
    DataHiddenPolicy whenHidden = DataHiddenPolicy::Throttle;
    bool visible = true;
    bool permissionGranted = false;
    bool preview = false;
};

struct DataSubscriptionResult
{
    std::uint64_t id = 0;
    std::string error;

    explicit operator bool() const noexcept
    {
        return id != 0 && error.empty();
    }
};

struct DataBrokerAction
{
    DataBrokerActionType type = DataBrokerActionType::Start;
    DataBrokerReason reason = DataBrokerReason::SubscriptionChanged;
    std::string topic;
    std::chrono::milliseconds effectiveInterval{ 0 };
};

struct DataProviderSnapshot
{
    DataProviderState state = DataProviderState::Stopped;
    std::size_t subscriptionCount = 0;
    std::size_t eligibleCount = 0;
    std::size_t visibleCount = 0;
    std::size_t hiddenCount = 0;
    std::size_t permissionDeniedCount = 0;
    std::size_t previewCount = 0;
    std::chrono::milliseconds effectiveInterval{ 0 };
    bool shared = false;
    DataBrokerReason lastReason = DataBrokerReason::SubscriptionChanged;
    std::string lastError;
};

struct DataSubscriptionSnapshot
{
    std::uint64_t id = 0;
    std::string instanceId;
    std::string topic;
    DataSubscriptionOptions options;
    bool eligible = false;
};

class WidgetDataBroker
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr std::size_t MaxSubscriptions = 4096;
    static constexpr std::size_t MaxSubscriptionsPerInstance = 128;
    static constexpr std::chrono::milliseconds MaxRequestedInterval{
        24 * 60 * 60 * 1000 };

    bool RegisterProvider(DataProviderDescriptor descriptor,
        std::string& error);
    DataSubscriptionResult Subscribe(std::string instanceId,
        std::string_view topic, DataSubscriptionOptions options,
        TimePoint now);
    bool Unsubscribe(std::uint64_t subscriptionId, TimePoint now);
    std::size_t SetInstanceVisible(std::string_view instanceId,
        bool visible, TimePoint now);
    std::size_t SetPermission(std::string_view instanceId,
        std::string_view permission, bool granted, TimePoint now);

    bool MarkStarted(std::string_view topic, bool succeeded,
        std::string error = {});
    void Tick(TimePoint now);
    void Shutdown(TimePoint now);

    std::optional<DataProviderSnapshot> Snapshot(
        std::string_view topic) const;
    std::optional<std::string> RequiredPermission(
        std::string_view topic) const;
    std::optional<DataSubscriptionSnapshot> SubscriptionSnapshot(
        std::uint64_t subscriptionId) const;
    std::vector<DataBrokerAction> DrainActions();
    std::size_t SubscriptionCount() const noexcept;

private:
    struct Subscription
    {
        std::uint64_t id = 0;
        std::string instanceId;
        std::string topic;
        DataSubscriptionOptions options;
    };

    struct Provider
    {
        DataProviderDescriptor descriptor;
        DataProviderState state = DataProviderState::Stopped;
        std::unordered_set<std::uint64_t> subscriptions;
        std::chrono::milliseconds effectiveInterval{ 0 };
        TimePoint idleDeadline{};
        DataBrokerReason lastReason = DataBrokerReason::SubscriptionChanged;
        std::string lastError;
    };

    struct Eligibility
    {
        std::size_t eligible = 0;
        std::size_t visible = 0;
        std::size_t hidden = 0;
        std::size_t permissionDenied = 0;
        std::size_t preview = 0;
        std::chrono::milliseconds effectiveInterval{ 0 };
    };

    Eligibility Evaluate(const Provider& provider) const;
    void Reconcile(Provider& provider, TimePoint now,
        DataBrokerReason reason, bool forceImmediateStop);
    void QueueAction(Provider& provider, DataBrokerActionType type,
        DataBrokerReason reason,
        std::chrono::milliseconds effectiveInterval);

    std::unordered_map<std::string, Provider> providers_;
    std::unordered_map<std::uint64_t, Subscription> subscriptions_;
    std::vector<DataBrokerAction> actions_;
    std::uint64_t nextSubscriptionId_ = 0;
};
}
