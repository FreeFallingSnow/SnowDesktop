#include "ui/input/private_pointer.h"
#include "app/app.h"

std::wstring DesktopApp::GetSelectedFilePreviewPath()
{
    using snowdesktop::file_preview::IsFileSystemPath;
    const auto searchPath = [](ScrollingItemWidget* view) -> std::optional<std::wstring> {
        if (!view || !view->UsesFolderSearch()) return std::nullopt;
        // Everything descendants belong to the current search projection, not
        // to items_ or the source widget's persistent folderEntries.
        for (auto* item : view->FolderSearchSelectedItems())
        {
            if (const auto* desktop = dynamic_cast<DesktopIcon*>(item);
                desktop && !desktop->GetDesktopItem()->desktopIconClsid.empty()) continue;
            auto path = item->GetPath();
            if (snowdesktop::file_preview::IsFileSystemPath(path)) return path;
        }
        // An active search with no eligible selection owns the scope too.
        return std::wstring{};
    };
    std::vector<SelectionController::Target> targets;
    if (IsCollectionPopupInteractive())
    {
        if (auto path = searchPath(GetCategorizedPopupView())) return *path;
        targets = GetPopupSelectionTargets();
    }
    else if (const auto index = GetSelectionWidgetIndex(); index < widgets_.size())
    {
        for (const auto& container : containers_)
            if (auto* view = dynamic_cast<ScrollingItemWidget*>(container.get());
                view && view->GetWidgetData() == &widgets_[index])
            {
                if (auto path = searchPath(view)) return *path;
                break;
            }
        targets = GetWidgetSelectionTargets(index);
    }
    else
    {
        for (const auto& entry : dockEntries_)
        {
            if (!entry.selected || entry.type != DockEntryType::DesktopItem) continue;
            const auto itemIndex = FindItemIndexByKey(entry.reference);
            if (itemIndex < items_.size() && items_[itemIndex].desktopIconClsid.empty() &&
                IsFileSystemPath(items_[itemIndex].parsingName)) return items_[itemIndex].parsingName;
        }
        targets = GetDesktopSelectionTargets();
    }
    // Use the visible scope's existing selection projection, including filtered
    // FileGroup sources. Never preview an old selection hidden behind a popup.
    for (const auto& target : targets)
    {
        if (!target.selected || !*target.selected) continue;
        for (const auto& item : items_)
            if (target.selected == &item.selected && item.desktopIconClsid.empty() &&
                IsFileSystemPath(item.parsingName)) return item.parsingName;
        const auto findEntry = [&](const DesktopWidget& widget) -> std::wstring {
            for (const auto& entry : widget.folderEntries)
                if (target.selected == &entry.selected && IsFileSystemPath(entry.fullPath))
                    return entry.fullPath;
            return {};
        };
        if (auto path = findEntry(dockFolderPopupWidget_); !path.empty()) return path;
        for (const auto& widget : widgets_)
            if (auto path = findEntry(widget); !path.empty()) return path;
    }
    return {};
}

bool DesktopApp::TryHandleFilePreviewKey(WPARAM key, bool repeated)
{
    using namespace snowdesktop::file_preview;
    const auto available = filePreview_.Current();
    if (std::none_of(available.providers.begin(), available.providers.end(),
            [key](const Provider& provider) { return provider.shortcut.key == key; })) return false;
    InputContext context;
    context.textInput = renameController_.IsActive() || typeToSelectImeComposing_ ||
        (widgetEngine_ && (widgetEngine_->HasFocusedHostInput() || widgetEngine_->IsHostInputComposing()));
    if (auto* view = GetCategorizedPopupView(); view && view->IsSearchFocused()) context.textInput = true;
    for (const auto& container : containers_)
        if (auto* view = dynamic_cast<ScrollingItemWidget*>(container.get());
            view && view->IsSearchFocused()) context.textInput = true;
    context.overlay = quickNavigationOpen_ || !luaWidgetPanelRequest_.widgetId.empty() ||
        dockTrayPopup_ || HasActiveContextMenuSession() || (systemPanel_ && systemPanel_->IsOpen()) ||
        (widgetEngine_ && widgetEngine_->HasHostViewKeyboardFocus());
    context.gesture = mouseDown_ || dragSession_.HasContext() || dragDropController_.IsTransportActive() ||
        widgetAction_ != WidgetAction::None || largeIconGesture_ || middleButtonWidgetMove_ ||
        marqueeActive_ || detailColumnResizeActive_ || widgetScrollbarDragging_ || popupScrollbarDragging_;
    if (!AllowsPreview(context)) return false;
    Shortcut pressed{static_cast<UINT>(key), 0};
    if (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) pressed.modifiers |= MOD_CONTROL;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) pressed.modifiers |= MOD_ALT;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000) pressed.modifiers |= MOD_SHIFT;
    if ((snowdesktop::private_pointer::AsyncKeyState(VK_LWIN) | snowdesktop::private_pointer::AsyncKeyState(VK_RWIN)) & 0x8000) pressed.modifiers |= MOD_WIN;
    if (!MatchProvider(available, pressed)) return false;
    auto path = GetSelectedFilePreviewPath();
    if (path.empty()) return false;
    const bool accepted = filePreview_.Request(pressed, std::move(path), repeated);
    if (accepted) CancelRenameClick();
    return accepted;
}
