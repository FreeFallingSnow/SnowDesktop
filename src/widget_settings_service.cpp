#include "widget_settings_service.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
WidgetSettingMutationStatus MutationStatusFor(
    WidgetSettingsBackendStatus status) noexcept
{
    switch (status)
    {
    case WidgetSettingsBackendStatus::Succeeded:
        return WidgetSettingMutationStatus::Applied;
    case WidgetSettingsBackendStatus::Unchanged:
        return WidgetSettingMutationStatus::Unchanged;
    case WidgetSettingsBackendStatus::Unavailable:
        return WidgetSettingMutationStatus::Unavailable;
    case WidgetSettingsBackendStatus::WidgetNotFound:
        return WidgetSettingMutationStatus::WidgetNotFound;
    case WidgetSettingsBackendStatus::SettingNotFound:
        return WidgetSettingMutationStatus::SettingNotFound;
    case WidgetSettingsBackendStatus::StaleSnapshot:
        return WidgetSettingMutationStatus::StaleSnapshot;
    case WidgetSettingsBackendStatus::InvalidValue:
        return WidgetSettingMutationStatus::InvalidValue;
    case WidgetSettingsBackendStatus::PersistenceFailed:
        return WidgetSettingMutationStatus::PersistenceFailed;
    default:
        return WidgetSettingMutationStatus::Failed;
    }
}

WidgetSettingMutationResult MutationResultFor(
    const WidgetSettingsBackendResult& result)
{
    WidgetSettingMutationResult mapped;
    mapped.status = MutationStatusFor(result.status);
    mapped.errorCode = result.errorCode;
    mapped.message = result.message;
    return mapped;
}

WidgetSettingsLoadResult LoadResultFor(
    const WidgetSettingsBackendResult& result)
{
    WidgetSettingsLoadResult mapped;
    mapped.status = MutationStatusFor(result.status);
    mapped.errorCode = result.errorCode;
    mapped.message = result.message;
    return mapped;
}

WidgetSettingsBackendResult Failed(
    WidgetSettingsBackendStatus status, std::string errorCode,
    std::string message = {})
{
    return { status, std::move(errorCode), std::move(message) };
}

constexpr std::size_t MaximumSearchQueryBytes = 8192;

bool IsValidUtf8(std::string_view value) noexcept
{
    std::size_t index = 0;
    while (index < value.size())
    {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        if (lead <= 0x7f)
        {
            length = 1;
            codePoint = lead;
        }
        else if (lead >= 0xc2 && lead <= 0xdf)
        {
            length = 2;
            codePoint = lead & 0x1f;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            length = 3;
            codePoint = lead & 0x0f;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            length = 4;
            codePoint = lead & 0x07;
        }
        else
            return false;
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && codePoint < 0x80) ||
            (length == 3 && codePoint < 0x800) ||
            (length == 4 && codePoint < 0x10000) ||
            codePoint > 0x10ffff ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff))
            return false;
        index += length;
    }
    return true;
}

bool IsValidOrdinarySearchKey(std::string_view key) noexcept
{
    return !key.empty() && key.size() <= 128 &&
        key.find('\0') == std::string_view::npos && IsValidUtf8(key) &&
        key != "__host" && !key.starts_with("__host.");
}

InteractionValue EmptyDefault(const WidgetSettingFieldSchema& schema)
{
    switch (schema.Kind())
    {
    case WidgetSettingKind::Boolean:
        return MakeWidgetSettingBoolean(false);
    case WidgetSettingKind::Integer:
    case WidgetSettingKind::Color:
    {
        long long value = 0;
        if (schema.Kind() == WidgetSettingKind::Integer &&
            schema.minimum > 0.0 &&
            !ReadWidgetSettingInteger(schema.minimum, value))
            value = 0;
        return MakeWidgetSettingInteger(value);
    }
    case WidgetSettingKind::FloatingPoint:
        return MakeWidgetSettingNumber(
            std::clamp(0.0, schema.minimum, schema.maximum));
    case WidgetSettingKind::Range:
        return MakeWidgetSettingNumber(SnapRangeSettingValue(
            schema.minimum, schema.minimum, schema.maximum, schema.step));
    case WidgetSettingKind::MultiSelect:
        return MakeWidgetSettingStringArray({});
    default:
        return MakeWidgetSettingString({});
    }
}

const WidgetSettingFieldState* FindField(
    const WidgetSettingsSnapshot& snapshot, std::string_view key)
{
    const auto field = std::find_if(snapshot.fields.begin(),
        snapshot.fields.end(), [&](const WidgetSettingFieldState& value) {
            return value.schema.key == key;
        });
    return field == snapshot.fields.end() ? nullptr : &*field;
}

WidgetSettingFieldState* FindField(
    WidgetSettingsSnapshot& snapshot, std::string_view key)
{
    const auto field = std::find_if(snapshot.fields.begin(),
        snapshot.fields.end(), [&](const WidgetSettingFieldState& value) {
            return value.schema.key == key;
        });
    return field == snapshot.fields.end() ? nullptr : &*field;
}

struct BuiltSnapshot
{
    WidgetSettingsBackendDescriptor descriptor;
    WidgetSettingsSnapshot snapshot;
};

