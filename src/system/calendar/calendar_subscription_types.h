#pragma once

#include <cstdint>
#include <string>

namespace snowdesktop::calendar
{

enum class SubscriptionStatus : std::uint8_t { Pending, Current, Failed };
enum class SubscriptionAction : std::uint8_t { Add, Refresh, Remove };
enum class SubscriptionResult : std::uint8_t
{
    Applied, Unavailable, InvalidLink, InvalidName, Duplicate, Limit, NotFound, SaveFailed
};

struct SubscriptionInfo
{
    std::string id;
    std::wstring name;
    SubscriptionStatus status = SubscriptionStatus::Pending;
    bool hasCache = false;
    bool operator==(const SubscriptionInfo&) const = default;
};

struct SubscriptionCommand
{
    SubscriptionAction action = SubscriptionAction::Add;
    std::string id;
    std::wstring name;
    std::wstring url;
};

} // namespace snowdesktop::calendar
