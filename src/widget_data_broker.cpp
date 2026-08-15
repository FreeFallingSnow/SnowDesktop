#include "widget_data_broker.h"

#include <algorithm>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
bool ValidDuration(std::chrono::milliseconds value)
{
    return value.count() >= 0 &&
        value <= WidgetDataBroker::MaxRequestedInterval;
}
}

bool WidgetDataBroker::RegisterProvider(
    DataProviderDescriptor descriptor, std::string& error)
{
    error.clear();
    if (descriptor.topic.empty())
    {
        error = "provider topic is required";
        return false;
    }
    if (providers_.contains(descriptor.topic))
    {
        error = "provider topic is already registered";
        return false;
    }
    if (descriptor.minimumInterval.count() <= 0 ||
        !ValidDuration(descriptor.minimumInterval) ||
        !ValidDuration(descriptor.hiddenInterval) ||
        !ValidDuration(descriptor.idleGrace))
    {
        error = "provider timing values are invalid";
        return false;
    }
    descriptor.hiddenInterval = std::max(
        descriptor.hiddenInterval, descriptor.minimumInterval);
    if (descriptor.highRisk)
    {
        descriptor.idleGrace = std::chrono::milliseconds::zero();
        descriptor.supportsHiddenContinue = false;
    }
    const std::string topic = descriptor.topic;
    providers_.emplace(topic, Provider{ std::move(descriptor) });
    return true;
}

DataSubscriptionResult WidgetDataBroker::Subscribe(
    std::string instanceId, std::string_view topic,
    DataSubscriptionOptions options, TimePoint now)
{
    if (instanceId.empty()) return { 0, "instance ID is required" };
    auto provider = providers_.find(std::string(topic));
    if (provider == providers_.end())
        return { 0, "data topic is not registered" };
    if (options.requestedInterval.count() <= 0 ||
        !ValidDuration(options.requestedInterval))
    {
        return { 0, "requested interval is invalid" };
    }
    if (options.rangeStart.size() > 10 || options.rangeEnd.size() > 10 ||
        options.rangeStart.empty() != options.rangeEnd.empty())
    {
        return { 0, "data range is invalid" };
    }
    if (topic == "audio.output.analysis" &&
        (!(options.audioWaveform || options.audioSpectrum ||
                options.audioRms || options.audioPeak) ||
            (options.audioWaveform &&
                (options.audioWaveformPoints < 16 ||
                    options.audioWaveformPoints > 256)) ||
            (options.audioSpectrum &&
                (options.audioSpectrumBins < 16 ||
                    options.audioSpectrumBins > 128))))
    {
        return { 0, "audio analysis options are invalid" };
    }
    if (subscriptions_.size() >= MaxSubscriptions)
        return { 0, "global data subscription limit exceeded" };
    const std::size_t instanceCount = static_cast<std::size_t>(
        std::count_if(subscriptions_.begin(), subscriptions_.end(),
            [&](const auto& entry) {
                return entry.second.instanceId == instanceId;
            }));
    if (instanceCount >= MaxSubscriptionsPerInstance)
        return { 0, "per-instance data subscription limit exceeded" };

    options.requestedInterval = std::max(
        options.requestedInterval,
        provider->second.descriptor.minimumInterval);
    std::uint64_t id = ++nextSubscriptionId_;
    if (id == 0) id = ++nextSubscriptionId_;
    subscriptions_.emplace(id, Subscription{
        id, std::move(instanceId), std::string(topic), options });
    provider->second.subscriptions.insert(id);
    Reconcile(provider->second, now,
        provider->second.subscriptions.size() == 1
            ? DataBrokerReason::FirstSubscriber
            : DataBrokerReason::SubscriptionChanged,
        false);
    return { id, {} };
}

bool WidgetDataBroker::Unsubscribe(
    std::uint64_t subscriptionId, TimePoint now)
{
    auto subscription = subscriptions_.find(subscriptionId);
    if (subscription == subscriptions_.end()) return false;
    auto provider = providers_.find(subscription->second.topic);
    if (provider != providers_.end())
        provider->second.subscriptions.erase(subscriptionId);
    subscriptions_.erase(subscription);
    if (provider != providers_.end())
    {
        Reconcile(provider->second, now,
            provider->second.subscriptions.empty()
                ? DataBrokerReason::LastSubscriber
                : DataBrokerReason::SubscriptionChanged,
            provider->second.descriptor.highRisk);
    }
    return true;
}

