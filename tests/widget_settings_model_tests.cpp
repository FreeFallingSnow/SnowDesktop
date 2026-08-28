#include "widget_settings_model.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
}

int main()
{
    using namespace snowdesktop::widget_runtime;

    const std::vector<std::pair<std::string_view, WidgetSettingKind>> kinds = {
        { "text", WidgetSettingKind::Text },
        { "password", WidgetSettingKind::Password },
        { "bool", WidgetSettingKind::Boolean },
        { "int", WidgetSettingKind::Integer },
        { "float", WidgetSettingKind::FloatingPoint },
        { "range", WidgetSettingKind::Range },
        { "color", WidgetSettingKind::Color },
        { "select", WidgetSettingKind::Select },
        { "multiSelect", WidgetSettingKind::MultiSelect },
        { "url", WidgetSettingKind::Url },
        { "date", WidgetSettingKind::Date },
        { "time", WidgetSettingKind::Time },
        { "appSearch", WidgetSettingKind::AppSearch },
        { "appReference", WidgetSettingKind::AppReference },
        { "desktopItemReference", WidgetSettingKind::DesktopItemReference },
        { "fileReference", WidgetSettingKind::FileReference },
        { "folderReference", WidgetSettingKind::FolderReference },
        { "fileHandle", WidgetSettingKind::FileHandle },
        { "folderHandle", WidgetSettingKind::FolderHandle },
    };
    bool allKindsMapped = true;
    for (const auto& [type, expected] : kinds)
        allKindsMapped = allKindsMapped &&
            WidgetSettingKindFromType(type) == expected;
    Check(allKindsMapped,
        "every v2 setting type maps to a backend-neutral kind");

    WidgetSettingFieldSchema derived;
    derived.rawType = "password";
    Check(derived.Kind() == WidgetSettingKind::Password &&
            derived.Channel() == WidgetSettingValueChannel::Secret,
        "password kind and channel are derived from the raw type");
    derived.rawType = "fileHandle";
    Check(derived.Kind() == WidgetSettingKind::FileHandle &&
            derived.Channel() ==
                WidgetSettingValueChannel::FilesystemHandle,
        "filesystem handles cannot retain an ordinary value channel");
    derived.rawType = "appReference";
    Check(derived.Kind() == WidgetSettingKind::AppReference &&
            derived.Channel() == WidgetSettingValueChannel::EntityReference,
        "entity references cannot retain an ordinary value channel");
    derived.rawType = "text";
    Check(derived.Kind() == WidgetSettingKind::Text &&
            derived.Channel() == WidgetSettingValueChannel::Ordinary,
        "ordinary fields derive the ordinary channel");

    WidgetSettingFieldSchema unknown;
    unknown.key = "future";
    unknown.rawType = "futureControl";
    InteractionValue normalized;
    std::string error;
    Check(unknown.Kind() == WidgetSettingKind::Unknown &&
            unknown.Channel() == WidgetSettingValueChannel::Ordinary &&
            unknown.Diagnostic() ==
                WidgetSettingDiagnostic::UnknownTypeTextFallback &&
            unknown.DiagnosticCode() ==
                "unknownSettingTypeTextFallback" &&
            NormalizeWidgetSettingValue(unknown,
                MakeWidgetSettingString("preserved"), normalized, error) &&
            normalized.string == "preserved",
        "unknown types use a diagnosed text fallback");

    WidgetSettingFieldSchema range;
    range.key = "scale";
    range.rawType = "range";
    range.minimum = 0.0;
    range.maximum = 10.0;
    range.step = 0.5;
    Check(NormalizeWidgetSettingValue(range,
                MakeWidgetSettingNumber(4.6), normalized, error) &&
            normalized.type == InteractionValue::Type::Number &&
            normalized.number == 4.5 && error.empty(),
        "range mutations snap independently of a UI toolkit");
    Check(NormalizeWidgetSettingValue(range,
                MakeWidgetSettingNumber(12.0), normalized, error) &&
            normalized.number == 10.0,
        "range mutations clamp values above their maximum");
    Check(NormalizeWidgetSettingValue(range,
                MakeWidgetSettingNumber(-3.0), normalized, error) &&
            normalized.number == 0.0,
        "range mutations clamp values below their minimum");

    WidgetSettingFieldSchema integer;
    integer.key = "count";
    integer.rawType = "int";
    integer.minimum = -100.0;
    integer.maximum = 100.0;
    Check(NormalizeWidgetSettingValue(integer,
                MakeWidgetSettingNumber(42.0), normalized, error) &&
            normalized.type == InteractionValue::Type::Integer &&
            normalized.integer == 42,
        "exact in-range floating values safely normalize to integers");
    integer.minimum = -(std::numeric_limits<double>::max)();
    integer.maximum = (std::numeric_limits<double>::max)();
    Check(!NormalizeWidgetSettingValue(integer,
                MakeWidgetSettingNumber(9223372036854775808.0),
                normalized, error) && error == "expectedInteger",
        "a double at 2^63 is rejected before conversion to long long");

    WidgetSettingFieldSchema choices;
    choices.key = "feeds";
    choices.rawType = "multiSelect";
    choices.options = {
        { "news", "News" }, { "weather", "Weather" },
        { "media", "Media" } };
    Check(NormalizeWidgetSettingValue(choices,
                MakeWidgetSettingStringArray({ "media", "news" }),
                normalized, error) && normalized.array.size() == 2 &&
            normalized.array[0].string == "news" &&
            normalized.array[1].string == "media",
        "multi-select values retain schema order instead of frontend order");

    WidgetSettingFieldSchema boolean;
    boolean.key = "enabled";
    boolean.rawType = "bool";
    InteractionValue decoded;
    std::string encoded;
    Check(DecodeLegacyWidgetSettingValue(
                boolean, "true", decoded, error) &&
            decoded.type == InteractionValue::Type::Boolean &&
            decoded.boolean &&
            EncodeLegacyWidgetSettingValue(
                boolean, decoded, encoded, error) && encoded == "1",
        "legacy boolean storage has a strict canonical round trip");
    Check(!DecodeLegacyWidgetSettingValue(
                boolean, "yes", decoded, error) &&
            error == "invalidBooleanEncoding",
        "legacy boolean storage rejects unknown encodings");
    Check(!DecodeLegacyWidgetSettingValue(
                integer, "12garbage", decoded, error) &&
            error == "invalidIntegerEncoding",
        "legacy integer storage rejects partial parses");

    WidgetSettingFieldSchema floating;
    floating.key = "opacity";
    floating.rawType = "float";
    floating.minimum = 0.0;
    floating.maximum = 1.0;
    const double precise = 0.12345678901234566;
    Check(EncodeLegacyWidgetSettingValue(floating,
                MakeWidgetSettingNumber(precise), encoded, error) &&
            DecodeLegacyWidgetSettingValue(
                floating, encoded, decoded, error) &&
            decoded.type == InteractionValue::Type::Number &&
            decoded.number == precise,
        "legacy floating-point storage preserves round-trip precision");
    Check(!EncodeLegacyWidgetSettingValue(range,
                MakeWidgetSettingNumber(4.5), encoded, error) &&
            error == "typedStorageRequired" &&
            !DecodeLegacyWidgetSettingValue(
                choices, "news", decoded, error) &&
            error == "typedStorageRequired",
        "range and multi-select reject the legacy string codec");

    WidgetSettingFieldState secret;
    secret.schema.key = "token";
    secret.schema.rawType = "password";
    secret.schema.required = true;
    secret.currentValue = MakeWidgetSettingString("plaintext");
    secret.defaultValue = MakeWidgetSettingString("default-secret");
    secret.hasStoredValue = true;
    secret.opaque.configured = false;
    secret.opaque.displayLabel = "plaintext";

    WidgetSettingFieldState dependent;
    dependent.schema.key = "endpoint";
    dependent.schema.rawType = "url";
    dependent.schema.dependsOn = "token";
    dependent.currentValue =
        MakeWidgetSettingString("https://example.com");
    dependent.defaultValue = dependent.currentValue;

    std::vector<WidgetSettingFieldState> fields = { secret, dependent };
    ResolveWidgetSettingFieldStates(fields);
    Check(!fields[0].valid && fields[0].validationError == "required" &&
            !fields[1].enabled &&
            fields[0].currentValue.type == InteractionValue::Type::Null &&
            fields[0].defaultValue.type == InteractionValue::Type::Null &&
            !fields[0].hasStoredValue &&
            fields[0].opaque.displayLabel.empty(),
        "opaque fields are scrubbed before validation and dependency evaluation");
    fields[0].opaque.configured = true;
    ResolveWidgetSettingFieldStates(fields);
    Check(fields[0].valid && fields[1].enabled &&
            fields[0].currentValue.type == InteractionValue::Type::Null,
        "configured secrets enable dependents without exposing their value");
    Check(!NormalizeWidgetSettingValue(fields[0].schema,
                MakeWidgetSettingString("secret"), normalized, error) &&
            error == "wrongValueChannel" &&
            !EncodeLegacyWidgetSettingValue(fields[0].schema,
                MakeWidgetSettingString("secret"), encoded, error) &&
            error == "wrongValueChannel" &&
            !DecodeLegacyWidgetSettingValue(fields[0].schema,
                "secret", decoded, error) &&
            error == "wrongValueChannel",
        "opaque settings reject ordinary normalization and legacy codecs");

    WidgetSettingFieldSchema presetRange = range;
    WidgetSettingFieldSchema presetSecret = fields[0].schema;
    std::vector<WidgetSettingFieldSchema> presetFields = {
        presetRange, presetSecret };
    WidgetSettingPresetSchema preset;
    preset.id = "large";
    preset.label = "Large";
    preset.values.emplace("scale", MakeWidgetSettingNumber(20.0));
    WidgetSettingPresetSchema presetNormalized;
    Check(NormalizeWidgetSettingPreset(presetFields, preset,
                presetNormalized, error) &&
            presetNormalized.values.at("scale").number == 10.0,
        "ordinary typed preset values are normalized by their schema");
    preset.values.emplace("token", MakeWidgetSettingString("leak"));
    preset.hostAppearanceValues.emplace("glassBlurRadius", "18.5");
    Check(NormalizeWidgetSettingPreset(presetFields, preset,
                presetNormalized, error) &&
            presetNormalized.values.contains("scale") &&
            !presetNormalized.values.contains("token") &&
            presetNormalized.hostAppearanceValues ==
                preset.hostAppearanceValues,
        "opaque preset entries are skipped while ordinary and host-owned preset values still apply");

    WidgetSettingsSnapshot snapshot;
    snapshot.widgetId = L"weather-instance";
    snapshot.generation = 7;
    snapshot.fields = fields;
    WidgetSettingPresetSchema unsafePreset;
    unsafePreset.id = "unsafe";
    unsafePreset.label = "Unsafe";
    unsafePreset.values.emplace(
        "token", MakeWidgetSettingString("must-be-removed"));
    unsafePreset.values.emplace(
        "endpoint", MakeWidgetSettingString("https://example.com"));
    snapshot.presets.push_back(std::move(unsafePreset));

    WidgetSettingsRevisionSource revisions(snapshot.widgetId,
        snapshot.generation);
    const WidgetSettingMutationGuard firstGuard = revisions.Publish(snapshot);
    const std::uint64_t firstRevision = snapshot.revision;
    Check(firstRevision == 1 && firstGuard.Matches(snapshot) &&
            snapshot.presets[0].values.count("token") == 0 &&
            snapshot.presets[0].values.count("endpoint") == 1,
        "publishing produces a bound guard and scrubs opaque preset values");

    snapshot.fields[1].currentValue =
        MakeWidgetSettingString("https://example.org");
    const WidgetSettingMutationGuard secondGuard = revisions.Publish(snapshot);
    const std::uint64_t secondRevision = snapshot.revision;
    snapshot.fields[1].currentValue =
        MakeWidgetSettingString("https://example.com");
    const WidgetSettingMutationGuard thirdGuard = revisions.Publish(snapshot);
    Check(firstRevision < secondRevision &&
            secondRevision < snapshot.revision &&
            !firstGuard.Matches(snapshot) &&
            !secondGuard.Matches(snapshot) && thirdGuard.Matches(snapshot),
        "monotonic revisions reject stale guards even after an ABA value change");

    WidgetSettingsSnapshot otherWidget = snapshot;
    otherWidget.widgetId = L"other-instance";
    Check(!thirdGuard.Matches(otherWidget) &&
            !WidgetSettingMutationGuard{}.Matches(snapshot),
        "mutation guards require matching widget identity and nonzero tokens");
    bool rejectedWrongPublisher = false;
    try
    {
        (void)revisions.Publish(otherWidget);
    }
    catch (const std::invalid_argument&)
    {
        rejectedWrongPublisher = true;
    }
    Check(rejectedWrongPublisher,
        "a revision source cannot publish a snapshot for another widget");

    if (failures != 0)
    {
        std::cerr << failures << " widget settings model checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget settings model checks passed\n";
    return EXIT_SUCCESS;
}
