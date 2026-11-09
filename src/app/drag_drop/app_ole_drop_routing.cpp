#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "app/popup/categorized_popup_scope.h"
#include "dock/dock_window_preview.h"
#include "widget/runtime/widget_engine.h"
#include "common/utils.h"
#include "drag_drop/ole_drag_rules.h"

// OLE surface classification, effect choice and coordinate conversion.

size_t DesktopApp::HitTestLuaFileDropTarget(POINT point) const
{
    if (!widgetEngine_ || IsPointOccludedByOpenPopup(point))
        return static_cast<size_t>(-1);
    const auto index = HitTestStandaloneWidgetIndex(point);
    if (index >= widgets_.size() || !IsLuaDesktopSurface(widgets_[index]) ||
        HitTestStandaloneWidget(index, point) != WidgetHit::Content)
        return static_cast<size_t>(-1);
    const auto* content = ResolveLuaWidgetContent(&widgets_[index]);
    if (!content) return static_cast<size_t>(-1);
    const RECT frame = GetStandaloneWidgetFrameRect(widgets_[index]);
    return widgetEngine_->HasFileDropTarget(content->id,
        point.x - frame.left, point.y - frame.top) ? index : static_cast<size_t>(-1);
}

bool DesktopApp::DeliverLuaFileDrop(POINT point, const std::vector<std::wstring>& paths)
{
    const auto index = HitTestLuaFileDropTarget(point);
    if (index >= widgets_.size()) return false;
    const auto* content = ResolveLuaWidgetContent(&widgets_[index]);
    if (!content) return false;
    const std::wstring instanceId = content->id;
    const RECT frame = GetStandaloneWidgetFrameRect(widgets_[index]);
    return widgetEngine_->InvokeFileDrop(instanceId,
        point.x - frame.left, point.y - frame.top, paths);
}

std::optional<DesktopApp::LuaTextDropSurface> DesktopApp::FindLuaTextDropSurface(
    POINT point) const
{
    if (!widgetEngine_ || desktopPassthroughActive_) return std::nullopt;
    if (!luaWidgetPanelAnimation_.IsHidden())
    {
        const RECT panel = GetLuaWidgetPanelRect();
        const RECT content = GetLuaWidgetPanelContentRect();
        if (luaWidgetPanelAnimation_.IsInteractive() &&
            !luaWidgetPanelRequest_.widgetId.empty() && PtInRect(&content, point))
        {
            return LuaTextDropSurface{luaWidgetPanelRequest_.widgetId,
                luaWidgetPanelRequest_.surface,
                {point.x - content.left, point.y - content.top}};
        }
        if (luaWidgetPanelRequest_.modal || PtInRect(&panel, point)) return std::nullopt;
    }
    if (IsPointOccludedByOpenPopup(point)) return std::nullopt;
    const auto index = HitTestStandaloneWidgetIndex(point);
    if (index >= widgets_.size() ||
        HitTestStandaloneWidget(index, point) != WidgetHit::Content)
        return std::nullopt;
    const RECT frame = GetStandaloneWidgetFrameRect(widgets_[index]);
    const auto* content = ResolveLuaWidgetContent(&widgets_[index]);
    if (!content) return std::nullopt;
    return LuaTextDropSurface{content->id, "desktop",
        {point.x - frame.left, point.y - frame.top}};
}

