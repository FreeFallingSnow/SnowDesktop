#include "widget_engine_settings_backend.h"

#include "name_pinyin.h"
#include "utils.h"
#include "widget_filesystem_handle_store.h"
#include "widget_preview_context.h"
#include "widget_secret_store.h"
#include "widget_storage_transaction.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
WidgetSettingsBackendResult BackendResult(
    WidgetSettingsBackendStatus status, std::string errorCode = {},
    std::string message = {})
{
    return { status, std::move(errorCode), std::move(message) };
}

WidgetSettingsBackendResult Success(bool changed = true)
{
    return BackendResult(changed ? WidgetSettingsBackendStatus::Succeeded
                                 : WidgetSettingsBackendStatus::Unchanged);
}

WidgetSettingsBackendResult WrongThread()
{
    return BackendResult(WidgetSettingsBackendStatus::Failed,
        "wrongThread", "widget settings backend requires its owner thread");
}

bool IsReservedDeclarativeSettingKey(std::string_view key) noexcept
{
    return key == "cornerRadius" || key == "barHeight" ||
        key == "bg" || key == "border" || key == "alpha" ||
        key == "borderAlpha" || key == "borderStyle" ||
        key == "borderWidth" || key == "edgeHighlightEnabled" ||
        key == "edgeHighlightWidth" || key == "edgeHighlightStrength" ||
        key == "gradientEndA" ||
        key == "shadowAlpha" || key == "shadowBlur" ||
        key == "shadowOffsetY" || key == "highlightAlpha" ||
        key == "noiseAlpha" || key == "glassEnabled" ||
        key == "glassBlurRadius" || key == "acrylicEnabled" ||
        key == "followPersonalization" || key == "__preset" ||
        key == "__contentTheme";
}

bool IsHostAppearancePresetKey(std::string_view key) noexcept
{
    return key == "followPersonalization" || key == "bg" ||
        key == "border" || key == "alpha" || key == "borderAlpha" ||
        key == "borderStyle" || key == "borderWidth" ||
        key == "edgeHighlightEnabled" || key == "edgeHighlightWidth" ||
        key == "edgeHighlightStrength" || key == "gradientEndA" ||
        key == "shadowAlpha" ||
        key == "shadowBlur" || key == "shadowOffsetY" ||
        key == "highlightAlpha" || key == "noiseAlpha" ||
        key == "glassEnabled" || key == "glassBlurRadius" ||
        key == "acrylicEnabled" || key == "__preset" ||
        key == "__contentTheme";
}

bool IsEntityReferenceType(std::string_view type) noexcept
{
    return type == "appReference" || type == "desktopItemReference" ||
        type == "fileReference" || type == "folderReference";
}

bool IsFilesystemHandleType(std::string_view type) noexcept
{
    return type == "fileHandle" || type == "folderHandle";
}

std::string_view EntityReferenceKind(std::string_view type) noexcept
{
    if (type == "appReference") return "app.reference";
    if (type == "desktopItemReference") return "desktop.item";
    if (type == "fileReference" || type == "folderReference")
        return "filesystem.reference";
    return {};
}

std::string_view EntityReferenceTypeFilter(std::string_view type) noexcept
{
    if (type == "fileReference") return "file";
    if (type == "folderReference") return "folder";
    return {};
}

std::optional<WidgetFilesystemHandleAccess> HandleAccess(
    std::string_view access) noexcept
{
    if (access == "read") return WidgetFilesystemHandleAccess::Read;
    if (access == "write") return WidgetFilesystemHandleAccess::Write;
    if (access == "readWrite")
        return WidgetFilesystemHandleAccess::ReadWrite;
    return std::nullopt;
}

std::string HandleMetadataKey(std::string_view key)
{
    return "__host.settingHandle." + std::string(key);
}

const LuaWidgetManifest::Setting* FindDeclaredSetting(
    const LuaWidget& widget, std::string_view key)
{
    const auto find = [key](const auto& settings)
        -> const LuaWidgetManifest::Setting* {
        const auto value = std::find_if(settings.begin(), settings.end(),
            [key](const LuaWidgetManifest::Setting& candidate) {
                return candidate.key == key &&
                    !IsReservedDeclarativeSettingKey(candidate.key);
            });
        return value == settings.end() ? nullptr : &*value;
    };
    if (const auto* manifest = find(widget.manifest.settings))
        return manifest;
    return find(widget.scriptSettings);
}

bool SchemaMatches(const LuaWidgetManifest::Setting& declaration,
    const WidgetSettingFieldSchema& field)
{
    return widget_settings_backend_detail::ConvertSetting(
        declaration).schema == field;
}

WidgetSettingGroupSchema ConvertGroup(
    const LuaWidgetManifest::SettingGroup& source)
{
    return { source.id, source.label, source.description,
        source.collapsible, source.defaultExpanded };
}

WidgetSettingPresetSchema ConvertPreset(
    const LuaWidgetManifest::SettingPreset& source,
    const LuaWidget& widget)
{
    WidgetSettingPresetSchema result;
    result.id = source.id;
    result.label = source.label;
    result.isDefault = source.isDefault;
    for (const auto& [key, encoded] : source.values)
    {
        if (IsHostAppearancePresetKey(key))
        {
            result.hostAppearanceValues.emplace(key, encoded);
            continue;
        }
        const auto* declaration = FindDeclaredSetting(widget, key);
        if (!declaration)
        {
            result.values.emplace(
                key, MakeWidgetSettingString(encoded));
            continue;
        }
        const auto converted =
            widget_settings_backend_detail::ConvertSetting(*declaration);
        InteractionValue value;
        std::string error;
        if (converted.schema.Kind() == WidgetSettingKind::Range)
        {
            double number = converted.schema.minimum;
            if (ParseFiniteSettingNumber(encoded, number))
                value = MakeWidgetSettingNumber(SnapRangeSettingValue(
                    number, converted.schema.minimum,
                    converted.schema.maximum, converted.schema.step));
            else
                value = MakeWidgetSettingString(encoded);
        }
        else if (converted.schema.Channel() ==
                WidgetSettingValueChannel::Ordinary &&
            DecodeLegacyWidgetSettingValue(
                converted.schema, encoded, value, error))
        {
        }
        else
            value = MakeWidgetSettingString(encoded);
        result.values.emplace(key, std::move(value));
    }
    for (const auto& [key, values] : source.arrayValues)
        result.values.insert_or_assign(
            key, MakeWidgetSettingStringArray(values));
    return result;
}

bool RequestIdentityMatches(
    const WidgetSettingSearchRequest& left,
    const WidgetSettingSearchRequest& right) noexcept
{
    return widget_settings_backend_detail::SearchIdentityMatches(
        left, right);
}

constexpr int NoSearchMatch = 9;

bool StartsWith(std::string_view value, std::string_view prefix) noexcept
{
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

int SearchRank(const WidgetAppCatalogEntry& entry,
    std::string_view foldedQuery, std::string_view pinyinQuery) noexcept
{
    if (entry.foldedTitle == foldedQuery) return 0;
    if (!pinyinQuery.empty())
    {
        if (entry.pinyinFull == pinyinQuery) return 1;
        if (entry.pinyinInitials == pinyinQuery) return 2;
        if (StartsWith(entry.pinyinFull, pinyinQuery)) return 3;
        if (StartsWith(entry.pinyinInitials, pinyinQuery)) return 4;
    }
    if (StartsWith(entry.foldedTitle, foldedQuery)) return 5;
    if (!pinyinQuery.empty())
    {
        if (entry.pinyinFull.find(pinyinQuery) != std::string::npos)
            return 6;
        if (entry.pinyinInitials.find(pinyinQuery) != std::string::npos)
            return 7;
    }
    return entry.foldedTitle.find(foldedQuery) != std::string::npos
        ? 8 : NoSearchMatch;
}
}

