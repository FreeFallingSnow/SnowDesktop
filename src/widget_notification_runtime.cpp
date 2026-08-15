#include "widget_notification_runtime.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
WidgetNotificationOperationResult Failure(std::string error)
{
    return { false, {}, std::move(error) };
}

bool InvokeHost(const WidgetNotificationCenter::HostCallback& host,
    const WidgetNotificationHostRequest& request) noexcept
{
    if (!host) return false;
    try
    {
        return host(request);
    }
    catch (...)
    {
        return false;
    }
}

bool ValidContent(const WidgetNotificationContent& content)
{
    if (content.title.empty() || content.message.empty() ||
        (content.progress && (!std::isfinite(*content.progress) ||
            *content.progress < 0.0 || *content.progress > 1.0)) ||
        content.actions.size() > 2)
        return false;
    std::unordered_set<std::string> ids;
    for (const auto& action : content.actions)
    {
        if (action.id.empty() || action.id.size() > 64 ||
            action.label.empty() || !ids.insert(action.id).second)
            return false;
    }
    return true;
}

WidgetNotificationHostRequest HostRequest(
    WidgetNotificationHostOperation operation,
    const std::string& id,
    const std::wstring& title = {},
    const std::wstring& message = {},
    const std::wstring& imagePath = {},
    const std::optional<double>& progress = std::nullopt,
    const std::vector<WidgetNotificationAction>& actions = {})
{
    return { operation, id, title, message, imagePath, progress, actions };
}
}

std::string WidgetNotificationCenter::AllocateId(
    std::uint64_t ownerToken)
{
    for (;;)
    {
        std::uint64_t sequence = ++nextId_;
        if (sequence == 0) sequence = ++nextId_;
        std::ostringstream stream;
        stream << "notification:" << std::hex << ownerToken << ':'
            << sequence;
        std::string id = stream.str();
        const bool collision = std::any_of(
            records_.begin(), records_.end(),
            [&id](const Record& record) { return record.id == id; });
        if (!collision) return id;
    }
}

void WidgetNotificationCenter::PruneExpired(Clock::time_point now)
{
    std::erase_if(records_, [now](const Record& record) {
        return record.state == State::Delivered &&
            now - record.updated >= DeliveredRecordLifetime;
    });
}

std::vector<WidgetNotificationCenter::Record>::iterator
WidgetNotificationCenter::FindOwned(
    std::uint64_t ownerToken, std::string_view id)
{
    return std::find_if(records_.begin(), records_.end(),
        [ownerToken, id](const Record& record) {
            return record.ownerToken == ownerToken && record.id == id;
        });
}

std::vector<WidgetNotificationCenter::Record>::const_iterator
WidgetNotificationCenter::FindOwned(
    std::uint64_t ownerToken, std::string_view id) const
{
    return std::find_if(records_.begin(), records_.end(),
        [ownerToken, id](const Record& record) {
            return record.ownerToken == ownerToken && record.id == id;
        });
}

std::size_t WidgetNotificationCenter::CountForOwner(
    std::uint64_t ownerToken) const
{
    return static_cast<std::size_t>(std::count_if(
        records_.begin(), records_.end(),
        [ownerToken](const Record& record) {
            return record.ownerToken == ownerToken;
        }));
}

WidgetNotificationOperationResult WidgetNotificationCenter::Show(
    std::uint64_t ownerToken, WidgetNotificationContent content,
    Clock::time_point now, const HostCallback& host)
{
    if (ownerToken == 0 || !ValidContent(content))
        return Failure("invalidArguments");
    if (!host) return Failure("providerUnavailable");
    PruneExpired(now);
    if (CountForOwner(ownerToken) >= MaximumRecordsPerOwner)
        return Failure("quotaExceeded");

    Record record;
    record.ownerToken = ownerToken;
    record.id = AllocateId(ownerToken);
    record.title = std::move(content.title);
    record.message = std::move(content.message);
    record.imagePath = std::move(content.imagePath);
    record.progress = content.progress;
    record.actions = std::move(content.actions);
    record.state = State::Delivered;
    record.updated = now;
    if (!InvokeHost(host, HostRequest(
            WidgetNotificationHostOperation::Show, record.id,
            record.title, record.message, record.imagePath,
            record.progress, record.actions)))
        return Failure("notificationFailed");
    const std::string id = record.id;
    records_.push_back(std::move(record));
    return { true, id, {} };
}

