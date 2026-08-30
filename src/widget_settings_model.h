#pragma once

#include "personalization.h"
#include "widget_interaction_region.h"
#include "widget_setting_rules.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
/**
 * Backend-neutral description of the declarative v2 widget setting types.
 * Unknown types intentionally remain representable so a settings frontend can
 * fall back to a text editor while the runtime records diagnostics.
 */
enum class WidgetSettingKind
{
    Unknown,
    Text,
    Password,
    Boolean,
    Integer,
    FloatingPoint,
    Range,
    Color,
    Select,
    MultiSelect,
    Url,
    Date,
    Time,
    AppSearch,
    AppReference,
    DesktopItemReference,
    FileReference,
    FolderReference,
    FileHandle,
    FolderHandle,
};

enum class WidgetSettingValueChannel
{
    Ordinary,
    Secret,
    FilesystemHandle,
    EntityReference,
};

enum class WidgetSettingDiagnostic
{
    None,
    UnknownTypeTextFallback,
};

inline WidgetSettingKind WidgetSettingKindFromType(
    std::string_view type) noexcept;
inline WidgetSettingValueChannel WidgetSettingChannelForKind(
    WidgetSettingKind kind) noexcept;
inline bool WidgetSettingUsesTypedStorage(WidgetSettingKind kind) noexcept;

struct WidgetSettingCondition
{
    std::string key;
    std::string operation;
    std::vector<std::string> values;

    bool operator==(const WidgetSettingCondition&) const = default;
};

struct WidgetSettingOption
{
    std::string value;
    std::string label;

    bool operator==(const WidgetSettingOption&) const = default;
};

struct WidgetSettingFieldSchema
{
    std::string key;
    std::string label;
    std::string description;
    std::string group;
    std::string validationMessage;
    std::string rawType;
    std::string searchKey;
    std::string binding;
    std::string access = "read";
    std::string emptyLabel;
    std::string noResultsLabel;
    double minimum = 0.0;
    double maximum = 100.0;
    double step = 1.0;
    int minimumLength = -1;
    int maximumLength = -1;
    bool required = false;
    std::optional<WidgetSettingCondition> showWhen;
    std::optional<WidgetSettingCondition> enabledWhen;
    std::string dependsOn;
    std::vector<WidgetSettingOption> options;
    std::vector<std::string> extensions;

    /** Kind and value channel are derived so they can never disagree. */
    [[nodiscard]] WidgetSettingKind Kind() const noexcept;
    [[nodiscard]] WidgetSettingValueChannel Channel() const noexcept;
    [[nodiscard]] WidgetSettingDiagnostic Diagnostic() const noexcept;
    [[nodiscard]] std::string_view DiagnosticCode() const noexcept;

    bool operator==(const WidgetSettingFieldSchema&) const = default;
};

struct WidgetSettingGroupSchema
{
    std::string id;
    std::string label;
    std::string description;
    bool collapsible = false;
    bool defaultExpanded = true;

    bool operator==(const WidgetSettingGroupSchema&) const = default;
};

struct WidgetSettingPresetSchema
{
    std::string id;
    std::string label;
    std::map<std::string, InteractionValue, std::less<>> values;
    bool isDefault = false;
    /**
     * Legacy host-owned appearance values are kept separate from declarative
     * fields.  They continue to use the established per-instance storage keys
     * and never become part of the public v2 schema.
     */
    std::map<std::string, std::string, std::less<>> hostAppearanceValues;

    bool operator==(const WidgetSettingPresetSchema&) const = default;
};

/** Effective host-owned appearance state for one component instance. */
struct WidgetHostAppearanceState
{
    bool followPersonalization = false;
    std::string presetId;
    int backgroundColor = 0x151A21;
    int borderColor = 0xFFFFFF;
    float backgroundOpacity = 0.36f;
    float borderOpacity = 0.40f;
    PanelBorderStyle borderStyle = PanelBorderStyle::Standard;
    float borderWidth = 1.0f;
    float borderEffectStrength = kDefaultDimensionalBorderStrength;
    float gradientEndOpacity = 0.0f;
    bool glassEnabled = false;
    bool acrylicEnabled = false;
    int contentTheme = 0;

