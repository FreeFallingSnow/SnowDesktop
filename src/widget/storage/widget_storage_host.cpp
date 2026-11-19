#include "widget_storage_host.h"

#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/preview/widget_preview_context.h"
#include "common/utils.h"
#include "widget/tasks/widget_filesystem_task_dispatch.h"
#include "widget/storage/widget_storage_runtime.h"
#include "widget/storage/widget_storage_value.h"
#include "data/data_paths.h"
#include "widget/storage/widget_storage_lua.h"
#include "widget/storage/widget_storage_write_budget.h"
#include "widget/storage/widget_secret_store.h"
#include "widget/settings/widget_settings_declarations.h"
#include "widget/settings/widget_setting_rules.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <utility>

using snowdesktop::widget_runtime::ActiveStorage;
using snowdesktop::widget_runtime::ReadStorage;
using snowdesktop::widget_runtime::SaveStorageFile;
using snowdesktop::widget_runtime::LoadStorageFile;
using snowdesktop::widget_runtime::StorageWriteWithinQuota;
using snowdesktop::widget_runtime::BoundWidgetId;
using snowdesktop::widget_runtime::IsEntityReferenceSettingType;
using snowdesktop::widget_runtime::IsFilesystemHandleSettingType;
using snowdesktop::widget_runtime::FilesystemSettingAccess;
using snowdesktop::widget_runtime::FilesystemSettingMetadataKey;
using snowdesktop::widget_runtime::EntityReferenceSettingTypeFilter;
using snowdesktop::widget_runtime::IsRemovedPanelEffectSettingKey;
using snowdesktop::widget_runtime::StorageOverlayScope;
using snowdesktop::widget_runtime::MutablePersistentStorage;

std::unordered_map<std::string, std::string>&
WidgetEngine::WidgetSettingsPersistentStorageForBackend() noexcept
{
    return snowdesktop::widget_runtime::MutablePersistentStorage();
}

const std::unordered_map<std::string, std::string>&
WidgetEngine::WidgetSettingsPersistentStorageForBackend() const noexcept
{
    return snowdesktop::widget_runtime::PersistentStorage();
}

bool WidgetEngine::PersistWidgetSettingsStorageForBackend()
{
    return SaveStorageFile();
}

const snowdesktop::widget_runtime::StorageMap&
WidgetEngine::WidgetSettingsReadStorageForBackend() const
{
    return ReadStorage();
}

bool WidgetEngine::StageWidgetSettingsPreviewForBackend(const void* owner,
    const std::string& prefix,
    const snowdesktop::widget_runtime::StorageMap& previous,
    const snowdesktop::widget_runtime::StorageMap& candidate)
{
    return snowdesktop::widget_runtime::StageStoragePreview(owner, prefix, previous, candidate);
}

bool WidgetEngine::CommitWidgetSettingsPreviewForBackend(const void* owner)
{
    return snowdesktop::widget_runtime::CommitStoragePreview(owner);
}

void WidgetEngine::RemoveWidgetSettingsPreviewForBackend(const void* owner) noexcept
{
    snowdesktop::widget_runtime::RemoveStoragePreview(owner);
}

static std::string BoundStoragePrefix(lua_State* state)
{
    return WideToUtf8(BoundWidgetId(state));
}

static const snowdesktop::widget_runtime::WidgetStorageLuaHost kStorageLuaHostServices{
    BoundStoragePrefix,
    +[](lua_State* state) {
        auto* d2d = GetD2D(state);
        return !d2d || !d2d->engine ||
            d2d->engine->RuntimeCanWriteWidgetStorage(BoundWidgetId(state));
    },
    +[](lua_State* state, const std::string& key, std::string& value) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeGetSecretReference(BoundWidgetId(state), key, value);
    },
    +[](lua_State* state, const std::string& key, std::string& value) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeGetFilesystemSettingHandle(BoundWidgetId(state), key, value);
    },
    +[](lua_State* state, const std::string& key) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeIsEntityReferenceSetting(BoundWidgetId(state), key);
    },
    +[](lua_State* state, const std::string& key, snowdesktop::widget_runtime::InteractionValue& value) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine &&
            d2d->engine->RuntimeGetTypedSettingDefault(BoundWidgetId(state), key, value);
    },
    +[](lua_State* state, const std::string& key) {
        auto* d2d = GetD2D(state);
        return d2d && d2d->engine
            ? d2d->engine->RuntimeGetStorageValue(BoundWidgetId(state), key) : std::string{};
    },
    +[](lua_State* state) {
        auto* d2d = GetD2D(state);
        if (!d2d || !d2d->engine) return std::vector<std::string>{};
        const auto widgetId = BoundWidgetId(state);
        auto keys = d2d->engine->RuntimeSecretStorageKeys(widgetId);
        const auto filesystemKeys = d2d->engine->RuntimeFilesystemSettingKeys(widgetId);
        keys.insert(keys.end(), filesystemKeys.begin(), filesystemKeys.end());
        return keys;
    },
};

