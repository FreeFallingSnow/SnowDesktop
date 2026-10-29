#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "app/popup/categorized_popup_scope.h"
#include "widget/runtime/widget_item_layout.h"

bool DesktopApp::TryHandleTypeToSelectChar(wchar_t ch, bool repeated)
{
    using namespace snowdesktop::type_to_select;
    InputContext context;
    context.textInput = renameController_.IsActive() ||
        (widgetEngine_ && widgetEngine_->HasFocusedHostInput());
    if (auto* view = GetCategorizedPopupView(); view && view->IsSearchFocused())
        context.textInput = true;
    for (const auto& container : containers_)
        if (auto* view = dynamic_cast<ScrollingItemWidget*>(container.get());
            view && view->IsSearchFocused())
            context.textInput = true;
    context.overlay = quickNavigationOpen_ ||
        !luaWidgetPanelRequest_.widgetId.empty() || dockTrayPopup_ ||
        HasActiveContextMenuSession() || typeToSelectSuppressChar_;
    context.gesture = mouseDown_ || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() ||
        widgetAction_ != WidgetAction::None || largeIconGesture_ ||
        middleButtonWidgetMove_ || marqueeActive_;
    context.commandModifiers =
        (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
        (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) != 0 ||
        (snowdesktop::private_pointer::AsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (snowdesktop::private_pointer::AsyncKeyState(VK_RWIN) & 0x8000) != 0;
    if (!AllowsTyping(context))
    {
        typeToSelectSession_.Reset();
        return false;
    }

    const bool popup = IsCollectionPopupInteractive();
    const size_t widgetIndex = popup ? popupWidgetIndex_ : GetSelectionWidgetIndex();
    DesktopWidget* widget = popup ? GetOpenPopupWidget() :
        (widgetIndex < widgets_.size() ? &widgets_[widgetIndex] : nullptr);
    std::wstring scope;
    std::vector<SelectionController::Target> targets;
    if (popup)
    {
        scope = GetPopupSelectionScope();
        targets = GetPopupSelectionTargets();
    }
    else if (widget)
    {
        if (IsWidgetCollapsed(*widget) ||
            (desktopIconsHidden_ && !widget->keepWhenDesktopHidden) ||
            std::none_of(gridPages_.begin(), gridPages_.end(),
                [&](const GridPage& page) { return page.id == widget->gridCell.pageId; }))
        {
            typeToSelectSession_.Reset();
            return false;
        }
        scope = GetWidgetSelectionScope(widgetIndex);
        targets = GetWidgetSelectionTargets(widgetIndex);
    }
    else
    {
        // Keep keyboard selection on its display. Without a selected file,
        // seed the scope from the display under the pointer, as arrow navigation does.
        std::wstring pageId;
        for (const auto& item : items_)
            if (item.selected && !IsItemInAnyWidget(item) &&
                std::any_of(gridPages_.begin(), gridPages_.end(),
                    [&](const GridPage& page) { return page.id == item.gridCell.pageId; }))
            {
                pageId = item.gridCell.pageId;
                break;
            }
        if (pageId.empty())
        {
            POINT point{};
            if (snowdesktop::private_pointer::CursorPosition(&point))
                for (const auto& page : gridPages_)
                    if (PtInRect(&page.workArea, point)) { pageId = page.id; break; }
        }
        if (pageId.empty() && !gridPages_.empty()) pageId = gridPages_.front().id;
        if (pageId.empty()) return false;
        scope = L"desktop:" + pageId;
        targets = GetDesktopSelectionTargets(pageId);
    }

    struct Name
    {
        std::wstring_view text;
        size_t itemIndex = static_cast<size_t>(-1);
        size_t entryIndex = static_cast<size_t>(-1);
    };
    std::unordered_map<const bool*, Name> names;
    names.reserve(items_.size());
    for (size_t index = 0; index < items_.size(); ++index)
        names.emplace(&items_[index].selected,
            Name{items_[index].name, index});
    const auto appendFolders = [&](const DesktopWidget& owner) {
        for (size_t index = 0; index < owner.folderEntries.size(); ++index)
            names.emplace(&owner.folderEntries[index].selected,
                Name{owner.folderEntries[index].name, static_cast<size_t>(-1), index});
    };
    if (widget)
    {
        appendFolders(*widget);
        if (widget->type == DesktopWidgetType::FileGroup)
            for (const auto& childId : widget->childWidgetIds)
            {
                const size_t child = FindWidgetIndexById(childId);
                if (child < widgets_.size()) appendFolders(widgets_[child]);
            }
    }
    std::vector<Candidate> candidates;
    candidates.reserve(targets.size());
    for (const auto& target : targets)
    {
        const auto name = names.find(target.selected);
        candidates.push_back({name == names.end() ? std::wstring_view{} : name->second.text,
            target.selected && *target.selected});
    }
    const auto match = typeToSelectSession_.Input(
        ch, GetTickCount64(), scope, candidates, context, repeated);
    if (!match) return ch >= L' ' && ch != 0x7f;

    const auto& target = targets[*match];
    const auto& name = names.at(target.selected);
    size_t visibleIndex = *match;
    if (widget && name.itemIndex < items_.size())
    {
        std::vector<std::wstring> keys;
        if (popup && !GetGroupPopupView()) keys = GetPopupItemKeys(*widget);
        else
            for (const auto& container : containers_)
                if (auto* view = dynamic_cast<WidgetContainer*>(container.get());
                    view && view->GetWidgetData() == widget)
                {
                    if (auto* group = dynamic_cast<CollectionGroup*>(view))
                        keys = group->GetVisibleItemKeys();
                    else if (auto* fileGroup = dynamic_cast<FileGroup*>(view))
                    {
                        if (!fileGroup->IsGroupSearchActive()) keys = fileGroup->GetHostedVisibleItemKeys();
                    }
                    else if (auto* categories = dynamic_cast<FileCategories*>(view))
                        keys = categories->GetSearchResultKeys();
                    else if (widget->type == DesktopWidgetType::Collection) keys = widget->itemKeys;
                    break;
                }
        const auto key = std::find_if(keys.begin(), keys.end(),
            [&](const std::wstring& value) { return FindItemIndexByKey(value) == name.itemIndex; });
        if (key != keys.end()) visibleIndex = static_cast<size_t>(key - keys.begin());
    }
    ClearSelection();
    if (dockFolderPopupOpen_)
        for (auto& entry : dockFolderPopupWidget_.folderEntries) entry.selected = false;
    *target.selected = true;
    selectionController_.RememberAnchor(scope, target.key);
    keyboardNavVisualFocus_ = true;
    popupFanActionFocused_ = false;

    if (widgetIndex < widgets_.size() && widget)
    {
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = widgetIndex;
        // Folder mappings store a source index; group views store a visible
        // index; simple collections/category panels store their item-key index.
        keyboardNavMemberIndex_ = static_cast<int>(visibleIndex);
        if (widget->type == DesktopWidgetType::FolderMapping)
            keyboardNavMemberIndex_ = static_cast<int>(name.entryIndex);
        else if ((widget->type == DesktopWidgetType::Collection ||
                    widget->type == DesktopWidgetType::FileCategories) &&
            name.itemIndex < items_.size())
        {
            const auto key = std::find_if(widget->itemKeys.begin(), widget->itemKeys.end(),
                [&](const std::wstring& value) {
                    return FindItemIndexByKey(value) == name.itemIndex;
                });
            keyboardNavMemberIndex_ = key == widget->itemKeys.end() ? -1 :
                static_cast<int>(key - widget->itemKeys.begin());
        }
    }

    if (popup && widget)
    {
        if (UsesCollectionPopupFan(*widget))
            EnsureCollectionPopupFanItemVisible(visibleIndex);
        else if (auto* group = GetGroupPopupView())
        {
            CategorizedPopupScope popupScope(group, GetCategorizedPopupFrame(popupRect_));
            ScrollWidgetToMember(widgetIndex, static_cast<int>(visibleIndex));
        }
        else
        {
            const RECT viewport = GetCollectionPopupContentRect(popupRect_);
            const RECT bounds = GetCollectionPopupItemRect(popupRect_, visibleIndex);
            popupScrollOffset_ = snowdesktop::widget_item_layout::ScrollOffsetToReveal(
                viewport, bounds, popupScrollOffset_,
                GetCollectionPopupMaxScrollOffset(*widget, popupRect_));
            ResetCollectionPopupAnimationCache();
        }
    }
    else if (widget)
    {
        ScrollWidgetToMember(widgetIndex, static_cast<int>(visibleIndex));
        // A compact collection's members beyond its inline capacity are shown
        // in the existing popup rather than leaving a hidden selection.
        if (widget->type == DesktopWidgetType::Collection && !widget->scrollContainerMode)
        {
            for (const auto& container : containers_)
                if (auto* collection = dynamic_cast<Collection*>(container.get());
                    collection && collection->GetWidgetData() == widget &&
                    visibleIndex >= collection->GetSlotCount())
                {
                    OpenCollectionPopupAt(widgetIndex,
                        POINT{widget->bounds.right - 1, widget->bounds.bottom - 1});
                    typeToSelectSession_.ContinueInScope(GetPopupSelectionScope());
                    if (UsesCollectionPopupFan(*widget))
                        EnsureCollectionPopupFanItemVisible(visibleIndex);
                    else
                    {
                        const RECT viewport = GetCollectionPopupContentRect(popupRect_);
                        popupScrollOffset_ = snowdesktop::widget_item_layout::ScrollOffsetToReveal(
                            viewport, GetCollectionPopupItemRect(popupRect_, visibleIndex),
                            popupScrollOffset_, GetCollectionPopupMaxScrollOffset(*widget, popupRect_));
                    }
                    break;
                }
        }
    }
    CancelRenameClick();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (popup || IsCollectionPopupInteractive()) InvalidateFloatingPopupWindow();
    return true;
}