    bool operator==(const WidgetHostAppearanceState&) const = default;
};

/** Atomic partial update for the established host appearance storage keys. */
struct WidgetHostAppearancePatch
{
    std::optional<bool> followPersonalization;
    std::optional<std::string> presetId;
    std::optional<int> backgroundColor;
    std::optional<int> borderColor;
    std::optional<float> backgroundOpacity;
    std::optional<float> borderOpacity;
    std::optional<PanelBorderStyle> borderStyle;
    std::optional<float> borderWidth;
    std::optional<float> borderEffectStrength;
    std::optional<float> gradientEndOpacity;
    std::optional<bool> glassEnabled;
    std::optional<bool> acrylicEnabled;
    std::optional<int> contentTheme;
    bool clearContentTheme = false;

    [[nodiscard]] bool Empty() const noexcept
    {
        return !followPersonalization && !presetId && !backgroundColor &&
            !borderColor && !backgroundOpacity && !borderOpacity &&
            !borderStyle && !borderWidth && !borderEffectStrength &&
            !gradientEndOpacity && !glassEnabled && !acrylicEnabled &&
            !contentTheme && !clearContentTheme;
    }

    bool operator==(const WidgetHostAppearancePatch&) const = default;
};

/** State for values that must never be exposed as ordinary storage strings. */
struct WidgetSettingOpaqueState
{
    bool configured = false;
    bool available = false;
    bool canChoose = false;
    bool canClear = false;
    std::string displayLabel;

    bool operator==(const WidgetSettingOpaqueState&) const = default;
};

struct WidgetSettingFieldState
{
    WidgetSettingFieldSchema schema;
    InteractionValue currentValue;
    InteractionValue defaultValue;
    bool hasStoredValue = false;
    bool visible = true;
    bool enabled = true;
    bool valid = true;
    std::string validationError;
    std::string diagnosticCode;
    std::string searchQuery;
    WidgetSettingOpaqueState opaque;

    bool operator==(const WidgetSettingFieldState&) const = default;
};

struct WidgetSettingsSnapshot
{
    std::wstring widgetId;
    std::string packageId;
    std::string widgetName;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    bool preview = false;
    bool customStyle = false;
    std::vector<WidgetSettingFieldState> fields;
    std::vector<WidgetSettingGroupSchema> groups;
    std::vector<WidgetSettingPresetSchema> presets;
    std::string defaultPresetId;
    WidgetHostAppearanceState hostAppearance;

    bool operator==(const WidgetSettingsSnapshot&) const = default;
};

struct WidgetSettingMutationGuard
{
    std::wstring widgetId;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return !widgetId.empty() && generation != 0 && revision != 0;
    }

    [[nodiscard]] bool Matches(
        const WidgetSettingsSnapshot& snapshot) const noexcept
    {
        return IsValid() && widgetId == snapshot.widgetId &&
            generation == snapshot.generation &&
            revision == snapshot.revision;
    }

    static WidgetSettingMutationGuard FromSnapshot(
        const WidgetSettingsSnapshot& snapshot)
    {
        return { snapshot.widgetId, snapshot.generation,
            snapshot.revision };
    }
};

enum class WidgetSettingMutationStatus
{
    Applied,
    Unchanged,
    Started,
    Cancelled,
    WidgetNotFound,
    SettingNotFound,
    PresetNotFound,
    StaleSnapshot,
    WrongValueChannel,
    InvalidValue,
    Disabled,
    Unavailable,
    PersistenceFailed,
    Failed,
};

struct WidgetSettingMutationResult
{
    WidgetSettingMutationStatus status = WidgetSettingMutationStatus::Failed;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::string errorCode;
    std::string message;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == WidgetSettingMutationStatus::Applied ||
            status == WidgetSettingMutationStatus::Unchanged ||
            status == WidgetSettingMutationStatus::Started ||
            status == WidgetSettingMutationStatus::Cancelled;
    }

    [[nodiscard]] bool Changed() const noexcept
    {
        return status == WidgetSettingMutationStatus::Applied;
    }
};

struct WidgetSettingSearchResult
{
    std::string id;
    std::string title;
    std::string source;
    std::string type;

