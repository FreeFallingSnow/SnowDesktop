#pragma once

#include "widget_filesystem_handle_store.h"

#include <windows.h>
#include <memory>

namespace snowdesktop::widget_runtime
{
// Captures the directory actually opened for ReadDirectoryChangesW. Queued
// events retain its identity after Stop without extending the OS watch lifetime.
class WidgetFilesystemWatchOrigin final
{
public:
    static std::shared_ptr<const WidgetFilesystemWatchOrigin> Open(
        const std::filesystem::path& directory, std::string& error);
    ~WidgetFilesystemWatchOrigin();
    // The worker takes ownership before publishing this origin to consumers.
    HANDLE TakeDirectoryHandle() const noexcept;
    WidgetFilesystemHandleGrantResult GrantChild(WidgetFilesystemHandleStore& store,
        WidgetFilesystemHandleOwner owner, std::wstring_view name,
        WidgetFilesystemHandleAccess access) const;

private:
    struct Impl;
    explicit WidgetFilesystemWatchOrigin(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}
