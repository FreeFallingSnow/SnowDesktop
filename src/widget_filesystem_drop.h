#pragma once

#include "widget_filesystem_handle_store.h"
#include <windows.h>
#include <algorithm>
#include <vector>

namespace snowdesktop::widget_runtime
{
// A failed drop or failed Lua delivery must only roll back this drop's grants.
class WidgetFilesystemDropGrant final
{
public:
    WidgetFilesystemDropGrant(WidgetFilesystemHandleStore& store,
        WidgetFilesystemHandleOwner owner) : store_(store), owner_(std::move(owner)) {}
    WidgetFilesystemDropGrant(const WidgetFilesystemDropGrant&) = delete;
    WidgetFilesystemDropGrant& operator=(const WidgetFilesystemDropGrant&) = delete;
    ~WidgetFilesystemDropGrant()
    {
        if (committed_) return;
        for (const auto& handle : created_)
        {
            std::string error;
            (void)store_.Revoke(owner_, handle, error);
        }
    }
    bool Acquire(const std::vector<std::wstring>& paths)
    {
        if (paths.empty() || paths.size() > 128 || !items.empty()) return false;
        for (const auto& path : paths)
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return false;
            auto grant = store_.Grant(owner_, path,
                (attributes & FILE_ATTRIBUTE_DIRECTORY) ? WidgetFilesystemHandleKind::Folder : WidgetFilesystemHandleKind::File,
                WidgetFilesystemHandleAccess::Read);
            if (!grant) return false;
            if (grant.created) created_.push_back(grant.entry->handle);
            if (std::none_of(items.begin(), items.end(), [&](const auto& item) {
                    return item.handle == grant.entry->handle;
                })) items.push_back(std::move(*grant.entry));
        }
        return true;
    }
    void Commit() noexcept { committed_ = true; }
    std::vector<WidgetFilesystemHandleEntry> items;
private:
    WidgetFilesystemHandleStore& store_;
    WidgetFilesystemHandleOwner owner_;
    std::vector<std::string> created_;
    bool committed_ = false;
};
}