    bool operator==(const WidgetSettingSearchResult&) const = default;
};

struct WidgetSettingSearchSnapshot
{
    std::wstring widgetId;
    std::string settingKey;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
    std::string query;
    bool pending = false;
    bool completed = false;
    std::string errorCode;
    std::vector<WidgetSettingSearchResult> results;

    bool operator==(const WidgetSettingSearchSnapshot&) const = default;
};

inline WidgetSettingKind WidgetSettingKindFromType(
    std::string_view type) noexcept
{
    if (type == "text") return WidgetSettingKind::Text;
    if (type == "password") return WidgetSettingKind::Password;
    if (type == "bool") return WidgetSettingKind::Boolean;
    if (type == "int") return WidgetSettingKind::Integer;
    if (type == "float") return WidgetSettingKind::FloatingPoint;
    if (type == "range") return WidgetSettingKind::Range;
    if (type == "color") return WidgetSettingKind::Color;
    if (type == "select") return WidgetSettingKind::Select;
    if (type == "multiSelect") return WidgetSettingKind::MultiSelect;
    if (type == "url") return WidgetSettingKind::Url;
    if (type == "date") return WidgetSettingKind::Date;
    if (type == "time") return WidgetSettingKind::Time;
    if (type == "appSearch") return WidgetSettingKind::AppSearch;
    if (type == "appReference") return WidgetSettingKind::AppReference;
    if (type == "desktopItemReference")
        return WidgetSettingKind::DesktopItemReference;
    if (type == "fileReference") return WidgetSettingKind::FileReference;
    if (type == "folderReference") return WidgetSettingKind::FolderReference;
    if (type == "fileHandle") return WidgetSettingKind::FileHandle;
    if (type == "folderHandle") return WidgetSettingKind::FolderHandle;
    return WidgetSettingKind::Unknown;
}

inline WidgetSettingValueChannel WidgetSettingChannelForKind(
    WidgetSettingKind kind) noexcept
{
    if (kind == WidgetSettingKind::Password)
        return WidgetSettingValueChannel::Secret;
    if (kind == WidgetSettingKind::FileHandle ||
        kind == WidgetSettingKind::FolderHandle)
        return WidgetSettingValueChannel::FilesystemHandle;
    if (kind == WidgetSettingKind::AppReference ||
        kind == WidgetSettingKind::DesktopItemReference ||
        kind == WidgetSettingKind::FileReference ||
        kind == WidgetSettingKind::FolderReference)
        return WidgetSettingValueChannel::EntityReference;
    return WidgetSettingValueChannel::Ordinary;
}

inline bool WidgetSettingUsesTypedStorage(WidgetSettingKind kind) noexcept
{
    return kind == WidgetSettingKind::Range ||
        kind == WidgetSettingKind::MultiSelect;
}

inline WidgetSettingKind WidgetSettingFieldSchema::Kind() const noexcept
{
    return WidgetSettingKindFromType(rawType);
}

inline WidgetSettingValueChannel
WidgetSettingFieldSchema::Channel() const noexcept
{
    return WidgetSettingChannelForKind(Kind());
}

inline WidgetSettingDiagnostic
WidgetSettingFieldSchema::Diagnostic() const noexcept
{
    return Kind() == WidgetSettingKind::Unknown
        ? WidgetSettingDiagnostic::UnknownTypeTextFallback
        : WidgetSettingDiagnostic::None;
}

inline std::string_view
WidgetSettingFieldSchema::DiagnosticCode() const noexcept
{
    return Diagnostic() == WidgetSettingDiagnostic::UnknownTypeTextFallback
        ? std::string_view("unknownSettingTypeTextFallback")
        : std::string_view{};
}

inline std::string FormatWidgetSettingNumber(double value)
{
    if (value == 0.0) value = 0.0;
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
        << value;
    return output.str();
}

inline InteractionValue MakeWidgetSettingString(std::string value)
{
    InteractionValue result;
    result.type = InteractionValue::Type::String;
    result.string = std::move(value);
    return result;
}

inline InteractionValue MakeWidgetSettingBoolean(bool value)
{
    InteractionValue result;
    result.type = InteractionValue::Type::Boolean;
    result.boolean = value;
    return result;
}

