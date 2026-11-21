#include "widget_task_result_store.h"

#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
template <class Value>
std::optional<Value> TakeResult(std::unordered_map<std::uint64_t, Value>& results, std::uint64_t id)
{
    auto node = results.extract(id);
    if (node.empty()) return std::nullopt;
    return std::move(node.mapped());
}
}

void WidgetTaskResultStore::StoreAppSearch(std::uint64_t id, WidgetAppSearchCompletion value)
{
    appSearch_.insert_or_assign(id, std::move(value));
}

void WidgetTaskResultStore::StoreItemSearch(std::uint64_t id, WidgetAppSearchCompletion value)
{
    itemSearch_.insert_or_assign(id, std::move(value));
}

void WidgetTaskResultStore::StoreCalendar(std::uint64_t id, calendar::MutationResult value)
{
    calendar_.insert_or_assign(id, std::move(value));
}

void WidgetTaskResultStore::StoreNotification(std::uint64_t id, std::string value)
{
    notification_.insert_or_assign(id, std::move(value));
}

void WidgetTaskResultStore::StoreLocation(std::uint64_t id, LocationResult value)
{
    location_.insert_or_assign(id, std::move(value));
}

WidgetTaskResults WidgetTaskResultStore::Take(std::uint64_t id)
{
    return {
        TakeResult(appSearch_, id),
        TakeResult(itemSearch_, id),
        TakeResult(calendar_, id),
        TakeResult(notification_, id),
        TakeResult(location_, id),
    };
}

void WidgetTaskResultStore::Discard(std::uint64_t id)
{
    appSearch_.erase(id);
    itemSearch_.erase(id);
    calendar_.erase(id);
    notification_.erase(id);
    location_.erase(id);
}

void WidgetTaskResultStore::DiscardAppSearch(std::uint64_t id)
{
    appSearch_.erase(id);
}

void WidgetTaskResultStore::DiscardItemSearch(std::uint64_t id)
{
    itemSearch_.erase(id);
}

void WidgetTaskResultStore::DiscardLocation(std::uint64_t id)
{
    location_.erase(id);
}

void WidgetTaskResultStore::Clear()
{
    appSearch_.clear();
    itemSearch_.clear();
    calendar_.clear();
    notification_.clear();
    location_.clear();
}
}