static std::string FindDeclaredDefaultValue(const LuaWidget& widget, const std::string& key)
{
    auto readPresetValue = [&key](const std::vector<LuaWidgetManifest::SettingPreset>& presets) {
        for (const auto& preset : presets)
        {
            if (!preset.isDefault && preset.id != "default")
                continue;
            auto it = preset.values.find(key);
            if (it != preset.values.end())
                return it->second;
        }
        return std::string{};
    };
    std::string value = readPresetValue(widget.manifest.presets);
    if (!value.empty()) return value;
    value = readPresetValue(widget.scriptPresets);
    if (!value.empty()) return value;

    auto readSettingValue = [&key](const std::vector<LuaWidgetManifest::Setting>& settings) {
        for (const auto& setting : settings)
            if (setting.key == key)
                return setting.defaultValue;
        return std::string{};
    };
    value = readSettingValue(widget.manifest.settings);
    if (!value.empty()) return value;
    return readSettingValue(widget.scriptSettings);
}

bool WidgetEngine::RuntimeCanWriteWidgetStorage(
    const std::wstring& widgetId) const
{
    const int index = FindWidget(widgetId);
    return index < 0 ||
        (!widgets_[index].interactionRegions.FrameOpen() &&
            !widgets_[index].panelFrameOpen);
}

bool WidgetEngine::RuntimeGetFilesystemSettingHandle(
    const std::wstring& widgetId, const std::string& key,
    std::string& handle) const
{
    handle.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const LuaWidgetManifest::Setting* declared = nullptr;
    const auto findSetting = [&](const auto& settings) {
        return std::find_if(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key &&
                    IsFilesystemHandleSettingType(setting.type);
            });
    };
    if (const auto found = findSetting(widget.manifest.settings);
        found != widget.manifest.settings.end())
        declared = &*found;
    else if (const auto scriptFound = findSetting(widget.scriptSettings);
        scriptFound != widget.scriptSettings.end())
        declared = &*scriptFound;
    if (!declared) return false;
    if (widget.preview || !filesystemHandleStore_) return true;

    const std::string prefix = WideToUtf8(widgetId) + ".";
    const auto stored = ActiveStorage().find(
        prefix + FilesystemSettingMetadataKey(key));
    if (stored == ActiveStorage().end()) return true;
    const auto entry = filesystemHandleStore_->Resolve(
        { WideToUtf8(widgetId), widget.packageId }, stored->second);
    const auto expectedAccess = FilesystemSettingAccess(declared->access);
    const auto expectedKind = declared->type == "folderHandle"
        ? snowdesktop::widget_runtime::WidgetFilesystemHandleKind::Folder
        : snowdesktop::widget_runtime::WidgetFilesystemHandleKind::File;
    if (!entry || !expectedAccess || entry->kind != expectedKind ||
        entry->access != *expectedAccess)
        return true;
    handle = entry->handle;
    return true;
}

bool WidgetEngine::RuntimeIsFilesystemSettingHandleValue(
    const std::wstring& widgetId, std::string_view handle) const
{
    if (handle.empty()) return false;
    const int index = FindWidget(widgetId);
    if (index < 0) return false;
    const LuaWidget& widget = widgets_[index];
    const auto matches = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (!IsFilesystemHandleSettingType(setting.type)) continue;
            const auto stored = ActiveStorage().find(
                WideToUtf8(widgetId) + "." +
                    FilesystemSettingMetadataKey(setting.key));
            if (stored != ActiveStorage().end() && stored->second == handle)
                return true;
        }
        return false;
    };
    return matches(widget.manifest.settings) ||
        matches(widget.scriptSettings);
}