inline InteractionValue MakeWidgetSettingInteger(long long value)
{
    InteractionValue result;
    result.type = InteractionValue::Type::Integer;
    result.integer = value;
    return result;
}

inline InteractionValue MakeWidgetSettingNumber(double value)
{
    InteractionValue result;
    result.type = InteractionValue::Type::Number;
    result.number = value;
    return result;
}

inline InteractionValue MakeWidgetSettingStringArray(
    const std::vector<std::string>& values)
{
    InteractionValue result;
    result.type = InteractionValue::Type::Array;
    result.array.reserve(values.size());
    for (const auto& value : values)
        result.array.push_back(MakeWidgetSettingString(value));
    return result;
}

inline bool ReadWidgetSettingInteger(std::string_view encoded,
    long long& value) noexcept
{
    if (encoded.empty()) return false;
    const char* begin = encoded.data();
    const char* end = begin + encoded.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

inline bool ReadWidgetSettingInteger(double encoded,
    long long& value) noexcept
{
    if (!std::isfinite(encoded) || std::trunc(encoded) != encoded)
        return false;
    char buffer[512]{};
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer),
        encoded, std::chars_format::fixed, 0);
    if (written.ec != std::errc{}) return false;
    return ReadWidgetSettingInteger(
        std::string_view(buffer, static_cast<std::size_t>(
            written.ptr - buffer)), value);
}

inline bool NormalizeWidgetSettingValue(
    const WidgetSettingFieldSchema& schema,
    const InteractionValue& input, InteractionValue& output,
    std::string& errorCode)
{
    errorCode.clear();
    if (schema.Channel() != WidgetSettingValueChannel::Ordinary)
    {
        errorCode = "wrongValueChannel";
        return false;
    }

    const auto fail = [&](std::string value) {
        errorCode = std::move(value);
        return false;
    };
    switch (schema.Kind())
    {
    case WidgetSettingKind::Boolean:
        if (input.type != InteractionValue::Type::Boolean)
            return fail("expectedBoolean");
        output = MakeWidgetSettingBoolean(input.boolean);
        if (schema.required && !input.boolean)
            return fail("required");
        return true;
    case WidgetSettingKind::Integer:
    {
        long long value = 0;
        if (input.type == InteractionValue::Type::Integer)
            value = input.integer;
        else if (input.type == InteractionValue::Type::Number &&
            ReadWidgetSettingInteger(input.number, value))
        {
        }
        else return fail("expectedInteger");
        if (static_cast<double>(value) < schema.minimum ||
            static_cast<double>(value) > schema.maximum)
            return fail("outOfRange");
        output = MakeWidgetSettingInteger(value);
        return true;
    }
    case WidgetSettingKind::Color:
    {
        if (input.type != InteractionValue::Type::Integer)
            return fail("expectedColor");
        if (input.integer < 0 || input.integer > 0xFFFFFF)
            return fail("outOfRange");
        output = MakeWidgetSettingInteger(input.integer);
        return true;
    }
    case WidgetSettingKind::FloatingPoint:
    case WidgetSettingKind::Range:
    {
        const WidgetSettingKind kind = schema.Kind();
        const double value = input.type == InteractionValue::Type::Number
            ? input.number
            : (input.type == InteractionValue::Type::Integer
                ? static_cast<double>(input.integer)
                : std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(value)) return fail("expectedNumber");
        if (kind == WidgetSettingKind::FloatingPoint &&
            (value < schema.minimum || value > schema.maximum))
            return fail("outOfRange");
        output = MakeWidgetSettingNumber(
            kind == WidgetSettingKind::Range
                ? SnapRangeSettingValue(value, schema.minimum,
                    schema.maximum, schema.step)
                : value);
        return true;
    }
    case WidgetSettingKind::MultiSelect:
    {
        if (input.type != InteractionValue::Type::Array)
            return fail("expectedStringArray");
        std::vector<std::string> selected;
        selected.reserve(input.array.size());
        for (const auto& item : input.array)
        {
            if (item.type != InteractionValue::Type::String)
                return fail("expectedStringArray");
            selected.push_back(item.string);
        }
        std::vector<std::string> options;
        options.reserve(schema.options.size());
        for (const auto& option : schema.options)
            options.push_back(option.value);
        if (!IsValidMultiSelectSettingValue(options, selected))
            return fail("invalidSelection");
        std::vector<std::string> ordered;
        ordered.reserve(selected.size());
        for (const auto& option : options)
            if (std::find(selected.begin(), selected.end(), option) !=
                selected.end())
                ordered.push_back(option);
        if (schema.required && ordered.empty()) return fail("required");
        output = MakeWidgetSettingStringArray(ordered);
        return true;
    }
    default:
        if (input.type != InteractionValue::Type::String)
            return fail("expectedString");
        if (!ValidateSettingTextValue(input.string, schema.required,
                schema.minimumLength, schema.maximumLength))
            return fail(input.string.empty() ? "required" : "invalidLength");
        if (schema.Kind() == WidgetSettingKind::Url &&
            !IsValidUrlSettingValue(input.string))
            return fail("invalidUrl");
        if (schema.Kind() == WidgetSettingKind::Date &&
            !IsValidDateSettingValue(input.string))
            return fail("invalidDate");
        if (schema.Kind() == WidgetSettingKind::Time &&
            !IsValidTimeSettingValue(input.string))
            return fail("invalidTime");
        if (schema.Kind() == WidgetSettingKind::Select &&
            std::none_of(schema.options.begin(), schema.options.end(),
                [&](const WidgetSettingOption& option) {
                    return option.value == input.string;
                }))
            return fail("invalidSelection");
        output = MakeWidgetSettingString(input.string);
        return true;
    }
}

