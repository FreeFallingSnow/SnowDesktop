#pragma once

#include "types.h"
#include <string>
#include <vector>

class WidgetEngine;

namespace snowdesktop
{

struct WidgetPreviewItem
{
    std::wstring key;
    std::wstring title;
    std::wstring glyph;
    std::wstring categoryId = L"all";
    std::wstring dateGroup;
    bool directory = false;
};

/** In-memory data graph used only while a component preview is rendered. */
class WidgetPreviewScene
{
public:
    WidgetPreviewItem& AddItem(WidgetPreviewItem item);
    DesktopWidget& AddWidget(DesktopWidget widget);
    void PreparePlaceholderModels(int bitmapSize, bool lightTheme);
    const WidgetPreviewItem* FindItem(const std::wstring& key) const;
    DesktopItem* FindDesktopItem(const std::wstring& key);
    FolderEntry* FindFolderEntry(const std::wstring& key);
    DesktopWidget* FindWidget(const std::wstring& id);
    const DesktopWidget* FindWidget(const std::wstring& id) const;

    const std::vector<WidgetPreviewItem>& Items() const { return items_; }
    const std::vector<DesktopWidget>& Widgets() const { return widgets_; }

private:
    std::vector<WidgetPreviewItem> items_;
    std::vector<DesktopItem> desktopItems_;
    std::vector<FolderEntry> folderEntries_;
    std::vector<DesktopWidget> widgets_;
};

struct WidgetRenderOptions
{
    WidgetPreviewScene* previewScene = nullptr;
    WidgetEngine* luaEngine = nullptr;
    POINT pointer{ -32000, -32000 };
    RECT frame{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool interactive = false;
    bool registerBackdrop = true;
};

} // namespace snowdesktop