ScrollingItemWidget* DesktopApp::FindNativeSearchTextDropSurface(
    POINT point, std::optional<RECT>& popupFrame) const
{
    popupFrame.reset();
    if (desktopPassthroughActive_) return nullptr;
    if (!luaWidgetPanelAnimation_.IsHidden())
    {
        const RECT panel = GetLuaWidgetPanelRect();
        if (luaWidgetPanelRequest_.modal || PtInRect(&panel, point)) return nullptr;
    }
    if (IsPointOccludedByOpenPopup(point))
    {
        if (!IsCollectionPopupInteractive()) return nullptr;
        auto* view = GetCategorizedPopupView();
        if (!view) return nullptr;
        const RECT frame = GetCategorizedPopupFrame(GetCollectionPopupRect(*GetOpenPopupWidget()));
        CategorizedPopupScope scope(view, frame);
        const RECT search = view->GetSearchBoxRect();
        if (!PtInRect(&search, point)) return nullptr;
        popupFrame = frame;
        return view;
    }
    if (HitTestStandaloneWidgetIndex(point) < widgets_.size())
        return nullptr;
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        auto* container = dynamic_cast<WidgetContainer*>(it->get());
        const auto* widget = container ? container->GetWidgetData() : nullptr;
        if (!widget || IsGroupedWidget(*widget) ||
            (desktopIconsHidden_ && !widget->keepWhenDesktopHidden))
            continue;
        const RECT frame = container->GetFrameRect();
        if (PtInRect(&frame, point))
            return container->HitTestWidget(point) == WidgetHit::SearchBox
                ? dynamic_cast<ScrollingItemWidget*>(container) : nullptr;
    }
    return nullptr;
}

const RECT* DesktopApp::GetNativeSearchTextDropCaret(const ScrollingItemWidget& view,
    const RECT& searchRect) const
{
    const auto* widget = view.GetWidgetData();
    return widget && nativeSearchTextDropPreview_ &&
        widget->id == nativeSearchTextDropPreview_->widgetId &&
        EqualRect(&searchRect, &nativeSearchTextDropPreview_->searchRect)
        ? &nativeSearchTextDropPreview_->caret : nullptr;
}

bool DesktopApp::IsWidgetTextComposing() const
{
    if ((widgetEngine_ && widgetEngine_->IsHostInputComposing()) ||
        (dockFolderPopupContainer_ && dockFolderPopupContainer_->IsSearchComposing()))
        return true;
    for (const auto& container : containers_)
        if (const auto* view = dynamic_cast<const ScrollingItemWidget*>(container.get());
            view && view->IsSearchComposing())
            return true;
    return false;
}

const snowdesktop::text_input::DragSelection* DesktopApp::WidgetTextMoveSelection(
    std::wstring_view widgetId, std::string_view controlId, std::string_view surface) const
{
    if (!widgetTextDragSource_) return nullptr;
    const auto& source = *widgetTextDragSource_;
    return source.widgetId == widgetId && source.controlId == controlId &&
        source.surface == surface &&
        (!source.popupGeneration || *source.popupGeneration == floatingPopupMouseHookGeneration_)
        ? &source.selection : nullptr;
}

void DesktopApp::InvalidateNativeSearchTextDrop(const std::wstring& widgetId, bool popup)
{
    if (popup)
        InvalidateCollectionPopupAnimation(false);
    else if (FindWidgetIndexById(widgetId) < widgets_.size())
        (void)QueueDesktopWidgetComposition(widgetId);
}

bool DesktopApp::ClearWidgetTextDropPreview()
{
    widgetTextDropMovesSelection_ = false;
    const bool hadLuaPreview = widgetEngine_ && widgetEngine_->ClearHostInputTextDropPreview();
    if (!nativeSearchTextDropPreview_) return hadLuaPreview;
    const auto old = std::exchange(nativeSearchTextDropPreview_, std::nullopt);
    InvalidateNativeSearchTextDrop(old->widgetId, old->popup);
    return true;
}