struct WidgetEngineSettingsBackend::SearchState final
    : std::enable_shared_from_this<SearchState>
{
    struct Record
    {
        WidgetSettingSearchRequest request;
        bool completed = false;
        std::unordered_map<std::string, WidgetAppSearchResult> results;
    };

    struct Work
    {
        WidgetSettingSearchRequest request;
        SearchCompletion completion;
        std::string foldedQuery;
        std::string pinyinQuery;
        std::vector<WidgetAppCatalogEntry> catalog;
    };

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Work> queue;
    std::unordered_map<std::uint64_t, Record> records;
    std::jthread worker;
    bool stopping = false;

    bool Start(Work work)
    {
        std::lock_guard lock(mutex);
        if (stopping || work.request.requestId == 0 ||
            records.contains(work.request.requestId))
            return false;
        std::erase_if(records, [&](const auto& item) {
            return item.second.request.widgetId == work.request.widgetId &&
                item.second.request.settingKey ==
                    work.request.settingKey;
        });
        records.emplace(work.request.requestId,
            Record{ work.request, false, {} });
        queue.push_back(std::move(work));
        if (!worker.joinable())
        {
            const auto self = shared_from_this();
            worker = std::jthread(
                [self](std::stop_token stopToken) {
                    self->WorkerMain(stopToken);
                });
        }
        condition.notify_one();
        return true;
    }

    WidgetSettingsBackendResult Cancel(
        const WidgetSettingSearchRequest& request)
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(request.requestId);
        if (found == records.end()) return Success(false);
        if (!RequestIdentityMatches(found->second.request, request))
            return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
                "staleSearchRequest");
        records.erase(found);
        return Success();
    }

    std::optional<WidgetAppSearchResult> FindResult(
        const WidgetSettingSearchRequest& request,
        std::string_view resultId)
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(request.requestId);
        if (found == records.end() || !found->second.completed ||
            !RequestIdentityMatches(found->second.request, request))
            return std::nullopt;
        const auto result = found->second.results.find(
            std::string(resultId));
        return result == found->second.results.end()
            ? std::nullopt
            : std::optional<WidgetAppSearchResult>(result->second);
    }

    void Erase(const WidgetSettingSearchRequest& request)
    {
        std::lock_guard lock(mutex);
        const auto found = records.find(request.requestId);
        if (found != records.end() &&
            RequestIdentityMatches(found->second.request, request))
            records.erase(found);
    }

    void Shutdown()
    {
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            stopping = true;
            queue.clear();
            records.clear();
        }
        worker.request_stop();
        condition.notify_all();
        if (worker.joinable()) worker.join();
    }

    void WorkerMain(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            Work work;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] {
                    return stopToken.stop_requested() || stopping ||
                        !queue.empty();
                });
                if (stopToken.stop_requested() || stopping) return;
                work = std::move(queue.front());
                queue.pop_front();
                const auto record = records.find(work.request.requestId);
                if (record == records.end() ||
                    !RequestIdentityMatches(
                        record->second.request, work.request))
                    continue;
            }

            std::array<std::vector<const WidgetAppCatalogEntry*>,
                NoSearchMatch> buckets;
            if (!work.foldedQuery.empty())
            {
                for (const auto& entry : work.catalog)
                {
                    const int rank = SearchRank(entry, work.foldedQuery,
                        work.pinyinQuery);
                    if (rank >= 0 && rank < NoSearchMatch)
                        buckets[static_cast<std::size_t>(rank)].push_back(
                            &entry);
                }
            }

            std::vector<WidgetSettingSearchResult> visible;
            std::unordered_map<std::string, WidgetAppSearchResult> hidden;
            visible.reserve(work.request.maximumResults);
            std::size_t candidateIndex = 0;
            bool full = false;
            for (const auto& bucket : buckets)
            {
                for (const auto* entry : bucket)
                {
                    if (visible.size() >= work.request.maximumResults)
                    {
                        full = true;
                        break;
                    }
                    std::string identity = entry->id;
                    identity.push_back('\n');
                    identity += entry->launchTarget;
                    identity.push_back('\n');
                    identity += std::to_string(candidateIndex++);
                    std::string resultId = MakeWidgetItemReference(
                        "widget.settings.search", identity);
                    if (resultId.empty()) continue;
                    while (hidden.contains(resultId))
                        resultId = MakeWidgetItemReference(
                            "widget.settings.search.collision",
                            identity + std::to_string(candidateIndex++));
                    hidden.emplace(resultId, WidgetAppSearchResult{
                        entry->id, entry->title, entry->launchTarget,
                        entry->source, entry->type });
                    visible.push_back({ resultId, entry->title,
                        entry->source, entry->type });
                }
                if (full) break;
            }

            SearchCompletion callback;
            {
                std::lock_guard lock(mutex);
                const auto record = records.find(work.request.requestId);
                if (stopping || record == records.end() ||
                    !RequestIdentityMatches(
                        record->second.request, work.request))
                    continue;
                record->second.completed = true;
                record->second.results = std::move(hidden);
                callback = std::move(work.completion);
            }
            if (callback)
            {
                WidgetSettingSearchCompletion completion;
                completion.widgetId = work.request.widgetId;
                completion.settingKey = work.request.settingKey;
                completion.generation = work.request.generation;
                completion.requestId = work.request.requestId;
                completion.result = Success();
                completion.results = std::move(visible);
                callback(std::move(completion));
            }
        }
    }
};

struct WidgetEngineSettingsBackend::PreviewState
{
    WidgetSettingsBackendDescriptor descriptor;
    std::unordered_map<std::string, std::optional<std::string>> originals;
    std::unordered_set<std::string> affectedKeys;
};

WidgetEngineSettingsBackend::WidgetEngineSettingsBackend(
    WidgetEngine& engine)
    : engine_(engine), ownerThreadId_(GetCurrentThreadId()),
      searches_(std::make_shared<SearchState>())
{
}

WidgetEngineSettingsBackend::~WidgetEngineSettingsBackend()
{
    RestorePreviewNoexcept();
    if (searches_) searches_->Shutdown();
}

