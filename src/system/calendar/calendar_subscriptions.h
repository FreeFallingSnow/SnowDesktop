#pragma once

#include "calendar_service.h"
#include "calendar_subscription_types.h"

#include <filesystem>
#include <memory>

namespace snowdesktop::calendar
{

class CalendarSubscriptions
{
public:
    explicit CalendarSubscriptions(std::filesystem::path directory);
    ~CalendarSubscriptions();

    void Load();
    bool Tick();
    std::vector<SubscriptionInfo> List() const;
    SubscriptionResult Apply(SubscriptionCommand command);
    void AppendEvents(std::vector<CalendarEvent>& result,
        const std::string& from, const std::string& to) const;
    std::optional<CalendarEvent> EventById(const std::string& id) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace snowdesktop::calendar
