#include "app.h"
#include "../ole_drag_rules.h"

// OLE surface classification, effect choice and coordinate conversion.

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

    if (belongsTo(floatingDockHwnd_) ||
        belongsTo(floatingPopupHwnd_) ||
        belongsTo(dragPreviewHwnd_) ||
        belongsTo(quickNavigationHwnd_))
        return true;
    if (dockWindowPreview_ &&
        belongsTo(dockWindowPreview_->GetWindow()))
        return true;
    return desktopBackdropCompositor_.IsBackdropWindow(window) ||
        desktopBackdropCompositor_.IsBackdropWindow(root) ||
        floatingDockBackdropCompositor_.IsBackdropWindow(window) ||
        floatingDockBackdropCompositor_.IsBackdropWindow(root) ||
        collectionPopupBackdropCompositor_.IsBackdropWindow(window) ||
        collectionPopupBackdropCompositor_.IsBackdropWindow(root) ||
        quickNavBackdropCompositor_.IsBackdropWindow(window) ||
        quickNavBackdropCompositor_.IsBackdropWindow(root);
}

bool DesktopApp::TryGetDesktopHoverPointFromCursor(
    POINT& point) const
{
    POINT screenPoint{};
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !GetCursorPos(&screenPoint))
        return false;
    if (!IsDesktopInteractionSurfaceWindow(
            WindowFromPoint(screenPoint)))
        return false;

    point = screenPoint;
    return ScreenToClient(hwnd_, &point) != FALSE;
}

bool DesktopApp::TryGetBaseDesktopHoverPointFromCursor(
    POINT& point) const
{
    POINT screenPoint{};
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !GetCursorPos(&screenPoint))
        return false;
    if (!IsBaseDesktopHoverSurfaceWindow(
            WindowFromPoint(screenPoint)))
        return false;

    point = screenPoint;
    return ScreenToClient(hwnd_, &point) != FALSE;
}

bool DesktopApp::TryGetNativeDragResumePointFromCursor(
    POINT& point) const
{
    POINT screenPoint{};
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !GetCursorPos(&screenPoint))
        return false;

    HWND hit = WindowFromPoint(screenPoint);
    HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
    if (!root)
        root = hit;
    const auto belongsTo = [hit, root](HWND candidate) {
        return candidate &&
            (hit == candidate || root == candidate ||
                IsChild(candidate, hit));
    };
    if (!belongsTo(hwnd_) &&
        !belongsTo(floatingDockHwnd_) &&
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
    if ((keyState & MK_ALT)) return DROPEFFECT_LINK;
    if ((keyState & MK_SHIFT)) return DROPEFFECT_MOVE;
    if ((keyState & MK_CONTROL)) return DROPEFFECT_COPY;

    DWORD available = allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if (!available) available = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    if (available & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (available & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return DROPEFFECT_LINK;
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
