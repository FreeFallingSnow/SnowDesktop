#pragma once

#include "system/calendar/calendar_service.h"
#include "widget_app_task_executor.h"
#include "widget_location_task_executor.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
struct WidgetTaskResults
{
    std::optional<WidgetAppSearchCompletion> appSearch;
    std::optional<WidgetAppSearchCompletion> itemSearch;
    std::optional<calendar::MutationResult> calendar;
    std::optional<std::string> notification;
    std::optional<LocationResult> location;
};

class WidgetTaskResultStore
{
public:
    void StoreAppSearch(std::uint64_t id, WidgetAppSearchCompletion value);
    void StoreItemSearch(std::uint64_t id, WidgetAppSearchCompletion value);
    void StoreCalendar(std::uint64_t id, calendar::MutationResult value);
    void StoreNotification(std::uint64_t id, std::string value);
    void StoreLocation(std::uint64_t id, LocationResult value);

    WidgetTaskResults Take(std::uint64_t id);
    void Discard(std::uint64_t id);
    void DiscardAppSearch(std::uint64_t id);
    void DiscardItemSearch(std::uint64_t id);
    void DiscardLocation(std::uint64_t id);
    void Clear();

private:
    std::unordered_map<std::uint64_t, WidgetAppSearchCompletion> appSearch_;
    std::unordered_map<std::uint64_t, WidgetAppSearchCompletion> itemSearch_;
    std::unordered_map<std::uint64_t, calendar::MutationResult> calendar_;
    std::unordered_map<std::uint64_t, std::string> notification_;
    std::unordered_map<std::uint64_t, LocationResult> location_;
};
}