WidgetSettingsBackendResult BuildSnapshot(
    IWidgetSettingsBackend& backend, std::wstring_view widgetId,
    BuiltSnapshot& built)
{
    WidgetSettingsBackendDescriptor descriptor;
    WidgetSettingsBackendResult described =
        backend.Describe(widgetId, descriptor);
    if (!described.Succeeded()) return described;
    if (descriptor.widgetId.empty() || descriptor.widgetId != widgetId ||
        descriptor.packageId.empty() || descriptor.generation == 0)
        return Failed(WidgetSettingsBackendStatus::Failed,
            "invalidWidgetDescriptor");

    WidgetSettingsSnapshot snapshot;
    snapshot.widgetId = descriptor.widgetId;
    snapshot.packageId = descriptor.packageId;
    snapshot.widgetName = descriptor.widgetName;
    snapshot.generation = descriptor.generation;
    snapshot.preview = descriptor.preview;
    snapshot.customStyle = descriptor.customStyle;
    snapshot.hostAppearance = descriptor.hostAppearance;

    std::vector<WidgetSettingSourceField> declarations =
        descriptor.manifestFields;
    declarations.insert(declarations.end(),
        descriptor.scriptFields.begin(), descriptor.scriptFields.end());
    snapshot.groups = descriptor.manifestGroups;
    snapshot.groups.insert(snapshot.groups.end(),
        descriptor.scriptGroups.begin(), descriptor.scriptGroups.end());
    snapshot.presets = descriptor.manifestPresets;
    snapshot.presets.insert(snapshot.presets.end(),
        descriptor.scriptPresets.begin(), descriptor.scriptPresets.end());

    std::unordered_set<std::string> fieldKeys;
    for (const auto& declaration : declarations)
        if (declaration.schema.key.empty() ||
            !fieldKeys.emplace(declaration.schema.key).second)
            return Failed(WidgetSettingsBackendStatus::Failed,
                "duplicateSettingKey");
    std::unordered_set<std::string> groupIds;
    for (const auto& group : snapshot.groups)
        if (group.id.empty() || !groupIds.emplace(group.id).second)
            return Failed(WidgetSettingsBackendStatus::Failed,
                "duplicateSettingGroup");
    std::unordered_set<std::string> presetIds;
    for (const auto& preset : snapshot.presets)
        if (preset.id.empty() || !presetIds.emplace(preset.id).second)
            return Failed(WidgetSettingsBackendStatus::Failed,
                "duplicateSettingPreset");

    std::unordered_set<std::string> searchKeys;
    std::vector<WidgetSettingFieldSchema> schemas;
    schemas.reserve(declarations.size());
    for (const auto& declaration : declarations)
    {
        if (declaration.schema.Kind() == WidgetSettingKind::AppSearch &&
            (!IsValidOrdinarySearchKey(declaration.schema.searchKey) ||
                fieldKeys.contains(declaration.schema.searchKey) ||
                !searchKeys.emplace(declaration.schema.searchKey).second))
            return Failed(WidgetSettingsBackendStatus::InvalidValue,
                "invalidSearchKey");
        schemas.push_back(declaration.schema);
    }

    std::vector<WidgetSettingPresetSchema> normalizedPresets;
    normalizedPresets.reserve(snapshot.presets.size());
    for (const auto& preset : snapshot.presets)
    {
        WidgetSettingPresetSchema normalized;
        std::string error;
        if (!NormalizeWidgetSettingPreset(
                schemas, preset, normalized, error))
            return Failed(WidgetSettingsBackendStatus::InvalidValue,
                error.empty() ? "invalidPreset" : error);
        normalizedPresets.push_back(std::move(normalized));
    }
    snapshot.presets = std::move(normalizedPresets);

    const WidgetSettingPresetSchema* defaultPreset = nullptr;
    for (const auto& preset : snapshot.presets)
        if (preset.isDefault || preset.id == "default")
        {
            defaultPreset = &preset;
            snapshot.defaultPresetId = preset.id;
            break;
        }

    snapshot.fields.reserve(declarations.size());
    for (const auto& declaration : declarations)
    {
        WidgetSettingFieldState field;
        field.schema = declaration.schema;
        if (field.schema.Channel() == WidgetSettingValueChannel::Ordinary)
        {
            InteractionValue authoredDefault = declaration.defaultValue;
            if (authoredDefault.type == InteractionValue::Type::Null)
                authoredDefault = EmptyDefault(field.schema);
            if (defaultPreset)
                if (const auto value = defaultPreset->values.find(
                        field.schema.key);
                    value != defaultPreset->values.end())
                    authoredDefault = value->second;

            std::string error;
            if (!NormalizeWidgetSettingValue(field.schema,
                    authoredDefault, field.defaultValue, error))
                field.defaultValue = authoredDefault;

            WidgetSettingBackendReadResult read = backend.ReadOrdinary(
                descriptor, field.schema,
                WidgetSettingUsesTypedStorage(field.schema.Kind()));
            if (!read.result.Succeeded()) return read.result;
            field.hasStoredValue = read.hasStoredValue;
            InteractionValue current = read.hasStoredValue
                ? read.value : field.defaultValue;
            if (!NormalizeWidgetSettingValue(
                    field.schema, current, field.currentValue, error))
                field.currentValue = std::move(current);

            if (field.schema.Kind() == WidgetSettingKind::AppSearch)
            {
                WidgetSettingBackendReadResult query =
                    backend.ReadSearchQuery(descriptor, field.schema);
                if (!query.result.Succeeded()) return query.result;
                if (query.hasStoredValue)
                {
                    if (query.value.type != InteractionValue::Type::String)
                        return Failed(
                            WidgetSettingsBackendStatus::PersistenceFailed,
                            "invalidSearchQueryStorage");
                    field.searchQuery = std::move(query.value.string);
                }
            }
        }
        else
        {
            WidgetSettingBackendOpaqueResult read =
                backend.ReadOpaque(descriptor, field.schema);
            if (!read.result.Succeeded()) return read.result;
            field.opaque = std::move(read.state);
            if (field.schema.Kind() == WidgetSettingKind::Password)
                field.opaque.displayLabel.clear();
        }
        snapshot.fields.push_back(std::move(field));
    }

    PrepareWidgetSettingsSnapshot(snapshot);
    built.descriptor = std::move(descriptor);
    built.snapshot = std::move(snapshot);
    return { WidgetSettingsBackendStatus::Succeeded, {}, {} };
}

template<typename Callback, typename Hint>
void NotifyNoexcept(const Callback& callback, Hint hint) noexcept
{
    if (!callback) return;
    try
    {
        callback(std::move(hint));
    }
    catch (...)
    {
        // Observers must never alter service results or terminate a backend
        // worker which delivered an asynchronous completion.
    }
}
}

struct WidgetSettingsService::State
{
    struct SearchState
    {
        WidgetSettingSearchRequest request;
        WidgetSettingSearchSnapshot snapshot;
    };

