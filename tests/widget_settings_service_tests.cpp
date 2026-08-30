#include "widget_settings_service.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

WidgetSettingsBackendResult Ok(bool changed = true)
{
    return { changed ? WidgetSettingsBackendStatus::Succeeded
                     : WidgetSettingsBackendStatus::Unchanged,
        {}, {} };
}

WidgetSettingSourceField Field(
    std::string key, std::string type, InteractionValue defaultValue)
{
    WidgetSettingSourceField result;
    result.schema.key = std::move(key);
    result.schema.label = result.schema.key + " label";
    result.schema.rawType = std::move(type);
    result.defaultValue = std::move(defaultValue);
    return result;
}

const WidgetSettingFieldState* Find(
    const WidgetSettingsSnapshot& snapshot, std::string_view key)
{
    for (const auto& field : snapshot.fields)
        if (field.schema.key == key) return &field;
    return nullptr;
}

class FakeBackend final : public IWidgetSettingsBackend
{
public:
    WidgetSettingsBackendDescriptor descriptor;
    std::map<std::string, InteractionValue, std::less<>> ordinary;
    std::map<std::string, std::string, std::less<>> searchQueries;
    std::map<std::string, InteractionValue, std::less<>> persistedOrdinary;
    std::map<std::string, std::string, std::less<>> persistedSearchQueries;
    std::map<std::string, WidgetSettingOpaqueState, std::less<>> opaque;
    std::vector<WidgetSettingOrdinaryWrite> lastWrites;
    std::vector<std::vector<WidgetSettingOrdinaryWrite>> transactions;
    WidgetHostAppearancePatch lastAppearancePatch;
    std::vector<WidgetHostAppearancePatch> appearanceTransactions;
    std::vector<WidgetSettingSearchRequest> searches;
    std::vector<WidgetSettingSearchRequest> cancellations;
    std::vector<SearchCompletion> searchCallbacks;
    WidgetSettingSearchRequest committedSearch;
    std::string committedResult;
    std::string lastOpaqueKey;
    std::string lastSecretLength;
    std::wstring lastOwnerWidget;
    std::string lastOwnerPackage;
    WidgetSettingsBackendResult mutationResult = Ok();
    bool describeAvailable = true;
    bool ordinaryAvailable = true;
    bool opaqueAvailable = true;
    bool searchAvailable = true;
    int ordinaryCalls = 0;
    int secretCalls = 0;
    int handleCalls = 0;
    int referenceCalls = 0;
    int previewCalls = 0;
    int persistCalls = 0;
    int revertCalls = 0;
    bool previewActive = false;
    bool applyingPreview = false;
    std::map<std::string, InteractionValue, std::less<>> previewOrdinary;
    std::map<std::string, std::string, std::less<>> previewSearchQueries;
    WidgetHostAppearanceState previewAppearance;
    WidgetHostAppearanceState persistedAppearance;
    std::vector<std::string> mutationOrder;

    const WidgetSettingFieldSchema* FindSchema(
        std::string_view key) const
    {
        for (const auto* fields : { &descriptor.manifestFields,
                 &descriptor.scriptFields })
            for (const auto& field : *fields)
                if (field.schema.key == key) return &field.schema;
        return nullptr;
    }

    WidgetSettingsBackendResult Describe(std::wstring_view widgetId,
        WidgetSettingsBackendDescriptor& output) override
    {
        if (!describeAvailable || widgetId != descriptor.widgetId)
            return { WidgetSettingsBackendStatus::WidgetNotFound,
                "widgetNotFound", {} };
        output = descriptor;
        return Ok(false);
    }

    WidgetSettingBackendReadResult ReadOrdinary(
        const WidgetSettingsBackendDescriptor&,
        const WidgetSettingFieldSchema& field,
        bool) override
    {
        ++ordinaryCalls;
        if (!ordinaryAvailable)
            return { { WidgetSettingsBackendStatus::Unavailable,
                         "ordinaryStorageUnavailable", {} },
                false, {} };
        const auto value = ordinary.find(field.key);
        if (value == ordinary.end()) return { Ok(false), false, {} };
        return { Ok(false), true, value->second };
    }

    WidgetSettingBackendReadResult ReadSearchQuery(
        const WidgetSettingsBackendDescriptor&,
        const WidgetSettingFieldSchema& field) override
    {
        const auto value = searchQueries.find(field.searchKey);
        return value == searchQueries.end()
            ? WidgetSettingBackendReadResult{ Ok(false), false, {} }
            : WidgetSettingBackendReadResult{ Ok(false), true,
                MakeWidgetSettingString(value->second) };
    }

    WidgetSettingBackendOpaqueResult ReadOpaque(
        const WidgetSettingsBackendDescriptor&,
        const WidgetSettingFieldSchema& field) override
    {
        if (!opaqueAvailable)
            return { { WidgetSettingsBackendStatus::Unavailable,
                         "opaqueStoreUnavailable", {} },
                {} };
        const auto value = opaque.find(field.key);
        return { Ok(false), value == opaque.end()
                ? WidgetSettingOpaqueState{}
                : value->second };
    }

    WidgetSettingsBackendResult ApplyOrdinaryTransaction(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const std::vector<WidgetSettingOrdinaryWrite>& writes) override
    {
        return ApplyHostAppearanceTransaction(widget, guard, {}, writes);
    }

    WidgetSettingsBackendResult ApplyHostAppearanceTransaction(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard&,
        const WidgetHostAppearancePatch& appearance,
        const std::vector<WidgetSettingOrdinaryWrite>& writes) override
    {
        ++ordinaryCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastWrites = writes;
        transactions.push_back(writes);
        lastAppearancePatch = appearance;
        appearanceTransactions.push_back(appearance);
        if (!mutationResult.Succeeded()) return mutationResult;
        if (appearance.followPersonalization)
            descriptor.hostAppearance.followPersonalization =
                *appearance.followPersonalization;
        if (appearance.presetId)
            descriptor.hostAppearance.presetId = *appearance.presetId;
        if (appearance.backgroundColor)
            descriptor.hostAppearance.backgroundColor =
                *appearance.backgroundColor;
        if (appearance.borderColor)
            descriptor.hostAppearance.borderColor = *appearance.borderColor;
        if (appearance.backgroundOpacity)
            descriptor.hostAppearance.backgroundOpacity =
                *appearance.backgroundOpacity;
        if (appearance.borderOpacity)
            descriptor.hostAppearance.borderOpacity =
                *appearance.borderOpacity;
        if (appearance.borderWidth)
            descriptor.hostAppearance.borderWidth = *appearance.borderWidth;
        if (appearance.edgeHighlightEnabled)
            descriptor.hostAppearance.edgeHighlightEnabled =
                *appearance.edgeHighlightEnabled;
        if (appearance.edgeHighlightWidth)
            descriptor.hostAppearance.edgeHighlightWidth =
                *appearance.edgeHighlightWidth;
        if (appearance.edgeHighlightStrength)
            descriptor.hostAppearance.edgeHighlightStrength =
                *appearance.edgeHighlightStrength;
        if (appearance.gradientEndOpacity)
            descriptor.hostAppearance.gradientEndOpacity =
                *appearance.gradientEndOpacity;
        if (appearance.glassEnabled)
            descriptor.hostAppearance.glassEnabled =
                *appearance.glassEnabled;
        if (appearance.acrylicEnabled)
            descriptor.hostAppearance.acrylicEnabled =
                *appearance.acrylicEnabled;
        if (appearance.contentTheme)
            descriptor.hostAppearance.contentTheme =
                *appearance.contentTheme;
        for (const auto& write : writes)
        {
            ordinary.insert_or_assign(write.key, write.value);
            if (write.searchQuery)
            {
                const auto* schema = FindSchema(write.key);
                if (schema && !schema->searchKey.empty())
                {
                    if (write.searchQuery->empty())
                        searchQueries.erase(schema->searchKey);
                    else
                        searchQueries.insert_or_assign(
                            schema->searchKey, *write.searchQuery);
                }
            }
        }
        if (!applyingPreview && mutationResult.Succeeded())
        {
            ++persistCalls;
            persistedOrdinary = ordinary;
            persistedSearchQueries = searchQueries;
            persistedAppearance = descriptor.hostAppearance;
            mutationOrder.push_back("persist");
        }
        return mutationResult;
    }

