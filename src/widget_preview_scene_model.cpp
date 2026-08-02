#include "widget_preview_scene.h"

#include <algorithm>

namespace snowdesktop
{

WidgetPreviewItem& WidgetPreviewScene::AddItem(WidgetPreviewItem item)
{
    items_.push_back(std::move(item));
    return items_.back();
}

DesktopWidget& WidgetPreviewScene::AddWidget(DesktopWidget widget)
{
    widgets_.push_back(std::move(widget));
    return widgets_.back();
}

const WidgetPreviewItem* WidgetPreviewScene::FindItem(
    const std::wstring& key) const
{
    const auto found = std::find_if(items_.begin(), items_.end(),
        [&](const WidgetPreviewItem& item) { return item.key == key; });
    return found == items_.end() ? nullptr : &*found;
}

DesktopItem* WidgetPreviewScene::FindDesktopItem(
    const std::wstring& key)
{
    const auto found = std::find_if(desktopItems_.begin(), desktopItems_.end(),
        [&](const DesktopItem& item) { return item.layoutKey == key; });
    return found == desktopItems_.end() ? nullptr : &*found;
}

FolderEntry* WidgetPreviewScene::FindFolderEntry(
    const std::wstring& key)
{
    const auto found = std::find_if(folderEntries_.begin(), folderEntries_.end(),
        [&](const FolderEntry& item) { return item.fullPath == key; });
    return found == folderEntries_.end() ? nullptr : &*found;
}

DesktopWidget* WidgetPreviewScene::FindWidget(const std::wstring& id)
{
    const auto found = std::find_if(widgets_.begin(), widgets_.end(),
        [&](const DesktopWidget& widget) { return widget.id == id; });
    return found == widgets_.end() ? nullptr : &*found;
}

const DesktopWidget* WidgetPreviewScene::FindWidget(
    const std::wstring& id) const
{
    const auto found = std::find_if(widgets_.begin(), widgets_.end(),
        [&](const DesktopWidget& widget) { return widget.id == id; });
    return found == widgets_.end() ? nullptr : &*found;
}

} // namespace snowdesktop