WidgetSettingsBackendResult WidgetEngineSettingsBackend::Describe(
    std::wstring_view widgetId,
    WidgetSettingsBackendDescriptor& descriptor)
{
    descriptor = {};
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(std::wstring(widgetId));
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (preview_ && preview_->descriptor.widgetId == widget.widgetId &&
        !widget_settings_backend_detail::DescriptorMatchesCurrent(
            preview_->descriptor, widget.widgetId, widget.packageId,
            widget.runtimeToken))
    {
        // A runtime reload invalidates every guard which could commit or
        // cancel the old preview. Restore only its touched keys before the
        // new generation is described.
        RestorePreviewNoexcept();
    }
    if (!widget.valid || widget.runtimeToken == 0 ||
        widget.packageId.empty())
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "widgetRuntimeUnavailable");

    descriptor.widgetId = widget.widgetId;
    descriptor.packageId = widget.packageId;
    descriptor.widgetName = widget.name;
    descriptor.generation = widget.runtimeToken;
    descriptor.preview = widget.preview;
    descriptor.customStyle = widget.customStyle;
    const auto appendFields = [](const auto& source, auto& destination) {
        for (const auto& field : source)
            if (!field.key.empty() &&
                !IsReservedDeclarativeSettingKey(field.key))
                destination.push_back(
                    widget_settings_backend_detail::ConvertSetting(field));
    };
    appendFields(widget.manifest.settings, descriptor.manifestFields);
    appendFields(widget.scriptSettings, descriptor.scriptFields);
    for (const auto& group : widget.manifest.settingGroups)
        descriptor.manifestGroups.push_back(ConvertGroup(group));
    for (const auto& group : widget.scriptSettingGroups)
        descriptor.scriptGroups.push_back(ConvertGroup(group));
    for (const auto& preset : widget.manifest.presets)
        descriptor.manifestPresets.push_back(
            ConvertPreset(preset, widget));
    for (const auto& preset : widget.scriptPresets)
        descriptor.scriptPresets.push_back(
            ConvertPreset(preset, widget));

    WidgetHostAppearanceState& appearance = descriptor.hostAppearance;
    float bgR = 21.0f / 255.0f;
    float bgG = 26.0f / 255.0f;
    float bgB = 33.0f / 255.0f;
    float borderR = 1.0f;
    float borderG = 1.0f;
    float borderB = 1.0f;
    float backgroundOpacity = 0.36f;
    float borderOpacity = 0.40f;
    float borderWidth = 1.0f;
    bool edgeHighlightEnabled = false;
    float edgeHighlightWidth = kDefaultEdgeHighlightWidth;
    float edgeHighlightStrength = kDefaultEdgeHighlightStrength;
    float gradientEndOpacity = 0.0f;
    bool glassEnabled = false;
    bool acrylicEnabled = false;
    (void)engine_.ReadCustomColors(widget.widgetId,
        bgR, bgG, bgB, backgroundOpacity,
        borderR, borderG, borderB, borderOpacity,
        borderWidth, edgeHighlightEnabled, edgeHighlightWidth,
        edgeHighlightStrength,
        gradientEndOpacity, glassEnabled, acrylicEnabled);
    const auto colorToInteger = [](float red, float green, float blue) {
        const auto channel = [](float value) {
            if (!std::isfinite(value)) value = 0.0f;
            value = std::clamp(value, 0.0f, 1.0f);
            return std::clamp(static_cast<int>(
                std::lround(value * 255.0f)), 0, 255);
        };
        return (channel(red) << 16) | (channel(green) << 8) |
            channel(blue);
    };
    appearance.backgroundColor = colorToInteger(bgR, bgG, bgB);
    appearance.borderColor = colorToInteger(borderR, borderG, borderB);
    const auto finiteOpacity = [](float value, float fallback) {
        return std::isfinite(value)
            ? std::clamp(value, 0.0f, 1.0f) : fallback;
    };
    appearance.backgroundOpacity = finiteOpacity(
        backgroundOpacity, 0.36f);
    appearance.borderOpacity = finiteOpacity(borderOpacity, 0.40f);
    appearance.borderWidth = std::clamp(borderWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    appearance.edgeHighlightEnabled = edgeHighlightEnabled;
    appearance.edgeHighlightWidth = std::clamp(edgeHighlightWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    appearance.edgeHighlightStrength = finiteOpacity(
        edgeHighlightStrength, kDefaultEdgeHighlightStrength);
    appearance.gradientEndOpacity = finiteOpacity(
        gradientEndOpacity, 0.0f);
    appearance.glassEnabled = glassEnabled;
    appearance.acrylicEnabled = acrylicEnabled;
    appearance.contentTheme = std::clamp(
        engine_.RuntimeGetWidgetTheme(widget.widgetId).contentTheme, 0, 1);

    const auto& storage = widget.preview
        ? widget.previewStorage
        : engine_.WidgetSettingsPersistentStorageForBackend();
    const std::string prefix = WideToUtf8(widget.widgetId) + ".";
    const auto stored = [&](std::string_view key) -> std::string_view {
        const auto value = storage.find(prefix + std::string(key));
        return value == storage.end()
            ? std::string_view{} : std::string_view(value->second);
    };
    const std::string_view follow = stored("followPersonalization");
    appearance.followPersonalization = follow.empty()
        ? widget.followPersonalizationDefault
        : follow == "1" || follow == "true";
    appearance.presetId = std::string(stored("__preset"));
    const std::string_view contentTheme = stored("__contentTheme");
    if (!contentTheme.empty())
    {
        const std::string encodedTheme(contentTheme);
        char* end = nullptr;
        const long parsed = std::strtol(encodedTheme.c_str(), &end, 10);
        if (end && *end == '\0')
            appearance.contentTheme = std::clamp(
                static_cast<int>(parsed), 0, 1);
    }

    if (appearance.presetId.empty())
    {
        const WidgetSettingPresetSchema* defaultPreset = nullptr;
        const WidgetSettingPresetSchema* firstPreset = nullptr;
        const auto inspectPresets = [&](const auto& presets) {
            for (const auto& preset : presets)
            {
                if (!firstPreset) firstPreset = &preset;
                if (!defaultPreset &&
                    (preset.isDefault || preset.id == "default"))
                {
                    defaultPreset = &preset;
                }
            }
        };
        inspectPresets(descriptor.manifestPresets);
        inspectPresets(descriptor.scriptPresets);
        const auto* fallback = defaultPreset
            ? defaultPreset : (descriptor.customStyle ? nullptr : firstPreset);
        appearance.presetId = fallback
            ? fallback->id
            : (descriptor.customStyle ? "__custom" : std::string{});
    }
    return Success(false);
}

WidgetSettingBackendReadResult WidgetEngineSettingsBackend::ReadOrdinary(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingFieldSchema& field, bool typedStorage)
{
    if (GetCurrentThreadId() != ownerThreadId_)
        return { WrongThread(), false, {} };
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return { BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
                     "widgetNotFound"), false, {} };
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::DescriptorMatchesCurrent(
            descriptor, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return { BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
                     "staleSnapshot"), false, {} };
    const auto* declaration = FindDeclaredSetting(widget, field.key);
    if (!declaration)
        return { BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
                     "settingNotFound"), false, {} };
    if (!SchemaMatches(*declaration, field))
        return { BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
                     "staleSettingSchema"), false, {} };
    const auto current =
        widget_settings_backend_detail::ConvertSetting(*declaration);
    if (current.schema.Channel() != WidgetSettingValueChannel::Ordinary ||
        typedStorage !=
            WidgetSettingUsesTypedStorage(current.schema.Kind()))
        return { BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                     typedStorage ? "unexpectedTypedStorage"
                                  : "typedStorageRequired"), false, {} };

    const auto& storage = widget.preview
        ? widget.previewStorage
        : engine_.WidgetSettingsPersistentStorageForBackend();
    const std::string prefix = WideToUtf8(widget.widgetId) + ".";
    const auto stored = storage.find(prefix + field.key);
    const auto marker = storage.find(prefix +
        TypedStorageMetadataKey(field.key));
    if (stored == storage.end())
    {
        if (marker != storage.end())
            return { BackendResult(
                         WidgetSettingsBackendStatus::PersistenceFailed,
                         "orphanTypedStorageMarker"), false, {} };
        return { Success(false), false, {} };
    }

    InteractionValue value;
    std::string error;
    if (typedStorage)
    {
        if (marker == storage.end() ||
            marker->second != TypedStorageMarker)
            return { BackendResult(
                         WidgetSettingsBackendStatus::PersistenceFailed,
                         "typedStorageMarkerMissing"), false, {} };
        if (!DecodeTypedStorageValue(stored->second, value, error))
            return { BackendResult(
                         WidgetSettingsBackendStatus::PersistenceFailed,
                         "invalidTypedStorage", std::move(error)),
                false, {} };
    }
    else
    {
        if (marker != storage.end())
            return { BackendResult(
                         WidgetSettingsBackendStatus::PersistenceFailed,
                         "unexpectedTypedStorageMarker"), false, {} };
        if (!DecodeLegacyWidgetSettingValue(
                current.schema, stored->second, value, error))
            return { BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                         error.empty() ? "invalidStoredValue" : error),
                false, {} };
    }
    return { Success(false), true, std::move(value) };
}

