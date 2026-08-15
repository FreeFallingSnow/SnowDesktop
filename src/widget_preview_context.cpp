#include "widget_preview_context.h"

#include <chrono>

namespace snowdesktop::widget_runtime
{
namespace
{
thread_local StorageMap* g_storageOverlay = nullptr;
thread_local bool g_dryLoad = false;
thread_local bool g_previewExecution = false;
}

bool IsDryLoad() noexcept
{
    return g_dryLoad;
}

bool IsPreviewExecution() noexcept
{
    return g_previewExecution;
}

StorageMap* CurrentStorageOverlay() noexcept
{
    return g_storageOverlay;
}

bool HasStorageOverlay() noexcept
{
    return g_storageOverlay != nullptr;
}

StorageMap& ActiveStorage(StorageMap& persistentStorage) noexcept
{
    return g_storageOverlay ? *g_storageOverlay : persistentStorage;
}

std::int64_t CurrentWallClockMilliseconds() noexcept
{
    if (g_previewExecution)
        return PreviewWallClockMilliseconds;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t CurrentMonotonicMilliseconds() noexcept
{
    if (g_previewExecution)
        return PreviewMonotonicMilliseconds;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

StorageOverlayScope::StorageOverlayScope(
    StorageMap* storageOverlay) noexcept
    : previousOverlay_(g_storageOverlay)
{
    g_storageOverlay = storageOverlay;
}

StorageOverlayScope::~StorageOverlayScope()
{
    g_storageOverlay = previousOverlay_;
}

DryLoadScope::DryLoadScope(bool dryLoad) noexcept
    : previousDryLoad_(g_dryLoad)
{
    g_dryLoad = dryLoad;
}

DryLoadScope::~DryLoadScope()
{
    g_dryLoad = previousDryLoad_;
}

PreviewExecutionScope::PreviewExecutionScope(
    StorageMap* previewStorage) noexcept
    : previousOverlay_(g_storageOverlay),
      previousDryLoad_(g_dryLoad),
      previousPreviewExecution_(g_previewExecution),
      active_(previewStorage != nullptr)
{
    if (active_)
    {
        g_storageOverlay = previewStorage;
        g_dryLoad = true;
        g_previewExecution = true;
    }
}

PreviewExecutionScope::~PreviewExecutionScope()
{
    if (active_)
    {
        g_storageOverlay = previousOverlay_;
        g_dryLoad = previousDryLoad_;
        g_previewExecution = previousPreviewExecution_;
    }
}
}