inline bool DecodeLegacyWidgetSettingValue(
    const WidgetSettingFieldSchema& schema, std::string_view encoded,
    InteractionValue& output, std::string& errorCode)
{
    errorCode.clear();
    if (schema.Channel() != WidgetSettingValueChannel::Ordinary)
    {
        errorCode = "wrongValueChannel";
        return false;
    }
    const WidgetSettingKind kind = schema.Kind();
    if (WidgetSettingUsesTypedStorage(kind))
    {
        errorCode = "typedStorageRequired";
        return false;
    }

    InteractionValue decoded;
    switch (kind)
    {
    case WidgetSettingKind::Boolean:
        if (encoded == "1" || encoded == "true")
            decoded = MakeWidgetSettingBoolean(true);
        else if (encoded == "0" || encoded == "false")
            decoded = MakeWidgetSettingBoolean(false);
        else
        {
            errorCode = "invalidBooleanEncoding";
            return false;
        }
        break;
    case WidgetSettingKind::Integer:
    case WidgetSettingKind::Color:
    {
        long long value = 0;
        if (!ReadWidgetSettingInteger(encoded, value))
        {
            errorCode = "invalidIntegerEncoding";
            return false;
        }
        decoded = MakeWidgetSettingInteger(value);
        break;
    }
    case WidgetSettingKind::FloatingPoint:
    {
        double value = 0.0;
        if (!ParseFiniteSettingNumber(encoded, value))
        {
            errorCode = "invalidNumberEncoding";
            return false;
        }
        decoded = MakeWidgetSettingNumber(value);
        break;
    }
    default:
        decoded = MakeWidgetSettingString(std::string(encoded));
        break;
    }
    return NormalizeWidgetSettingValue(
        schema, decoded, output, errorCode);
}

inline bool EncodeLegacyWidgetSettingValue(
    const WidgetSettingFieldSchema& schema,
    const InteractionValue& value, std::string& output,
    std::string& errorCode)
{
    output.clear();
    errorCode.clear();
    if (schema.Channel() != WidgetSettingValueChannel::Ordinary)
    {
        errorCode = "wrongValueChannel";
        return false;
    }
    const WidgetSettingKind kind = schema.Kind();
    if (WidgetSettingUsesTypedStorage(kind))
    {
        errorCode = "typedStorageRequired";
        return false;
    }

    InteractionValue normalized;
    if (!NormalizeWidgetSettingValue(
            schema, value, normalized, errorCode))
        return false;
    switch (kind)
    {
    case WidgetSettingKind::Boolean:
        output = normalized.boolean ? "1" : "0";
        return true;
    case WidgetSettingKind::Integer:
    case WidgetSettingKind::Color:
        output = std::to_string(normalized.integer);
        return true;
    case WidgetSettingKind::FloatingPoint:
        output = FormatWidgetSettingNumber(normalized.number);
        return true;
    default:
        output = normalized.string;
        return true;
    }
}

