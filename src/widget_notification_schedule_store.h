#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetPersistedNotificationAction
{
    std::string id;
    std::string label;
};

struct WidgetPersistedNotificationSchedule
{
    std::string instanceId;
    std::string packageId;
    std::string notificationId;
    std::string title;
    std::string message;
    std::int64_t dueMs = 0;
    std::string imageResource;
    std::optional<double> progress;
    std::vector<WidgetPersistedNotificationAction> actions;
};

class WidgetNotificationScheduleStore
{
public:
    static constexpr std::size_t MaximumEntries = 512;
    static constexpr std::size_t MaximumFileBytes = 2 * 1024 * 1024;

    bool LoadText(std::string_view text, std::string& error);
    std::string Serialize() const;

    bool Upsert(WidgetPersistedNotificationSchedule entry,
        std::string& error);
    bool Remove(std::string_view instanceId,
        std::string_view notificationId);
    std::size_t RemoveInstance(std::string_view instanceId);
    bool UpdateContent(std::string_view instanceId,
        std::string_view notificationId,
        const std::optional<std::string>& title,
        const std::optional<std::string>& message,
        const std::optional<std::string>& imageResource,
        const std::optional<std::optional<double>>& progress,
        const std::optional<
            std::vector<WidgetPersistedNotificationAction>>& actions);
    std::optional<WidgetPersistedNotificationSchedule> Find(
        std::string_view instanceId,
        std::string_view notificationId) const;
    std::vector<WidgetPersistedNotificationSchedule> ForInstance(
        std::string_view instanceId,
        std::string_view packageId) const;
    std::vector<WidgetPersistedNotificationSchedule> Due(
        std::int64_t nowMs) const;
    const std::vector<WidgetPersistedNotificationSchedule>& Entries()
        const noexcept { return entries_; }

private:
    static bool Validate(
        const WidgetPersistedNotificationSchedule& entry,
        std::string& error);

    std::vector<WidgetPersistedNotificationSchedule> entries_;
};
}