    struct Session
    {
        WidgetSettingsBackendDescriptor descriptor;
        WidgetSettingsSnapshot snapshot;
        std::unordered_map<std::string, SearchState> searches;
    };

    struct EventCallbacks
    {
        SnapshotChangedCallback snapshotChanged;
        SearchCompletedCallback searchCompleted;
    };

    explicit State(IWidgetSettingsBackend& value) : backend(value) {}

    IWidgetSettingsBackend& backend;
    mutable std::mutex mutex;
    std::unordered_map<std::wstring, Session> sessions;
    // Kept after Close/CloseAll so an old guard can never become valid again
    // when the same runtime generation is opened in a new view session.
    std::unordered_map<std::wstring, std::uint64_t>
        lastPublishedRevisions;
    std::shared_ptr<const EventCallbacks> callbacks;
    std::uint64_t nextSearchRequestId = 0;
};

WidgetSettingsService::WidgetSettingsService(IWidgetSettingsBackend& backend)
    : state_(std::make_shared<State>(backend))
{
}

WidgetSettingsService::~WidgetSettingsService()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->callbacks.reset();
    }
    CloseAll();
}

void WidgetSettingsService::SetEventCallbacks(
    SnapshotChangedCallback snapshotChanged,
    SearchCompletedCallback searchCompleted)
{
    std::shared_ptr<const State::EventCallbacks> callbacks;
    if (snapshotChanged || searchCompleted)
    {
        auto value = std::make_shared<State::EventCallbacks>();
        value->snapshotChanged = std::move(snapshotChanged);
        value->searchCompleted = std::move(searchCompleted);
        callbacks = std::move(value);
    }
    std::lock_guard lock(state_->mutex);
    state_->callbacks = std::move(callbacks);
}

WidgetSettingsLoadResult WidgetSettingsService::Load(std::wstring widgetId)
{
    if (widgetId.empty())
        return LoadResultFor(Failed(
            WidgetSettingsBackendStatus::WidgetNotFound,
            "widgetNotFound"));

    BuiltSnapshot built;
    WidgetSettingsBackendResult result =
        BuildSnapshot(state_->backend, widgetId, built);
    if (!result.Succeeded()) return LoadResultFor(result);

    WidgetSettingsLoadResult loaded;
    loaded.status = WidgetSettingMutationStatus::Applied;
    std::vector<WidgetSettingSearchRequest> replacedRequests;
    std::shared_ptr<const State::EventCallbacks> callbacks;
    WidgetSettingsSnapshotChanged changed;
    {
        std::lock_guard lock(state_->mutex);
        auto& lastRevision =
            state_->lastPublishedRevisions[widgetId];
        if (lastRevision ==
            (std::numeric_limits<std::uint64_t>::max)())
        {
            return LoadResultFor(Failed(
                WidgetSettingsBackendStatus::Failed,
                "widgetSettingsRevisionExhausted"));
        }
        built.snapshot.revision = ++lastRevision;
        auto existing = state_->sessions.find(widgetId);
        if (existing != state_->sessions.end() &&
            existing->second.snapshot.generation ==
                built.snapshot.generation)
        {
            for (auto search = existing->second.searches.begin();
                search != existing->second.searches.end();)
            {
                const auto* previousField = FindField(
                    existing->second.snapshot, search->first);
                const auto* field = FindField(
                    built.snapshot, search->first);
                const bool stillValid = previousField && field &&
                    previousField->schema == field->schema && field->enabled &&
                    ((field->schema.Kind() == WidgetSettingKind::AppSearch &&
                         field->searchQuery == search->second.request.query) ||
                        field->schema.Kind() ==
                            WidgetSettingKind::AppReference);
                if (stillValid)
                {
                    ++search;
                    continue;
                }
                replacedRequests.push_back(search->second.request);
                search = existing->second.searches.erase(search);
            }
            existing->second.descriptor = std::move(built.descriptor);
            existing->second.snapshot = std::move(built.snapshot);
            loaded.snapshot = existing->second.snapshot;
        }
        else
        {
            if (existing != state_->sessions.end())
            {
                for (const auto& [key, search] :
                    existing->second.searches)
                {
                    (void)key;
                    replacedRequests.push_back(search.request);
                }
            }
            State::Session session;
            session.descriptor = std::move(built.descriptor);
            session.snapshot = std::move(built.snapshot);
            loaded.snapshot = session.snapshot;
            state_->sessions.insert_or_assign(
                widgetId, std::move(session));
        }
        changed = { loaded.snapshot->widgetId,
            loaded.snapshot->generation, loaded.snapshot->revision };
        callbacks = state_->callbacks;
    }
    for (const auto& request : replacedRequests)
        (void)state_->backend.CancelSearch(request);
    if (callbacks)
        NotifyNoexcept(callbacks->snapshotChanged, std::move(changed));
    return loaded;
}

WidgetSettingsLoadResult WidgetSettingsService::Reload(
    std::wstring_view widgetId)
{
    return Load(std::wstring(widgetId));
}

std::optional<WidgetSettingsSnapshot> WidgetSettingsService::Snapshot(
    std::wstring_view widgetId) const
{
    std::lock_guard lock(state_->mutex);
    const auto found = state_->sessions.find(std::wstring(widgetId));
    return found == state_->sessions.end()
        ? std::nullopt
        : std::optional<WidgetSettingsSnapshot>(found->second.snapshot);
}

void WidgetSettingsService::Close(std::wstring_view widgetId)
{
    std::vector<WidgetSettingSearchRequest> requests;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->sessions.find(std::wstring(widgetId));
        if (found == state_->sessions.end()) return;
        for (const auto& [key, search] : found->second.searches)
        {
            (void)key;
            if (search.snapshot.pending) requests.push_back(search.request);
        }
        state_->sessions.erase(found);
    }
    for (const auto& request : requests)
        (void)state_->backend.CancelSearch(request);
}