inline std::vector<std::string> WidgetSettingConditionValues(
    const WidgetSettingFieldState& state)
{
    if (state.schema.Channel() != WidgetSettingValueChannel::Ordinary)
        return state.opaque.configured
            ? std::vector<std::string>{ "1" }
            : std::vector<std::string>{};
    const auto& value = state.currentValue;
    switch (value.type)
    {
    case InteractionValue::Type::Boolean:
        return { value.boolean ? "1" : "0" };
    case InteractionValue::Type::Integer:
        return { std::to_string(value.integer) };
    case InteractionValue::Type::Number:
        return { FormatWidgetSettingNumber(
            state.schema.Kind() == WidgetSettingKind::Range
                ? SnapRangeSettingValue(value.number, state.schema.minimum,
                    state.schema.maximum, state.schema.step)
                : value.number) };
    case InteractionValue::Type::String:
        return value.string.empty()
            ? std::vector<std::string>{}
            : std::vector<std::string>{ value.string };
    case InteractionValue::Type::Array:
    {
        std::vector<std::string> result;
        for (const auto& item : value.array)
            if (item.type == InteractionValue::Type::String)
                result.push_back(item.string);
        return result;
    }
    default:
        return {};
    }
}

inline void ScrubOpaqueWidgetSettingFieldState(
    WidgetSettingFieldState& field)
{
    if (field.schema.Channel() == WidgetSettingValueChannel::Ordinary)
        return;
    field.currentValue = {};
    field.defaultValue = {};
    field.hasStoredValue = false;
    if (field.schema.Kind() == WidgetSettingKind::Password)
        field.opaque.displayLabel.clear();
}

inline void ResolveWidgetSettingFieldStates(
    std::vector<WidgetSettingFieldState>& fields)
{
    std::unordered_map<std::string_view, WidgetSettingFieldState*> byKey;
    byKey.reserve(fields.size());
    for (auto& field : fields)
    {
        ScrubOpaqueWidgetSettingFieldState(field);
        field.diagnosticCode = field.schema.DiagnosticCode();
        byKey.emplace(field.schema.key, &field);
    }

    const auto evaluate = [&](const WidgetSettingCondition& condition) {
        const auto source = byKey.find(condition.key);
        if (source == byKey.end()) return false;
        std::vector<std::string> expected = condition.values;
        if (source->second->schema.Kind() == WidgetSettingKind::Range)
        {
            for (auto& value : expected)
            {
                double parsed = source->second->schema.minimum;
                if (ParseFiniteSettingNumber(value, parsed))
                    value = FormatWidgetSettingNumber(SnapRangeSettingValue(
                        parsed, source->second->schema.minimum,
                        source->second->schema.maximum,
                        source->second->schema.step));
            }
        }
        return EvaluateSettingCondition(condition.operation,
            WidgetSettingConditionValues(*source->second), expected);
    };

    for (auto& field : fields)
    {
        field.visible = !field.schema.showWhen || evaluate(*field.schema.showWhen);
        field.enabled = !field.schema.enabledWhen ||
            evaluate(*field.schema.enabledWhen);
        if (field.enabled && !field.schema.dependsOn.empty())
        {
            WidgetSettingCondition dependency;
            dependency.key = field.schema.dependsOn;
            dependency.operation = "truthy";
            field.enabled = evaluate(dependency);
        }

        field.validationError.clear();
        if (field.schema.Channel() != WidgetSettingValueChannel::Ordinary)
        {
            field.valid = !field.schema.required || field.opaque.configured;
            if (!field.valid) field.validationError = "required";
            continue;
        }
        InteractionValue normalized;
        field.valid = NormalizeWidgetSettingValue(field.schema,
            field.currentValue, normalized, field.validationError);
        if (field.valid) field.currentValue = std::move(normalized);
    }
}