std::vector<std::string> WidgetEngine::RuntimeFilesystemSettingKeys(
    const std::wstring& widgetId) const
{
    std::vector<std::string> result;
    const int index = FindWidget(widgetId);
    if (index < 0) return result;
    const LuaWidget& widget = widgets_[index];
    const auto append = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (!IsFilesystemHandleSettingType(setting.type)) continue;
            std::string handle;
            if (RuntimeGetFilesystemSettingHandle(
                    widgetId, setting.key, handle) && !handle.empty() &&
                std::find(result.begin(), result.end(), setting.key) ==
                    result.end())
                result.push_back(setting.key);
        }
    };
    append(widget.manifest.settings);
    append(widget.scriptSettings);
    std::sort(result.begin(), result.end());
    return result;
}

std::string WidgetEngine::RuntimeGetStorageValue(const std::wstring& widgetId, const std::string& key) const
{
    std::string secretReference;
    if (RuntimeGetSecretReference(widgetId, key, secretReference))
        return secretReference;
    std::string filesystemHandle;
    if (RuntimeGetFilesystemSettingHandle(
            widgetId, key, filesystemHandle))
        return filesystemHandle;
    if (RuntimeIsEntityReferenceSetting(widgetId, key)) return {};
    const std::string prefix = WideToUtf8(widgetId);
    const std::string fullKey = prefix + "." + key;
    int idx = FindWidget(widgetId);
    auto& storage = idx >= 0 && widgets_[idx].preview
        ? widgets_[idx].previewStorage : ReadStorage();
    auto it = storage.find(fullKey);
    if (it != storage.end())
    {
        const auto marker = storage.find(prefix + "." +
            snowdesktop::widget_runtime::TypedStorageMetadataKey(key));
        if (marker != storage.end() &&
            marker->second == snowdesktop::widget_runtime::TypedStorageMarker)
            return {};
        return it->second;
    }

    if (idx < 0) return {};
    const LuaWidget& widget = widgets_[idx];
    if (key == "followPersonalization" && widget.customStyle &&
        widget.followPersonalizationDefault)
        return "1";
    return FindDeclaredDefaultValue(widget, key);
}

bool WidgetEngine::RuntimeGetTypedSettingDefault(
    const std::wstring& widgetId, const std::string& key,
    snowdesktop::widget_runtime::InteractionValue& value) const
{
    using Value = snowdesktop::widget_runtime::InteractionValue;
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const LuaWidgetManifest::Setting* declared = nullptr;
    const auto findSetting = [&](const auto& settings) {
        return std::find_if(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key &&
                    (setting.type == "range" ||
                        setting.type == "multiSelect");
            });
    };
    if (const auto found = findSetting(widget.manifest.settings);
        found != widget.manifest.settings.end())
        declared = &*found;
    else if (const auto scriptFound = findSetting(widget.scriptSettings);
        scriptFound != widget.scriptSettings.end())
        declared = &*scriptFound;
    if (!declared) return false;

    const auto defaultPreset = [&](const auto& presets) ->
        const LuaWidgetManifest::SettingPreset* {
        const auto found = std::find_if(presets.begin(), presets.end(),
            [](const LuaWidgetManifest::SettingPreset& preset) {
                return preset.isDefault || preset.id == "default";
            });
        return found == presets.end() ? nullptr : &*found;
    };
    const LuaWidgetManifest::SettingPreset* preset =
        defaultPreset(widget.manifest.presets);
    if (!preset) preset = defaultPreset(widget.scriptPresets);

    if (declared->type == "range")
    {
        std::string encoded = declared->defaultValue;
        if (encoded.empty() && preset)
        {
            const auto found = preset->values.find(key);
            if (found != preset->values.end()) encoded = found->second;
        }
        double number = declared->minValue;
        (void)snowdesktop::widget_runtime::ParseFiniteSettingNumber(
            encoded, number);
        value = {};
        value.type = Value::Type::Number;
        value.number = snowdesktop::widget_runtime::SnapRangeSettingValue(
            number, declared->minValue, declared->maxValue,
            declared->stepValue);
        return true;
    }

    std::vector<std::string> selected = declared->defaultValues;
    if (selected.empty() && preset)
    {
        const auto found = preset->arrayValues.find(key);
        if (found != preset->arrayValues.end()) selected = found->second;
    }
    value = {};
    value.type = Value::Type::Array;
    for (const auto& item : selected)
    {
        Value entry;
        entry.type = Value::Type::String;
        entry.string = item;
        value.array.push_back(std::move(entry));
    }
    return true;
}