void WidgetSettingsService::CloseAll()
{
    std::vector<WidgetSettingSearchRequest> requests;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& [widgetId, session] : state_->sessions)
        {
            (void)widgetId;
            for (const auto& [key, search] : session.searches)
            {
                (void)key;
                if (search.snapshot.pending)
                    requests.push_back(search.request);
            }
        }
        state_->sessions.clear();
    }
    for (const auto& request : requests)
        (void)state_->backend.CancelSearch(request);
}

namespace
{
struct SessionCopy
{
    WidgetSettingsBackendDescriptor descriptor;
    WidgetSettingsSnapshot snapshot;
};

WidgetSettingMutationResult GuardSession(
    const std::shared_ptr<WidgetSettingsService::State>& state,
    const WidgetSettingMutationGuard& guard, SessionCopy& copy)
{
    if (!guard.IsValid())
        return { WidgetSettingMutationStatus::StaleSnapshot, 0, 0,
            "staleSnapshot", {} };
    std::lock_guard lock(state->mutex);
    const auto found = state->sessions.find(guard.widgetId);
    if (found == state->sessions.end())
        return { WidgetSettingMutationStatus::WidgetNotFound, 0, 0,
            "widgetNotFound", {} };
    if (!guard.Matches(found->second.snapshot))
        return { WidgetSettingMutationStatus::StaleSnapshot,
            found->second.snapshot.generation,
            found->second.snapshot.revision, "staleSnapshot", {} };
    copy.descriptor = found->second.descriptor;
    copy.snapshot = found->second.snapshot;
    return { WidgetSettingMutationStatus::Unchanged,
        copy.snapshot.generation, copy.snapshot.revision, {}, {} };
}

WidgetSettingMutationResult WithIdentity(
    WidgetSettingMutationResult result,
    const WidgetSettingsSnapshot& snapshot)
{
    result.generation = snapshot.generation;
    result.revision = snapshot.revision;
    return result;
}

void InvalidateSearches(
    const std::shared_ptr<WidgetSettingsService::State>& state,
    std::wstring_view widgetId, const std::vector<std::string>& keys)
{
    if (keys.empty()) return;
    std::vector<WidgetSettingSearchRequest> requests;
    {
        std::lock_guard lock(state->mutex);
        const auto session = state->sessions.find(std::wstring(widgetId));
        if (session == state->sessions.end()) return;
        for (const auto& key : keys)
        {
            const auto search = session->second.searches.find(key);
            if (search == session->second.searches.end()) continue;
            requests.push_back(search->second.request);
            session->second.searches.erase(search);
        }
    }
    for (const auto& request : requests)
        (void)state->backend.CancelSearch(request);
}

std::vector<std::string> SearchSettingKeys(
    const WidgetSettingsSnapshot& snapshot)
{
    std::vector<std::string> keys;
    for (const auto& field : snapshot.fields)
        if (field.schema.Kind() == WidgetSettingKind::AppSearch ||
            field.schema.Kind() == WidgetSettingKind::AppReference)
            keys.push_back(field.schema.key);
    return keys;
}

bool ApplyHostAppearancePresetValue(WidgetHostAppearancePatch& patch,
    std::string_view key, std::string_view encoded, std::string& error)
{
    error.clear();
    const std::string value(encoded);
    if (key == "followPersonalization")
    {
        patch.followPersonalization = value == "1" || value == "true";
        return true;
    }
    if (key == "glassEnabled")
    {
        patch.glassEnabled = value == "1" || value == "true";
        return true;
    }
    if (key == "acrylicEnabled")
    {
        patch.acrylicEnabled = value == "1" || value == "true";
        return true;
    }
    if (key == "bg" || key == "border")
    {
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 0);
        if (end == value.c_str() || !end || *end != '\0' ||
            parsed < 0 || parsed > 0xFFFFFF)
        {
            error = "invalidAppearanceColor";
            return false;
        }
        if (key == "bg")
            patch.backgroundColor = static_cast<int>(parsed);
        else
            patch.borderColor = static_cast<int>(parsed);
        return true;
    }
    if (key == "alpha" || key == "borderAlpha" ||
        key == "gradientEndA")
    {
        char* end = nullptr;
        const float parsed = std::strtof(value.c_str(), &end);
        if (end == value.c_str() || !end || *end != '\0' ||
            !std::isfinite(parsed) || parsed < 0.0f || parsed > 1.0f)
        {
            error = "invalidAppearanceOpacity";
            return false;
        }
        if (key == "alpha") patch.backgroundOpacity = parsed;
        else if (key == "borderAlpha") patch.borderOpacity = parsed;
        else patch.gradientEndOpacity = parsed;
        return true;
    }
    if (key == "__contentTheme")
    {
        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || !end || *end != '\0' ||
            parsed < 0 || parsed > 1)
        {
            error = "invalidContentTheme";
            return false;
        }
        patch.contentTheme = static_cast<int>(parsed);
        return true;
    }
    if (key == "glassBlurRadius")
    {
        // Component blur is host-shared. The legacy editor accepted this key
        // in a preset but its setStorage path deliberately ignored it.
        return true;
    }
    if (key == "shadowAlpha" || key == "shadowBlur" ||
        key == "shadowOffsetY" || key == "highlightAlpha" ||
        key == "noiseAlpha")
    {
        // These retired panel-effect keys are filtered by the current runtime
        // and storage loader.  Accept and ignore them so an older package's
        // preset does not make the whole settings surface unavailable.
        return true;
    }
    error = "unknownAppearancePresetValue";
    return false;
}

WidgetSettingMutationResult ReloadAfterMutation(
    WidgetSettingsService& service, const SessionCopy& before,
    const WidgetSettingsBackendResult& backendResult)
{
    WidgetSettingMutationResult result = MutationResultFor(backendResult);
    if (!backendResult.Succeeded())
        return WithIdentity(std::move(result), before.snapshot);
    if (backendResult.status == WidgetSettingsBackendStatus::Unchanged)
        return WithIdentity(std::move(result), before.snapshot);
    WidgetSettingsLoadResult reloaded =
        service.Reload(before.snapshot.widgetId);
    if (!reloaded.Succeeded() || !reloaded.snapshot)
    {
        // The host mutation may already be durable. Drop the old snapshot so
        // it cannot be used to replay or overwrite state that we failed to
        // observe after the commit.
        service.Close(before.snapshot.widgetId);
        result.status = reloaded.status;
        result.errorCode = std::move(reloaded.errorCode);
        result.message = std::move(reloaded.message);
        return WithIdentity(std::move(result), before.snapshot);
    }
    result.generation = reloaded.snapshot->generation;
    result.revision = reloaded.snapshot->revision;
    return result;
}
}