bool DesktopApp::HandleWidgetTextDragHover(
    POINT point, DWORD keyState, DWORD& effect)
{
    widgetTextDropMovesSelection_ = false;
    if (!externalWidgetTextAvailable_ || (keyState & MK_RBUTTON))
    {
        ClearWidgetTextDropPreview();
        return false;
    }
    const bool canCopy = (effect & DROPEFFECT_COPY) != 0 && !IsWidgetTextComposing();
    std::optional<bool> accepted;
    const snowdesktop::text_input::DragSelection* move = nullptr;
    std::optional<RECT> popupFrame;
    if (auto* view = FindNativeSearchTextDropSurface(point, popupFrame))
    {
        const std::wstring widgetId = view->GetWidgetData()->id;
        move = WidgetTextMoveSelection(widgetId, {}, "search");
        RECT caret{};
        RECT searchRect{};
        {
            std::optional<CategorizedPopupScope> scope;
            if (popupFrame) scope.emplace(view, *popupFrame);
            searchRect = view->GetSearchBoxRect();
            accepted = canCopy && view->GetSearchTextDropCaret(point, caret, move);
        }
        if (*accepted)
        {
            if (widgetEngine_) widgetEngine_->ClearHostInputTextDropPreview();
            const bool popup = popupFrame.has_value();
            if (!nativeSearchTextDropPreview_ ||
                nativeSearchTextDropPreview_->widgetId != widgetId ||
                nativeSearchTextDropPreview_->popup != popup)
                ClearWidgetTextDropPreview();
            if (!nativeSearchTextDropPreview_ ||
                !EqualRect(&nativeSearchTextDropPreview_->searchRect, &searchRect) ||
                !EqualRect(&nativeSearchTextDropPreview_->caret, &caret))
            {
                nativeSearchTextDropPreview_ = NativeSearchTextDropPreview{
                    widgetId, searchRect, caret, popup};
                InvalidateNativeSearchTextDrop(widgetId, popup);
            }
        }
        else
            ClearWidgetTextDropPreview();
    }
    else
    {
        if (nativeSearchTextDropPreview_) ClearWidgetTextDropPreview();
        if (const auto targetSurface = FindLuaTextDropSurface(point))
        {
            if (widgetTextDragSource_ && widgetTextDragSource_->widgetId == targetSurface->widgetId &&
                widgetTextDragSource_->surface == targetSurface->surface)
            {
                if (const auto key = widgetEngine_->FindHostInputTextDropTarget(
                        targetSurface->widgetId, targetSurface->local, targetSurface->surface))
                    move = WidgetTextMoveSelection(targetSurface->widgetId, *key, targetSurface->surface);
            }
            accepted = widgetEngine_->UpdateHostInputTextDropPreview(
                targetSurface->widgetId, targetSurface->local, targetSurface->surface, canCopy, move);
        }
        else if (widgetEngine_)
            widgetEngine_->ClearHostInputTextDropPreview();
    }
    if (!accepted) return false;

    CancelCollectionPopupDwell();
    CancelCollectionGroupTabDwell();
    dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
    HideDragHintWindow();
    effect = *accepted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    widgetTextDropMovesSelection_ = *accepted && move;
    return true;
}