WidgetNotificationOperationResult WidgetNotificationCenter::Schedule(
    std::uint64_t ownerToken, WidgetNotificationContent content,
    Clock::time_point due, Clock::time_point now)
{
    if (ownerToken == 0 || !ValidContent(content) || due <= now ||
        due - now > MaximumScheduleDelay)
        return Failure("invalidArguments");
    PruneExpired(now);
    const std::size_t ownerCount = CountForOwner(ownerToken);
    if (ownerCount >= MaximumRecordsPerOwner)
        return Failure("quotaExceeded");
    const std::size_t scheduledCount =
        static_cast<std::size_t>(std::count_if(
            records_.begin(), records_.end(),
            [ownerToken](const Record& record) {
                return record.ownerToken == ownerToken &&
                    record.state == State::Scheduled;
            }));
    if (scheduledCount >= MaximumScheduledPerOwner)
        return Failure("quotaExceeded");

    Record record;
    record.ownerToken = ownerToken;
    record.id = AllocateId(ownerToken);
    record.title = std::move(content.title);
    record.message = std::move(content.message);
    record.imagePath = std::move(content.imagePath);
    record.progress = content.progress;
    record.actions = std::move(content.actions);
    record.state = State::Scheduled;
    record.due = due;
    record.updated = now;
    const std::string id = record.id;
    records_.push_back(std::move(record));
    return { true, id, {} };
}

WidgetNotificationOperationResult WidgetNotificationCenter::RestoreScheduled(
    std::uint64_t ownerToken, std::string id,
    WidgetNotificationContent content,
    Clock::time_point due, Clock::time_point now)
{
    if (ownerToken == 0 || id.empty() || id.size() > 128 ||
        !ValidContent(content) ||
        due - now > MaximumScheduleDelay)
        return Failure("invalidArguments");
    if (now - due > DeliveredRecordLifetime)
        return Failure("expired");
    PruneExpired(now);
    if (FindOwned(ownerToken, id) != records_.end())
        return Failure("alreadyExists");
    const std::size_t ownerCount = CountForOwner(ownerToken);
    if (ownerCount >= MaximumRecordsPerOwner)
        return Failure("quotaExceeded");
    const std::size_t scheduledCount =
        static_cast<std::size_t>(std::count_if(
            records_.begin(), records_.end(),
            [ownerToken](const Record& record) {
                return record.ownerToken == ownerToken &&
                    record.state == State::Scheduled;
            }));
    if (scheduledCount >= MaximumScheduledPerOwner)
        return Failure("quotaExceeded");

    Record record;
    record.ownerToken = ownerToken;
    record.id = std::move(id);
    record.title = std::move(content.title);
    record.message = std::move(content.message);
    record.imagePath = std::move(content.imagePath);
    record.progress = content.progress;
    record.actions = std::move(content.actions);
    record.state = State::Scheduled;
    record.due = due;
    record.updated = now;
    records_.push_back(std::move(record));
    return { true, records_.back().id, {} };
}

WidgetNotificationOperationResult WidgetNotificationCenter::Update(
    std::uint64_t ownerToken, std::string_view id,
    WidgetNotificationPatch patch, const HostCallback& host)
{
    const bool supplied = patch.title || patch.message || patch.imagePath ||
        patch.progress || patch.actions;
    if (ownerToken == 0 || id.empty() || !supplied ||
        (patch.title && patch.title->empty()) ||
        (patch.message && patch.message->empty()))
        return Failure("invalidArguments");
    const auto found = FindOwned(ownerToken, id);
    if (found == records_.end()) return Failure("notFound");
    WidgetNotificationContent updated;
    updated.title = patch.title ? std::move(*patch.title) : found->title;
    updated.message = patch.message ? std::move(*patch.message) : found->message;
    updated.imagePath = patch.imagePath
        ? std::move(*patch.imagePath) : found->imagePath;
    updated.progress = patch.progress
        ? *patch.progress : found->progress;
    updated.actions = patch.actions
        ? std::move(*patch.actions) : found->actions;
    if (!ValidContent(updated)) return Failure("invalidArguments");
    if (found->state == State::Delivered)
    {
        if (!host) return Failure("providerUnavailable");
        if (!InvokeHost(host, HostRequest(
                WidgetNotificationHostOperation::Update, found->id,
                updated.title, updated.message, updated.imagePath,
                updated.progress, updated.actions)))
            return Failure("notificationFailed");
    }
    found->title = std::move(updated.title);
    found->message = std::move(updated.message);
    found->imagePath = std::move(updated.imagePath);
    found->progress = updated.progress;
    found->actions = std::move(updated.actions);
    found->updated = Clock::now();
    return { true, found->id, {} };
}