WidgetSettingMutationResult WidgetSettingsService::SetOrdinary(
    const WidgetSettingMutationGuard& guard, std::string_view key,
    const InteractionValue& value)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto* field = FindField(session.snapshot, key);
    if (!field)
        return { WidgetSettingMutationStatus::SettingNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "settingNotFound", {} };
    if (field->schema.Channel() != WidgetSettingValueChannel::Ordinary)
        return { WidgetSettingMutationStatus::WrongValueChannel,
            session.snapshot.generation, session.snapshot.revision,
            "wrongValueChannel", {} };
    if (!field->enabled)
        return { WidgetSettingMutationStatus::Disabled,
            session.snapshot.generation, session.snapshot.revision,
            "settingDisabled", {} };

    InteractionValue normalized;
    std::string error;
    if (!NormalizeWidgetSettingValue(
            field->schema, value, normalized, error))
        return { WidgetSettingMutationStatus::InvalidValue,
            session.snapshot.generation, session.snapshot.revision,
            std::move(error), {} };
    if (normalized == field->currentValue)
        return { WidgetSettingMutationStatus::Unchanged,
            session.snapshot.generation, session.snapshot.revision, {}, {} };

    const std::vector<WidgetSettingOrdinaryWrite> writes = {
        { field->schema.key, std::move(normalized),
            WidgetSettingUsesTypedStorage(field->schema.Kind()) }
    };
    return ReloadAfterMutation(*this, session,
        state_->backend.ApplyOrdinaryTransaction(
            session.descriptor, guard, writes));
}

WidgetSettingMutationResult WidgetSettingsService::SetSearchQuery(
    const WidgetSettingMutationGuard& guard, std::string_view key,
    std::string query)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto* field = FindField(session.snapshot, key);
    if (!field)
        return { WidgetSettingMutationStatus::SettingNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "settingNotFound", {} };
    if (field->schema.Kind() != WidgetSettingKind::AppSearch ||
        !IsValidOrdinarySearchKey(field->schema.searchKey))
        return { WidgetSettingMutationStatus::InvalidValue,
            session.snapshot.generation, session.snapshot.revision,
            "invalidSearchKey", {} };
    if (!field->enabled)
        return { WidgetSettingMutationStatus::Disabled,
            session.snapshot.generation, session.snapshot.revision,
            "settingDisabled", {} };
    if (query.size() > MaximumSearchQueryBytes)
        return { WidgetSettingMutationStatus::InvalidValue,
            session.snapshot.generation, session.snapshot.revision,
            "searchQueryTooLong", {} };
    if (!IsValidUtf8(query))
        return { WidgetSettingMutationStatus::InvalidValue,
            session.snapshot.generation, session.snapshot.revision,
            "invalidSearchQuery", {} };
    if (query == field->searchQuery)
        return { WidgetSettingMutationStatus::Unchanged,
            session.snapshot.generation, session.snapshot.revision, {}, {} };

    const std::string settingKey = field->schema.key;
    if (session.descriptor.preview)
    {
        WidgetSettingMutationResult result;
        std::vector<WidgetSettingSearchRequest> invalidatedRequests;
        std::shared_ptr<const State::EventCallbacks> callbacks;
        WidgetSettingsSnapshotChanged changed;
        {
            std::lock_guard lock(state_->mutex);
            const auto current = state_->sessions.find(guard.widgetId);
            if (current == state_->sessions.end() ||
                !guard.Matches(current->second.snapshot))
                return { WidgetSettingMutationStatus::StaleSnapshot,
                    session.snapshot.generation, session.snapshot.revision,
                    "staleSnapshot", {} };
            auto* currentField = FindField(
                current->second.snapshot, settingKey);
            if (!currentField ||
                currentField->schema.Kind() != WidgetSettingKind::AppSearch)
                return { WidgetSettingMutationStatus::SettingNotFound,
                    current->second.snapshot.generation,
                    current->second.snapshot.revision,
                    "settingNotFound", {} };

            auto& lastRevision =
                state_->lastPublishedRevisions[guard.widgetId];
            lastRevision = std::max(
                lastRevision, current->second.snapshot.revision);
            if (lastRevision ==
                (std::numeric_limits<std::uint64_t>::max)())
                return { WidgetSettingMutationStatus::Failed,
                    current->second.snapshot.generation,
                    current->second.snapshot.revision,
                    "widgetSettingsRevisionExhausted", {} };
            currentField->currentValue = MakeWidgetSettingString({});
            currentField->hasStoredValue = false;
            currentField->searchQuery = std::move(query);
            PrepareWidgetSettingsSnapshot(current->second.snapshot);
            current->second.snapshot.revision = ++lastRevision;

            if (const auto search = current->second.searches.find(settingKey);
                search != current->second.searches.end())
            {
                invalidatedRequests.push_back(search->second.request);
                current->second.searches.erase(search);
            }
            changed = { current->second.snapshot.widgetId,
                current->second.snapshot.generation,
                current->second.snapshot.revision };
            callbacks = state_->callbacks;
            result = { WidgetSettingMutationStatus::Applied,
                current->second.snapshot.generation,
                current->second.snapshot.revision, {}, {} };
        }
        for (const auto& request : invalidatedRequests)
            (void)state_->backend.CancelSearch(request);
        if (callbacks)
            NotifyNoexcept(callbacks->snapshotChanged, std::move(changed));
        return result;
    }

    WidgetSettingOrdinaryWrite write;
    write.key = settingKey;
    write.value = MakeWidgetSettingString({});
    write.searchQuery = std::move(query);
    WidgetSettingsBackendResult backendResult =
        state_->backend.ApplyOrdinaryTransaction(
            session.descriptor, guard, { std::move(write) });
    if (backendResult.Succeeded())
        InvalidateSearches(state_, session.snapshot.widgetId,
            { settingKey });
    return ReloadAfterMutation(*this, session, backendResult);
}