WidgetSettingBackendReadResult
WidgetEngineSettingsBackend::ReadSearchQuery(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingFieldSchema& field)
{
    if (GetCurrentThreadId() != ownerThreadId_)
        return { WrongThread(), false, {} };
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return { BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
                     "widgetNotFound"), false, {} };
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::DescriptorMatchesCurrent(
            descriptor, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return { BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
                     "staleSnapshot"), false, {} };
    const auto* declaration = FindDeclaredSetting(widget, field.key);
    if (!declaration)
        return { BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
                     "settingNotFound"), false, {} };
    if (!SchemaMatches(*declaration, field))
        return { BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
                     "staleSettingSchema"), false, {} };
    if (declaration->type != "appSearch" || field.searchKey.empty() ||
        field.searchKey == field.key)
        return { BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                     "invalidSearchKey"), false, {} };

    const auto& storage = widget.preview
        ? widget.previewStorage
        : engine_.WidgetSettingsPersistentStorageForBackend();
    WidgetStorageTransaction transaction(
        storage, WideToUtf8(widget.widgetId));
    std::string error;
    const auto stored = transaction.Get(field.searchKey, error);
    if (!error.empty())
        return { BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                     "invalidSearchKey", std::move(error)), false, {} };
    const auto marker = transaction.GetHostMetadata(
        TypedStorageMetadataKey(field.searchKey), error);
    if (!error.empty())
        return { BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                     "invalidSearchKey", std::move(error)), false, {} };
    widget_settings_backend_detail::DecodedSearchQueryStorage decoded;
    const auto storedView = stored
        ? std::optional<std::string_view>(*stored) : std::nullopt;
    const auto markerView = marker
        ? std::optional<std::string_view>(*marker) : std::nullopt;
    if (!widget_settings_backend_detail::DecodeSearchQueryStorage(
            storedView, markerView, decoded, error))
        return { BackendResult(
                     WidgetSettingsBackendStatus::PersistenceFailed,
                     "invalidSearchQueryStorage", std::move(error)),
            false, {} };
    return decoded.hasStoredValue
        ? WidgetSettingBackendReadResult{ Success(false), true,
            MakeWidgetSettingString(std::move(decoded.value)) }
        : WidgetSettingBackendReadResult{ Success(false), false, {} };
}

