#include "widget_engine_settings_backend.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

namespace
{
using namespace snowdesktop::widget_runtime;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return stream
        ? std::string(std::istreambuf_iterator<char>(stream), {})
        : std::string{};
}
}

int main(int argc, char* argv[])
{
    using namespace snowdesktop::widget_runtime;
    namespace detail = widget_settings_backend_detail;

    LuaWidgetManifest::Setting range;
    range.key = "scale";
    range.label = "Scale";
    range.description = "Scale description";
    range.group = "appearance";
    range.validationMessage = "Invalid scale";
    range.type = "range";
    range.defaultValue = "4.6";
    range.minValue = 0.0;
    range.maxValue = 10.0;
    range.stepValue = 0.5;
    range.required = true;
    range.dependsOn = "enabled";
    range.showWhen = LuaWidgetManifest::SettingCondition{
        "mode", "equals", { "advanced" } };
    const auto convertedRange = detail::ConvertSetting(range);
    Check(convertedRange.schema.key == "scale" &&
            convertedRange.schema.description == "Scale description" &&
            convertedRange.schema.group == "appearance" &&
            convertedRange.schema.validationMessage == "Invalid scale" &&
            convertedRange.schema.Kind() == WidgetSettingKind::Range &&
            convertedRange.schema.showWhen &&
            convertedRange.schema.showWhen->key == "mode" &&
            convertedRange.schema.dependsOn == "enabled" &&
            convertedRange.defaultValue.type ==
                InteractionValue::Type::Number &&
            convertedRange.defaultValue.number == 4.5,
        "engine declarations map complete range schema and typed defaults");

    LuaWidgetManifest::Setting choices;
    choices.key = "feeds";
    choices.type = "multiSelect";
    choices.options = { "news", "weather" };
    choices.optionLabels = { "News", "Weather" };
    choices.defaultValues = { "weather" };
    const auto convertedChoices = detail::ConvertSetting(choices);
    Check(convertedChoices.schema.options.size() == 2 &&
            convertedChoices.schema.options[1].label == "Weather" &&
            convertedChoices.defaultValue.type ==
                InteractionValue::Type::Array &&
            convertedChoices.defaultValue.array.size() == 1 &&
            convertedChoices.defaultValue.array[0].string == "weather",
        "multi-select declarations retain option labels and array defaults");

    LuaWidgetManifest::Setting appSearch;
    appSearch.key = "selectedApp";
    appSearch.type = "appSearch";
    appSearch.searchKey = "appQuery";
    const auto convertedSearch = detail::ConvertSetting(appSearch);
    Check(convertedSearch.schema.Kind() == WidgetSettingKind::AppSearch &&
            convertedSearch.schema.searchKey == "appQuery",
        "appSearch declarations retain their companion query storage key");

    detail::DecodedSearchQueryStorage decodedQuery;
    std::string error;
    Check(detail::DecodeSearchQueryStorage(
              std::optional<std::string_view>("legacy query"),
              std::nullopt, decodedQuery, error) &&
            decodedQuery.hasStoredValue &&
            decodedQuery.value == "legacy query",
        "appSearch reads legacy untyped companion strings");
    std::string typedQuery;
    Check(EncodeTypedStorageValue(
              MakeWidgetSettingString("typed query"), typedQuery, error) &&
            detail::DecodeSearchQueryStorage(
                std::optional<std::string_view>(typedQuery),
                std::optional<std::string_view>(TypedStorageMarker),
                decodedQuery, error) &&
            decodedQuery.hasStoredValue &&
            decodedQuery.value == "typed query",
        "appSearch migrates historical typed string companions when reading");
    std::string typedNumber;
    Check(EncodeTypedStorageValue(
              MakeWidgetSettingNumber(4), typedNumber, error) &&
            !detail::DecodeSearchQueryStorage(
                std::optional<std::string_view>(typedNumber),
                std::optional<std::string_view>(TypedStorageMarker),
                decodedQuery, error) &&
            !error.empty() &&
            !detail::DecodeSearchQueryStorage(
                std::nullopt,
                std::optional<std::string_view>(TypedStorageMarker),
                decodedQuery, error),
        "appSearch rejects non-string typed companions and orphan markers");

    for (const std::string type : { "password", "fileHandle",
            "folderHandle", "appReference", "desktopItemReference",
            "fileReference", "folderReference" })
    {
        LuaWidgetManifest::Setting opaque;
        opaque.key = "opaque";
        opaque.type = type;
        opaque.defaultValue = "must-not-escape";
        opaque.defaultValues = { "must-not-escape" };
        const auto converted = detail::ConvertSetting(opaque);
        Check(converted.defaultValue.type == InteractionValue::Type::Null &&
                converted.schema.Channel() !=
                    WidgetSettingValueChannel::Ordinary,
            "opaque declaration defaults are scrubbed during mapping");
    }

    WidgetSettingOrdinaryWrite rangeWrite{
        "scale", MakeWidgetSettingNumber(6.24), true };
    detail::EncodedOrdinaryWrite encodedRange;
    Check(detail::EncodeOrdinaryWrite(convertedRange.schema,
                rangeWrite, encodedRange, error) &&
            encodedRange.typedMarker && !encodedRange.value.empty(),
        "range writes require a typed marker and typed payload");
    InteractionValue decodedRange;
    Check(DecodeTypedStorageValue(encodedRange.value,
                decodedRange, error) &&
            decodedRange.type == InteractionValue::Type::Number &&
            decodedRange.number == 6.0,
        "range typed payload round trips its normalized value");
    rangeWrite.typedStorage = false;
    Check(!detail::EncodeOrdinaryWrite(convertedRange.schema,
                rangeWrite, encodedRange, error) &&
            error == "typedStorageRequired",
        "range cannot degrade to legacy string storage");

    WidgetSettingOrdinaryWrite multiWrite{
        "feeds", MakeWidgetSettingStringArray({ "weather", "news" }),
        true };
    detail::EncodedOrdinaryWrite encodedMulti;
    Check(detail::EncodeOrdinaryWrite(convertedChoices.schema,
                multiWrite, encodedMulti, error) &&
            encodedMulti.typedMarker &&
            DecodeTypedStorageValue(encodedMulti.value,
                decodedRange, error) &&
            decodedRange.type == InteractionValue::Type::Array &&
            decodedRange.array[0].string == "news" &&
            decodedRange.array[1].string == "weather",
        "multi-select payload remains typed and follows schema order");

    LuaWidgetManifest::Setting boolean;
    boolean.key = "enabled";
    boolean.type = "bool";
    const auto convertedBoolean = detail::ConvertSetting(boolean);
    detail::EncodedOrdinaryWrite encodedBoolean;
    Check(detail::EncodeOrdinaryWrite(convertedBoolean.schema,
                { "enabled", MakeWidgetSettingBoolean(true), false },
                encodedBoolean, error) &&
            !encodedBoolean.typedMarker && encodedBoolean.value == "1",
        "ordinary booleans use the strict legacy codec without a marker");

    WidgetSettingFieldState secret;
    secret.schema.key = "token";
    secret.schema.rawType = "password";
    secret.currentValue = MakeWidgetSettingString("plaintext");
    secret.defaultValue = MakeWidgetSettingString("plaintext-default");
    secret.hasStoredValue = true;
    secret.opaque.configured = true;
    secret.opaque.displayLabel = "plaintext-label";
    std::vector<WidgetSettingFieldState> fields{ secret };
    ResolveWidgetSettingFieldStates(fields);
    Check(fields[0].currentValue.type == InteractionValue::Type::Null &&
            fields[0].defaultValue.type == InteractionValue::Type::Null &&
            !fields[0].hasStoredValue &&
            fields[0].opaque.displayLabel.empty(),
        "opaque snapshots expose configuration state without value material");

    WidgetSettingsBackendDescriptor descriptor;
    descriptor.widgetId = L"instance";
    descriptor.packageId = "package";
    descriptor.generation = 7;
    WidgetSettingMutationGuard guard{ L"instance", 7, 3 };
    Check(detail::DescriptorMatchesCurrent(descriptor, L"instance",
                "package", 7) &&
            detail::MutationIdentityMatches(descriptor, guard,
                L"instance", "package", 7) &&
            !detail::MutationIdentityMatches(descriptor, guard,
                L"instance", "package", 8),
        "runtime-token generation changes invalidate old mutation guards");

    WidgetSettingSearchRequest expected{
        L"instance", "package", "application", "calc", 7, 11, 8 };
    WidgetSettingSearchRequest same = expected;
    WidgetSettingSearchRequest staleGeneration = expected;
    staleGeneration.generation = 8;
    WidgetSettingSearchRequest staleRequest = expected;
    staleRequest.requestId = 12;
    Check(detail::SearchIdentityMatches(expected, same) &&
            !detail::SearchIdentityMatches(expected, staleGeneration) &&
            !detail::SearchIdentityMatches(expected, staleRequest),
        "search identity rejects old generations and superseded requests");

    Check(argc >= 2,
        "repository root is supplied for backend source contracts");
    if (argc >= 2)
    {
        const std::filesystem::path root(argv[1]);
        const std::string source = ReadFile(
            root / "src" / "widget_engine_settings_backend.cpp");
        Check(!source.empty(), "backend source is readable for contracts");
        Check(source.find("RuntimeSetStorageValue") == std::string::npos,
            "backend never routes settings through ordinary runtime writes");
        Check(source.find("WidgetStorageTransaction") != std::string::npos &&
                source.find("ApplyHostAppearanceTransaction") !=
                    std::string::npos &&
                source.find("if (IsHostAppearancePresetKey(key))") !=
                    std::string::npos &&
                source.find("hostAppearanceValues.emplace(key, encoded)") !=
                    std::string::npos &&
                source.find("followPersonalization") != std::string::npos &&
                source.find("__preset") != std::string::npos &&
                source.find("__contentTheme") != std::string::npos &&
                source.find("gradientEndA") != std::string::npos &&
                source.find("borderStyle") != std::string::npos &&
                source.find("borderWidth") != std::string::npos &&
                source.find("borderEffectStrength") != std::string::npos &&
                source.find("invalidBorderStyle") != std::string::npos &&
                source.find("invalidBorderWidth") != std::string::npos &&
                source.find("glassBlurRadius") != std::string::npos &&
                source.find("glassEnabled") != std::string::npos &&
                source.find("acrylicEnabled") != std::string::npos &&
                source.find("PersistWidgetSettingsStorageForBackend") !=
                    std::string::npos &&
                source.find("TypedStorageMetadataKey") !=
                    std::string::npos &&
                source.find("ReadSearchQuery") != std::string::npos &&
                source.find("write.searchQuery") != std::string::npos &&
                source.find("write.searchQuery = request.query") !=
                    std::string::npos &&
                source.find("writeSchema.required = false") !=
                    std::string::npos &&
                source.find("transaction.Remove(converted.schema.searchKey") !=
                    std::string::npos &&
                source.find("TypedStorageMetadataKey(converted.schema.searchKey)") !=
                    std::string::npos &&
                source.find("searchQueryMetadataRejected") !=
                    std::string::npos &&
                source.find("WidgetSecretStore") != std::string::npos &&
                source.find("filesystemHandleStore_->Grant") !=
                    std::string::npos &&
                source.find("RuntimeBindHostLogicalSlot") !=
                    std::string::npos,
            "source keeps host appearance transactional and typed, secret, handle, and reference channels distinct");
        Check(source.find(
                  "PreviewHostAppearanceTransaction") !=
                    std::string::npos &&
                source.find("if (!persist)") != std::string::npos &&
                source.find("preview_->originals.try_emplace") !=
                    std::string::npos &&
                source.find("CommitPreview(") != std::string::npos &&
                source.find("RevertPreview(") != std::string::npos &&
                source.find(
                    "PersistWidgetSettingsStorageForBackend()") !=
                    std::string::npos &&
                source.find("storage.insert_or_assign(key, *value)") !=
                    std::string::npos &&
                source.find("storage.erase(key)") != std::string::npos &&
                source.find("previewCommitRequired") !=
                    std::string::npos,
            "transient previews touch live storage without saving, then commit or restore only guarded touched keys");
    }

    if (failures == 0)
    {
        std::cout << "widget engine settings backend checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " widget engine settings backend check(s) failed\n";
    return EXIT_FAILURE;
}