WidgetSettingMutationResult WidgetSettingsService::SetSecret(
    const WidgetSettingMutationGuard& guard, std::string_view key,
    std::string_view plaintext)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto* field = FindField(session.snapshot, key);
    if (!field)
        return { WidgetSettingMutationStatus::SettingNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "settingNotFound", {} };
    if (field->schema.Channel() != WidgetSettingValueChannel::Secret)
        return { WidgetSettingMutationStatus::WrongValueChannel,
            session.snapshot.generation, session.snapshot.revision,
            "wrongValueChannel", {} };
    if (!field->enabled)
        return { WidgetSettingMutationStatus::Disabled,
            session.snapshot.generation, session.snapshot.revision,
            "settingDisabled", {} };
    const WidgetSettingsBackendResult result = plaintext.empty()
        ? state_->backend.ClearSecret(
            session.descriptor, guard, field->schema)
        : state_->backend.SetSecret(
            session.descriptor, guard, field->schema, plaintext);
    return ReloadAfterMutation(*this, session, result);
}

WidgetSettingMutationResult WidgetSettingsService::ChooseFilesystemHandle(
    const WidgetSettingMutationGuard& guard, std::string_view key)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto* field = FindField(session.snapshot, key);
    if (!field)
        return { WidgetSettingMutationStatus::SettingNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "settingNotFound", {} };
    if (field->schema.Channel() !=
        WidgetSettingValueChannel::FilesystemHandle)
        return { WidgetSettingMutationStatus::WrongValueChannel,
            session.snapshot.generation, session.snapshot.revision,
            "wrongValueChannel", {} };
    if (!field->enabled)
        return { WidgetSettingMutationStatus::Disabled,
            session.snapshot.generation, session.snapshot.revision,
            "settingDisabled", {} };
    return ReloadAfterMutation(*this, session,
        state_->backend.ChooseFilesystemHandle(
            session.descriptor, guard, field->schema));
}

WidgetSettingMutationResult WidgetSettingsService::OpenEntityReferencePicker(
    const WidgetSettingMutationGuard& guard, std::string_view key)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto* field = FindField(session.snapshot, key);
    if (!field)
        return { WidgetSettingMutationStatus::SettingNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "settingNotFound", {} };
    if (field->schema.Channel() !=
        WidgetSettingValueChannel::EntityReference)
        return { WidgetSettingMutationStatus::WrongValueChannel,
            session.snapshot.generation, session.snapshot.revision,
            "wrongValueChannel", {} };
    if (!field->enabled)
        return { WidgetSettingMutationStatus::Disabled,
            session.snapshot.generation, session.snapshot.revision,
            "settingDisabled", {} };
    return ReloadAfterMutation(*this, session,
        state_->backend.OpenEntityReferencePicker(
            session.descriptor, guard, field->schema));
}

WidgetSettingMutationResult WidgetSettingsService::ClearOpaque(
    const WidgetSettingMutationGuard& guard, std::string_view key)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto* field = FindField(session.snapshot, key);
    if (!field)
        return { WidgetSettingMutationStatus::SettingNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "settingNotFound", {} };
    if (!field->enabled)
        return { WidgetSettingMutationStatus::Disabled,
            session.snapshot.generation, session.snapshot.revision,
            "settingDisabled", {} };
    WidgetSettingsBackendResult result;
    switch (field->schema.Channel())
    {
    case WidgetSettingValueChannel::Secret:
        result = state_->backend.ClearSecret(
            session.descriptor, guard, field->schema);
        break;
    case WidgetSettingValueChannel::FilesystemHandle:
        result = state_->backend.ClearFilesystemHandle(
            session.descriptor, guard, field->schema);
        break;
    case WidgetSettingValueChannel::EntityReference:
        result = state_->backend.ClearEntityReference(
            session.descriptor, guard, field->schema);
        break;
    default:
        return { WidgetSettingMutationStatus::WrongValueChannel,
            session.snapshot.generation, session.snapshot.revision,
            "wrongValueChannel", {} };
    }
    return ReloadAfterMutation(*this, session, result);
}

WidgetSettingMutationResult WidgetSettingsService::ApplyPreset(
    const WidgetSettingMutationGuard& guard, std::string_view presetId)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto preset = std::find_if(session.snapshot.presets.begin(),
        session.snapshot.presets.end(), [&](const auto& value) {
            return value.id == presetId;
        });
    if (preset == session.snapshot.presets.end())
        return { WidgetSettingMutationStatus::PresetNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "presetNotFound", {} };

    std::vector<std::string> invalidatedSearchKeys =
        SearchSettingKeys(session.snapshot);
    std::vector<WidgetSettingOrdinaryWrite> writes;
    writes.reserve(preset->values.size());
    for (const auto& [key, value] : preset->values)
    {
        const auto* field = FindField(session.snapshot, key);
        if (!field)
            return { WidgetSettingMutationStatus::InvalidValue,
                session.snapshot.generation, session.snapshot.revision,
                "settingNotFound", {} };
        if (field->schema.Channel() !=
                WidgetSettingValueChannel::Ordinary)
            continue;
        WidgetSettingOrdinaryWrite write{ key, value,
            WidgetSettingUsesTypedStorage(field->schema.Kind()) };
        if (field->schema.Kind() == WidgetSettingKind::AppSearch)
        {
            write.searchQuery = std::string{};
        }
        writes.push_back(std::move(write));
    }
    WidgetHostAppearancePatch appearance;
    appearance.presetId = preset->id;
    if (session.snapshot.customStyle)
    {
        // Component themes default both effects off when a preset omits them,
        // exactly as the legacy editor did before applying its values.
        appearance.glassEnabled = false;
        appearance.acrylicEnabled = false;
    }
    std::string appearanceError;
    for (const auto& [key, value] : preset->hostAppearanceValues)
    {
        if (key == "__preset" ||
            (session.snapshot.customStyle &&
                (key == "followPersonalization" ||
                    key == "__contentTheme")))
            continue;
        if (!ApplyHostAppearancePresetValue(
                appearance, key, value, appearanceError))
        {
            return { WidgetSettingMutationStatus::InvalidValue,
                session.snapshot.generation, session.snapshot.revision,
                std::move(appearanceError), {} };
        }
    }
    if (session.snapshot.customStyle)
    {
        // A component preset inherits the current global content theme until
        // the user explicitly chooses Light or Dark again.
        appearance.contentTheme.reset();
        appearance.clearContentTheme = true;
    }
    WidgetSettingsBackendResult backendResult =
        state_->backend.ApplyHostAppearanceTransaction(
            session.descriptor, guard, appearance, writes);
    if (backendResult.Succeeded())
        InvalidateSearches(state_, session.snapshot.widgetId,
            invalidatedSearchKeys);
    return ReloadAfterMutation(*this, session, backendResult);
}