    WidgetSettingsBackendResult PreviewHostAppearanceTransaction(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetHostAppearancePatch& appearance,
        const std::vector<WidgetSettingOrdinaryWrite>& writes) override
    {
        if (guard.widgetId != descriptor.widgetId ||
            guard.generation != descriptor.generation ||
            widget.widgetId != descriptor.widgetId ||
            widget.packageId != descriptor.packageId ||
            widget.generation != descriptor.generation)
            return { WidgetSettingsBackendStatus::StaleSnapshot,
                "staleSnapshot", {} };
        if (!previewActive)
        {
            previewOrdinary = ordinary;
            previewSearchQueries = searchQueries;
            previewAppearance = descriptor.hostAppearance;
            previewActive = true;
        }
        ++previewCalls;
        mutationOrder.push_back("preview");
        applyingPreview = true;
        const auto result = ApplyHostAppearanceTransaction(
            widget, guard, appearance, writes);
        applyingPreview = false;
        return result;
    }

    WidgetSettingsBackendResult CommitPreview(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard) override
    {
        if (guard.widgetId != descriptor.widgetId ||
            guard.generation != descriptor.generation ||
            widget.widgetId != descriptor.widgetId ||
            widget.packageId != descriptor.packageId ||
            widget.generation != descriptor.generation)
            return { WidgetSettingsBackendStatus::StaleSnapshot,
                "staleSnapshot", {} };
        if (!previewActive) return Ok(false);
        ++persistCalls;
        persistedOrdinary = ordinary;
        persistedSearchQueries = searchQueries;
        persistedAppearance = descriptor.hostAppearance;
        previewActive = false;
        mutationOrder.push_back("commitPreview");
        return Ok();
    }

    WidgetSettingsBackendResult RevertPreview(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard) override
    {
        if (guard.widgetId != descriptor.widgetId ||
            guard.generation != descriptor.generation ||
            widget.widgetId != descriptor.widgetId ||
            widget.packageId != descriptor.packageId ||
            widget.generation != descriptor.generation)
            return { WidgetSettingsBackendStatus::StaleSnapshot,
                "staleSnapshot", {} };
        if (!previewActive) return Ok(false);
        ordinary = previewOrdinary;
        searchQueries = previewSearchQueries;
        descriptor.hostAppearance = previewAppearance;
        previewActive = false;
        ++revertCalls;
        mutationOrder.push_back("revertPreview");
        return Ok();
    }

    WidgetSettingsBackendResult SetSecret(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard&,
        const WidgetSettingFieldSchema& field,
        std::string_view plaintext) override
    {
        ++secretCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastOpaqueKey = field.key;
        lastSecretLength = std::to_string(plaintext.size());
        if (mutationResult.Succeeded())
            opaque[field.key] = { true, true, false, true, {} };
        return mutationResult;
    }

