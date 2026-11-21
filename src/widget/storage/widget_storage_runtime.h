#pragma once

#include "widget/preview/widget_preview_context.h"

namespace snowdesktop::widget_runtime
{
void OpenStorageFile(std::wstring path);
void LoadStorageFile();
bool SaveStorageFile();
StorageMap& ActiveStorage();
const StorageMap& ReadStorage();
StorageMap& MutablePersistentStorage() noexcept;
const StorageMap& PersistentStorage() noexcept;
bool StageStoragePreview(const void* owner, const std::string& prefix,
    const StorageMap& previous, const StorageMap& candidate);
bool CommitStoragePreview(const void* owner);
void RemoveStoragePreview(const void* owner) noexcept;
bool StorageWriteWithinQuota(const std::string& prefix,
    const std::string& key, const std::string& value);
}
