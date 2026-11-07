#pragma once

#include "widget/packages/widget_package.h"
#include "widget/settings/widget_settings_service.h"
#include "widget/storage/widget_storage_value.h"

namespace snowdesktop::widget_runtime
{
/** One normalization path for isolated previews and atomic instance creation. */
inline bool NormalizeCreationPreset(
    const widget::CreationPreset& preset,
    const std::vector<WidgetSettingFieldState>& fields,
    std::vector<WidgetSettingOrdinaryWrite>& writes,
    std::unordered_map<std::string, std::string>& previewValues,
    std::string& error)
{
    std::vector<WidgetSettingOrdinaryWrite> pending;
    std::unordered_map<std::string, std::string> encoded;
    for (const auto& [key, raw] : preset.values)
    {
        const auto field = std::find_if(fields.begin(), fields.end(),
            [&](const auto& candidate) { return candidate.schema.key == key; });
        if (field == fields.end() ||
            field->schema.Channel() != WidgetSettingValueChannel::Ordinary)
        {
            error = "creationSettingNotOrdinary";
            return false;
        }
        InteractionValue value;
        if (!DecodeLegacyWidgetSettingValue(field->schema, raw, value, error))
            return false;
        const bool typed = WidgetSettingUsesTypedStorage(field->schema.Kind());
        std::string stored;
        if (typed ? !EncodeTypedStorageValue(value, stored, error)
                  : !EncodeLegacyWidgetSettingValue(field->schema, value, stored, error))
            return false;
        encoded[key] = std::move(stored);
        if (typed) encoded[TypedStorageMetadataKey(key)] = TypedStorageMarker;
        WidgetSettingOrdinaryWrite write{key, std::move(value), typed};
        if (field->schema.Kind() == WidgetSettingKind::AppSearch)
        {
            write.searchQuery = std::string{};
            encoded[field->schema.searchKey] = {};
        }
        pending.push_back(std::move(write));
    }
    writes = std::move(pending);
    previewValues = std::move(encoded);
    error.clear();
    return true;
}
}
