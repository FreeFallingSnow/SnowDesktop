#pragma once

#include "widget/settings/widget_settings_service.h"
#include "widget/runtime/widget_logical_slot.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop
{
// Runs on the host owner thread after the asynchronous picker closes. The
// engine binding/persistence boundary is supplied by DesktopApp; the settings
// service must be refreshed after that boundary, never when the picker opens.
template<typename Bind, typename Notify>
bool CommitWidgetReferencePicker(
    widget_runtime::IWidgetSettingsService* service,
    const std::wstring& widgetId, std::uint64_t generation,
    std::string_view slotId, widget_runtime::LogicalSlotItem candidate,
    Bind&& bind, Notify&& notify, std::string& error)
{
    namespace wr = widget_runtime;
    const auto before = service ? service->Snapshot(widgetId) : std::nullopt;
    if (before && before->generation != generation)
    {
        error = "logical slot picker settings owner is stale";
        return false;
    }

    wr::LogicalSlotChange change;
    if (!bind(std::move(candidate), change, error)) return false;
    if (!before) return true;

    std::vector<std::string> keys;
    for (const auto& field : before->fields)
        if (field.schema.Channel() == wr::WidgetSettingValueChannel::EntityReference &&
            field.schema.binding == slotId)
            keys.push_back(field.schema.key);
    if (keys.empty()) return true;
    if (change.operation != "unchanged") notify(std::move(keys));

    // A settings window may close or switch to a new runtime during the Lua
    // notification. Do not recreate its closed session or refresh a new owner.
    const auto current = service->Snapshot(widgetId);
    if (!current || current->generation != generation) return true;
    const auto guard = wr::WidgetSettingMutationGuard::FromSnapshot(*current);
    for (const auto& field : current->fields)
        if (field.schema.Channel() == wr::WidgetSettingValueChannel::EntityReference &&
            field.schema.binding == slotId)
            (void)service->CancelSearch(guard, field.schema.key);
    const auto refreshed = service->Reload(widgetId);
    if (refreshed.Succeeded()) return true;
    error = refreshed.errorCode;
    return false;
}
}