std::size_t WidgetDataBroker::SetInstanceVisible(
    std::string_view instanceId, bool visible, TimePoint now)
{
    std::unordered_set<std::string> changedTopics;
    for (auto& [_, subscription] : subscriptions_)
    {
        if (subscription.instanceId != instanceId ||
            subscription.options.visible == visible)
            continue;
        subscription.options.visible = visible;
        changedTopics.insert(subscription.topic);
    }
    for (const std::string& topic : changedTopics)
    {
        Provider& provider = providers_.at(topic);
        Reconcile(provider, now, DataBrokerReason::VisibilityChanged,
            !visible && provider.descriptor.highRisk);
    }
    return changedTopics.size();
}

std::size_t WidgetDataBroker::SetPermission(
    std::string_view instanceId, std::string_view permission,
    bool granted, TimePoint now)
{
    std::unordered_set<std::string> changedTopics;
    for (auto& [_, subscription] : subscriptions_)
    {
        if (subscription.instanceId != instanceId)
            continue;
        Provider& provider = providers_.at(subscription.topic);
        if (provider.descriptor.requiredPermission != permission ||
            subscription.options.permissionGranted == granted)
            continue;
        subscription.options.permissionGranted = granted;
        changedTopics.insert(subscription.topic);
    }
    for (const std::string& topic : changedTopics)
    {
        Reconcile(providers_.at(topic), now,
            granted ? DataBrokerReason::SubscriptionChanged
                    : DataBrokerReason::PermissionRevoked,
            !granted);
    }
    return changedTopics.size();
}

bool WidgetDataBroker::MarkStarted(
    std::string_view topic, bool succeeded, std::string error)
{
    auto provider = providers_.find(std::string(topic));
    if (provider == providers_.end() ||
        provider->second.state != DataProviderState::Starting)
        return false;
    provider->second.lastError = std::move(error);
    provider->second.state = succeeded
        ? DataProviderState::Active
        : DataProviderState::Stopped;
    if (succeeded) provider->second.lastError.clear();
    return true;
}

void WidgetDataBroker::Tick(TimePoint now)
{
    for (auto& [_, provider] : providers_)
    {
        if (provider.state != DataProviderState::IdleGrace ||
            now < provider.idleDeadline)
            continue;
        provider.state = DataProviderState::Stopped;
        provider.effectiveInterval = std::chrono::milliseconds::zero();
        QueueAction(provider, DataBrokerActionType::Stop,
            DataBrokerReason::IdleGraceExpired,
            std::chrono::milliseconds::zero());
    }
}

void WidgetDataBroker::Shutdown(TimePoint now)
{
    (void)now;
    for (auto& [_, provider] : providers_)
    {
        if (provider.state != DataProviderState::Stopped)
        {
            provider.state = DataProviderState::Stopped;
            provider.effectiveInterval = std::chrono::milliseconds::zero();
            QueueAction(provider, DataBrokerActionType::Stop,
                DataBrokerReason::Shutdown,
                std::chrono::milliseconds::zero());
        }
        provider.subscriptions.clear();
    }
    subscriptions_.clear();
}

std::optional<DataProviderSnapshot> WidgetDataBroker::Snapshot(
    std::string_view topic) const
{
    auto provider = providers_.find(std::string(topic));
    if (provider == providers_.end()) return std::nullopt;
    const Eligibility eligibility = Evaluate(provider->second);
    return DataProviderSnapshot{
        provider->second.state,
        provider->second.subscriptions.size(),
        eligibility.eligible,
        eligibility.visible,
        eligibility.hidden,
        eligibility.permissionDenied,
        eligibility.preview,
        provider->second.effectiveInterval,
        eligibility.eligible > 1,
        provider->second.lastReason,
        provider->second.lastError,
    };
}

std::optional<std::string> WidgetDataBroker::RequiredPermission(
    std::string_view topic) const
{
    auto provider = providers_.find(std::string(topic));
    if (provider == providers_.end()) return std::nullopt;
    return provider->second.descriptor.requiredPermission;
}

std::optional<DataSubscriptionSnapshot>
WidgetDataBroker::SubscriptionSnapshot(
    std::uint64_t subscriptionId) const
{
    auto subscription = subscriptions_.find(subscriptionId);
    if (subscription == subscriptions_.end()) return std::nullopt;
    auto provider = providers_.find(subscription->second.topic);
    if (provider == providers_.end()) return std::nullopt;
    const auto& options = subscription->second.options;
    bool eligible = !options.preview &&
        (options.permissionGranted ||
            provider->second.descriptor.requiredPermission.empty());
    if (eligible && !options.visible &&
        (provider->second.descriptor.highRisk ||
            options.whenHidden == DataHiddenPolicy::Pause))
    {
        eligible = false;
    }
    return DataSubscriptionSnapshot{
        subscription->second.id,
        subscription->second.instanceId,
        subscription->second.topic,
        options,
        eligible,
    };
}