WidgetSettingBackendOpaqueResult WidgetEngineSettingsBackend::ReadOpaque(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingFieldSchema& field)
{
    if (GetCurrentThreadId() != ownerThreadId_)
        return { WrongThread(), {} };
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return { BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
                     "widgetNotFound"), {} };
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::DescriptorMatchesCurrent(
            descriptor, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return { BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
                     "staleSnapshot"), {} };
    const auto* declaration = FindDeclaredSetting(widget, field.key);
    if (!declaration)
        return { BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
                     "settingNotFound"), {} };
    if (!SchemaMatches(*declaration, field))
        return { BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
                     "staleSettingSchema"), {} };
    const auto current =
        widget_settings_backend_detail::ConvertSetting(*declaration);

    WidgetSettingOpaqueState state;
    const std::string instanceId = WideToUtf8(widget.widgetId);
    if (current.schema.Kind() == WidgetSettingKind::Password)
    {
        state.available = !widget.preview && engine_.secretStore_ != nullptr;
        state.canChoose = state.available;
        if (state.available)
            state.configured = !engine_.secretStore_->Reference(
                widget.packageId, instanceId, field.key).empty();
        state.canClear = state.available && state.configured;
        return { Success(false), std::move(state) };
    }

    if (current.schema.Channel() ==
        WidgetSettingValueChannel::FilesystemHandle)
    {
        const auto& storage =
            engine_.WidgetSettingsPersistentStorageForBackend();
        const auto raw = widget.preview ? storage.end() : storage.find(
            instanceId + "." + HandleMetadataKey(field.key));
        const std::string handle = raw == storage.end()
            ? std::string{} : raw->second;
        state.configured = !handle.empty();
        const auto access = HandleAccess(declaration->access);
        std::vector<std::string> extensions;
        const bool extensionsValid = NormalizeFilesystemSettingExtensions(
            declaration->extensions, extensions);
        state.canChoose = !widget.preview && engine_.filePickerCallback_ &&
            engine_.filesystemHandleStore_ && access && extensionsValid;
        state.canClear = !widget.preview && state.configured;
        if (state.configured && !widget.preview && access &&
            engine_.filesystemHandleStore_)
        {
            const auto entry = engine_.filesystemHandleStore_->Resolve(
                { instanceId, widget.packageId }, handle);
            const auto expectedKind = declaration->type == "folderHandle"
                ? WidgetFilesystemHandleKind::Folder
                : WidgetFilesystemHandleKind::File;
            if (entry && entry->kind == expectedKind &&
                entry->access == *access)
            {
                state.available = true;
                std::filesystem::path name = entry->path.filename();
                if (name.empty()) name = entry->path.root_name();
                state.displayLabel = WideToUtf8(name.wstring());
            }
        }
        return { Success(false), std::move(state) };
    }

    if (current.schema.Channel() == WidgetSettingValueChannel::EntityReference)
    {
        const auto declarationIt =
            widget.logicalSlots.Declarations().find(
                current.schema.binding);
        const auto* snapshot = widget.logicalSlots.Find(
            current.schema.binding);
        const bool bindingValid = !current.schema.binding.empty() &&
            declarationIt != widget.logicalSlots.Declarations().end() &&
            snapshot && snapshot->kind == LogicalSlotKind::Binding;
        state.canChoose = !widget.preview && bindingValid &&
            engine_.logicalSlotPickerCallback_;
        if (bindingValid && !snapshot->items.empty())
        {
            const auto& item = snapshot->items.front();
            state.configured = true;
            state.available = item.available;
            state.displayLabel = item.title;
            state.canClear = !widget.preview &&
                declarationIt->second.allowClear;
        }
        return { Success(false), std::move(state) };
    }
    return { BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                 "wrongValueChannel"), {} };
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::ApplyOrdinaryTransaction(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const std::vector<WidgetSettingOrdinaryWrite>& writes)
{
    return ApplyHostAppearanceTransaction(
        descriptor, guard, {}, writes);
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::ApplyHostAppearanceTransaction(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetHostAppearancePatch& appearance,
    const std::vector<WidgetSettingOrdinaryWrite>& writes)
{
    return ApplyHostAppearanceTransactionImpl(
        descriptor, guard, appearance, writes, true);
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::PreviewHostAppearanceTransaction(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetHostAppearancePatch& appearance,
    const std::vector<WidgetSettingOrdinaryWrite>& writes)
{
    return ApplyHostAppearanceTransactionImpl(
        descriptor, guard, appearance, writes, false);
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::ApplyHostAppearanceTransactionImpl(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetHostAppearancePatch& appearance,
    const std::vector<WidgetSettingOrdinaryWrite>& writes,
    bool persist)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    const std::wstring widgetId = widget.widgetId;
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    if (widget.preview || HasStorageOverlay())
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "persistentStorageUnavailable");
    if (preview_)
    {
        const bool sameIdentity =
            preview_->descriptor.widgetId == descriptor.widgetId &&
            preview_->descriptor.packageId == descriptor.packageId &&
            preview_->descriptor.generation == descriptor.generation;
        if (!sameIdentity)
            return BackendResult(WidgetSettingsBackendStatus::Unavailable,
                "previewAlreadyActive");
        if (persist)
            return BackendResult(WidgetSettingsBackendStatus::Unavailable,
                "previewCommitRequired");
    }
    if (writes.empty() && appearance.Empty()) return Success(false);
    if (appearance.contentTheme && appearance.clearContentTheme)
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "ambiguousContentThemeWrite");

    auto& storage = engine_.WidgetSettingsPersistentStorageForBackend();
    WidgetStorageTransaction transaction(storage,
        WideToUtf8(widget.widgetId));
    std::unordered_set<std::string> keys;
    const auto setAppearance = [&](std::string key, std::string value,
                                   std::string& error) {
        if (!keys.emplace(key).second)
        {
            error = "duplicateSettingWrite";
            return false;
        }
        bool changed = false;
        if (!transaction.Set(key, std::move(value), changed, error))
            return false;
        // Legacy setStorage always removed a stale typed-storage marker for
        // these host-owned raw keys in the same save operation.
        bool metadataChanged = false;
        return transaction.RemoveHostMetadata(
            TypedStorageMetadataKey(key), metadataChanged, error);
    };
    std::string appearanceError;
    if (appearance.followPersonalization &&
        !setAppearance("followPersonalization",
            *appearance.followPersonalization ? "1" : "0",
            appearanceError))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "appearanceWriteRejected", std::move(appearanceError));
    if (appearance.presetId &&
        !setAppearance("__preset", *appearance.presetId, appearanceError))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "appearanceWriteRejected", std::move(appearanceError));
    const auto setColor = [&](std::string key,
                              const std::optional<int>& value) {
        if (!value) return true;
        if (*value < 0 || *value > 0xFFFFFF)
        {
            appearanceError = "appearance color is outside 0x000000-0xFFFFFF";
            return false;
        }
        return setAppearance(std::move(key), std::to_string(*value),
            appearanceError);
    };
    if (!setColor("bg", appearance.backgroundColor) ||
        !setColor("border", appearance.borderColor))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "invalidAppearanceColor", std::move(appearanceError));
    const auto setOpacity = [&](std::string key,
                                const std::optional<float>& value) {
        if (!value) return true;
        if (!std::isfinite(*value) || *value < 0.0f || *value > 1.0f)
        {
            appearanceError = "appearance opacity is outside 0-1";
            return false;
        }
        return setAppearance(std::move(key), std::to_string(*value),
            appearanceError);
    };
    if (!setOpacity("alpha", appearance.backgroundOpacity) ||
        !setOpacity("borderAlpha", appearance.borderOpacity) ||
        !setOpacity("gradientEndA", appearance.gradientEndOpacity) ||
        !setOpacity("edgeHighlightStrength",
            appearance.edgeHighlightStrength))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "invalidAppearanceOpacity", std::move(appearanceError));
    const auto setWidth = [&](std::string key,
                              const std::optional<float>& value,
                              std::string_view errorCode) {
        if (!value) return std::optional<WidgetSettingsBackendResult>{};
        if (!std::isfinite(*value) ||
            *value < kMinimumWidgetBorderWidth ||
            *value > kMaximumWidgetBorderWidth)
            return std::optional<WidgetSettingsBackendResult>{BackendResult(
                WidgetSettingsBackendStatus::InvalidValue,
                std::string(errorCode))};
        if (!setAppearance(std::move(key), std::to_string(*value),
                appearanceError))
            return std::optional<WidgetSettingsBackendResult>{BackendResult(
                WidgetSettingsBackendStatus::InvalidValue,
                "appearanceWriteRejected", std::move(appearanceError))};
        return std::optional<WidgetSettingsBackendResult>{};
    };
    if (const auto error = setWidth("borderWidth", appearance.borderWidth,
            "invalidBorderWidth"))
        return *error;
    if (appearance.edgeHighlightEnabled &&
        !setAppearance("edgeHighlightEnabled",
            *appearance.edgeHighlightEnabled ? "1" : "0", appearanceError))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "appearanceWriteRejected", std::move(appearanceError));
    if (const auto error = setWidth("edgeHighlightWidth",
            appearance.edgeHighlightWidth, "invalidEdgeHighlightWidth"))
        return *error;
    if (appearance.glassEnabled &&
        !setAppearance("glassEnabled",
            *appearance.glassEnabled ? "1" : "0", appearanceError))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "appearanceWriteRejected", std::move(appearanceError));
    if (appearance.acrylicEnabled &&
        !setAppearance("acrylicEnabled",
            *appearance.acrylicEnabled ? "1" : "0", appearanceError))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "appearanceWriteRejected", std::move(appearanceError));
    if (appearance.contentTheme)
    {
        if (*appearance.contentTheme < 0 || *appearance.contentTheme > 1)
            return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                "invalidContentTheme");
        if (!setAppearance("__contentTheme",
                std::to_string(*appearance.contentTheme), appearanceError))
            return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                "appearanceWriteRejected", std::move(appearanceError));
    }
    else if (appearance.clearContentTheme &&
        !setAppearance("__contentTheme", "", appearanceError))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "appearanceWriteRejected", std::move(appearanceError));
    for (const auto& write : writes)
    {
        if (!keys.emplace(write.key).second)
            return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                "duplicateSettingWrite");
        const auto* declaration = FindDeclaredSetting(widget, write.key);
        if (!declaration)
            return BackendResult(
                WidgetSettingsBackendStatus::SettingNotFound,
                "settingNotFound");
        const auto converted =
            widget_settings_backend_detail::ConvertSetting(*declaration);
        const bool writesSearchQuery = write.searchQuery.has_value();
        if (writesSearchQuery)
        {
            if (converted.schema.Kind() != WidgetSettingKind::AppSearch ||
                converted.schema.searchKey.empty() ||
                converted.schema.searchKey == converted.schema.key ||
                converted.schema.searchKey == "__host" ||
                converted.schema.searchKey.starts_with("__host.") ||
                !keys.emplace(converted.schema.searchKey).second)
                return BackendResult(
                    WidgetSettingsBackendStatus::InvalidValue,
                    "invalidSearchKey");
            if (write.searchQuery->size() >
                    WidgetStorageTransaction::kMaximumValueBytes)
                return BackendResult(
                    WidgetSettingsBackendStatus::InvalidValue,
                    "searchQueryTooLong");
        }
        widget_settings_backend_detail::EncodedOrdinaryWrite encoded;
        std::string error;
        WidgetSettingFieldSchema writeSchema = converted.schema;
        if (writesSearchQuery)
            writeSchema.required = false;
        if (!widget_settings_backend_detail::EncodeOrdinaryWrite(
                writeSchema, write, encoded, error))
            return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                error.empty() ? "invalidValue" : error);
        bool changed = false;
        if (!transaction.Set(write.key, std::move(encoded.value),
                changed, error))
            return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                "storageWriteRejected", std::move(error));
        bool metadataChanged = false;
        const std::string metadata = TypedStorageMetadataKey(write.key);
        const bool metadataOk = encoded.typedMarker
            ? transaction.SetHostMetadata(metadata,
                std::string(TypedStorageMarker), metadataChanged, error)
            : transaction.RemoveHostMetadata(
                metadata, metadataChanged, error);
        if (!metadataOk)
            return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
                "storageMetadataRejected", std::move(error));
        if (writesSearchQuery)
        {
            bool queryChanged = false;
            const bool queryOk = write.searchQuery->empty()
                ? transaction.Remove(converted.schema.searchKey,
                    queryChanged, error)
                : transaction.Set(converted.schema.searchKey,
                    *write.searchQuery, queryChanged, error);
            if (!queryOk)
                return BackendResult(
                    WidgetSettingsBackendStatus::InvalidValue,
                    "searchQueryWriteRejected", std::move(error));
            bool queryMetadataChanged = false;
            if (!transaction.RemoveHostMetadata(
                    TypedStorageMetadataKey(converted.schema.searchKey),
                    queryMetadataChanged, error))
                return BackendResult(
                    WidgetSettingsBackendStatus::InvalidValue,
                    "searchQueryMetadataRejected", std::move(error));
        }
    }
    std::string error;
    if (!transaction.ValidateCommit(error))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "storageQuotaExceeded", std::move(error));
    if (!transaction.Changed()) return Success(false);
    std::vector<std::string> affectedKeys(keys.begin(), keys.end());
    auto candidate = transaction.TakeCandidate();
    if (!persist)
    {
        if (!preview_)
        {
            preview_ = std::make_unique<PreviewState>();
            preview_->descriptor = descriptor;
        }
        const std::string prefix = WideToUtf8(widget.widgetId) + ".";
        for (const auto& [key, value] : storage)
        {
            if (!key.starts_with(prefix)) continue;
            const auto next = candidate.find(key);
            if (next == candidate.end() || next->second != value)
                preview_->originals.try_emplace(key, value);
        }
        for (const auto& [key, value] : candidate)
        {
            (void)value;
            if (!key.starts_with(prefix) || storage.contains(key))
                continue;
            preview_->originals.try_emplace(key, std::nullopt);
        }
        preview_->affectedKeys.insert(keys.begin(), keys.end());
        storage.swap(candidate);
        engine_.RuntimeNotifySettingsChanged(widgetId, affectedKeys, true);
        engine_.RuntimeInvalidateHost(widgetId);
        return Success();
    }

    storage.swap(candidate);
    if (!engine_.PersistWidgetSettingsStorageForBackend())
    {
        storage.swap(candidate);
        return BackendResult(
            WidgetSettingsBackendStatus::PersistenceFailed,
            "storagePersistenceFailed");
    }
    engine_.RuntimeNotifySettingsChanged(
        widgetId, std::move(affectedKeys), false);
    engine_.RuntimeInvalidateHost(widgetId);
    return Success();
}