WidgetNotificationOperationResult WidgetNotificationCenter::Dismiss(
    std::uint64_t ownerToken, std::string_view id,
    const HostCallback& host)
{
    if (ownerToken == 0 || id.empty()) return Failure("invalidArguments");
    const auto found = FindOwned(ownerToken, id);
    if (found == records_.end()) return Failure("notFound");
    if (found->state != State::Delivered)
        return Failure("invalidState");
    if (!host) return Failure("providerUnavailable");
    if (!InvokeHost(host, HostRequest(
            WidgetNotificationHostOperation::Dismiss, found->id)))
        return Failure("notificationFailed");
    const std::string resultId = found->id;
    records_.erase(found);
    return { true, resultId, {} };
}

WidgetNotificationOperationResult WidgetNotificationCenter::Cancel(
    std::uint64_t ownerToken, std::string_view id)
{
    if (ownerToken == 0 || id.empty()) return Failure("invalidArguments");
    const auto found = FindOwned(ownerToken, id);
    if (found == records_.end()) return Failure("notFound");
    if (found->state != State::Scheduled)
        return Failure("invalidState");
    const std::string resultId = found->id;
    records_.erase(found);
    return { true, resultId, {} };
}

WidgetNotificationActivation WidgetNotificationCenter::Activate(
    std::string_view id, std::string_view actionId)
{
    if (id.empty() || actionId.empty()) return {};
    const auto found = std::find_if(records_.begin(), records_.end(),
        [id](const Record& record) {
            return record.id == id && record.state == State::Delivered;
        });
    if (found == records_.end()) return {};
    const bool allowed = std::any_of(
        found->actions.begin(), found->actions.end(),
        [actionId](const WidgetNotificationAction& action) {
            return action.id == actionId;
        });
    if (!allowed) return {};
    WidgetNotificationActivation activation{
        true, found->ownerToken, found->id, std::string(actionId) };
    records_.erase(found);
    return activation;
}

std::vector<WidgetNotificationDelivery>
WidgetNotificationCenter::DispatchDue(Clock::time_point now,
    const DeliveryAdmission& admit, const HostCallback& host)
{
    PruneExpired(now);
    std::vector<WidgetNotificationDelivery> deliveries;
    for (auto iterator = records_.begin(); iterator != records_.end();)
    {
        if (iterator->state != State::Scheduled || iterator->due > now)
        {
            ++iterator;
            continue;
        }
        WidgetNotificationDelivery delivery;
        delivery.ownerToken = iterator->ownerToken;
        delivery.id = iterator->id;
        if (!admit)
        {
            delivery.error = "providerUnavailable";
        }
        else
        {
            delivery.error = admit(iterator->ownerToken);
        }
        if (delivery.error.empty())
        {
            if (!host)
            {
                delivery.error = "providerUnavailable";
            }
            else if (!InvokeHost(host, HostRequest(
                    WidgetNotificationHostOperation::Show,
                    iterator->id, iterator->title, iterator->message,
                    iterator->imagePath, iterator->progress,
                    iterator->actions)))
            {
                delivery.error = "notificationFailed";
            }
        }
        delivery.ok = delivery.error.empty();
        deliveries.push_back(delivery);
        if (delivery.ok)
        {
            iterator->state = State::Delivered;
            iterator->updated = now;
            ++iterator;
        }
        else
        {
            iterator = records_.erase(iterator);
        }
    }
    return deliveries;
}

void WidgetNotificationCenter::RemoveOwner(
    std::uint64_t ownerToken, const HostCallback& host)
{
    for (const auto& record : records_)
    {
        if (record.ownerToken == ownerToken &&
            record.state == State::Delivered && host)
        {
            (void)InvokeHost(host, HostRequest(
                WidgetNotificationHostOperation::Dismiss, record.id));
        }
    }
    std::erase_if(records_, [ownerToken](const Record& record) {
        return record.ownerToken == ownerToken;
    });
}

void WidgetNotificationCenter::Clear(const HostCallback& host)
{
    if (host)
    {
        for (const auto& record : records_)
        {
            if (record.state == State::Delivered)
            {
                (void)InvokeHost(host, HostRequest(
                    WidgetNotificationHostOperation::Dismiss, record.id));
            }
        }
    }
    records_.clear();
}
}
