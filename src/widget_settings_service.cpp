#include "widget_settings_service.h"

#include <algorithm>
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

    std::vector<WidgetSettingFieldSchema> schemas;
    schemas.reserve(declarations.size());
    for (const auto& declaration : declarations)
        schemas.push_back(declaration.schema);

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
        std::unique_ptr<WidgetSettingsRevisionSource> revisions;
        std::unordered_map<std::string, SearchState> searches;
    };

    explicit State(IWidgetSettingsBackend& value) : backend(value) {}

    IWidgetSettingsBackend& backend;
    mutable std::mutex mutex;
    std::unordered_map<std::wstring, Session> sessions;
    std::uint64_t nextSearchRequestId = 0;
};

WidgetSettingsService::WidgetSettingsService(IWidgetSettingsBackend& backend)
    : state_(std::make_shared<State>(backend))
{
}

WidgetSettingsService::~WidgetSettingsService()
{
    CloseAll();
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
    {
        std::lock_guard lock(state_->mutex);
        auto existing = state_->sessions.find(widgetId);
        if (existing != state_->sessions.end() &&
            existing->second.snapshot.generation ==
                built.snapshot.generation)
        {
            existing->second.descriptor = std::move(built.descriptor);
            (void)existing->second.revisions->Publish(built.snapshot);
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
                    if (search.snapshot.pending)
                        replacedRequests.push_back(search.request);
                }
            }
            State::Session session;
            session.descriptor = std::move(built.descriptor);
            session.revisions =
                std::make_unique<WidgetSettingsRevisionSource>(
                    built.snapshot.widgetId, built.snapshot.generation);
            (void)session.revisions->Publish(built.snapshot);
            session.snapshot = std::move(built.snapshot);
            loaded.snapshot = session.snapshot;
            state_->sessions.insert_or_assign(
                widgetId, std::move(session));
        }
    }
    for (const auto& request : replacedRequests)
        (void)state_->backend.CancelSearch(request);
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

    std::vector<WidgetSettingOrdinaryWrite> writes;
    writes.reserve(preset->values.size());
    for (const auto& [key, value] : preset->values)
    {
        const auto* field = FindField(session.snapshot, key);
        if (!field || field->schema.Channel() !=
                WidgetSettingValueChannel::Ordinary)
            return { WidgetSettingMutationStatus::InvalidValue,
                session.snapshot.generation, session.snapshot.revision,
                "opaquePresetValue", {} };
        writes.push_back({ key, value,
            WidgetSettingUsesTypedStorage(field->schema.Kind()) });
    }
    if (writes.empty())
        return { WidgetSettingMutationStatus::Unchanged,
            session.snapshot.generation, session.snapshot.revision, {}, {} };
    return ReloadAfterMutation(*this, session,
        state_->backend.ApplyOrdinaryTransaction(
            session.descriptor, guard, writes));
}

WidgetSettingMutationResult WidgetSettingsService::Reset(
    const WidgetSettingMutationGuard& guard)
{
    SessionCopy session;
    WidgetSettingMutationResult checked =
        GuardSession(state_, guard, session);
    if (!checked.Succeeded()) return checked;
    std::vector<WidgetSettingOrdinaryWrite> writes;
    for (const auto& field : session.snapshot.fields)
        if (field.schema.Channel() == WidgetSettingValueChannel::Ordinary)
            writes.push_back({ field.schema.key, field.defaultValue,
                WidgetSettingUsesTypedStorage(field.schema.Kind()) });
    if (writes.empty())
        return { WidgetSettingMutationStatus::Unchanged,
            session.snapshot.generation, session.snapshot.revision, {}, {} };
    return ReloadAfterMutation(*this, session,
        state_->backend.ApplyOrdinaryTransaction(
            session.descriptor, guard, writes));
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
        request, [weak](WidgetSettingSearchCompletion completion) {
            const auto state = weak.lock();
            if (!state) return;
            std::lock_guard lock(state->mutex);
            const auto session = state->sessions.find(completion.widgetId);
            if (session == state->sessions.end() ||
                session->second.snapshot.generation !=
                    completion.generation)
                return;
            const auto search = session->second.searches.find(
                completion.settingKey);
            if (search == session->second.searches.end() ||
                search->second.request.requestId != completion.requestId)
                return;
            search->second.snapshot.pending = false;
            search->second.snapshot.completed =
                completion.result.Succeeded();
            search->second.snapshot.errorCode =
                completion.result.Succeeded()
                ? std::string{} : completion.result.errorCode;
            search->second.snapshot.results = completion.result.Succeeded()
                ? std::move(completion.results)
                : std::vector<WidgetSettingSearchResult>{};
            if (search->second.snapshot.results.size() >
                search->second.request.maximumResults)
                search->second.snapshot.results.resize(
                    search->second.request.maximumResults);
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