    WidgetSettingsBackendResult ClearSecret(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override
    {
        (void)guard;
        ++secretCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastOpaqueKey = field.key;
        if (mutationResult.Succeeded()) opaque.erase(field.key);
        return mutationResult;
    }

    WidgetSettingsBackendResult ChooseFilesystemHandle(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard&,
        const WidgetSettingFieldSchema& field) override
    {
        ++handleCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastOpaqueKey = field.key;
        if (mutationResult.Succeeded())
            opaque[field.key] = {
                true, true, true, true, "chosen.txt" };
        return mutationResult;
    }

    WidgetSettingsBackendResult ClearFilesystemHandle(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override
    {
        (void)guard;
        ++handleCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastOpaqueKey = field.key;
        if (mutationResult.Succeeded()) opaque.erase(field.key);
        return mutationResult;
    }

    WidgetSettingsBackendResult OpenEntityReferencePicker(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard&,
        const WidgetSettingFieldSchema& field) override
    {
        ++referenceCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastOpaqueKey = field.key;
        if (mutationResult.Succeeded())
            opaque[field.key] = {
                true, true, true, true, "Calculator" };
        return mutationResult;
    }

    WidgetSettingsBackendResult ClearEntityReference(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) override
    {
        (void)guard;
        ++referenceCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastOpaqueKey = field.key;
        if (mutationResult.Succeeded()) opaque.erase(field.key);
        return mutationResult;
    }

    WidgetSettingsBackendResult StartSearch(
        WidgetSettingSearchRequest request,
        SearchCompletion completion) override
    {
        if (!searchAvailable)
            return { WidgetSettingsBackendStatus::Unavailable,
                "searchUnavailable", {} };
        searches.push_back(std::move(request));
        searchCallbacks.push_back(std::move(completion));
        return Ok();
    }

    WidgetSettingsBackendResult CancelSearch(
        const WidgetSettingSearchRequest& request) override
    {
        cancellations.push_back(request);
        return Ok();
    }

    WidgetSettingsBackendResult CommitSearchResult(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingSearchRequest& request,
        std::string_view resultId) override
    {
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        committedSearch = request;
        committedResult = resultId;
        if (widget.preview)
            return { WidgetSettingsBackendStatus::Unavailable,
                "previewReadOnly", {} };
        const auto* schema = FindSchema(request.settingKey);
        if (!schema || schema->Kind() != WidgetSettingKind::AppSearch)
            return { WidgetSettingsBackendStatus::SettingNotFound,
                "settingNotFound", {} };
        WidgetSettingOrdinaryWrite write;
        write.key = request.settingKey;
        write.value = MakeWidgetSettingString("safe visible title");
        write.searchQuery = request.query;
        return ApplyOrdinaryTransaction(widget, guard, { std::move(write) });
    }

    void Complete(std::size_t index,
        std::vector<WidgetSettingSearchResult> results,
        WidgetSettingsBackendResult result = Ok())
    {
        const auto& request = searches.at(index);
        WidgetSettingSearchCompletion completion;
        completion.widgetId = request.widgetId;
        completion.settingKey = request.settingKey;
        completion.generation = request.generation;
        completion.requestId = request.requestId;
        completion.result = std::move(result);
        completion.results = std::move(results);
        searchCallbacks.at(index)(std::move(completion));
    }
};

FakeBackend MakeBackend()
{
    FakeBackend backend;
    backend.descriptor.widgetId = L"widget-1";
    backend.descriptor.packageId = "package.example";
    backend.descriptor.widgetName = "Example";
    backend.descriptor.generation = 7;

    auto enabled = Field("enabled", "bool",
        MakeWidgetSettingBoolean(true));
    WidgetSettingGroupSchema mainGroup;
    mainGroup.id = "main";
    mainGroup.label = "Main";
    enabled.schema.group = mainGroup.id;
    backend.descriptor.manifestGroups.push_back(mainGroup);
    backend.descriptor.manifestFields.push_back(enabled);

    auto scale = Field("scale", "range",
        MakeWidgetSettingNumber(1.0));
    scale.schema.minimum = 0.0;
    scale.schema.maximum = 2.0;
    scale.schema.step = 0.25;
    backend.descriptor.manifestFields.push_back(scale);

    auto password = Field("token", "password", {});
    password.schema.required = false;
    backend.descriptor.manifestFields.push_back(password);
    auto file = Field("file", "fileHandle", {});
    backend.descriptor.manifestFields.push_back(file);
    auto app = Field("app", "appReference", {});
    app.schema.binding = "appBinding";
    backend.descriptor.manifestFields.push_back(app);
    auto search = Field("appSearch", "appSearch",
        MakeWidgetSettingString({}));
    search.schema.searchKey = "appQuery";
    backend.descriptor.manifestFields.push_back(search);

    auto feeds = Field("feeds", "multiSelect",
        MakeWidgetSettingStringArray({ "weather" }));
    feeds.schema.options = {
        { "news", "News" }, { "weather", "Weather" } };
    backend.descriptor.scriptFields.push_back(feeds);
    WidgetSettingGroupSchema advanced;
    advanced.id = "advanced";
    advanced.label = "Advanced";
    backend.descriptor.scriptGroups.push_back(advanced);

    WidgetSettingPresetSchema preset;
    preset.id = "compact";
    preset.label = "Compact";
    preset.values["scale"] = MakeWidgetSettingNumber(0.5);
    preset.values["feeds"] =
        MakeWidgetSettingStringArray({ "news" });
    preset.hostAppearanceValues["bg"] = "1122867";
    backend.descriptor.scriptPresets.push_back(preset);
    return backend;
}
}

int main()
{
    using namespace snowdesktop::widget_runtime;

    FakeBackend backend = MakeBackend();
    backend.ordinary["enabled"] = MakeWidgetSettingBoolean(false);
    backend.ordinary["appSearch"] =
        MakeWidgetSettingString("Calculator");
    backend.searchQueries["appQuery"] = "initial query";
    backend.opaque["token"] = {
        true, true, false, true, "must never escape" };
    WidgetSettingsService service(backend);
    std::vector<WidgetSettingsSnapshotChanged> snapshotEvents;
    std::vector<WidgetSettingSearchCompleted> searchEvents;
    bool snapshotCallbackReentered = false;
    bool searchCallbackReentered = false;
    const auto installRecordingCallbacks = [&]() {
        service.SetEventCallbacks(
            [&](WidgetSettingsSnapshotChanged event) {
                const auto current = service.Snapshot(event.widgetId);
                snapshotCallbackReentered = current &&
                    current->generation == event.generation &&
                    current->revision == event.revision;
                snapshotEvents.push_back(std::move(event));
            },
            [&](WidgetSettingSearchCompleted event) {
                const auto current = service.SearchSnapshot(
                    event.widgetId, event.settingKey);
                searchCallbackReentered = current &&
                    current->generation == event.generation &&
                    current->requestId == event.requestId &&
                    !current->pending;
                searchEvents.push_back(std::move(event));
            });
    };
    installRecordingCallbacks();
    WidgetSettingsLoadResult loaded = service.Load(L"widget-1");
    Check(loaded.Succeeded() && loaded.snapshot &&
            loaded.snapshot->fields.size() == 7 &&
            loaded.snapshot->fields.front().schema.key == "enabled" &&
            loaded.snapshot->fields.back().schema.key == "feeds" &&
            loaded.snapshot->groups.size() == 2 &&
            loaded.snapshot->presets.size() == 1,
        "manifest and script fields, groups, and presets merge in source order");
    Check(snapshotEvents.size() == 1 && snapshotCallbackReentered &&
            snapshotEvents.front().widgetId == L"widget-1" &&
            snapshotEvents.front().generation ==
                loaded.snapshot->generation &&
            snapshotEvents.front().revision == loaded.snapshot->revision,
        "snapshot events run after publication and may reenter Snapshot without deadlock");
    Check(loaded.snapshot && Find(*loaded.snapshot, "enabled") &&
            !Find(*loaded.snapshot, "enabled")->currentValue.boolean &&
            Find(*loaded.snapshot, "enabled")->hasStoredValue,
        "snapshot reads real ordinary backend values instead of authored defaults");
    Check(loaded.snapshot && Find(*loaded.snapshot, "token") &&
            Find(*loaded.snapshot, "token")->currentValue.type ==
                InteractionValue::Type::Null &&
            Find(*loaded.snapshot, "token")->opaque.configured &&
            Find(*loaded.snapshot, "token")->opaque.displayLabel.empty(),
        "password snapshots expose configuration state but no plaintext or label");
    Check(loaded.snapshot && Find(*loaded.snapshot, "appSearch") &&
            Find(*loaded.snapshot, "appSearch")->searchQuery ==
                "initial query",
        "appSearch snapshots read the separate persisted searchKey value");

    FakeBackend languageBackend = MakeBackend();
    languageBackend.ordinary["enabled"] =
        MakeWidgetSettingBoolean(false);
    languageBackend.searchQueries["appQuery"] = "localized query";
    languageBackend.opaque["token"] = {
        true, true, false, true, "must never escape" };
    languageBackend.opaque["file"] = {
        true, false, true, false, "Owned file handle" };
    languageBackend.opaque["app"] = {
        true, false, false, true, "Logical app reference" };
    WidgetSettingsService languageService(languageBackend);
    const auto languageBefore = languageService.Load(L"widget-1");
    auto languageGuard = WidgetSettingMutationGuard::FromSnapshot(
        *languageBefore.snapshot);
    (void)languageService.StartSearch(
        languageGuard, "appSearch", "old generation");
    const std::size_t languageSearchIndex =
        languageBackend.searches.size() - 1;
    languageBackend.descriptor.generation = 8;
    languageBackend.descriptor.widgetName = "Localized widget";
    languageBackend.descriptor.manifestFields.front().schema.label =
        "Localized enabled";
    languageBackend.descriptor.manifestFields.front().schema.description =
        "Localized description";
    languageBackend.descriptor.manifestGroups.front().label =
        "Localized group";
    languageBackend.descriptor.manifestGroups.front().description =
        "Localized group description";
    languageBackend.descriptor.scriptPresets.front().label =
        "Localized preset";
    const auto languageAfter = languageService.Reload(L"widget-1");
    languageBackend.Complete(languageSearchIndex,
        { { "stale", "Stale", "apps", "application" } });
    const auto* localizedEnabled = languageAfter.snapshot
        ? Find(*languageAfter.snapshot, "enabled") : nullptr;
    const auto* localizedSecret = languageAfter.snapshot
        ? Find(*languageAfter.snapshot, "token") : nullptr;
    const auto* localizedFile = languageAfter.snapshot
        ? Find(*languageAfter.snapshot, "file") : nullptr;
    const auto* localizedReference = languageAfter.snapshot
        ? Find(*languageAfter.snapshot, "app") : nullptr;
    const auto* localizedSearch = languageAfter.snapshot
        ? Find(*languageAfter.snapshot, "appSearch") : nullptr;
    Check(languageAfter.Succeeded() && languageAfter.snapshot &&
            languageAfter.snapshot->generation == 8 &&
            languageAfter.snapshot->widgetName == "Localized widget" &&
            localizedEnabled &&
            localizedEnabled->schema.label == "Localized enabled" &&
            localizedEnabled->schema.description ==
                "Localized description" &&
            !localizedEnabled->currentValue.boolean &&
            languageAfter.snapshot->groups.front().label ==
                "Localized group" &&
            languageAfter.snapshot->groups.front().description ==
                "Localized group description" &&
            languageAfter.snapshot->presets.front().label ==
                "Localized preset" &&
            localizedSecret && localizedSecret->opaque.configured &&
            localizedSecret->currentValue.type ==
                InteractionValue::Type::Null &&
            localizedSecret->opaque.displayLabel.empty() &&
            localizedFile && localizedFile->opaque.configured &&
            localizedFile->currentValue.type ==
                InteractionValue::Type::Null &&
            localizedReference && localizedReference->opaque.configured &&
            localizedReference->currentValue.type ==
                InteractionValue::Type::Null &&
            localizedSearch && localizedSearch->searchQuery ==
                "localized query" &&
            !languageService.SearchSnapshot(L"widget-1", "appSearch"),
        "language-generation reload updates labels, groups and presets while preserving values and opaque channels and rejecting old search results");

    FakeBackend transientBackend = MakeBackend();
    transientBackend.ordinary["scale"] =
        MakeWidgetSettingNumber(1.0);
    transientBackend.persistedOrdinary = transientBackend.ordinary;
    transientBackend.descriptor.customStyle = true;
    transientBackend.persistedAppearance =
        transientBackend.descriptor.hostAppearance;
    WidgetSettingsService transientService(transientBackend);
    auto transientLoad = transientService.Load(L"widget-1");
    const auto originalTransientGuard =
        WidgetSettingMutationGuard::FromSnapshot(*transientLoad.snapshot);
    const auto transientPreview = transientService.PreviewOrdinary(
        originalTransientGuard, "scale", MakeWidgetSettingNumber(1.5));
    auto transientSnapshot = transientService.Snapshot(L"widget-1");
    Check(transientPreview.status == WidgetSettingMutationStatus::Applied &&
            transientSnapshot &&
            Find(*transientSnapshot, "scale")->currentValue.number == 1.5 &&
            transientBackend.ordinary["scale"].number == 1.5 &&
            transientBackend.persistedOrdinary["scale"].number == 1.0 &&
            transientBackend.persistCalls == 0 &&
            transientBackend.previewActive,
        "numeric preview is visible in the live snapshot without persisting storage");
    const auto stalePreviewDecision = transientService.RevertPreview(
        originalTransientGuard);
    Check(stalePreviewDecision.status ==
                WidgetSettingMutationStatus::StaleSnapshot &&
            transientBackend.previewActive &&
            transientBackend.ordinary["scale"].number == 1.5,
        "an old preview guard cannot commit or revert a newer revision");
    auto transientGuard = WidgetSettingMutationGuard::FromSnapshot(
        *transientSnapshot);
    const auto transientCommit =
        transientService.CommitPreview(transientGuard);
    Check(transientCommit.status == WidgetSettingMutationStatus::Applied &&
            !transientBackend.previewActive &&
            transientBackend.persistCalls == 1 &&
            transientBackend.persistedOrdinary["scale"].number == 1.5 &&
            transientBackend.mutationOrder ==
                std::vector<std::string>{"preview", "commitPreview"},
        "preview commit atomically persists the live value after preview publication");

    transientSnapshot = transientService.Snapshot(L"widget-1");
    transientGuard = WidgetSettingMutationGuard::FromSnapshot(
        *transientSnapshot);
    WidgetHostAppearancePatch transientAppearance;
    transientAppearance.backgroundColor = 0x123456;
    const auto appearancePreview =
        transientService.PreviewHostAppearance(
            transientGuard, transientAppearance);
    transientSnapshot = transientService.Snapshot(L"widget-1");
    transientGuard = WidgetSettingMutationGuard::FromSnapshot(
        *transientSnapshot);
    const auto appearanceRevert =
        transientService.RevertPreview(transientGuard);
    transientSnapshot = transientService.Snapshot(L"widget-1");
    Check(appearancePreview.status == WidgetSettingMutationStatus::Applied &&
            appearanceRevert.status == WidgetSettingMutationStatus::Applied &&
            transientSnapshot &&
            transientSnapshot->hostAppearance.backgroundColor ==
                transientBackend.persistedAppearance.backgroundColor &&
            transientBackend.persistCalls == 1 &&
            transientBackend.revertCalls == 1,
        "appearance preview can be reverted without a persistent write");

    transientGuard = WidgetSettingMutationGuard::FromSnapshot(
        *transientSnapshot);
    (void)transientService.PreviewOrdinary(transientGuard, "scale",
        MakeWidgetSettingNumber(0.5));
    transientService.Close(L"widget-1");
    Check(!transientBackend.previewActive &&
            transientBackend.ordinary["scale"].number == 1.5 &&
            transientBackend.persistedOrdinary["scale"].number == 1.5 &&
            transientBackend.mutationOrder.back() == "revertPreview",
        "closing an unflushed service session reverts its transient preview");

    WidgetSettingMutationGuard guard =
        WidgetSettingMutationGuard::FromSnapshot(*loaded.snapshot);
    WidgetHostAppearancePatch unavailableAppearance;
    unavailableAppearance.backgroundColor = 0x123456;
    Check(service.UpdateHostAppearance(
                guard, unavailableAppearance).status ==
                WidgetSettingMutationStatus::Unavailable &&
            backend.appearanceTransactions.empty(),
        "direct host appearance editing remains unavailable when customStyle is disabled");
    WidgetSettingMutationResult range = service.SetOrdinary(
        guard, "scale", MakeWidgetSettingNumber(1.62));
    Check(range.status == WidgetSettingMutationStatus::Applied &&
            backend.lastWrites.size() == 1 &&
            backend.lastWrites[0].key == "scale" &&
            backend.lastWrites[0].typedStorage &&
            backend.lastWrites[0].value.number == 1.5,
        "range values are normalized and require atomic typed storage");
    Check(service.SetOrdinary(guard, "enabled",
                MakeWidgetSettingBoolean(true)).status ==
                WidgetSettingMutationStatus::StaleSnapshot,
        "a successful mutation invalidates the previous revision guard");

    auto snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    WidgetSettingMutationResult secret =
        service.SetSecret(guard, "token", "super-secret");
    Check(secret.status == WidgetSettingMutationStatus::Applied &&
            backend.secretCalls == 1 && backend.ordinaryCalls > 0 &&
            backend.lastOpaqueKey == "token" &&
            backend.lastSecretLength == "12" &&
            !backend.ordinary.contains("token"),
        "password mutations use only the secret channel");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    WidgetSettingMutationResult handle =
        service.ChooseFilesystemHandle(guard, "file");
    snapshot = service.Snapshot(L"widget-1");
    Check(handle.status == WidgetSettingMutationStatus::Applied &&
            backend.handleCalls == 1 &&
            backend.lastOwnerWidget == L"widget-1" &&
            backend.lastOwnerPackage == "package.example" &&
            Find(*snapshot, "file")->opaque.displayLabel == "chosen.txt" &&
            !backend.ordinary.contains("file"),
        "filesystem picker mutations remain owner scoped and opaque");

    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    WidgetSettingMutationResult reference =
        service.OpenEntityReferencePicker(guard, "app");
    snapshot = service.Snapshot(L"widget-1");
    Check(reference.status == WidgetSettingMutationStatus::Applied &&
            backend.referenceCalls == 1 &&
            Find(*snapshot, "app")->opaque.displayLabel == "Calculator" &&
            !backend.ordinary.contains("app"),
        "logical references expose only a host-provided display label");

    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    const std::size_t transactionsBeforePreset =
        backend.transactions.size();
    WidgetSettingMutationResult preset =
        service.ApplyPreset(guard, "compact");
    Check(preset.status == WidgetSettingMutationStatus::Applied &&
            backend.transactions.size() == transactionsBeforePreset + 1 &&
            backend.lastWrites.size() == 2 &&
            backend.lastWrites[0].typedStorage &&
            backend.lastWrites[1].typedStorage &&
            backend.lastAppearancePatch.presetId == "compact" &&
            backend.lastAppearancePatch.backgroundColor == 1122867 &&
            backend.descriptor.hostAppearance.presetId == "compact",
        "preset selection is one transaction, persists __preset and legacy host values immediately, and preserves typed values");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    WidgetSettingMutationResult reset = service.Reset(guard);
    bool resetHasOpaque = false;
    bool resetTypedRange = false;
    bool resetTypedMulti = false;
    bool resetSearchQuery = false;
    for (const auto& write : backend.lastWrites)
    {
        resetHasOpaque = resetHasOpaque || write.key == "token" ||
            write.key == "file" || write.key == "app";
        resetTypedRange = resetTypedRange ||
            (write.key == "scale" && write.typedStorage);
        resetTypedMulti = resetTypedMulti ||
            (write.key == "feeds" && write.typedStorage);
        resetSearchQuery = resetSearchQuery ||
            (write.key == "appSearch" && write.searchQuery &&
                write.searchQuery->empty());
    }
    Check(reset.status == WidgetSettingMutationStatus::Applied &&
            !resetHasOpaque && resetTypedRange && resetTypedMulti &&
            resetSearchQuery,
        "reset is one ordinary transaction, clears appSearch searchKey, and never resets opaque channels as strings");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    const std::size_t transactionsBeforeSearchQuery =
        backend.transactions.size();
    WidgetSettingMutationResult queryMutation = service.SetSearchQuery(
        guard, "appSearch", "cal");
    snapshot = service.Snapshot(L"widget-1");
    Check(queryMutation.status == WidgetSettingMutationStatus::Applied &&
            backend.transactions.size() ==
                transactionsBeforeSearchQuery + 1 &&
            backend.lastWrites.size() == 1 &&
            backend.lastWrites[0].key == "appSearch" &&
            backend.lastWrites[0].searchQuery ==
                std::optional<std::string>("cal") &&
            snapshot &&
            Find(*snapshot, "appSearch")->searchQuery == "cal" &&
            Find(*snapshot, "appSearch")->currentValue.string.empty(),
        "editing appSearch atomically persists searchKey and clears the selected display value");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    const std::size_t transactionsBeforeSameQuery =
        backend.transactions.size();
    Check(service.SetSearchQuery(guard, "appSearch", "cal").status ==
                WidgetSettingMutationStatus::Unchanged &&
            backend.transactions.size() == transactionsBeforeSameQuery,
        "submitting an unchanged appSearch query does not clear or rewrite storage");

    WidgetSettingMutationResult firstSearch = service.StartSearch(
        guard, "appSearch", "cal");
    WidgetSettingMutationResult secondSearch = service.StartSearch(
        guard, "appSearch", "calc");
    const std::size_t searchEventsBeforeCompletion =
        searchEvents.size();
    Check(firstSearch.status == WidgetSettingMutationStatus::Started &&
            secondSearch.status == WidgetSettingMutationStatus::Started &&
            backend.searches.size() == 2 &&
            backend.cancellations.size() == 1,
        "new searches cancel the previous request for the same setting");
    backend.Complete(0, { { "old", "Old", "apps", "application" } });
    auto searchSnapshot = service.SearchSnapshot(
        L"widget-1", "appSearch");
    Check(searchSnapshot && searchSnapshot->pending &&
            searchSnapshot->results.empty() &&
            searchEvents.size() == searchEventsBeforeCompletion,
        "a superseded search completion is discarded by request id");
    backend.Complete(1, {
        { "calculator", "Calculator", "apps", "application" } });
    searchSnapshot = service.SearchSnapshot(L"widget-1", "appSearch");
    Check(searchSnapshot && !searchSnapshot->pending &&
            searchSnapshot->completed &&
            searchSnapshot->results.size() == 1 &&
            searchSnapshot->results[0].id == "calculator" &&
            searchEvents.size() == searchEventsBeforeCompletion + 1 &&
            searchCallbackReentered &&
            searchEvents.back().requestId == searchSnapshot->requestId,
        "the current request publishes a path-free search snapshot");
    backend.Complete(1, {
        { "duplicate", "Duplicate", "apps", "application" } });
    Check(searchEvents.size() == searchEventsBeforeCompletion + 1 &&
            service.SearchSnapshot(L"widget-1", "appSearch") ==
                searchSnapshot,
        "a duplicate completion for a non-pending request is ignored");
    const std::uint64_t searchRequestId = searchSnapshot->requestId;
    Check(service.CommitSearchResult(guard, "appSearch",
                searchRequestId, "missing").status ==
                WidgetSettingMutationStatus::InvalidValue,
        "search commits reject ids absent from the request-scoped result set");
    WidgetSettingMutationResult committed = service.CommitSearchResult(
        guard, "appSearch", searchRequestId, "calculator");
    const auto committedSnapshot = service.Snapshot(L"widget-1");
    Check(committed.status == WidgetSettingMutationStatus::Applied &&
            backend.committedResult == "calculator" &&
            backend.committedSearch.requestId == searchRequestId &&
            backend.lastWrites.size() == 1 &&
            backend.lastWrites[0].searchQuery ==
                std::optional<std::string>("calc") &&
            backend.searchQueries["appQuery"] == "calc" &&
            committedSnapshot &&
            Find(*committedSnapshot, "appSearch")->searchQuery == "calc" &&
            Find(*committedSnapshot, "appSearch")->currentValue.string ==
                "safe visible title",
        "direct search selection commits its request query and opaque result id in one transaction");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    const std::size_t searchEventsBeforeFailure = searchEvents.size();
    (void)service.StartSearch(guard, "appSearch", "unavailable");
    const std::size_t failedSearchIndex = backend.searches.size() - 1;
    backend.Complete(failedSearchIndex, {}, {
        WidgetSettingsBackendStatus::Failed, "catalogFailed", {} });
    auto failedSearch = service.SearchSnapshot(
        L"widget-1", "appSearch");
    Check(failedSearch && !failedSearch->pending &&
            !failedSearch->completed &&
            failedSearch->errorCode == "catalogFailed" &&
            searchEvents.size() == searchEventsBeforeFailure + 1 &&
            searchEvents.back().requestId == failedSearch->requestId,
        "an accepted failed completion publishes one reentrant search hint");

    (void)service.StartSearch(guard, "appSearch", "paint");
    const std::size_t staleIndex = backend.searches.size() - 1;
    backend.descriptor.generation = 8;
    WidgetSettingsLoadResult generationReload = service.Reload(L"widget-1");
    const std::size_t searchEventsBeforeStaleGeneration =
        searchEvents.size();
    backend.Complete(staleIndex,
        { { "paint", "Paint", "apps", "application" } });
    Check(generationReload.Succeeded() && generationReload.snapshot &&
            generationReload.snapshot->generation == 8 &&
            !service.SearchSnapshot(L"widget-1", "appSearch") &&
            backend.cancellations.size() == 2 &&
            searchEvents.size() == searchEventsBeforeStaleGeneration,
        "widget reload cancels replaced work and drops old-generation async results");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    const WidgetSettingMutationGuard guardBeforeClose = guard;
    const std::uint64_t revisionBeforeClose = snapshot->revision;
    (void)service.StartSearch(guard, "appSearch", "close-race");
    const std::size_t closedSearchIndex = backend.searches.size() - 1;
    const std::size_t searchEventsBeforeClose = searchEvents.size();
    service.Close(L"widget-1");
    backend.Complete(closedSearchIndex,
        { { "closed", "Closed", "apps", "application" } });
    Check(!service.Snapshot(L"widget-1") &&
            !service.SearchSnapshot(L"widget-1", "appSearch") &&
            searchEvents.size() == searchEventsBeforeClose,
        "closing removes the session before cancellation and drops delayed completions");

    WidgetSettingsLoadResult reopened = service.Load(L"widget-1");
    Check(reopened.Succeeded() && reopened.snapshot &&
            reopened.snapshot->generation == guardBeforeClose.generation &&
            reopened.snapshot->revision > revisionBeforeClose &&
            service.SetOrdinary(guardBeforeClose, "enabled",
                MakeWidgetSettingBoolean(true)).status ==
                WidgetSettingMutationStatus::StaleSnapshot,
        "Close and Load preserve monotonic per-widget revisions and reject an old guard");

    service.SetEventCallbacks(
        [](WidgetSettingsSnapshotChanged) {
            throw std::runtime_error("snapshot observer failure");
        },
        [](WidgetSettingSearchCompleted) {
            throw std::runtime_error("search observer failure");
        });
    WidgetSettingsLoadResult throwingReload = service.Reload(L"widget-1");
    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    (void)service.StartSearch(guard, "appSearch", "throwing-observer");
    const std::size_t throwingSearchIndex = backend.searches.size() - 1;
    bool observerEscaped = false;
    try
    {
        backend.Complete(throwingSearchIndex,
            { { "safe", "Safe", "apps", "application" } });
    }
    catch (...)
    {
        observerEscaped = true;
    }
    const auto throwingSearch = service.SearchSnapshot(
        L"widget-1", "appSearch");
    Check(throwingReload.Succeeded() && !observerEscaped &&
            throwingSearch && throwingSearch->completed,
        "observer exceptions cannot escape snapshot publication or a backend completion");

    installRecordingCallbacks();
    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    const std::size_t searchEventsBeforeRecovery = searchEvents.size();
    (void)service.StartSearch(guard, "appSearch", "observer-recovery");
    const std::size_t recoverySearchIndex = backend.searches.size() - 1;
    backend.Complete(recoverySearchIndex,
        { { "recovered", "Recovered", "apps", "application" } });
    Check(searchEvents.size() == searchEventsBeforeRecovery + 1 &&
            searchCallbackReentered,
        "a later observer still receives completions after an earlier observer threw");

    service.SetEventCallbacks({}, {});
    const std::size_t snapshotEventsBeforeClear = snapshotEvents.size();
    const std::size_t searchEventsBeforeClear = searchEvents.size();
    WidgetSettingsLoadResult silentReload = service.Reload(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*silentReload.snapshot);
    (void)service.StartSearch(guard, "appSearch", "detached-observer");
    const std::size_t detachedSearchIndex = backend.searches.size() - 1;
    backend.Complete(detachedSearchIndex,
        { { "detached", "Detached", "apps", "application" } });
    Check(snapshotEvents.size() == snapshotEventsBeforeClear &&
            searchEvents.size() == searchEventsBeforeClear,
        "atomically clearing callbacks suppresses later snapshot and search hints");

    FakeBackend unavailable = MakeBackend();
    unavailable.mutationResult = {
        WidgetSettingsBackendStatus::Unavailable,
        "typedStorageUnavailable", {} };
    WidgetSettingsService unavailableService(unavailable);
    auto unavailableLoad = unavailableService.Load(L"widget-1");
    auto unavailableGuard = WidgetSettingMutationGuard::FromSnapshot(
        *unavailableLoad.snapshot);
    WidgetSettingMutationResult unavailableRange =
        unavailableService.SetOrdinary(unavailableGuard, "scale",
            MakeWidgetSettingNumber(1.75));
    Check(unavailableRange.status ==
            WidgetSettingMutationStatus::Unavailable &&
            unavailable.ordinary.find("scale") == unavailable.ordinary.end() &&
            unavailable.lastWrites.size() == 1 &&
            unavailable.lastWrites[0].typedStorage,
        "an unavailable typed backend never degrades a range into a string write");

    FakeBackend invalid = MakeBackend();
    invalid.descriptor.scriptFields.push_back(
        Field("enabled", "text", MakeWidgetSettingString({})));
    WidgetSettingsService invalidService(invalid);
    WidgetSettingsLoadResult invalidLoad = invalidService.Load(L"widget-1");
    Check(!invalidLoad.Succeeded() &&
            invalidLoad.errorCode == "duplicateSettingKey",
        "service rejects ambiguous manifest and script setting merges");

    FakeBackend invalidSearch = MakeBackend();
    for (auto& field : invalidSearch.descriptor.manifestFields)
        if (field.schema.Kind() == WidgetSettingKind::AppSearch)
            field.schema.searchKey.clear();
    WidgetSettingsService invalidSearchService(invalidSearch);
    WidgetSettingsLoadResult invalidSearchLoad =
        invalidSearchService.Load(L"widget-1");
    Check(!invalidSearchLoad.Succeeded() &&
            invalidSearchLoad.errorCode == "invalidSearchKey",
        "service rejects appSearch declarations without a separate searchKey");

    FakeBackend collidingSearch = MakeBackend();
    for (auto& field : collidingSearch.descriptor.manifestFields)
        if (field.schema.Kind() == WidgetSettingKind::AppSearch)
            field.schema.searchKey = "scale";
    WidgetSettingsService collidingSearchService(collidingSearch);
    const WidgetSettingsLoadResult collidingSearchLoad =
        collidingSearchService.Load(L"widget-1");
    Check(!collidingSearchLoad.Succeeded() &&
            collidingSearchLoad.errorCode == "invalidSearchKey",
        "service rejects an appSearch searchKey that aliases any primary setting key");

    FakeBackend sharedSearch = MakeBackend();
    auto secondSearchField = Field("secondAppSearch", "appSearch",
        MakeWidgetSettingString({}));
    secondSearchField.schema.searchKey = "appQuery";
    sharedSearch.descriptor.scriptFields.push_back(
        std::move(secondSearchField));
    WidgetSettingsService sharedSearchService(sharedSearch);
    const WidgetSettingsLoadResult sharedSearchLoad =
        sharedSearchService.Load(L"widget-1");
    Check(!sharedSearchLoad.Succeeded() &&
            sharedSearchLoad.errorCode == "invalidSearchKey",
        "service rejects appSearch declarations that share a companion key");

    FakeBackend reservedSearch = MakeBackend();
    for (auto& field : reservedSearch.descriptor.manifestFields)
        if (field.schema.Kind() == WidgetSettingKind::AppSearch)
            field.schema.searchKey = "__host.typedStorage.scale";
    WidgetSettingsService reservedSearchService(reservedSearch);
    const WidgetSettingsLoadResult reservedSearchLoad =
        reservedSearchService.Load(L"widget-1");
    Check(!reservedSearchLoad.Succeeded() &&
            reservedSearchLoad.errorCode == "invalidSearchKey",
        "service rejects reserved host metadata keys as appSearch companions");

    FakeBackend presetRaceBackend = MakeBackend();
    WidgetSettingsService presetRaceService(presetRaceBackend);
    auto presetRaceLoad = presetRaceService.Load(L"widget-1");
    auto presetRaceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *presetRaceLoad.snapshot);
    (void)presetRaceService.SetSearchQuery(
        presetRaceGuard, "appSearch", "paint");
    auto presetRaceSnapshot = presetRaceService.Snapshot(L"widget-1");
    presetRaceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *presetRaceSnapshot);
    (void)presetRaceService.StartSearch(
        presetRaceGuard, "appSearch", "paint");
    const std::uint64_t presetRequestId =
        presetRaceBackend.searches.back().requestId;
    const std::size_t presetSearchIndex =
        presetRaceBackend.searches.size() - 1;
    const auto presetRaceResult = presetRaceService.ApplyPreset(
        presetRaceGuard, "compact");
    presetRaceSnapshot = presetRaceService.Snapshot(L"widget-1");
    presetRaceBackend.Complete(presetSearchIndex,
        { { "paint", "Paint", "apps", "application" } });
    const auto presetStaleCommit =
        presetRaceService.CommitSearchResult(
            WidgetSettingMutationGuard::FromSnapshot(*presetRaceSnapshot),
            "appSearch", presetRequestId, "paint");
    Check(presetRaceResult.status ==
                WidgetSettingMutationStatus::Applied &&
            presetRaceBackend.cancellations.size() == 1 &&
            !presetRaceService.SearchSnapshot(
                L"widget-1", "appSearch") &&
            presetStaleCommit.status ==
                WidgetSettingMutationStatus::StaleSnapshot,
        "preset application cancels searches even when the preset does not contain appSearch");

    FakeBackend schemaRaceBackend = MakeBackend();
    WidgetSettingsService schemaRaceService(schemaRaceBackend);
    auto schemaRaceLoad = schemaRaceService.Load(L"widget-1");
    auto schemaRaceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *schemaRaceLoad.snapshot);
    (void)schemaRaceService.SetSearchQuery(
        schemaRaceGuard, "appSearch", "calc");
    auto schemaRaceSnapshot = schemaRaceService.Snapshot(L"widget-1");
    schemaRaceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *schemaRaceSnapshot);
    (void)schemaRaceService.StartSearch(
        schemaRaceGuard, "appSearch", "calc");
    const std::size_t schemaSearchIndex =
        schemaRaceBackend.searches.size() - 1;
    for (auto& field : schemaRaceBackend.descriptor.manifestFields)
        if (field.schema.Kind() == WidgetSettingKind::AppSearch)
            field.schema.noResultsLabel = "Nothing here";
    const auto schemaReload = schemaRaceService.Reload(L"widget-1");
    schemaRaceBackend.Complete(schemaSearchIndex,
        { { "calculator", "Calculator", "apps", "application" } });
    Check(schemaReload.Succeeded() &&
            schemaRaceBackend.cancellations.size() == 1 &&
            !schemaRaceService.SearchSnapshot(L"widget-1", "appSearch"),
        "same-generation reload cancels a search when its field schema changes");

    FakeBackend resetRaceBackend = MakeBackend();
    WidgetSettingsService resetRaceService(resetRaceBackend);
    auto resetRaceLoad = resetRaceService.Load(L"widget-1");
    auto resetRaceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *resetRaceLoad.snapshot);
    (void)resetRaceService.SetSearchQuery(
        resetRaceGuard, "appSearch", "calc");
    auto resetRaceSnapshot = resetRaceService.Snapshot(L"widget-1");
    resetRaceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *resetRaceSnapshot);
    const auto resetSearchStarted = resetRaceService.StartSearch(
        resetRaceGuard, "appSearch", "calc");
    const std::uint64_t resetRequestId =
        resetRaceBackend.searches.back().requestId;
    const std::size_t resetSearchIndex =
        resetRaceBackend.searches.size() - 1;
    const auto resetRaceResult = resetRaceService.Reset(resetRaceGuard);
    resetRaceSnapshot = resetRaceService.Snapshot(L"widget-1");
    resetRaceBackend.Complete(resetSearchIndex,
        { { "calculator", "Calculator", "apps", "application" } });
    const auto resetStaleCommit = resetRaceService.CommitSearchResult(
        WidgetSettingMutationGuard::FromSnapshot(*resetRaceSnapshot),
        "appSearch", resetRequestId, "calculator");
    Check(resetSearchStarted.status ==
                WidgetSettingMutationStatus::Started &&
            resetRaceResult.status == WidgetSettingMutationStatus::Applied &&
            resetRaceBackend.cancellations.size() == 1 &&
            !resetRaceBackend.searchQueries.contains("appQuery") &&
            Find(*resetRaceSnapshot, "appSearch")->searchQuery.empty() &&
            !resetRaceService.SearchSnapshot(L"widget-1", "appSearch") &&
            resetStaleCommit.status ==
                WidgetSettingMutationStatus::StaleSnapshot,
        "reset cancels the request and prevents delayed results or old result ids from restoring a selection");

    FakeBackend previewBackend = MakeBackend();
    previewBackend.descriptor.preview = true;
    WidgetSettingsService previewService(previewBackend);
    auto previewLoad = previewService.Load(L"widget-1");
    auto previewGuard = WidgetSettingMutationGuard::FromSnapshot(
        *previewLoad.snapshot);
    const std::size_t previewTransactions =
        previewBackend.transactions.size();
    const auto previewQuery = previewService.SetSearchQuery(
        previewGuard, "appSearch", "calc");
    auto previewSnapshot = previewService.Snapshot(L"widget-1");
    previewGuard = WidgetSettingMutationGuard::FromSnapshot(
        *previewSnapshot);
    const auto previewStarted = previewService.StartSearch(
        previewGuard, "appSearch", "calc");
    const std::size_t previewSearchIndex =
        previewBackend.searches.size() - 1;
    previewBackend.Complete(previewSearchIndex,
        { { "calculator", "Calculator", "apps", "application" } });
    const auto previewSearch = previewService.SearchSnapshot(
        L"widget-1", "appSearch");
    const std::uint64_t previewRequestId =
        previewSearch ? previewSearch->requestId : 0;
    const auto previewCommit = previewService.CommitSearchResult(
        previewGuard, "appSearch", previewRequestId, "calculator");
    previewSnapshot = previewService.Snapshot(L"widget-1");
    Check(previewQuery.status == WidgetSettingMutationStatus::Applied &&
            previewStarted.status == WidgetSettingMutationStatus::Started &&
            previewSearch && previewSearch->completed &&
            previewCommit.status ==
                WidgetSettingMutationStatus::Unavailable &&
            previewCommit.errorCode == "previewReadOnly" &&
            previewBackend.transactions.size() == previewTransactions &&
            previewBackend.searchQueries.empty() && previewSnapshot &&
            Find(*previewSnapshot, "appSearch")->searchQuery == "calc" &&
            Find(*previewSnapshot, "appSearch")->currentValue.string.empty(),
        "preview keeps query edits in the session, can search, and cannot commit a result");

    FakeBackend appearanceBackend = MakeBackend();
    appearanceBackend.descriptor.customStyle = true;
    appearanceBackend.descriptor.hostAppearance = {
        .followPersonalization = false,
        .presetId = "__custom",
        .backgroundColor = 0x102030,
        .borderColor = 0xE0D0C0,
        .backgroundOpacity = 0.25f,
        .borderOpacity = 0.5f,
        .borderWidth = 1.0f,
        .edgeHighlightEnabled = false,
        .edgeHighlightWidth = kDefaultEdgeHighlightWidth,
        .edgeHighlightStrength = kDefaultEdgeHighlightStrength,
        .gradientEndOpacity = 0.75f,
        .glassEnabled = false,
        .acrylicEnabled = false,
        .contentTheme = 0,
    };
    appearanceBackend.descriptor.scriptPresets[0]
        .hostAppearanceValues = {
            { "bg", "1193046" },
            { "alpha", "0.625" },
            { "glassEnabled", "1" },
            { "glassBlurRadius", "18.5" },
            { "shadowBlur", "20" },
            { "followPersonalization", "1" },
            { "__contentTheme", "1" },
            { "__preset", "must-not-win" },
        };
    WidgetSettingPresetSchema explicitBorderPreset;
    explicitBorderPreset.id = "outlined";
    explicitBorderPreset.label = "Outlined";
    explicitBorderPreset.hostAppearanceValues = {
        { "glassEnabled", "0" },
        { "borderWidth", "2.5" },
        { "edgeHighlightEnabled", "1" },
        { "edgeHighlightWidth", "3.5" },
        { "edgeHighlightStrength", "0.25" },
    };
    appearanceBackend.descriptor.scriptPresets.push_back(
        explicitBorderPreset);
    WidgetSettingPresetSchema invalidBorderStrengthPreset;
    invalidBorderStrengthPreset.id = "invalid-border-strength";
    invalidBorderStrengthPreset.label = "Invalid border strength";
    invalidBorderStrengthPreset.hostAppearanceValues = {
        { "edgeHighlightStrength", "1.1" },
    };
    appearanceBackend.descriptor.scriptPresets.push_back(
        invalidBorderStrengthPreset);
    WidgetSettingPresetSchema invalidBorderStylePreset;
    invalidBorderStylePreset.id = "invalid-border-style";
    invalidBorderStylePreset.label = "Invalid border style";
    invalidBorderStylePreset.hostAppearanceValues = {
        { "borderStyle", "2" },
    };
    appearanceBackend.descriptor.scriptPresets.push_back(
        invalidBorderStylePreset);
    WidgetSettingPresetSchema invalidBorderWidthPreset;
    invalidBorderWidthPreset.id = "invalid-border-width";
    invalidBorderWidthPreset.label = "Invalid border width";
    invalidBorderWidthPreset.hostAppearanceValues = {
        { "borderWidth", "4.5" },
    };
    appearanceBackend.descriptor.scriptPresets.push_back(
        invalidBorderWidthPreset);
    WidgetSettingPresetSchema invalidEdgeWidthPreset;
    invalidEdgeWidthPreset.id = "invalid-edge-width";
    invalidEdgeWidthPreset.label = "Invalid edge width";
    invalidEdgeWidthPreset.hostAppearanceValues = {
        { "edgeHighlightWidth", "0.25" },
    };
    appearanceBackend.descriptor.scriptPresets.push_back(
        invalidEdgeWidthPreset);
    // Opaque entries authored in a preset are ignored without preventing the
    // ordinary and host appearance values from applying.
    appearanceBackend.descriptor.scriptPresets[0].values["token"] =
        MakeWidgetSettingString("must-not-write");
    WidgetSettingsService appearanceService(appearanceBackend);
    auto appearanceLoad = appearanceService.Load(L"widget-1");
    Check(appearanceLoad.Succeeded() && appearanceLoad.snapshot &&
            appearanceLoad.snapshot->hostAppearance ==
                appearanceBackend.descriptor.hostAppearance,
        "snapshots carry the host-owned component appearance state");
    auto appearanceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *appearanceLoad.snapshot);
    WidgetHostAppearancePatch livePatch;
    livePatch.backgroundColor = 0xABCDEF;
    livePatch.backgroundOpacity = 0.4f;
    const auto liveAppearance = appearanceService.UpdateHostAppearance(
        appearanceGuard, livePatch);
    auto appearanceSnapshot = appearanceService.Snapshot(L"widget-1");
    Check(liveAppearance.status == WidgetSettingMutationStatus::Applied &&
            appearanceSnapshot &&
            appearanceSnapshot->hostAppearance.backgroundColor ==
                0xABCDEF &&
            appearanceSnapshot->hostAppearance.backgroundOpacity == 0.4f &&
            appearanceBackend.lastAppearancePatch == livePatch,
        "live custom appearance updates round-trip through a typed snapshot");
    appearanceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *appearanceSnapshot);
    const auto appearancePreset = appearanceService.ApplyPreset(
        appearanceGuard, "compact");
    appearanceSnapshot = appearanceService.Snapshot(L"widget-1");
    Check(appearancePreset.status == WidgetSettingMutationStatus::Applied &&
            appearanceBackend.lastAppearancePatch.presetId == "compact" &&
            appearanceBackend.lastAppearancePatch.backgroundColor ==
                1193046 &&
            appearanceBackend.lastAppearancePatch.backgroundOpacity ==
                0.625f &&
            appearanceBackend.lastAppearancePatch.glassEnabled == true &&
            appearanceBackend.lastAppearancePatch.acrylicEnabled == false &&
            appearanceBackend.lastAppearancePatch.edgeHighlightEnabled ==
                true &&
            appearanceBackend.lastAppearancePatch.edgeHighlightWidth ==
                kDefaultEdgeHighlightWidth &&
            appearanceBackend.lastAppearancePatch.borderWidth == 1.0f &&
            appearanceBackend.lastAppearancePatch.borderOpacity == 0.0f &&
            appearanceBackend.lastAppearancePatch.edgeHighlightStrength ==
                kDefaultEdgeHighlightStrength &&
            !appearanceBackend.lastAppearancePatch.followPersonalization &&
            !appearanceBackend.lastAppearancePatch.contentTheme &&
            appearanceBackend.lastAppearancePatch.clearContentTheme &&
            !appearanceBackend.ordinary.contains("token") &&
            appearanceSnapshot &&
            appearanceSnapshot->hostAppearance.presetId == "compact",
        "legacy component themes migrate glass to an independent edge highlight while atomically persisting editable host appearance and ordinary values");
    appearanceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *appearanceSnapshot);
    const auto explicitBorderResult = appearanceService.ApplyPreset(
        appearanceGuard, "outlined");
    appearanceSnapshot = appearanceService.Snapshot(L"widget-1");
    Check(explicitBorderResult.status ==
                WidgetSettingMutationStatus::Applied &&
            appearanceBackend.lastAppearancePatch.glassEnabled == false &&
            appearanceBackend.lastAppearancePatch.borderWidth == 2.5f &&
            appearanceBackend.lastAppearancePatch.edgeHighlightEnabled ==
                true &&
            appearanceBackend.lastAppearancePatch.edgeHighlightWidth ==
                3.5f &&
            appearanceBackend.lastAppearancePatch.edgeHighlightStrength ==
                0.25f && appearanceSnapshot &&
            !appearanceSnapshot->hostAppearance.glassEnabled &&
            appearanceSnapshot->hostAppearance.edgeHighlightEnabled,
        "explicit component border and edge-highlight fields remain independent from the material toggle");
    appearanceGuard = WidgetSettingMutationGuard::FromSnapshot(
        *appearanceSnapshot);
    const std::size_t appearanceTransactionsBeforeInvalid =
        appearanceBackend.appearanceTransactions.size();
    const auto invalidBorderStrengthResult = appearanceService.ApplyPreset(
        appearanceGuard, "invalid-border-strength");
    const auto invalidBorderStyleResult = appearanceService.ApplyPreset(
        appearanceGuard, "invalid-border-style");
    const auto invalidBorderWidthResult = appearanceService.ApplyPreset(
        appearanceGuard, "invalid-border-width");
    const auto invalidEdgeWidthResult = appearanceService.ApplyPreset(
        appearanceGuard, "invalid-edge-width");
    Check(invalidBorderStrengthResult.status ==
                WidgetSettingMutationStatus::InvalidValue &&
            invalidBorderStrengthResult.errorCode ==
                "invalidAppearanceOpacity" &&
            invalidBorderStyleResult.status ==
                WidgetSettingMutationStatus::InvalidValue &&
            invalidBorderStyleResult.errorCode == "invalidBorderStyle" &&
            invalidBorderWidthResult.status ==
                WidgetSettingMutationStatus::InvalidValue &&
            invalidBorderWidthResult.errorCode == "invalidBorderWidth" &&
            invalidEdgeWidthResult.status ==
                WidgetSettingMutationStatus::InvalidValue &&
            invalidEdgeWidthResult.errorCode ==
                "invalidEdgeHighlightWidth" &&
            appearanceBackend.appearanceTransactions.size() ==
                appearanceTransactionsBeforeInvalid,
        "invalid component border and edge-highlight preset ranges are rejected before persistence");

    FakeBackend utf8Backend = MakeBackend();
    WidgetSettingsService utf8Service(utf8Backend);
    auto utf8Load = utf8Service.Load(L"widget-1");
    auto utf8Guard = WidgetSettingMutationGuard::FromSnapshot(
        *utf8Load.snapshot);
    const std::string maximumUtf8Query =
        std::string(8190, 'a') + "\xC3\xA9";
    const auto maximumUtf8Result = utf8Service.SetSearchQuery(
        utf8Guard, "appSearch", maximumUtf8Query);
    auto utf8Snapshot = utf8Service.Snapshot(L"widget-1");
    utf8Guard = WidgetSettingMutationGuard::FromSnapshot(*utf8Snapshot);
    const std::size_t utf8Transactions = utf8Backend.transactions.size();
    const std::string oversizedUtf8Query =
        std::string(8191, 'a') + "\xC3\xA9";
    const auto oversizedUtf8Result = utf8Service.SetSearchQuery(
        utf8Guard, "appSearch", oversizedUtf8Query);
    const std::string invalidUtf8Query("\xC3\x28", 2);
    const auto invalidUtf8Result = utf8Service.SetSearchQuery(
        utf8Guard, "appSearch", invalidUtf8Query);
    Check(maximumUtf8Query.size() == 8192 &&
            maximumUtf8Result.status ==
                WidgetSettingMutationStatus::Applied &&
            Find(*utf8Snapshot, "appSearch")->searchQuery ==
                maximumUtf8Query &&
            oversizedUtf8Result.status ==
                WidgetSettingMutationStatus::InvalidValue &&
            oversizedUtf8Result.errorCode == "searchQueryTooLong" &&
            invalidUtf8Result.status ==
                WidgetSettingMutationStatus::InvalidValue &&
            invalidUtf8Result.errorCode == "invalidSearchQuery" &&
            utf8Backend.transactions.size() == utf8Transactions,
        "appSearch query limits count UTF-8 bytes and reject malformed boundary input without writes");

    if (failures == 0)
    {
        std::cout << "widget settings service checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " widget settings service check(s) failed\n";
    return EXIT_FAILURE;
}