WidgetSettingsBackendResult WidgetEngineSettingsBackend::CommitPreview(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    if (!preview_) return Success(false);
    if (preview_->descriptor.widgetId != descriptor.widgetId ||
        preview_->descriptor.packageId != descriptor.packageId ||
        preview_->descriptor.generation != descriptor.generation)
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "stalePreview");
    if (!engine_.PersistWidgetSettingsStorageForBackend())
        return BackendResult(WidgetSettingsBackendStatus::PersistenceFailed,
            "storagePersistenceFailed");
    preview_.reset();
    engine_.RuntimeInvalidateHost(widget.widgetId);
    return Success();
}

WidgetSettingsBackendResult WidgetEngineSettingsBackend::RevertPreview(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    if (!preview_) return Success(false);
    if (preview_->descriptor.widgetId != descriptor.widgetId ||
        preview_->descriptor.packageId != descriptor.packageId ||
        preview_->descriptor.generation != descriptor.generation)
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "stalePreview");
    std::vector<std::string> affectedKeys(
        preview_->affectedKeys.begin(), preview_->affectedKeys.end());
    RestorePreviewNoexcept();
    engine_.RuntimeNotifySettingsChanged(
        widget.widgetId, std::move(affectedKeys), false);
    return Success();
}

void WidgetEngineSettingsBackend::RestorePreviewNoexcept() noexcept
{
    if (!preview_) return;
    try
    {
        auto& storage = engine_.WidgetSettingsPersistentStorageForBackend();
        for (const auto& [key, value] : preview_->originals)
        {
            if (value)
                storage.insert_or_assign(key, *value);
            else
                storage.erase(key);
        }
        const std::wstring widgetId = preview_->descriptor.widgetId;
        preview_.reset();
        if (!widgetId.empty()) engine_.RuntimeInvalidateHost(widgetId);
    }
    catch (...)
    {
        preview_.reset();
    }
}

WidgetSettingsBackendResult WidgetEngineSettingsBackend::SetSecret(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetSettingFieldSchema& field, std::string_view plaintext)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    const auto* declaration = FindDeclaredSetting(widget, field.key);
    if (!declaration)
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    if (!SchemaMatches(*declaration, field) ||
        declaration->type != "password")
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSettingSchema");
    if (widget.preview || !engine_.secretStore_)
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "secretStoreUnavailable");
    if (plaintext.empty()) return ClearSecret(descriptor, guard, field);
    if (plaintext.size() > WidgetSecretStore::MaximumSecretBytes)
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "secretTooLarge");
    std::string reference;
    std::string error;
    if (!engine_.secretStore_->Set(widget.packageId,
            WideToUtf8(widget.widgetId), field.key, plaintext,
            reference, error))
        return BackendResult(WidgetSettingsBackendStatus::PersistenceFailed,
            "secretPersistenceFailed", std::move(error));
    engine_.RuntimeInvalidateHost(widget.widgetId);
    return Success();
}

