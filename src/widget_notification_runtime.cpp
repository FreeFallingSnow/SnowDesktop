#include "widget_notification_runtime.h"

#include <algorithm>
#include <sstream>
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
}

std::string WidgetNotificationCenter::AllocateId(
    std::uint64_t ownerToken)
{
    std::uint64_t sequence = ++nextId_;
    if (sequence == 0) sequence = ++nextId_;
    std::ostringstream stream;
    stream << "notification:" << std::hex << ownerToken << ':' << sequence;
    return stream.str();
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
    std::uint64_t ownerToken, std::wstring title, std::wstring message,
    Clock::time_point now, const HostCallback& host)
{
    if (ownerToken == 0 || title.empty() || message.empty())
        return Failure("invalidArguments");
    if (!host) return Failure("providerUnavailable");
    PruneExpired(now);
    if (CountForOwner(ownerToken) >= MaximumRecordsPerOwner)
        return Failure("quotaExceeded");

    Record record;
    record.ownerToken = ownerToken;
    record.id = AllocateId(ownerToken);
    record.title = std::move(title);
    record.message = std::move(message);
    record.state = State::Delivered;
    record.updated = now;
    if (!InvokeHost(host, { WidgetNotificationHostOperation::Show,
            record.id, record.title, record.message }))
        return Failure("notificationFailed");
    const std::string id = record.id;
    records_.push_back(std::move(record));
    return { true, id, {} };
}

WidgetNotificationOperationResult WidgetNotificationCenter::Schedule(
    std::uint64_t ownerToken, std::wstring title, std::wstring message,
    Clock::time_point due, Clock::time_point now)
{
    if (ownerToken == 0 || title.empty() || message.empty() || due <= now ||
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
    record.title = std::move(title);
    record.message = std::move(message);
    record.state = State::Scheduled;
    record.due = due;
    record.updated = now;
    const std::string id = record.id;
    records_.push_back(std::move(record));
    return { true, id, {} };
}

WidgetNotificationOperationResult WidgetNotificationCenter::Update(
    std::uint64_t ownerToken, std::string_view id,
    std::optional<std::wstring> title,
    std::optional<std::wstring> message, const HostCallback& host)
{
    if (ownerToken == 0 || id.empty() || (!title && !message) ||
        (title && title->empty()) || (message && message->empty()))
        return Failure("invalidArguments");
    const auto found = FindOwned(ownerToken, id);
    if (found == records_.end()) return Failure("notFound");
    std::wstring updatedTitle = title ? std::move(*title) : found->title;
    std::wstring updatedMessage = message ? std::move(*message) : found->message;
    if (found->state == State::Delivered)
    {
        if (!host) return Failure("providerUnavailable");
        if (!InvokeHost(host, { WidgetNotificationHostOperation::Update,
                found->id, updatedTitle, updatedMessage }))
            return Failure("notificationFailed");
    }
    found->title = std::move(updatedTitle);
    found->message = std::move(updatedMessage);
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
    if (!InvokeHost(host, { WidgetNotificationHostOperation::Dismiss,
            found->id, {}, {} }))
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
            else if (!InvokeHost(host,
                    { WidgetNotificationHostOperation::Show,
                        iterator->id, iterator->title,
                        iterator->message }))
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
            (void)InvokeHost(host,
                { WidgetNotificationHostOperation::Dismiss,
                    record.id, {}, {} });
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
                (void)InvokeHost(host,
                    { WidgetNotificationHostOperation::Dismiss,
                        record.id, {}, {} });
            }
        }
    }
    records_.clear();
}
}
