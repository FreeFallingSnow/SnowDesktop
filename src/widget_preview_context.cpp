#include "widget_preview_context.h"

namespace snowdesktop::widget_runtime
{
namespace
{
thread_local StorageMap* g_storageOverlay = nullptr;
thread_local bool g_dryLoad = false;
}

bool IsDryLoad() noexcept
{
    return g_dryLoad;
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
      active_(previewStorage != nullptr)
{
    if (active_)
    {
        g_storageOverlay = previewStorage;
        g_dryLoad = true;
    }
}

PreviewExecutionScope::~PreviewExecutionScope()
{
    if (active_)
    {
        g_storageOverlay = previousOverlay_;
        g_dryLoad = previousDryLoad_;
    }
}
}