void WidgetEngine::RuntimeSetStorageValue(const std::wstring& widgetId, const std::string& key, const std::string& value)
{
    if (key.empty() || IsRemovedPanelEffectSettingKey(key)) return;
    std::string secretReference;
    if (RuntimeGetSecretReference(widgetId, key, secretReference)) return;
    std::string filesystemHandle;
    if (RuntimeGetFilesystemSettingHandle(
            widgetId, key, filesystemHandle)) return;
    if (RuntimeIsEntityReferenceSetting(widgetId, key)) return;
    const std::string prefix = WideToUtf8(widgetId);
    if (!StorageWriteWithinQuota(prefix, key, value))
    {
        RuntimeRecordError(widgetId, "Widget storage quota exceeded");
        return;
    }
    const int index = FindWidget(widgetId);
    if (index >= 0 && widgets_[index].preview)
    {
        auto& storage = widgets_[index].previewStorage;
        storage[prefix + "." + key] = value;
        storage.erase(prefix + "." +
            snowdesktop::widget_runtime::TypedStorageMetadataKey(key));
        return;
    }
    auto& storage = ActiveStorage();
    storage[prefix + "." + key] = value;
    storage.erase(prefix + "." +
        snowdesktop::widget_runtime::TypedStorageMetadataKey(key));
    if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
}

bool WidgetEngine::RuntimeGetSecretReference(
    const std::wstring& widgetId, const std::string& key,
    std::string& reference) const
{
    reference.clear();
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const auto declaresPassword = [&](const auto& settings) {
        return std::any_of(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key && setting.type == "password";
            });
    };
    if (!declaresPassword(widget.manifest.settings) &&
        !declaresPassword(widget.scriptSettings))
        return false;
    if (!widget.preview && secretStore_)
    {
        reference = secretStore_->Reference(widget.packageId,
            WideToUtf8(widgetId), key);
    }
    return true;
}

bool WidgetEngine::RuntimeIsEntityReferenceSetting(
    const std::wstring& widgetId, const std::string& key) const
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return false;
    const LuaWidget& widget = widgets_[index];
    const auto declaresReference = [&](const auto& settings) {
        return std::any_of(settings.begin(), settings.end(),
            [&](const LuaWidgetManifest::Setting& setting) {
                return setting.key == key &&
                    IsEntityReferenceSettingType(setting.type);
            });
    };
    return declaresReference(widget.manifest.settings) ||
        declaresReference(widget.scriptSettings);
}

std::vector<std::string> WidgetEngine::RuntimeSecretStorageKeys(
    const std::wstring& widgetId) const
{
    std::vector<std::string> result;
    const int index = FindWidget(widgetId);
    if (index < 0 || widgets_[index].preview || !secretStore_)
        return result;
    const LuaWidget& widget = widgets_[index];
    const std::string instanceId = WideToUtf8(widgetId);
    const auto append = [&](const auto& settings) {
        for (const auto& setting : settings)
        {
            if (setting.type == "password" &&
                !secretStore_->Reference(widget.packageId, instanceId,
                    setting.key).empty() &&
                std::find(result.begin(), result.end(), setting.key) ==
                    result.end())
                result.push_back(setting.key);
        }
    };
    append(widget.manifest.settings);
    append(widget.scriptSettings);
    std::sort(result.begin(), result.end());
    return result;
}

void WidgetEngine::ReloadStorage()
{
    LoadStorageFile();
}

void WidgetEngine::SetInitializationExperimentStoragePath(const std::wstring& path)
{
    snowdesktop::widget_runtime::OpenStorageFile(path);
}

static std::filesystem::path WidgetSecretStorePath()
{
    const std::filesystem::path data = GetDataDirectoryPath();
    return data.parent_path() / L"PrivateState" /
        L"SnowDesktop.widget-secrets.bin";
}

void WidgetEngine::InitializeWidgetStorage()
{
    // Init storage path
    snowdesktop::widget_runtime::OpenStorageFile(GetDataFilePath(L"SnowDesktop.storage.json"));
    secretStore_ = std::make_unique<
        snowdesktop::widget_runtime::WidgetSecretStore>(
            WidgetSecretStorePath());
    std::string secretStoreError;
    if (!secretStore_->Load(secretStoreError))
    {
        const std::string diagnostic =
            "SnowDesktop widget secret store load failed: " +
            secretStoreError + "\n";
        OutputDebugStringA(diagnostic.c_str());
    }
}