std::optional<bool> DesktopApp::DeliverWidgetTextDrop(
    POINT point, IDataObject* source, bool copyAllowed)
{
    std::optional<RECT> popupFrame;
    if (auto* view = FindNativeSearchTextDropSurface(point, popupFrame))
    {
        if (!copyAllowed || IsWidgetTextComposing()) return false;
        const std::wstring widgetId = view->GetWidgetData()->id;
        const bool popup = popupFrame.has_value();
        const auto* move = WidgetTextMoveSelection(widgetId, {}, "search");
        const std::wstring text = move ? std::wstring{} : ReadDroppedUnicodeText(source);
        if ((!move && text.empty()) || IsWidgetTextComposing()) return false;
        view = FindNativeSearchTextDropSurface(point, popupFrame);
        if (!view || popupFrame.has_value() != popup ||
            view->GetWidgetData()->id != widgetId)
            return false;
        {
            std::optional<CategorizedPopupScope> scope;
            if (popupFrame) scope.emplace(view, *popupFrame);
            if (!view->DropSearchText(point, text, move)) return false;
        }
        if (widgetEngine_) widgetEngine_->BlurHostInput(false);
        for (const auto& container : containers_)
            if (auto* other = dynamic_cast<ScrollingItemWidget*>(container.get()); other && other != view)
                other->SetSearchFocused(false);
        if (dockFolderPopupContainer_ && dockFolderPopupContainer_.get() != view)
            dockFolderPopupContainer_->SetSearchFocused(false);
        if (!popup)
        {
            const size_t index = FindWidgetIndexById(widgetId);
            if (index < widgets_.size()) SelectWidgetOnly(index);
        }
        FocusDesktopInputWindow();
        UpdateHostInputImePosition();
        InvalidateNativeSearchTextDrop(widgetId, false);
        if (popup) InvalidateNativeSearchTextDrop(widgetId, true);
        return true;
    }
    const auto targetSurface = FindLuaTextDropSurface(point);
    if (!targetSurface) return std::nullopt;
    const auto target = widgetEngine_->FindHostInputTextDropTarget(
        targetSurface->widgetId, targetSurface->local, targetSurface->surface);
    if (!target) return std::nullopt;
    if (!copyAllowed || target->empty() || IsWidgetTextComposing()) return false;

    // GetData can pump messages. Recheck the desktop surface and control
    // identity before writing to whatever is currently under the pointer.
    const auto* move = WidgetTextMoveSelection(targetSurface->widgetId, *target, targetSurface->surface);
    const std::wstring text = move ? std::wstring{} : ReadDroppedUnicodeText(source);
    if ((!move && text.empty()) || IsWidgetTextComposing()) return false;
    const auto current = FindLuaTextDropSurface(point);
    if (!current || current->widgetId != targetSurface->widgetId ||
        current->surface != targetSurface->surface ||
        !widgetEngine_->DropHostInputText(
            current->widgetId, *target, current->local, current->surface, text, move))
        return false;

    if (current->surface == "desktop") ClearSelection();
    UpdateHostInputImePosition();
    return true;
}

bool DesktopApp::TryStartWidgetTextDrag(POINT point, WPARAM keys)
{
    if (!mouseDown_ || !(keys & MK_LBUTTON) || widgetTextDragSource_ ||
        dragDropController_.IsTransportActive() || snowdesktop::private_pointer::Active())
        return false;
    if (std::abs(point.x - mouseDownPoint_.x) < GetSystemMetrics(SM_CXDRAG) &&
        std::abs(point.y - mouseDownPoint_.y) < GetSystemMetrics(SM_CYDRAG))
        return false;

    WidgetTextDragSource source;
    const auto captureSearch = [&](ScrollingItemWidget* view) {
        if (!view || !view->GetWidgetData()) return false;
        auto selected = view->TakeSearchTextDrag();
        if (!selected) return false;
        source.widgetId = view->GetWidgetData()->id;
        source.surface = "search";
        source.selection = std::move(*selected);
        if (view == dockFolderPopupContainer_.get())
            source.popupGeneration = floatingPopupMouseHookGeneration_;
        return true;
    };
    bool captured = false;
    if (auto* view = GetCategorizedPopupView())
        captured = captureSearch(view);
    if (!captured)
        for (const auto& container : containers_)
            if (captureSearch(dynamic_cast<ScrollingItemWidget*>(container.get())))
            {
                captured = true;
                break;
            }
    if (!captured && widgetEngine_)
    {
        auto selected = widgetEngine_->TakeHostInputTextDrag(
            source.widgetId, source.controlId, source.surface);
        if (selected)
        {
            source.selection = std::move(*selected);
            if (source.surface != "desktop")
                source.popupGeneration = floatingPopupMouseHookGeneration_;
            captured = true;
        }
    }
    if (!captured) return false;

    // DoDragDrop pumps a nested window loop and takes capture itself. The
    // original gesture is already consumed; it cannot become an item drag.
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    luaWidgetPanelMouseDown_ = false;
    luaWidgetPanelCaptureHwnd_ = nullptr;
    ReleaseCapturePreservingPointerState();
    RunWidgetTextDrag(std::move(source));
    return true;
}

bool DesktopApp::IsSameWindowTree(HWND parent, HWND window)
{
    return parent != nullptr && window != nullptr && (window == parent || IsChild(parent, window));
}

