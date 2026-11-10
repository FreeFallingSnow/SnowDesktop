#pragma once

#include "settings/settings_fwd.h"
#include "system/calendar/calendar_service.h"
#include "system/calendar/calendar_subscription_types.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::winui
{

struct CalendarPageActions
{
    std::function<void(std::uint64_t, std::function<void(GeneralSettings&)>)> commitGeneral;
    std::function<std::optional<std::vector<calendar::CalendarEvent>>(std::uint64_t)> events;
    std::function<calendar::MutationResult(std::uint64_t, calendar::CalendarEvent, bool)> mutate;
    std::function<std::optional<std::vector<calendar::CalendarSeries>>(std::uint64_t)> series;
    std::function<std::optional<calendar::CalendarEvent>(std::uint64_t, std::string)> occurrence;
    std::function<calendar::MutationResult(std::uint64_t, calendar::CalendarSeries, bool)> mutateSeries;
    std::function<std::optional<std::vector<calendar::SubscriptionInfo>>(std::uint64_t)> subscriptions;
    std::function<std::optional<calendar::SubscriptionDetails>(std::uint64_t, std::string)> subscriptionDetails;
    std::function<calendar::SubscriptionResult(std::uint64_t, calendar::SubscriptionCommand)> subscription;
};

} // namespace snowdesktop::winui
