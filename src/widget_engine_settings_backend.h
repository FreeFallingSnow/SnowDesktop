#pragma once

#include "widget_engine.h"
#include "widget_settings_service.h"
#include "widget_storage_value.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace widget_settings_backend_detail
{
inline WidgetSettingCondition ConvertCondition(
    const LuaWidgetManifest::SettingCondition& source)
{
    return { source.key, source.operation, source.values };
}

inline WidgetSettingSourceField ConvertSetting(
    const LuaWidgetManifest::Setting& source)
{
    WidgetSettingSourceField result;
    auto& schema = result.schema;
    schema.key = source.key;
    schema.label = source.label;
    schema.description = source.description;
    schema.group = source.group;
    schema.validationMessage = source.validationMessage;
    schema.rawType = source.type.empty() ? "text" : source.type;
    schema.searchKey = source.searchKey;
    schema.binding = source.binding;
    schema.access = source.access;
    schema.emptyLabel = source.emptyLabel;
    schema.noResultsLabel = source.noResultsLabel;
    schema.minimum = source.minValue;
    schema.maximum = source.maxValue;
    schema.step = source.stepValue;
    schema.minimumLength = source.minLength;
    schema.maximumLength = source.maxLength;
    schema.required = source.required;
    schema.dependsOn = source.dependsOn;
    schema.extensions = source.extensions;
    if (source.showWhen)
        schema.showWhen = ConvertCondition(*source.showWhen);
    if (source.enabledWhen)
        schema.enabledWhen = ConvertCondition(*source.enabledWhen);
    schema.options.reserve(source.options.size());
    for (std::size_t index = 0; index < source.options.size(); ++index)
    {
        const std::string& value = source.options[index];
        const std::string& label = index < source.optionLabels.size() &&
                !source.optionLabels[index].empty()
            ? source.optionLabels[index] : value;
        schema.options.push_back({ value, label });
    }

    switch (schema.Kind())
    {
    case WidgetSettingKind::Password:
    case WidgetSettingKind::AppReference:
    case WidgetSettingKind::DesktopItemReference:
    case WidgetSettingKind::FileReference:
    case WidgetSettingKind::FolderReference:
    case WidgetSettingKind::FileHandle:
    case WidgetSettingKind::FolderHandle:
        // Opaque declarations never transport authored values.
        result.defaultValue = {};
        break;
    case WidgetSettingKind::Boolean:
        result.defaultValue = MakeWidgetSettingBoolean(
            source.defaultValue == "1" || source.defaultValue == "true");
        break;
    case WidgetSettingKind::Integer:
    case WidgetSettingKind::Color:
    {
        long long value = 0;
        (void)ReadWidgetSettingInteger(source.defaultValue, value);
        result.defaultValue = MakeWidgetSettingInteger(value);
        break;
    }
    case WidgetSettingKind::FloatingPoint:
    {
        double value = 0.0;
        (void)ParseFiniteSettingNumber(source.defaultValue, value);
        result.defaultValue = MakeWidgetSettingNumber(value);
        break;
    }
    case WidgetSettingKind::Range:
    {
        double value = source.minValue;
        (void)ParseFiniteSettingNumber(source.defaultValue, value);
        result.defaultValue = MakeWidgetSettingNumber(
            SnapRangeSettingValue(value, source.minValue,
                source.maxValue, source.stepValue));
        break;
    }
    case WidgetSettingKind::MultiSelect:
        result.defaultValue =
            MakeWidgetSettingStringArray(source.defaultValues);
        break;
    default:
        result.defaultValue = MakeWidgetSettingString(source.defaultValue);
        break;
    }
    return result;
}

struct EncodedOrdinaryWrite
{
    std::string value;
    bool typedMarker = false;
};

inline bool EncodeOrdinaryWrite(
    const WidgetSettingFieldSchema& schema,
    const WidgetSettingOrdinaryWrite& write,
    EncodedOrdinaryWrite& encoded, std::string& error)
{
    encoded = {};
    error.clear();
    if (schema.key != write.key ||
        schema.Channel() != WidgetSettingValueChannel::Ordinary)
    {
        error = "wrongValueChannel";
        return false;
    }
    const bool typed = WidgetSettingUsesTypedStorage(schema.Kind());
    if (typed != write.typedStorage)
    {
        error = typed ? "typedStorageRequired"
                      : "unexpectedTypedStorage";
        return false;
    }
    InteractionValue normalized;
    if (!NormalizeWidgetSettingValue(
            schema, write.value, normalized, error))
        return false;
    encoded.typedMarker = typed;
    return typed
        ? EncodeTypedStorageValue(normalized, encoded.value, error)
        : EncodeLegacyWidgetSettingValue(
            schema, normalized, encoded.value, error);
}

struct DecodedSearchQueryStorage
{
    bool hasStoredValue = false;
    std::string value;
};

inline bool DecodeSearchQueryStorage(
    std::optional<std::string_view> stored,
    std::optional<std::string_view> typedMarker,
    DecodedSearchQueryStorage& decoded, std::string& error)
{
    decoded = {};
    error.clear();
    if (!stored)
    {
        if (typedMarker)
        {
            error = "search query has an orphan typed-storage marker";
            return false;
        }
        return true;
    }
    if (!typedMarker)
    {
        decoded.hasStoredValue = true;
        decoded.value.assign(stored->data(), stored->size());
        return true;
    }
    if (*typedMarker != TypedStorageMarker)
    {
        error = "search query typed-storage marker is invalid";
        return false;
    }

    InteractionValue typed;
    if (!DecodeTypedStorageValue(*stored, typed, error)) return false;
    if (typed.type != InteractionValue::Type::String)
    {
        error = "search query typed storage must contain a string";
        return false;
    }
    decoded.hasStoredValue = true;
    decoded.value = std::move(typed.string);
    return true;
}

inline bool DescriptorMatchesCurrent(
    const WidgetSettingsBackendDescriptor& descriptor,
    std::wstring_view currentWidgetId,
    std::string_view currentPackageId,
    std::uint64_t currentRuntimeToken) noexcept
{
    return !descriptor.widgetId.empty() && descriptor.generation != 0 &&
        descriptor.widgetId == currentWidgetId &&
        descriptor.packageId == currentPackageId &&
        descriptor.generation == currentRuntimeToken;
}

inline bool MutationIdentityMatches(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    std::wstring_view currentWidgetId,
    std::string_view currentPackageId,
    std::uint64_t currentRuntimeToken) noexcept
{
    return guard.IsValid() && guard.widgetId == descriptor.widgetId &&
        guard.generation == descriptor.generation &&
        DescriptorMatchesCurrent(descriptor, currentWidgetId,
            currentPackageId, currentRuntimeToken);
}

inline bool SearchIdentityMatches(
    const WidgetSettingSearchRequest& expected,
    const WidgetSettingSearchRequest& actual) noexcept
{
    return expected.widgetId == actual.widgetId &&
        expected.packageId == actual.packageId &&
        expected.settingKey == actual.settingKey &&
        expected.generation == actual.generation &&
        expected.requestId != 0 &&
        expected.requestId == actual.requestId;
}
}