WidgetSettingsBackendResult WidgetEngineSettingsBackend::ClearSecret(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetSettingFieldSchema& field)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    const auto* declaration = FindDeclaredSetting(widget, field.key);
    if (!declaration)
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    if (!SchemaMatches(*declaration, field) ||
        declaration->type != "password")
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSettingSchema");
    if (widget.preview || !engine_.secretStore_)
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "secretStoreUnavailable");
    bool removed = false;
    std::string error;
    if (!engine_.secretStore_->RemoveSetting(widget.packageId,
            WideToUtf8(widget.widgetId), field.key, removed, error))
        return BackendResult(WidgetSettingsBackendStatus::PersistenceFailed,
            "secretPersistenceFailed", std::move(error));
    if (removed) engine_.RuntimeInvalidateHost(widget.widgetId);
    return Success(removed);
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::ChooseFilesystemHandle(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetSettingFieldSchema& field)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    const auto* declaration = FindDeclaredSetting(widget, field.key);
    if (!declaration)
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    if (!SchemaMatches(*declaration, field) ||
        !IsFilesystemHandleType(declaration->type))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSettingSchema");
    if (widget.preview || !engine_.filePickerCallback_ ||
        !engine_.filesystemHandleStore_ || HasStorageOverlay())
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "filesystemPickerUnavailable");
    const auto access = HandleAccess(declaration->access);
    std::vector<std::string> extensions;
    if (!access || !NormalizeFilesystemSettingExtensions(
            declaration->extensions, extensions))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "invalidFilesystemHandleDeclaration");
    const std::wstring widgetId = widget.widgetId;
    const std::string packageId = widget.packageId;
    const std::uint64_t generation = widget.runtimeToken;
    const std::string settingType = declaration->type;

    LuaWidgetFilePickerRequest request;
    request.access = *access;
    if (settingType == "folderHandle")
        request.kind = LuaWidgetFilePickerKind::Folder;
    else if (*access == WidgetFilesystemHandleAccess::Write)
        request.kind = LuaWidgetFilePickerKind::SaveFile;
    else
        request.kind = LuaWidgetFilePickerKind::OpenFile;
    for (const auto& extension : extensions)
        request.extensions.push_back(Utf8ToWide(extension));

    LuaWidgetFilePickerResult selected;
    try
    {
        selected = engine_.filePickerCallback_(request);
    }
    catch (...)
    {
        selected.error = "pickerFailed";
    }
    if (!selected)
        return selected.canceled
            ? Success(false)
            : BackendResult(WidgetSettingsBackendStatus::Failed,
                selected.error.empty() ? "pickerFailed" : selected.error);

    const int currentIndex = engine_.FindWidget(widgetId);
    if (currentIndex < 0 ||
        engine_.widgets_[currentIndex].runtimeToken != generation ||
        engine_.widgets_[currentIndex].packageId != packageId ||
        !engine_.filesystemHandleStore_)
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");

    const auto kind = settingType == "folderHandle"
        ? WidgetFilesystemHandleKind::Folder
        : WidgetFilesystemHandleKind::File;
    const std::string instanceId = WideToUtf8(widgetId);
    auto grant = engine_.filesystemHandleStore_->Grant(
        { instanceId, packageId }, selected.path, kind,
        *access, false);
    if (!grant)
        return BackendResult(WidgetSettingsBackendStatus::PersistenceFailed,
            "filesystemHandleGrantFailed", std::move(grant.error));

    auto& storage = engine_.WidgetSettingsPersistentStorageForBackend();
    const std::string metadata = HandleMetadataKey(field.key);
    const auto old = storage.find(instanceId + "." + metadata);
    const std::string oldHandle = old == storage.end()
        ? std::string{} : old->second;
    WidgetStorageTransaction transaction(storage, instanceId);
    bool changed = false;
    std::string error;
    if (!transaction.SetHostMetadata(metadata, grant.entry->handle,
            changed, error) || !transaction.ValidateCommit(error))
    {
        std::string revokeError;
        (void)engine_.filesystemHandleStore_->Revoke(
            { instanceId, packageId }, grant.entry->handle,
            revokeError);
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "filesystemHandleMetadataRejected", std::move(error));
    }
    auto previous = transaction.TakeCandidate();
    storage.swap(previous);
    if (!engine_.PersistWidgetSettingsStorageForBackend())
    {
        storage.swap(previous);
        std::string revokeError;
        (void)engine_.filesystemHandleStore_->Revoke(
            { instanceId, packageId }, grant.entry->handle,
            revokeError);
        return BackendResult(WidgetSettingsBackendStatus::PersistenceFailed,
            "storagePersistenceFailed");
    }

    if (!oldHandle.empty() && oldHandle != grant.entry->handle)
    {
        std::vector<std::uint64_t> taskIds;
        for (const auto& [taskId, taskHandle] :
            engine_.filesystemTaskHandles_)
            if (taskHandle == oldHandle) taskIds.push_back(taskId);
        for (const auto taskId : taskIds)
            (void)engine_.RuntimeCancelTask(
                widgetId, generation, taskId);
        std::vector<std::uint64_t> subscriptionIds;
        for (const auto& [subscriptionId, binding] :
            engine_.filesystemWatchBindings_)
            if (binding.sourceHandle == oldHandle)
                subscriptionIds.push_back(subscriptionId);
        for (const auto subscriptionId : subscriptionIds)
            (void)engine_.RuntimeUnsubscribeData(subscriptionId);
        std::string revokeError;
        if (!engine_.filesystemHandleStore_->Revoke(
                { instanceId, packageId }, oldHandle,
                revokeError) && !revokeError.empty())
            engine_.RuntimeRecordError(widgetId,
                "settings filesystem handle cleanup: " + revokeError);
    }
    engine_.RuntimeInvalidateHost(widgetId);
    return Success();
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::ClearFilesystemHandle(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetSettingFieldSchema& field)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    const auto* declaration = FindDeclaredSetting(widget, field.key);
    if (!declaration)
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    if (!SchemaMatches(*declaration, field) ||
        !IsFilesystemHandleType(declaration->type))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSettingSchema");
    if (widget.preview || !engine_.filesystemHandleStore_ ||
        HasStorageOverlay())
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "filesystemHandleStoreUnavailable");

    const std::string instanceId = WideToUtf8(widget.widgetId);
    const std::string metadata = HandleMetadataKey(field.key);
    auto& storage = engine_.WidgetSettingsPersistentStorageForBackend();
    const auto old = storage.find(instanceId + "." + metadata);
    if (old == storage.end() || old->second.empty()) return Success(false);
    const std::string oldHandle = old->second;
    WidgetStorageTransaction transaction(storage, instanceId);
    bool changed = false;
    std::string error;
    if (!transaction.RemoveHostMetadata(metadata, changed, error) ||
        !transaction.ValidateCommit(error))
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "filesystemHandleMetadataRejected", std::move(error));
    auto previous = transaction.TakeCandidate();
    storage.swap(previous);
    if (!engine_.PersistWidgetSettingsStorageForBackend())
    {
        storage.swap(previous);
        return BackendResult(WidgetSettingsBackendStatus::PersistenceFailed,
            "storagePersistenceFailed");
    }

    std::vector<std::uint64_t> taskIds;
    for (const auto& [taskId, taskHandle] : engine_.filesystemTaskHandles_)
        if (taskHandle == oldHandle) taskIds.push_back(taskId);
    for (const auto taskId : taskIds)
        (void)engine_.RuntimeCancelTask(
            widget.widgetId, widget.runtimeToken, taskId);
    std::vector<std::uint64_t> subscriptionIds;
    for (const auto& [subscriptionId, binding] :
        engine_.filesystemWatchBindings_)
        if (binding.sourceHandle == oldHandle)
            subscriptionIds.push_back(subscriptionId);
    for (const auto subscriptionId : subscriptionIds)
        (void)engine_.RuntimeUnsubscribeData(subscriptionId);
    std::string revokeError;
    if (!engine_.filesystemHandleStore_->Revoke(
            { instanceId, widget.packageId }, oldHandle, revokeError) &&
        !revokeError.empty())
        engine_.RuntimeRecordError(widget.widgetId,
            "settings filesystem handle cleanup: " + revokeError);
    engine_.RuntimeInvalidateHost(widget.widgetId);
    return Success();
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::OpenEntityReferencePicker(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetSettingFieldSchema& field)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    const auto* setting = FindDeclaredSetting(widget, field.key);
    if (!setting)
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    if (!SchemaMatches(*setting, field) ||
        !IsEntityReferenceType(setting->type))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSettingSchema");
    const auto declaration =
        widget.logicalSlots.Declarations().find(setting->binding);
    const auto* snapshot = widget.logicalSlots.Find(setting->binding);
    if (widget.preview || !engine_.logicalSlotPickerCallback_ ||
        declaration == widget.logicalSlots.Declarations().end() ||
        !snapshot || snapshot->kind != LogicalSlotKind::Binding)
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "entityReferencePickerUnavailable");
    const std::string expectedKind(EntityReferenceKind(setting->type));
    if (std::find(declaration->second.accepts.begin(),
            declaration->second.accepts.end(), expectedKind) ==
        declaration->second.accepts.end())
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "invalidEntityReferenceBinding");
    LogicalSlotPickerRequest request;
    request.widgetId = widget.widgetId;
    request.slotId = setting->binding;
    request.kind = LogicalSlotKind::Binding;
    request.accepts = { expectedKind };
    request.referenceType =
        std::string(EntityReferenceTypeFilter(setting->type));
    request.targetIndex = 0;
    try
    {
        if (!engine_.logicalSlotPickerCallback_(request))
            return BackendResult(WidgetSettingsBackendStatus::Unavailable,
                "entityReferencePickerUnavailable");
        const int currentIndex = engine_.FindWidget(descriptor.widgetId);
        if (currentIndex < 0 ||
            engine_.widgets_[currentIndex].runtimeToken !=
                descriptor.generation ||
            engine_.widgets_[currentIndex].packageId !=
                descriptor.packageId)
            return BackendResult(
                WidgetSettingsBackendStatus::StaleSnapshot,
                "staleSnapshot");
        return Success();
    }
    catch (...)
    {
        return BackendResult(WidgetSettingsBackendStatus::Failed,
            "entityReferencePickerFailed");
    }
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::ClearEntityReference(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetSettingFieldSchema& field)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    const auto* setting = FindDeclaredSetting(widget, field.key);
    if (!setting)
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    if (!SchemaMatches(*setting, field) ||
        !IsEntityReferenceType(setting->type))
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSettingSchema");
    const auto declaration =
        widget.logicalSlots.Declarations().find(setting->binding);
    const auto* snapshot = widget.logicalSlots.Find(setting->binding);
    if (widget.preview ||
        declaration == widget.logicalSlots.Declarations().end() ||
        !snapshot || snapshot->kind != LogicalSlotKind::Binding ||
        !declaration->second.allowClear)
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "entityReferenceClearUnavailable");
    if (snapshot->items.empty()) return Success(false);
    LogicalSlotChange change;
    std::string error;
    if (!engine_.RuntimeRemoveHostLogicalSlotItem(widget.widgetId,
            setting->binding, snapshot->items.front().id, change, error,
            "host.settings.winui"))
        return BackendResult(WidgetSettingsBackendStatus::PersistenceFailed,
            "entityReferenceClearFailed", std::move(error));
    return Success(change.operation != "unchanged");
}