inline bool NormalizeWidgetSettingPreset(
    const std::vector<WidgetSettingFieldSchema>& fields,
    const WidgetSettingPresetSchema& input,
    WidgetSettingPresetSchema& output, std::string& errorCode)
{
    WidgetSettingPresetSchema normalized;
    normalized.id = input.id;
    normalized.label = input.label;
    normalized.hostAppearanceValues = input.hostAppearanceValues;
    normalized.isDefault = input.isDefault;
    errorCode.clear();

    for (const auto& [key, value] : input.values)
    {
        const auto field = std::find_if(fields.begin(), fields.end(),
            [&](const WidgetSettingFieldSchema& candidate) {
                return candidate.key == key;
            });
        if (field == fields.end())
        {
            errorCode = "settingNotFound";
            return false;
        }
        if (field->Channel() != WidgetSettingValueChannel::Ordinary)
        {
            // The legacy editor skipped secret, handle, and logical-reference
            // entries while still applying the preset's ordinary values.
            continue;
        }
        InteractionValue valueNormalized;
        if (!NormalizeWidgetSettingValue(
                *field, value, valueNormalized, errorCode))
            return false;
        normalized.values.emplace(key, std::move(valueNormalized));
    }
    output = std::move(normalized);
    return true;
}

inline void ScrubOpaqueWidgetSettingPresetValues(
    const std::vector<WidgetSettingFieldState>& fields,
    std::vector<WidgetSettingPresetSchema>& presets)
{
    for (auto& preset : presets)
    {
        for (auto value = preset.values.begin();
            value != preset.values.end();)
        {
            const auto field = std::find_if(fields.begin(), fields.end(),
                [&](const WidgetSettingFieldState& candidate) {
                    return candidate.schema.key == value->first;
                });
            if (field != fields.end() &&
                field->schema.Channel() !=
                    WidgetSettingValueChannel::Ordinary)
                value = preset.values.erase(value);
            else
                ++value;
        }
    }
}

inline void PrepareWidgetSettingsSnapshot(
    WidgetSettingsSnapshot& snapshot)
{
    ResolveWidgetSettingFieldStates(snapshot.fields);
    ScrubOpaqueWidgetSettingPresetValues(
        snapshot.fields, snapshot.presets);
}

/**
 * Per-instance monotonic revision publisher. Content hashes are deliberately
 * not used: publishing A, B, and A again must produce three distinct guards.
 */
class WidgetSettingsRevisionSource
{
public:
    WidgetSettingsRevisionSource(
        std::wstring widgetId, std::uint64_t generation)
        : widgetId_(std::move(widgetId)), generation_(generation)
    {
        if (widgetId_.empty() || generation_ == 0)
            throw std::invalid_argument(
                "widget settings revision source requires an instance and generation");
    }

    WidgetSettingsRevisionSource(const WidgetSettingsRevisionSource&) = delete;
    WidgetSettingsRevisionSource& operator=(
        const WidgetSettingsRevisionSource&) = delete;
    WidgetSettingsRevisionSource(WidgetSettingsRevisionSource&&) = delete;
    WidgetSettingsRevisionSource& operator=(
        WidgetSettingsRevisionSource&&) = delete;

    [[nodiscard]] WidgetSettingMutationGuard Publish(
        WidgetSettingsSnapshot& snapshot)
    {
        if (snapshot.widgetId != widgetId_ ||
            snapshot.generation != generation_)
            throw std::invalid_argument(
                "widget settings snapshot identity does not match its revision source");
        if (revision_ == (std::numeric_limits<std::uint64_t>::max)())
            throw std::overflow_error(
                "widget settings revision counter exhausted");
        PrepareWidgetSettingsSnapshot(snapshot);
        snapshot.revision = ++revision_;
        return { widgetId_, generation_, revision_ };
    }

    [[nodiscard]] std::uint64_t CurrentRevision() const noexcept
    {
        return revision_;
    }

private:
    std::wstring widgetId_;
    std::uint64_t generation_ = 0;
    std::uint64_t revision_ = 0;
};
}