WidgetSettingMutationResult WidgetSettingsService::UpdateHostAppearance(
    const WidgetSettingMutationGuard& guard,
    const WidgetHostAppearancePatch& patch)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    if (patch.Empty())
        return { WidgetSettingMutationStatus::Unchanged,
            session.snapshot.generation, session.snapshot.revision, {}, {} };
    if (!session.snapshot.customStyle)
        return { WidgetSettingMutationStatus::Unavailable,
            session.snapshot.generation, session.snapshot.revision,
            "customStyleUnavailable", {} };
    return ReloadAfterMutation(*this, session,
        state_->backend.ApplyHostAppearanceTransaction(
            session.descriptor, guard, patch, {}));
}

WidgetSettingMutationResult WidgetSettingsService::Reset(
    const WidgetSettingMutationGuard& guard)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    std::vector<std::string> invalidatedSearchKeys =
        SearchSettingKeys(session.snapshot);
    std::vector<WidgetSettingOrdinaryWrite> writes;
    for (const auto& field : session.snapshot.fields)
        if (field.schema.Channel() == WidgetSettingValueChannel::Ordinary)
        {
            WidgetSettingOrdinaryWrite write{ field.schema.key,
                field.defaultValue,
                WidgetSettingUsesTypedStorage(field.schema.Kind()) };
            if (field.schema.Kind() == WidgetSettingKind::AppSearch)
            {
                write.searchQuery = std::string{};
            }
            writes.push_back(std::move(write));
        }
    if (writes.empty())
    {
        InvalidateSearches(state_, session.snapshot.widgetId,
            invalidatedSearchKeys);
        return { WidgetSettingMutationStatus::Unchanged,
            session.snapshot.generation, session.snapshot.revision, {}, {} };
    }
    WidgetSettingsBackendResult backendResult =
        state_->backend.ApplyOrdinaryTransaction(
            session.descriptor, guard, writes);
    if (backendResult.Succeeded())
        InvalidateSearches(state_, session.snapshot.widgetId,
            invalidatedSearchKeys);
    return ReloadAfterMutation(*this, session, backendResult);
}

WidgetSettingMutationResult WidgetSettingsService::StartSearch(
    const WidgetSettingMutationGuard& guard, std::string_view key,
    std::string query, std::size_t maximumResults)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    const auto* field = FindField(session.snapshot, key);
    if (!field)
        return { WidgetSettingMutationStatus::SettingNotFound,
            session.snapshot.generation, session.snapshot.revision,
            "settingNotFound", {} };
    if (field->schema.Kind() != WidgetSettingKind::AppSearch &&
        field->schema.Kind() != WidgetSettingKind::AppReference)
        return { WidgetSettingMutationStatus::WrongValueChannel,
            session.snapshot.generation, session.snapshot.revision,
            "searchUnsupported", {} };
    if (!field->enabled)
        return { WidgetSettingMutationStatus::Disabled,
            session.snapshot.generation, session.snapshot.revision,
            "settingDisabled", {} };
    if (query.size() > MaximumSearchQueryBytes)
        return { WidgetSettingMutationStatus::InvalidValue,
            session.snapshot.generation, session.snapshot.revision,
            "searchQueryTooLong", {} };
    if (!IsValidUtf8(query))
        return { WidgetSettingMutationStatus::InvalidValue,
            session.snapshot.generation, session.snapshot.revision,
            "invalidSearchQuery", {} };
    if (maximumResults == 0) maximumResults = 1;
    maximumResults = std::min<std::size_t>(maximumResults, 64);

    WidgetSettingSearchRequest previous;
    bool cancelPrevious = false;
    WidgetSettingSearchRequest request;
    {
        std::lock_guard lock(state_->mutex);
        auto found = state_->sessions.find(guard.widgetId);
        if (found == state_->sessions.end() ||
            !guard.Matches(found->second.snapshot))
            return { WidgetSettingMutationStatus::StaleSnapshot,
                session.snapshot.generation, session.snapshot.revision,
                "staleSnapshot", {} };
        auto old = found->second.searches.find(std::string(key));
        if (old != found->second.searches.end() &&
            old->second.snapshot.pending)
        {
            previous = old->second.request;
            cancelPrevious = true;
        }
        std::uint64_t requestId = ++state_->nextSearchRequestId;
        if (requestId == 0) requestId = ++state_->nextSearchRequestId;
        request = { session.snapshot.widgetId,
            session.snapshot.packageId, std::string(key), std::move(query),
            session.snapshot.generation, requestId, maximumResults };
        State::SearchState search;
        search.request = request;
        search.snapshot = { request.widgetId, request.settingKey,
            request.generation, request.requestId, request.query,
            true, false, {}, {} };
        found->second.searches.insert_or_assign(
            request.settingKey, std::move(search));
    }
    if (cancelPrevious)
        (void)state_->backend.CancelSearch(previous);

    const std::weak_ptr<State> weak = state_;
    WidgetSettingsBackendResult started = state_->backend.StartSearch(
        request, [weak](WidgetSettingSearchCompletion completion) noexcept {
            try
            {
                const auto state = weak.lock();
                if (!state) return;
                std::shared_ptr<const State::EventCallbacks> callbacks;
                WidgetSettingSearchCompleted completed;
                {
                    std::lock_guard lock(state->mutex);
                    const auto session = state->sessions.find(
                        completion.widgetId);
                    if (session == state->sessions.end() ||
                        session->second.snapshot.generation !=
                            completion.generation)
                        return;
                    const auto search = session->second.searches.find(
                        completion.settingKey);
                    if (search == session->second.searches.end() ||
                        !search->second.snapshot.pending ||
                        search->second.request.widgetId !=
                            completion.widgetId ||
                        search->second.request.settingKey !=
                            completion.settingKey ||
                        search->second.request.generation !=
                            completion.generation ||
                        search->second.request.requestId !=
                            completion.requestId)
                        return;

                    const bool succeeded =
                        completion.result.Succeeded();
                    if (succeeded)
                    {
                        if (completion.results.size() >
                            search->second.request.maximumResults)
                        {
                            completion.results.resize(
                                search->second.request.maximumResults);
                        }
                        search->second.snapshot.errorCode.clear();
                        search->second.snapshot.results =
                            std::move(completion.results);
                    }
                    else
                    {
                        search->second.snapshot.errorCode =
                            std::move(completion.result.errorCode);
                        search->second.snapshot.results.clear();
                    }
                    search->second.snapshot.completed = succeeded;
                    search->second.snapshot.pending = false;
                    completed = { search->second.request.widgetId,
                        search->second.request.settingKey,
                        search->second.request.generation,
                        search->second.request.requestId };
                    callbacks = state->callbacks;
                }
                if (callbacks)
                    NotifyNoexcept(callbacks->searchCompleted,
                        std::move(completed));
            }
            catch (...)
            {
                // Backend completion threads must remain alive even if a
                // notification or snapshot publication cannot be allocated.
            }
        });
    if (!started.Succeeded())
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->sessions.find(guard.widgetId);
        if (found != state_->sessions.end())
            if (const auto search = found->second.searches.find(
                    std::string(key));
                search != found->second.searches.end() &&
                    search->second.request.requestId == request.requestId)
            {
                search->second.snapshot.pending = false;
                search->second.snapshot.errorCode = started.errorCode;
            }
        return WithIdentity(MutationResultFor(started), session.snapshot);
    }
    return { WidgetSettingMutationStatus::Started,
        session.snapshot.generation, session.snapshot.revision, {}, {} };
}