/**
 * @brief 判断是否为已知的桌面表层窗口
 * @param window 待检查窗口句柄
 * @return 若属于桌面表层窗口体系返回 true
 */
bool DesktopApp::IsKnownDesktopSurfaceWindow(HWND window) const
{
    if (!window) return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root) root = window;

    if (IsSameWindowTree(hwnd_, window) || window == hwnd_ || root == hwnd_) return true;
    if (luaInlineEdit_ && (IsSameWindowTree(luaInlineEdit_, window) || root == luaInlineEdit_)) return true;
    if (hintHwnd_ && (IsSameWindowTree(hintHwnd_, window) || root == hintHwnd_)) return true;
    if (controlHwnd_ && (IsSameWindowTree(controlHwnd_, window) || root == controlHwnd_)) return true;
    if (inputHwnd_ && (IsSameWindowTree(inputHwnd_, window) || root == inputHwnd_)) return true;

    auto isSurface = [&](HWND candidate) {
        return candidate && (window == candidate || root == candidate || IsChild(candidate, window));
    };
    if (isSurface(desktopWindows_.host) || isSurface(desktopWindows_.progman) ||
        isSurface(desktopWindows_.defView) || isSurface(desktopWindows_.listView))
        return true;

    HWND desktop = GetDesktopWindow();
    return window == desktop || root == desktop;
}

bool DesktopApp::IsBaseDesktopHoverSurfaceWindow(
    HWND window) const
{
    if (!window)
        return false;
    if (desktopBackdropCompositor_.IsBackdropWindow(window))
        return true;

    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    if (window == hwnd_ || root == hwnd_ ||
        IsSameWindowTree(hwnd_, window))
        return true;

    const auto belongsTo = [window, root](HWND candidate) {
        return candidate &&
            (window == candidate || root == candidate ||
                IsChild(candidate, window));
    };
    if (belongsTo(desktopWindows_.host) ||
        belongsTo(desktopWindows_.progman) ||
        belongsTo(desktopWindows_.defView) ||
        belongsTo(desktopWindows_.listView))
        return true;

    const HWND desktop = GetDesktopWindow();
    return window == desktop || root == desktop;
}

bool DesktopApp::IsDesktopInteractionSurfaceWindow(
    HWND window) const
{
    if (!window)
        return false;
    if (IsKnownDesktopSurfaceWindow(window))
        return true;

    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    const auto belongsTo = [window, root](HWND candidate) {
        return candidate &&
            (window == candidate || root == candidate ||
                IsChild(candidate, window));
    };

    if (IsPersistentDockHostWindow(window) ||
        IsPersistentDockHostWindow(root) ||
        belongsTo(floatingPopupHwnd_) ||
        belongsTo(dragPreviewHwnd_) ||
        belongsTo(quickNavigation_->hwnd))
        return true;
    if (dockWindowPreview_)
        for (const HWND popup : dockWindowPreview_->GetWindows())
            if (belongsTo(popup)) return true;
    return desktopBackdropCompositor_.IsBackdropWindow(window) ||
        desktopBackdropCompositor_.IsBackdropWindow(root) ||
        IsPersistentDockBackdropWindow(window) ||
        IsPersistentDockBackdropWindow(root) ||
        collectionPopupBackdropCompositor_.IsBackdropWindow(window) ||
        collectionPopupBackdropCompositor_.IsBackdropWindow(root) ||
        quickNavigation_->backdropCompositor.IsBackdropWindow(window) ||
        quickNavigation_->backdropCompositor.IsBackdropWindow(root);
}

bool DesktopApp::TryGetDesktopHoverPointFromCursor(
    POINT& point) const
{
    POINT screenPoint{};
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !snowdesktop::private_pointer::CursorPosition(&screenPoint))
        return false;
    if (!IsDesktopInteractionSurfaceWindow(
            snowdesktop::private_pointer::WindowAtPoint(screenPoint)))
        return false;

    point = screenPoint;
    return ScreenToClient(hwnd_, &point) != FALSE;
}