bool WidgetEngine::MigrateWidgetStorage(lua_State* state, const LuaWidget& pending,
    const LuaRuntimeQuota& quota)
{
    const auto& widgetId = pending.widgetId;
    const std::string storagePrefix = WideToUtf8(widgetId) + ".";
    const std::string dataVersionKey = storagePrefix + "__host.dataVersion";
    auto& activeStorage = ActiveStorage();
    int storedDataVersion = 1;
    if (const auto stored = activeStorage.find(dataVersionKey);
        stored != activeStorage.end())
        storedDataVersion = std::max(1, std::atoi(stored->second.c_str()));
    if (pending.manifest.dataVersion < storedDataVersion)
    {
        RuntimeRecordError(widgetId,
            "Widget package dataVersion is older than the instance storage");
        return false;
    }
    if (pending.manifest.dataVersion > storedDataVersion)
    {
        std::unordered_map<std::string, std::string> migratedStorage =
            activeStorage;
        auto* parentOverlay =
            snowdesktop::widget_runtime::CurrentStorageOverlay();
        bool migrationFailed = false;
        std::string migrationFailure;
        {
            StorageOverlayScope migrationScope(&migratedStorage);
            lua_getfield(state, -1, "migrateStorage");
            if (lua_isfunction(state, -1))
            {
                lua_pushinteger(state, storedDataVersion);
                lua_pushinteger(state, pending.manifest.dataVersion);
                if (snowdesktop::lua_runtime::ProtectedCall(
                        state, 2, 0, 1000000,
                        std::chrono::milliseconds(100)) != LUA_OK)
                {
                    migrationFailed = true;
                    const char* migrationError = lua_tostring(state, -1);
                    migrationFailure = migrationError
                        ? migrationError : "(storage migration error)";
                    lua_pop(state, 1);
                }
            }
            else
                lua_pop(state, 1);
        }
        if (migrationFailed)
        {
            RuntimeRecordError(widgetId,
                "Widget storage migration failed: " + migrationFailure);
            RecordWidgetHostFailure(widgetId,
                "Widget storage migration failed: " + migrationFailure,
                quota.memoryExceeded || quota.executionExceeded);
            return false;
        }
        migratedStorage[dataVersionKey] =
            std::to_string(pending.manifest.dataVersion);
        if (parentOverlay)
            *parentOverlay = std::move(migratedStorage);
        else
        {
            MutablePersistentStorage() = std::move(migratedStorage);
            SaveStorageFile();
        }
    }
    else if (!activeStorage.contains(dataVersionKey))
    {
        activeStorage[dataVersionKey] =
            std::to_string(pending.manifest.dataVersion);
        if (!snowdesktop::widget_runtime::HasStorageOverlay()) SaveStorageFile();
    }
    return true;
}

void WidgetEngine::PurgeOpaqueWidgetSettings(const LuaWidget& widget)
{
    const std::string storagePrefix = WideToUtf8(widget.widgetId) + ".";
    if (!widget.preview && !snowdesktop::widget_runtime::HasStorageOverlay())
    {
        bool removedPlaintextOpaqueSetting = false;
        const auto purgeOpaqueSettingValues = [&](const auto& settings) {
            for (const auto& setting : settings)
            {
                if (setting.type != "password" &&
                    !IsFilesystemHandleSettingType(setting.type) &&
                    !IsEntityReferenceSettingType(setting.type))
                    continue;
                removedPlaintextOpaqueSetting =
                    MutablePersistentStorage().erase(storagePrefix + setting.key) > 0 ||
                    removedPlaintextOpaqueSetting;
                removedPlaintextOpaqueSetting =
                    MutablePersistentStorage().erase(storagePrefix +
                        snowdesktop::widget_runtime::
                            TypedStorageMetadataKey(setting.key)) > 0 ||
                    removedPlaintextOpaqueSetting;
            }
        };
        purgeOpaqueSettingValues(widget.manifest.settings);
        purgeOpaqueSettingValues(widget.scriptSettings);
        if (removedPlaintextOpaqueSetting) SaveStorageFile();
    }
}

void BindWidgetHostStorage(lua_State* state, bool hasRenderContext)
{
    snowdesktop::widget_runtime::BindWidgetStorageContext(
        state, hasRenderContext ? &kStorageLuaHostServices : nullptr);
}