/**
 * Secure production bridge between one WidgetEngine and the declarative v2
 * widget settings service. The engine must outlive this backend.
 */
class WidgetEngineSettingsBackend final : public IWidgetSettingsBackend
{
public:
    explicit WidgetEngineSettingsBackend(WidgetEngine& engine);
    ~WidgetEngineSettingsBackend() override;

    WidgetEngineSettingsBackend(const WidgetEngineSettingsBackend&) = delete;
    WidgetEngineSettingsBackend& operator=(
        const WidgetEngineSettingsBackend&) = delete;

    WidgetSettingsBackendResult Describe(std::wstring_view widgetId,
        WidgetSettingsBackendDescriptor& descriptor) override;
    WidgetSettingBackendReadResult ReadOrdinary(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingFieldSchema& field,
        bool typedStorage) override;
    WidgetSettingBackendReadResult ReadSearchQuery(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingFieldSchema& field) override;
    WidgetSettingBackendOpaqueResult ReadOpaque(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingFieldSchema& field) override;
    WidgetSettingsBackendResult ApplyOrdinaryTransaction(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const std::vector<WidgetSettingOrdinaryWrite>& writes) override;
    WidgetSettingsBackendResult SetSecret(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field,
        std::string_view plaintext) override;
    WidgetSettingsBackendResult ClearSecret(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override;
    WidgetSettingsBackendResult ChooseFilesystemHandle(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override;
    WidgetSettingsBackendResult ClearFilesystemHandle(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override;
    WidgetSettingsBackendResult OpenEntityReferencePicker(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override;
    WidgetSettingsBackendResult ClearEntityReference(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override;
    WidgetSettingsBackendResult StartSearch(
        WidgetSettingSearchRequest request,
        SearchCompletion completion) override;
    WidgetSettingsBackendResult CancelSearch(
        const WidgetSettingSearchRequest& request) override;
    WidgetSettingsBackendResult CommitSearchResult(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingSearchRequest& request,
        std::string_view resultId) override;

private:
    struct SearchState;

    WidgetEngine& engine_;
    std::uint32_t ownerThreadId_ = 0;
    std::shared_ptr<SearchState> searches_;
};

std::unique_ptr<IWidgetSettingsBackend>
CreateWidgetEngineSettingsBackend(WidgetEngine& engine);
}