bool DesktopApp::TryGetNativeDragHoverPointFromCursor(
    POINT& point) const
{
    POINT screenPoint{};
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !snowdesktop::private_pointer::CursorPosition(&screenPoint))
        return false;

    const HWND hit =
        ResolveWindowBelowDragPreviewAt(screenPoint);
    if (!IsDesktopInteractionSurfaceWindow(hit))
        return false;

    point = screenPoint;
    return ScreenToClient(hwnd_, &point) != FALSE;
}

bool DesktopApp::TryGetBaseDesktopHoverPointFromCursor(
    POINT& point) const
{
    POINT screenPoint{};
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !snowdesktop::private_pointer::CursorPosition(&screenPoint))
        return false;
    if (!IsBaseDesktopHoverSurfaceWindow(
            snowdesktop::private_pointer::WindowAtPoint(screenPoint)))
        return false;

    point = screenPoint;
    return ScreenToClient(hwnd_, &point) != FALSE;
}

bool DesktopApp::TryGetNativeDragResumePointFromCursor(
    POINT& point) const
{
    POINT screenPoint{};
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !snowdesktop::private_pointer::CursorPosition(&screenPoint))
        return false;

    HWND hit = snowdesktop::private_pointer::WindowAtPoint(screenPoint);
    HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    if (!root)
        root = hit;
    const auto belongsTo = [hit, root](HWND candidate) {
        return candidate &&
            (hit == candidate || root == candidate ||
                IsChild(candidate, hit));
    };
    if (!IsBaseDesktopHoverSurfaceWindow(hit) &&
        !IsPersistentDockHostWindow(hit) &&
        !IsPersistentDockHostWindow(root) &&
        !belongsTo(floatingPopupHwnd_))
    {
        return false;
    }

    point = screenPoint;
    return ScreenToClient(hwnd_, &point) != FALSE;
}

/**
 * @brief 判断指定点是否位于外部可放置窗口上
 * @param clientPoint 客户端坐标点
 * @return 如果是外部窗口返回 true
 */
bool DesktopApp::IsExternalDropWindowAt(POINT clientPoint) const
{
    // A software pointer has no desktop-wide OLE input stream. Keep its
    // native in-app drag on that existing path; crossing process boundaries
    // requires a separately implemented input adapter.
    if (snowdesktop::private_pointer::Active()) return false;
    POINT screenPoint = clientPoint;
    ClientToScreen(hwnd_, &screenPoint);
    HWND hit = ResolveWindowBelowDragPreviewAt(screenPoint);
    if (!hit) return false;
    HWND root = GetAncestor(hit, GA_ROOT);
    if (!root) root = hit;
    return snowdesktop::ole_drag_rules::
        IsExternalDropSurface(
            true,
            IsDesktopInteractionSurfaceWindow(hit),
            IsWindowVisible(root) != FALSE);
}

/**
 * @brief 根据修饰键状态和允许的效果选择拖放效果
 * @param keyState 键盘修饰键状态
 * @param allowed 允许的拖放效果标志
 * @return 选择的 DROPEFFECT
 */
DWORD DesktopApp::ChooseDropEffect(DWORD keyState, DWORD allowed) const
{
    return snowdesktop::ole_drag_rules::ChooseDropEffect(keyState, allowed);
}

// ── OLE drag-drop ───────────────────────────────────────────

/**
 * @brief 将屏幕坐标转换为客户端坐标
 * @param screen 屏幕坐标点
 * @return 客户端坐标点
 */
POINT DesktopApp::ScreenPointToClient(POINTL screen) const
{
    POINT pt{ screen.x, screen.y };
    if (hwnd_ && IsWindow(hwnd_))
        ScreenToClient(hwnd_, &pt);
    return pt;
}

