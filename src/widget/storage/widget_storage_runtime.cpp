#include "widget_storage_runtime.h"

#include "widget_settings_preview.h"
#include "widget_storage_persistence.h"
#include "widget_storage_transaction.h"
#include "widget/settings/widget_settings_declarations.h"

#include <windows.h>
#include <filesystem>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
StorageMap g_storage;
WidgetSettingsPreviewLayers g_settingsPreviews;
std::wstring g_storagePath;
}

void OpenStorageFile(std::wstring path)
{
    g_storagePath = std::move(path);
    LoadStorageFile();
}

StorageMap& ActiveStorage()
{
    g_settingsPreviews.InvalidateReads();
    return snowdesktop::widget_runtime::ActiveStorage(g_storage);
}

const StorageMap& ReadStorage()
{
    if (const auto overlay = snowdesktop::widget_runtime::CurrentStorageOverlay())
        return *overlay;
    return g_settingsPreviews.Read(g_storage);
}


void LoadStorageFile()
{
    g_settingsPreviews.Clear();
    g_storage.clear();
    if (g_storagePath.empty()) return;

    std::error_code ec;
    const bool storageExists =
        std::filesystem::is_regular_file(g_storagePath, ec);
    if (storageExists && !snowdesktop::widget_runtime::storage_persistence::ReadStorageMap(
            g_storagePath, g_storage, IsRemovedPanelEffectSettingKey))
    {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        wchar_t suffix[64]{};
        swprintf_s(suffix, L".corrupt-%04u%02u%02u-%02u%02u%02u.json",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
            now.wSecond);
        const std::wstring quarantine = g_storagePath + suffix;
        MoveFileExW(g_storagePath.c_str(), quarantine.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
}

bool SaveStorageFile()
{
    g_settingsPreviews.InvalidateReads();
    if (g_storagePath.empty()) return false;
    return snowdesktop::widget_runtime::storage_persistence::SaveStorageMap(g_storagePath, g_storage);
}

StorageMap& MutablePersistentStorage() noexcept
{
    g_settingsPreviews.InvalidateReads();
    return g_storage;
}

const StorageMap& PersistentStorage() noexcept
{
    return g_storage;
}

bool StageStoragePreview(const void* owner, const std::string& prefix,
    const StorageMap& previous, const StorageMap& candidate)
{
    return g_settingsPreviews.Update(owner, prefix, previous, candidate);
}

bool CommitStoragePreview(const void* owner)
{
    return g_settingsPreviews.Commit(owner, g_storage, [] { return SaveStorageFile(); },
        [](const auto& candidate, const std::string& prefix) {
            WidgetStorageTransaction transaction(candidate, prefix.substr(0, prefix.size() - 1));
            std::string error;
            return transaction.ValidateCommit(error);
        });
}

void RemoveStoragePreview(const void* owner) noexcept
{
    g_settingsPreviews.Remove(owner);
}

bool StorageWriteWithinQuota(const std::string& prefix,
    const std::string& key, const std::string& value)
{
    constexpr std::size_t kMaxStorageKeys = 256;
    constexpr std::size_t kMaxStorageKeyBytes = 128;
    constexpr std::size_t kMaxStorageValueBytes = 64 * 1024;
    constexpr std::size_t kMaxStorageBytes = 1024 * 1024;
    if (key.empty() || key.size() > kMaxStorageKeyBytes ||
        value.size() > kMaxStorageValueBytes)
        return false;
    const std::string fullKey = prefix + "." + key;
    std::size_t count = 0;
    std::size_t bytes = 0;
    for (const auto& [storedKey, storedValue] : ActiveStorage())
    {
        if (!storedKey.starts_with(prefix + ".")) continue;
        if (storedKey.starts_with(prefix + ".__host.")) continue;
        if (storedKey == fullKey) continue;
        ++count;
        bytes += storedKey.size() + storedValue.size();
    }
    return count < kMaxStorageKeys &&
        bytes + fullKey.size() + value.size() <= kMaxStorageBytes;
}

}