WidgetSettingMutationResult WidgetSettingsService::CancelSearch(
    const WidgetSettingMutationGuard& guard, std::string_view key)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    WidgetSettingSearchRequest request;
    {
        std::lock_guard lock(state_->mutex);
        auto found = state_->sessions.find(guard.widgetId);
        if (found == state_->sessions.end()) return checked;
        const auto search = found->second.searches.find(std::string(key));
        if (search == found->second.searches.end() ||
            !search->second.snapshot.pending)
            return { WidgetSettingMutationStatus::Unchanged,
                session.snapshot.generation, session.snapshot.revision,
                {}, {} };
        request = search->second.request;
    }
    WidgetSettingsBackendResult cancelled =
        state_->backend.CancelSearch(request);
    if (!cancelled.Succeeded())
        return WithIdentity(
            MutationResultFor(cancelled), session.snapshot);
    {
        std::lock_guard lock(state_->mutex);
        auto found = state_->sessions.find(guard.widgetId);
        if (found != state_->sessions.end())
            if (auto search = found->second.searches.find(std::string(key));
                search != found->second.searches.end() &&
                    search->second.request.requestId == request.requestId)
            {
                search->second.snapshot.pending = false;
                search->second.snapshot.completed = false;
                search->second.snapshot.errorCode = "cancelled";
                search->second.snapshot.results.clear();
            }
    }
    return { WidgetSettingMutationStatus::Cancelled,
        session.snapshot.generation, session.snapshot.revision, {}, {} };
}

WidgetSettingMutationResult WidgetSettingsService::CommitSearchResult(
    const WidgetSettingMutationGuard& guard, std::string_view key,
    std::uint64_t requestId, std::string_view resultId)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    WidgetSettingSearchRequest request;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->sessions.find(guard.widgetId);
        if (found == state_->sessions.end()) return checked;
        const auto search = found->second.searches.find(std::string(key));
        if (search == found->second.searches.end() ||
            search->second.request.requestId != requestId ||
            !search->second.snapshot.completed)
            return { WidgetSettingMutationStatus::StaleSnapshot,
                session.snapshot.generation, session.snapshot.revision,
                "staleSearchResult", {} };
        const bool known = std::any_of(
            search->second.snapshot.results.begin(),
            search->second.snapshot.results.end(),
            [&](const WidgetSettingSearchResult& result) {
                return result.id == resultId;
            });
        if (!known)
            return { WidgetSettingMutationStatus::InvalidValue,
                session.snapshot.generation, session.snapshot.revision,
                "searchResultNotFound", {} };
        request = search->second.request;
    }
    WidgetSettingsBackendResult committed =
        state_->backend.CommitSearchResult(
            session.descriptor, guard, request, resultId);
    if (committed.Succeeded())
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->sessions.find(guard.widgetId);
        if (found != state_->sessions.end())
            found->second.searches.erase(std::string(key));
    }
    return ReloadAfterMutation(*this, session, committed);
}

std::optional<WidgetSettingSearchSnapshot>
WidgetSettingsService::SearchSnapshot(
    std::wstring_view widgetId, std::string_view key) const
{
    std::lock_guard lock(state_->mutex);
    const auto session = state_->sessions.find(std::wstring(widgetId));
    if (session == state_->sessions.end()) return std::nullopt;
    const auto search = session->second.searches.find(std::string(key));
    return search == session->second.searches.end()
        ? std::nullopt
        : std::optional<WidgetSettingSearchSnapshot>(
            search->second.snapshot);
}
}
