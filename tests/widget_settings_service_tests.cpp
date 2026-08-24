#include "widget_settings_service.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
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
    std::map<std::string, WidgetSettingOpaqueState, std::less<>> opaque;
    std::vector<WidgetSettingOrdinaryWrite> lastWrites;
    std::vector<std::vector<WidgetSettingOrdinaryWrite>> transactions;
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
        const WidgetSettingMutationGuard&,
        const std::vector<WidgetSettingOrdinaryWrite>& writes) override
    {
        ++ordinaryCalls;
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        lastWrites = writes;
        transactions.push_back(writes);
        if (!mutationResult.Succeeded()) return mutationResult;
        for (const auto& write : writes)
            ordinary.insert_or_assign(write.key, write.value);
        return mutationResult;
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
        const WidgetSettingMutationGuard&,
        const WidgetSettingSearchRequest& request,
        std::string_view resultId) override
    {
        lastOwnerWidget = widget.widgetId;
        lastOwnerPackage = widget.packageId;
        committedSearch = request;
        committedResult = resultId;
        if (mutationResult.Succeeded())
            ordinary[request.settingKey] =
                MakeWidgetSettingString("safe visible title");
        return mutationResult;
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
    backend.descriptor.scriptPresets.push_back(preset);
    return backend;
}
}

int main()
{
    using namespace snowdesktop::widget_runtime;

    FakeBackend backend = MakeBackend();
    backend.ordinary["enabled"] = MakeWidgetSettingBoolean(false);
    backend.opaque["token"] = {
        true, true, false, true, "must never escape" };
    WidgetSettingsService service(backend);
    WidgetSettingsLoadResult loaded = service.Load(L"widget-1");
    Check(loaded.Succeeded() && loaded.snapshot &&
            loaded.snapshot->fields.size() == 7 &&
            loaded.snapshot->fields.front().schema.key == "enabled" &&
            loaded.snapshot->fields.back().schema.key == "feeds" &&
            loaded.snapshot->groups.size() == 2 &&
            loaded.snapshot->presets.size() == 1,
        "manifest and script fields, groups, and presets merge in source order");
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

    WidgetSettingMutationGuard guard =
        WidgetSettingMutationGuard::FromSnapshot(*loaded.snapshot);
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
            backend.lastWrites[1].typedStorage,
        "preset application is one transaction and preserves typed values");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    WidgetSettingMutationResult reset = service.Reset(guard);
    bool resetHasOpaque = false;
    bool resetTypedRange = false;
    bool resetTypedMulti = false;
    for (const auto& write : backend.lastWrites)
    {
        resetHasOpaque = resetHasOpaque || write.key == "token" ||
            write.key == "file" || write.key == "app";
        resetTypedRange = resetTypedRange ||
            (write.key == "scale" && write.typedStorage);
        resetTypedMulti = resetTypedMulti ||
            (write.key == "feeds" && write.typedStorage);
    }
    Check(reset.status == WidgetSettingMutationStatus::Applied &&
            !resetHasOpaque && resetTypedRange && resetTypedMulti,
        "reset is one ordinary transaction and never resets opaque channels as strings");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    WidgetSettingMutationResult firstSearch = service.StartSearch(
        guard, "appSearch", "cal");
    WidgetSettingMutationResult secondSearch = service.StartSearch(
        guard, "appSearch", "calc");
    Check(firstSearch.status == WidgetSettingMutationStatus::Started &&
            secondSearch.status == WidgetSettingMutationStatus::Started &&
            backend.searches.size() == 2 &&
            backend.cancellations.size() == 1,
        "new searches cancel the previous request for the same setting");
    backend.Complete(0, { { "old", "Old", "apps", "application" } });
    auto searchSnapshot = service.SearchSnapshot(
        L"widget-1", "appSearch");
    Check(searchSnapshot && searchSnapshot->pending &&
            searchSnapshot->results.empty(),
        "a superseded search completion is discarded by request id");
    backend.Complete(1, {
        { "calculator", "Calculator", "apps", "application" } });
    searchSnapshot = service.SearchSnapshot(L"widget-1", "appSearch");
    Check(searchSnapshot && !searchSnapshot->pending &&
            searchSnapshot->completed &&
            searchSnapshot->results.size() == 1 &&
            searchSnapshot->results[0].id == "calculator",
        "the current request publishes a path-free search snapshot");
    const std::uint64_t searchRequestId = searchSnapshot->requestId;
    Check(service.CommitSearchResult(guard, "appSearch",
                searchRequestId, "missing").status ==
                WidgetSettingMutationStatus::InvalidValue,
        "search commits reject ids absent from the request-scoped result set");
    WidgetSettingMutationResult committed = service.CommitSearchResult(
        guard, "appSearch", searchRequestId, "calculator");
    Check(committed.status == WidgetSettingMutationStatus::Applied &&
            backend.committedResult == "calculator" &&
            backend.committedSearch.requestId == searchRequestId,
        "search selection commits by opaque result id and request id");

    snapshot = service.Snapshot(L"widget-1");
    guard = WidgetSettingMutationGuard::FromSnapshot(*snapshot);
    (void)service.StartSearch(guard, "appSearch", "paint");
    const std::size_t staleIndex = backend.searches.size() - 1;
    backend.descriptor.generation = 8;
    WidgetSettingsLoadResult generationReload = service.Reload(L"widget-1");
    backend.Complete(staleIndex,
        { { "paint", "Paint", "apps", "application" } });
    Check(generationReload.Succeeded() && generationReload.snapshot &&
            generationReload.snapshot->generation == 8 &&
            !service.SearchSnapshot(L"widget-1", "appSearch") &&
            backend.cancellations.size() == 2,
        "widget reload cancels replaced work and drops old-generation async results");

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

    if (failures == 0)
    {
        std::cout << "widget settings service checks passed\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " widget settings service check(s) failed\n";
    return EXIT_FAILURE;
}