WidgetSettingsBackendResult WidgetEngineSettingsBackend::StartSearch(
    WidgetSettingSearchRequest request, SearchCompletion completion)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(request.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (request.generation == 0 || request.requestId == 0 ||
        request.packageId != widget.packageId ||
        request.generation != widget.runtimeToken)
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    const auto* setting = FindDeclaredSetting(widget, request.settingKey);
    if (!setting || (setting->type != "appSearch" &&
            setting->type != "appReference"))
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    if (!completion || !engine_.applicationCatalogProvider_)
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "applicationCatalogUnavailable");

    LuaApplicationCatalogSnapshot catalog;
    try
    {
        catalog = engine_.applicationCatalogProvider_();
    }
    catch (...)
    {
        return BackendResult(WidgetSettingsBackendStatus::Failed,
            "applicationCatalogFailed");
    }
    if (catalog.state != "ready")
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            catalog.state == "indexing" ? "appIndexNotReady"
                                        : "applicationCatalogUnavailable");
    const int currentIndex = engine_.FindWidget(request.widgetId);
    if (currentIndex < 0 ||
        engine_.widgets_[currentIndex].runtimeToken !=
            request.generation ||
        engine_.widgets_[currentIndex].packageId != request.packageId)
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    if (catalog.entries.size() > 20000)
        catalog.entries.resize(20000);
    request.maximumResults = std::clamp<std::size_t>(
        request.maximumResults, 1, 64);
    const std::wstring queryWide = Utf8ToWide(request.query);
    SearchState::Work work;
    work.request = request;
    work.completion = std::move(completion);
    work.foldedQuery = WideToUtf8(ToUpperInvariant(queryWide));
    work.pinyinQuery = BuildNamePinyinFullKey(queryWide);
    work.catalog = std::move(catalog.entries);
    if (!searches_->Start(std::move(work)))
        return BackendResult(WidgetSettingsBackendStatus::Failed,
            "searchTaskUnavailable");
    return Success();
}

WidgetSettingsBackendResult WidgetEngineSettingsBackend::CancelSearch(
    const WidgetSettingSearchRequest& request)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    return searches_->Cancel(request);
}

WidgetSettingsBackendResult
WidgetEngineSettingsBackend::CommitSearchResult(
    const WidgetSettingsBackendDescriptor& descriptor,
    const WidgetSettingMutationGuard& guard,
    const WidgetSettingSearchRequest& request,
    std::string_view resultId)
{
    if (GetCurrentThreadId() != ownerThreadId_) return WrongThread();
    const int index = engine_.FindWidget(descriptor.widgetId);
    if (index < 0)
        return BackendResult(WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound");
    const LuaWidget& widget = engine_.widgets_[index];
    if (!widget_settings_backend_detail::MutationIdentityMatches(
            descriptor, guard, widget.widgetId, widget.packageId,
            widget.runtimeToken) || request.widgetId != descriptor.widgetId ||
        request.packageId != descriptor.packageId ||
        request.generation != descriptor.generation)
    {
        searches_->Erase(request);
        return BackendResult(WidgetSettingsBackendStatus::StaleSnapshot,
            "staleSnapshot");
    }
    const auto* setting = FindDeclaredSetting(widget, request.settingKey);
    if (!setting || (setting->type != "appSearch" &&
            setting->type != "appReference"))
        return BackendResult(WidgetSettingsBackendStatus::SettingNotFound,
            "settingNotFound");
    const auto selected = searches_->FindResult(request, resultId);
    if (!selected)
        return BackendResult(WidgetSettingsBackendStatus::InvalidValue,
            "searchResultNotFound");

    WidgetSettingsBackendResult result;
    if (widget.preview)
        return BackendResult(WidgetSettingsBackendStatus::Unavailable,
            "previewReadOnly");
    if (setting->type == "appSearch")
    {
        WidgetSettingOrdinaryWrite write;
        write.key = setting->key;
        write.value = MakeWidgetSettingString(selected->title);
        write.searchQuery = request.query;
        result = ApplyOrdinaryTransaction(
            descriptor, guard, { std::move(write) });
    }
    else
    {
        const auto declaration =
            widget.logicalSlots.Declarations().find(setting->binding);
        const auto* snapshot = widget.logicalSlots.Find(setting->binding);
        if (declaration == widget.logicalSlots.Declarations().end() ||
            !snapshot || snapshot->kind != LogicalSlotKind::Binding)
            return BackendResult(WidgetSettingsBackendStatus::Unavailable,
                "invalidEntityReferenceBinding");
        LogicalSlotItem candidate;
        candidate.kind = "app.reference";
        candidate.title = selected->title;
        candidate.source = "host.settings.winui";
        candidate.type = selected->type;
        candidate.target = selected->launchTarget;
        candidate.available = true;
        LogicalSlotChange change;
        std::string error;
        result = engine_.RuntimeBindHostLogicalSlot(widget.widgetId,
                setting->binding, std::move(candidate), 0, change, error,
                "host.settings.winui")
            ? Success(change.operation != "unchanged")
            : BackendResult(
                WidgetSettingsBackendStatus::PersistenceFailed,
                "entityReferenceCommitFailed", std::move(error));
    }
    if (result.Succeeded()) searches_->Erase(request);
    return result;
}

std::unique_ptr<IWidgetSettingsBackend>
CreateWidgetEngineSettingsBackend(WidgetEngine& engine)
{
    return std::make_unique<WidgetEngineSettingsBackend>(engine);
}
}