bool DesktopApp::CommitLuaLogicalSlotDrop(
    const std::wstring& widgetId, const std::string& slotId,
    const std::vector<Item*>& sourceItems, std::size_t targetIndex)
{
    if (!widgetEngine_ || sourceItems.size() != 1 || !sourceItems.front())
        return false;
    const auto surface = widgetEngine_->RuntimeLogicalSlotSurface(
        widgetId, slotId);
    if (!surface) return false;
    const auto accepts = [&surface](std::string_view kind) {
        return std::find(surface->accepts.begin(), surface->accepts.end(),
            kind) != surface->accepts.end();
    };

    Item* sourceItem = sourceItems.front();
    snowdesktop::widget_runtime::LogicalSlotItem candidate;
    candidate.title = WideToUtf8(sourceItem->GetTitle());
    const std::wstring sourcePath = sourceItem->GetPath();

    DesktopItem* desktopItem = nullptr;
    if (auto* desktopIcon = dynamic_cast<DesktopIcon*>(sourceItem))
        desktopItem = desktopIcon->GetDesktopItem();
    else if (auto* dockItem = dynamic_cast<DockEntryItem*>(sourceItem);
        dockItem && dockItem->GetEntryType() == DockEntryType::DesktopItem)
    {
        const std::size_t itemIndex = FindItemIndexByKey(
            dockItem->GetReference());
        if (itemIndex < items_.size()) desktopItem = &items_[itemIndex];
    }
    else if (auto* frequentItem =
            dynamic_cast<DockFrequentItem*>(sourceItem))
    {
        const std::size_t itemIndex = frequentItem->GetItemIndex();
        if (itemIndex < items_.size()) desktopItem = &items_[itemIndex];
    }

    if (desktopItem)
    {
        const std::wstring target = !desktopItem->parsingName.empty()
            ? desktopItem->parsingName
            : (!desktopItem->layoutKey.empty() ? desktopItem->layoutKey
                : desktopItem->desktopIconClsid);
        if (target.empty()) return false;
        if (desktopItem->isApplicationShortcut &&
            accepts("app.reference"))
            candidate.kind = "app.reference";
        else if (accepts("desktop.item"))
            candidate.kind = "desktop.item";
        else if (!sourcePath.empty() && accepts("filesystem.reference"))
            candidate.kind = "filesystem.reference";
        else
            return false;
        candidate.target = WideToUtf8(target);
        candidate.source = "desktop.drop";
        candidate.type = desktopItem->typeName.empty()
            ? (desktopItem->isApplicationShortcut
                ? "application" : "desktop.item")
            : WideToUtf8(desktopItem->typeName);
    }
    else if (dynamic_cast<FolderEntryIcon*>(sourceItem) ||
        dynamic_cast<ExternalFileItem*>(sourceItem) ||
        (!sourcePath.empty() && !dynamic_cast<Widget*>(sourceItem)))
    {
        if (sourcePath.empty() || !accepts("filesystem.reference"))
            return false;
        candidate.kind = "filesystem.reference";
        candidate.target = WideToUtf8(sourcePath);
        candidate.source = dynamic_cast<ExternalFileItem*>(sourceItem)
            ? "explorer.drop" : "widget.drop";
        const DWORD attributes = GetFileAttributesW(sourcePath.c_str());
        candidate.type = attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            ? "folder" : "file";
    }
    else
        return false;

    if (candidate.title.empty()) candidate.title = candidate.target;
    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    if (!widgetEngine_->RuntimeBindHostLogicalSlot(widgetId, slotId,
            std::move(candidate), targetIndex, change, error))
    {
        if (!error.empty())
            widgetEngine_->RuntimeRecordError(widgetId,
                "logical slot drop: " + error);
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

/**
 * @brief COM IDropTarget::DragEnter 实现
 * @param dataObject 拖放数据对象
 * @param keyState 键盘修饰键状态
 * @param point 鼠标屏幕坐标
 * @param effect [in/out] 拖放效果
 * @return S_OK 或错误码
 */