std::vector<DataSubscriptionSnapshot>
WidgetDataBroker::SubscriptionSnapshots(std::string_view topic) const
{
    std::vector<DataSubscriptionSnapshot> result;
    const auto provider = providers_.find(std::string(topic));
    if (provider == providers_.end()) return result;
    result.reserve(provider->second.subscriptions.size());
    for (const std::uint64_t id : provider->second.subscriptions)
    {
        const auto snapshot = SubscriptionSnapshot(id);
        if (snapshot) result.push_back(*snapshot);
    }
    return result;
}

std::vector<DataBrokerAction> WidgetDataBroker::DrainActions()
{
    return std::exchange(actions_, {});
}

std::size_t WidgetDataBroker::SubscriptionCount() const noexcept
{
    return subscriptions_.size();
}

WidgetDataBroker::Eligibility WidgetDataBroker::Evaluate(
    const Provider& provider) const
{
    Eligibility result;
    for (const std::uint64_t id : provider.subscriptions)
    {
        auto subscription = subscriptions_.find(id);
        if (subscription == subscriptions_.end()) continue;
        const auto& options = subscription->second.options;
        if (options.preview)
        {
            ++result.preview;
            continue;
        }
        if (!options.permissionGranted &&
            !provider.descriptor.requiredPermission.empty())
        {
            ++result.permissionDenied;
            continue;
        }

        std::chrono::milliseconds interval =
            std::max(options.requestedInterval,
                provider.descriptor.minimumInterval);
        if (options.visible)
        {
            ++result.visible;
        }
        else
        {
            ++result.hidden;
            if (provider.descriptor.highRisk ||
                options.whenHidden == DataHiddenPolicy::Pause)
                continue;
            if (options.whenHidden == DataHiddenPolicy::Throttle ||
                !provider.descriptor.supportsHiddenContinue)
            {
                interval = std::max(interval,
                    provider.descriptor.hiddenInterval);
            }
        }
        ++result.eligible;
        if (result.effectiveInterval.count() == 0)
            result.effectiveInterval = interval;
        else
            result.effectiveInterval = std::min(
                result.effectiveInterval, interval);
    }
    return result;
}

void WidgetDataBroker::Reconcile(Provider& provider, TimePoint now,
    DataBrokerReason reason, bool forceImmediateStop)
{
    const Eligibility eligibility = Evaluate(provider);
    if (eligibility.eligible != 0)
    {
        const auto previousInterval = provider.effectiveInterval;
        provider.effectiveInterval = eligibility.effectiveInterval;
        if (provider.state == DataProviderState::Stopped)
        {
            provider.state = DataProviderState::Starting;
            QueueAction(provider, DataBrokerActionType::Start,
                reason, provider.effectiveInterval);
        }
        else if (provider.state == DataProviderState::IdleGrace)
        {
            provider.state = DataProviderState::Active;
            QueueAction(provider, DataBrokerActionType::Reconfigure,
                reason, provider.effectiveInterval);
        }
        else if (provider.state == DataProviderState::Starting &&
            previousInterval != provider.effectiveInterval)
        {
            QueueAction(provider, DataBrokerActionType::Reconfigure,
                reason, provider.effectiveInterval);
        }
        else if (provider.state == DataProviderState::Active &&
            previousInterval != provider.effectiveInterval)
        {
            QueueAction(provider, DataBrokerActionType::Reconfigure,
                reason, provider.effectiveInterval);
        }
        provider.lastReason = reason;
        return;
    }

    provider.effectiveInterval = std::chrono::milliseconds::zero();
    if (provider.state == DataProviderState::Stopped) return;
    if (forceImmediateStop || provider.descriptor.highRisk ||
        provider.descriptor.idleGrace.count() == 0)
    {
        provider.state = DataProviderState::Stopped;
        QueueAction(provider, DataBrokerActionType::Stop, reason,
            std::chrono::milliseconds::zero());
    }
    else if (provider.state != DataProviderState::IdleGrace)
    {
        provider.state = DataProviderState::IdleGrace;
        provider.idleDeadline = now + provider.descriptor.idleGrace;
        provider.lastReason = reason;
    }
}

void WidgetDataBroker::QueueAction(Provider& provider,
    DataBrokerActionType type, DataBrokerReason reason,
    std::chrono::milliseconds effectiveInterval)
{
    provider.lastReason = reason;
    actions_.push_back({
        type,
        reason,
        provider.descriptor.topic,
        effectiveInterval,
    });
}
}
