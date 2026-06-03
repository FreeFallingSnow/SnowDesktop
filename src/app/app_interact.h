#pragma once
// Inline implementations for DesktopApp — Interaction & Tray.
// This file is included by app.h after the class definition.

#include "drop_model.h"

// ── Interaction ─────────────────────────────────────────────

inline int DesktopApp::HitTestItem(POINT pt) const
{
    // Backward-compat wrapper: returns items_ index for Shell/COM code
    DesktopIcon* icon = HitTestIcon(pt);
    if (!icon) return -1;
    DesktopItem* di = icon->GetDesktopItem();
    for (size_t j = 0; j < items_.size(); ++j)
        if (&items_[j] == di) return static_cast<int>(j);
    return -1;
}

inline DesktopIcon* DesktopApp::HitTestIcon(POINT pt) const
{
    for (int i = static_cast<int>(items_oo_.size()) - 1; i >= 0; --i)
    {
        auto* icon = dynamic_cast<DesktopIcon*>(items_oo_[i].get());
        if (!icon) continue;
        DesktopItem* di = icon->GetDesktopItem();
        if (!di || IsRectEmptyRect(di->bounds)) continue;
        if (!di->layoutKey.empty() && collectedKeysCache_.count(ToUpperInvariant(di->layoutKey))) continue;
        RECT selRect = GetItemSelectionRect(di->bounds, di->selected);
        if (PtInRect(&selRect, pt)) return icon;
    }
    return nullptr;
}

inline bool DesktopApp::IsItemInAnyWidget(const DesktopItem& item) const
{
    std::wstring key = ToUpperInvariant(item.layoutKey);
    if (key.empty()) return false;
    for (const auto& widget : widgets_)
    {
        for (const auto& rawKey : widget.itemKeys)
        {
            if (ToUpperInvariant(rawKey) == key)
                return true;
        }
    }
    return false;
}

inline RECT DesktopApp::GetStandaloneWidgetFrameRect(const DesktopWidget& widget) const
{
    RECT rect = widget.bounds;
    for (const auto& page : gridPages_)
    {
        if (page.id != widget.gridCell.pageId) continue;
        int halfGapX = std::max(2, page.gapX / 2);
        int halfGapY = std::max(2, page.gapY / 2);
        if (widget.gridCell.column > 0) rect.left -= halfGapX;
        if (widget.gridCell.row > 0) rect.top -= halfGapY;
        if (widget.gridCell.column + widget.gridSpan.columns < page.columns) rect.right += halfGapX;
        if (widget.gridCell.row + widget.gridSpan.rows < page.rows) rect.bottom += halfGapY;
        break;
    }
    if (rect.right - rect.left > 16 && rect.bottom - rect.top > 16)
        InflateRect(&rect, -4, -4);
    return rect;
}

inline RECT DesktopApp::GetStandaloneWidgetMoveHandleRect(const DesktopWidget& widget) const
{
    RECT frame = GetStandaloneWidgetFrameRect(widget);
    constexpr int handleHeight = 24;
    return {
        frame.left + 4,
        std::max<LONG>(frame.top, frame.bottom - handleHeight - 2),
        frame.right - 4,
        frame.bottom - 2
    };
}

inline RECT DesktopApp::GetStandaloneWidgetResizeHandleRect(const DesktopWidget& widget) const
{
    RECT handle = GetStandaloneWidgetMoveHandleRect(widget);
    constexpr int handleWidth = 24;
    return {
        std::max<LONG>(handle.left, handle.right - handleWidth),
        handle.top,
        handle.right,
        handle.bottom
    };
}

inline WidgetHit DesktopApp::HitTestStandaloneWidget(size_t widgetIndex, POINT pt) const
{
    if (widgetIndex >= widgets_.size()) return WidgetHit::None;
    const DesktopWidget& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::LuaScript) return WidgetHit::None;

    RECT frame = GetStandaloneWidgetFrameRect(widget);
    if (!PtInRect(&frame, pt)) return WidgetHit::None;
    RECT resize = GetStandaloneWidgetResizeHandleRect(widget);
    if (PtInRect(&resize, pt)) return WidgetHit::ResizeHandle;
    RECT move = GetStandaloneWidgetMoveHandleRect(widget);
    if (PtInRect(&move, pt)) return WidgetHit::MoveHandle;
    return WidgetHit::Content;
}

inline size_t DesktopApp::HitTestStandaloneWidgetIndex(POINT pt) const
{
    for (size_t n = widgets_.size(); n > 0; --n)
    {
        size_t i = n - 1;
        if (HitTestStandaloneWidget(i, pt) != WidgetHit::None)
            return i;
    }
    return static_cast<size_t>(-1);
}

inline std::string LuaWidgetWideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        result.data(), len, nullptr, nullptr);
    return result;
}

inline std::vector<LuaDesktopItemInfo> DesktopApp::BuildLuaDesktopSnapshot(bool selectedOnly) const
{
    std::vector<LuaDesktopItemInfo> result;
    auto appendDesktopItem = [&](const DesktopItem& item, const std::wstring& source) {
        if (selectedOnly && !item.selected) return;
        LuaDesktopItemInfo info;
        info.id = LuaWidgetWideToUtf8(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        info.title = LuaWidgetWideToUtf8(item.name);
        info.path = LuaWidgetWideToUtf8(item.parsingName);
        info.source = LuaWidgetWideToUtf8(source);
        info.type = LuaWidgetWideToUtf8(item.typeName.empty() ? L"desktopItem" : item.typeName);
        info.selected = item.selected;
        result.push_back(std::move(info));
    };

    for (const auto& item : items_)
    {
        if (!IsItemInAnyWidget(item))
            appendDesktopItem(item, L"desktop");
    }

    for (const auto& widget : widgets_)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            for (const auto& entry : widget.folderEntries)
            {
                if (selectedOnly && !entry.selected) continue;
                LuaDesktopItemInfo info;
                info.id = LuaWidgetWideToUtf8(entry.fullPath);
                info.title = LuaWidgetWideToUtf8(entry.name);
                info.path = LuaWidgetWideToUtf8(entry.fullPath);
                info.source = LuaWidgetWideToUtf8(widget.title.empty() ? L"folderMapping" : widget.title);
                info.type = entry.isDirectory ? "folder" : "file";
                info.selected = entry.selected;
                result.push_back(std::move(info));
            }
            continue;
        }

        for (const auto& key : widget.itemKeys)
        {
            size_t idx = FindItemIndexByKey(key);
            if (idx != static_cast<size_t>(-1))
                appendDesktopItem(items_[idx], widget.title.empty() ? L"widget" : widget.title);
        }
    }
    return result;
}

inline bool DesktopApp::LuaOpenPath(const std::wstring& path)
{
    if (path.empty()) return false;
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

inline bool DesktopApp::LuaRevealPath(const std::wstring& path)
{
    if (path.empty()) return false;
    std::wstring params = L"/select,\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

inline void DesktopApp::LuaSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title)
{
    if (title.empty()) return;
    for (auto& widget : widgets_)
    {
        if (widget.id != widgetId) continue;
        if (widget.title == title) return;
        widget.title = title;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

inline void DesktopApp::BeginLuaInlineTextEdit(const LuaInlineTextEditRequest& request)
{
    if (renameEdit_ != nullptr || request.widgetId.empty() || request.storageKey.empty())
        return;
    if (luaInlineEdit_ != nullptr)
        CommitLuaInlineTextEdit(false);

    size_t widgetIndex = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].id == request.widgetId && widgets_[i].type == DesktopWidgetType::LuaScript)
        {
            widgetIndex = i;
            break;
        }
    }
    if (widgetIndex == static_cast<size_t>(-1))
        return;

    RECT frame = GetStandaloneWidgetFrameRect(widgets_[widgetIndex]);
    RECT rect = {
        frame.left + request.localRect.left,
        frame.top + request.localRect.top,
        frame.left + request.localRect.right,
        frame.top + request.localRect.bottom
    };
    rect.left = std::max<LONG>(frame.left + 2, std::min<LONG>(rect.left, frame.right - 4));
    rect.top = std::max<LONG>(frame.top + 2, std::min<LONG>(rect.top, frame.bottom - 4));
    rect.right = std::min<LONG>(std::max<LONG>(rect.right, rect.left + 24), frame.right - 2);
    rect.bottom = std::min<LONG>(std::max<LONG>(rect.bottom, rect.top + 22), frame.bottom - 2);
    if (IsRectEmptyRect(rect))
        return;

    RECT screenRect = rect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    std::wstring initial = Utf8ToWide(request.text);
    DWORD style = WS_POPUP | WS_VISIBLE | ES_LEFT | ES_NOHIDESEL | ES_AUTOVSCROLL;
    if (request.multiline)
        style |= ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL;
    else
        style |= ES_AUTOHSCROLL;

    luaInlineEdit_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", initial.c_str(), style,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!luaInlineEdit_)
        return;

    luaInlineEditWidgetId_ = request.widgetId;
    luaInlineEditStorageKey_ = request.storageKey;
    luaInlineEditMultiline_ = request.multiline;
    luaInlineEditTextColor_ = RGB((request.textColor >> 16) & 0xFF,
        (request.textColor >> 8) & 0xFF, request.textColor & 0xFF);

    if (luaInlineEditFont_) DeleteObject(luaInlineEditFont_);
    luaInlineEditFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(luaInlineEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(luaInlineEditFont_ ? luaInlineEditFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(luaInlineEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    SetWindowSubclass(luaInlineEdit_, &DesktopApp::LuaInlineEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(luaInlineEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    if (request.selectAll)
        SendMessageW(luaInlineEdit_, EM_SETSEL, 0, -1);
    else
        SendMessageW(luaInlineEdit_, EM_SETSEL,
            static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SetFocus(luaInlineEdit_);
}

inline bool DesktopApp::IsPointOverWidgetChrome(POINT pt) const
{
    for (auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc) continue;
        if (wc->HitTestWidget(pt) != WidgetHit::None)
            return true;
    }
    return HitTestStandaloneWidgetIndex(pt) != static_cast<size_t>(-1);
}

inline void DesktopApp::InvalidateDragStaticScene()
{
    if (!dragSession_.IsActive()) return;
    dragSession_.InvalidateStaticScene();
    dragRenderCache_.Reset();
}

inline void DesktopApp::EndDragSession()
{
    dragSession_.End();
    dragRenderCache_.Reset();
}

inline void DesktopApp::ShowSettingsWindow()
{
    if (settingsWindow_)
        settingsWindow_->Show();
    else
        MessageBeep(MB_ICONWARNING);
}

inline void DesktopApp::RefreshDragTargetAt(POINT clientPoint, int mods)
{
    if (!dragSession_.IsActive()) return;

    dragSession_.UpdatePoint(clientPoint);

    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    popupDragTargetSlot_.reset();

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, clientPoint))
        {
            WidgetContainer* popupContainer = nullptr;
            for (auto& c : containers_)
            {
                popupContainer = dynamic_cast<WidgetContainer*>(c.get());
                if (popupContainer && popupContainer->GetWidgetData() == &widgets_[popupWidgetIndex_])
                    break;
                popupContainer = nullptr;
            }

            if (popupContainer)
            {
                std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                RECT content = GetCollectionPopupContentRect(popup);
                size_t slotIndex = 0;
                RECT slotBounds = content;
                HitRegion region = HitRegion::Empty;
                Item* handoffItem = nullptr;

                if (!popupKeys.empty())
                {
                    bool foundItem = false;
                    for (size_t i = 0; i < popupKeys.size(); ++i)
                    {
                        RECT itemRect = GetCollectionPopupItemRect(popup, i);
                        RECT clipped = itemRect;
                        clipped.top = std::max(clipped.top, content.top);
                        clipped.bottom = std::min(clipped.bottom, content.bottom);
                        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, clientPoint))
                            continue;

                        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                        if (itemIndex != static_cast<size_t>(-1) && !items_[itemIndex].selected)
                        {
                            RECT iconRect = GetItemIconRect(itemRect);
                            RECT handoffRect = { iconRect.left - 4, iconRect.top - 2,
                                                 iconRect.right + 4, iconRect.bottom + 4 };
                            if (PtInRect(&handoffRect, clientPoint))
                            {
                                region = HitRegion::Handoff;
                                slotBounds = itemRect;
                                handoffItem = popupContainer->GetMemberItem(i);
                            }
                        }

                        if (region != HitRegion::Handoff)
                        {
                            slotIndex = i;
                            slotBounds = itemRect;
                            region = clientPoint.x < itemRect.left + (itemRect.right - itemRect.left) / 2
                                ? HitRegion::SortBefore
                                : HitRegion::SortAfter;
                        }
                        foundItem = true;
                        break;
                    }

                    if (!foundItem)
                    {
                        slotIndex = popupKeys.size() - 1;
                        slotBounds = GetCollectionPopupItemRect(popup, slotIndex);
                        region = HitRegion::SortAfter;
                    }
                }

                popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
                if (handoffItem)
                    popupDragTargetSlot_->SetItem(handoffItem);
                targetContainer = popupContainer;
                targetSlot = popupDragTargetSlot_.get();
                targetRegion = region;
            }
        }
    }

    if (!targetContainer)
    {
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            Slot* slot = nullptr;
            HitRegion region = (*it)->HitTestDrag(clientPoint, slot);
            if (region != HitRegion::None)
            {
                targetContainer = it->get();
                targetSlot = slot;
                targetRegion = region;
                break;
            }
        }
    }

    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion,
            dragSession_.Items(), dragSession_.Source(), mods);
    ShowDragHintWindow(clientPoint, hint);
}

inline void DesktopApp::RebindDragSourceAfterRebuild()
{
    if (!dragSession_.IsActive() || dragSession_.Items().empty()) return;

    Container* source = nullptr;
    const DragSourceList& oldSourceList = dragSession_.SourceList();
    if (oldSourceList.hasOriginWidget)
    {
        for (auto& c : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(c.get());
            DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
            if (data && data->id == oldSourceList.originWidgetId)
            {
                source = widget;
                break;
            }
        }
    }
    else
    {
        source = GetDesktopGrid();
    }

    if (!source)
    {
        EndDragSession();
        return;
    }

    std::vector<Item*> reboundItems = source->GetSelectedItems();
    if (reboundItems.empty())
    {
        EndDragSession();
        return;
    }
    DragSourceList reboundList = BuildDragSourceList(reboundItems, source);
    dragSession_.RebindSource(source, std::move(reboundItems), std::move(reboundList));
}

inline void DesktopApp::ClearSelection()
{
    for (auto& item : items_)
        item.selected = false;
    for (auto& widget : widgets_)
    {
        widget.selected = false;
        for (auto& entry : widget.folderEntries)
            entry.selected = false;
    }
}

inline void DesktopApp::ClearSelectionOutsideWidget(size_t widgetIndex)
{
    std::unordered_set<std::wstring> allowedKeys;
    if (widgetIndex < widgets_.size())
    {
        for (const auto& key : widgets_[widgetIndex].itemKeys)
            allowedKeys.insert(ToUpperInvariant(key));
    }

    for (auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!allowedKeys.contains(key))
            item.selected = false;
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        widgets_[wi].selected = false;
        if (wi == widgetIndex && widgets_[wi].type == DesktopWidgetType::FolderMapping)
            continue;
        for (auto& entry : widgets_[wi].folderEntries)
            entry.selected = false;
    }
}

inline void DesktopApp::ClearSelectionOutsideDesktop()
{
    for (auto& item : items_)
    {
        if (IsItemInAnyWidget(item))
            item.selected = false;
    }
    for (auto& widget : widgets_)
    {
        widget.selected = false;
        for (auto& entry : widget.folderEntries)
            entry.selected = false;
    }
}

inline void DesktopApp::SelectOnly(int index)
{
    ClearSelection();
    if (index >= 0 && static_cast<size_t>(index) < items_.size())
    {
        // Find the OO icon for this item
        items_[index].selected = true;
    }
}

inline void DesktopApp::ToggleSelection(int index)
{
    if (index >= 0 && static_cast<size_t>(index) < items_.size())
        items_[index].selected = !items_[index].selected;
}

inline void DesktopApp::SelectWidgetOnly(size_t index)
{
    ClearSelection();
    for (auto& w : widgets_)
    {
        w.selected = (&w == &widgets_[index]);
        for (auto& e : w.folderEntries) e.selected = false;
    }
}

inline void DesktopApp::OnLeftButtonDown(WPARAM wp, LPARAM lp)
{
    if (renameEdit_ != nullptr) return;
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    SetFocus(hwnd_);
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    mouseDown_ = true;
    mouseDownPoint_ = pt;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    marqueeRect_ = MakeRect(pt.x, pt.y, pt.x, pt.y);

    if (HandlePageNavClick(pt)) return;

    bool ctrl = (wp & MK_CONTROL) != 0;

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (!PtInRect(&popup, pt))
        {
            CloseCollectionPopup();
            mouseDown_ = false;
            return;
        }

        std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
        RECT content = GetCollectionPopupContentRect(popup);
        bool clickedPopupItem = false;
        for (size_t i = 0; i < popupKeys.size(); ++i)
        {
            RECT itemRect = GetCollectionPopupItemRect(popup, i);
            RECT clipped = itemRect;
            clipped.top = std::max(clipped.top, content.top);
            clipped.bottom = std::min(clipped.bottom, content.bottom);
            if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;

            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
            if (itemIndex != static_cast<size_t>(-1))
            {
                if (ctrl)
                {
                    ClearSelectionOutsideWidget(popupWidgetIndex_);
                    ToggleSelection(static_cast<int>(itemIndex));
                }
                else if (!items_[itemIndex].selected)
                {
                    SelectOnly(static_cast<int>(itemIndex));
                }
                else
                {
                    ClearSelectionOutsideWidget(popupWidgetIndex_);
                }
                WidgetContainer* wc = nullptr;
                for (auto& c : containers_)
                {
                    wc = dynamic_cast<WidgetContainer*>(c.get());
                    if (wc && wc->GetWidgetData() == &widgets_[popupWidgetIndex_]) break;
                    wc = nullptr;
                }
                popupMouseDownItem_ = std::make_unique<DesktopIcon>(&items_[itemIndex], wc, this);
                popupMouseDownItem_->SetBounds(itemRect);
                mouseDownHit_ = popupMouseDownItem_.get();
                clickedPopupItem = true;
            }
            break;
        }
        if (!clickedPopupItem && !ctrl)
            ClearSelection();

        if (!clickedPopupItem)
            mouseDownHit_ = nullptr;
        marqueeWidgetIndex_ = popupWidgetIndex_;
        mouseDownWidgetIndex_ = popupWidgetIndex_;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // ── Widget hit-test ─────────────────────────────────────
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    widgetAction_ = WidgetAction::None;
    for (size_t n = widgets_.size(); n > 0; --n)
    {
        size_t wi = n - 1;
        WidgetHit wh = HitTestStandaloneWidget(wi, pt);
        if (wh == WidgetHit::None) continue;

        if (wh == WidgetHit::ResizeHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingResize;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (wh == WidgetHit::MoveHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingMove;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            RECT bounds = widgets_[wi].bounds;
            dragGroupOriginX_ = bounds.left;
            dragGroupOriginY_ = bounds.top;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        SelectWidgetOnly(wi);
        mouseDownWidgetIndex_ = wi;
        mouseDownHit_ = nullptr;
        SetCapture(hwnd_);
        if (widgetEngine_ && widgets_[wi].type == DesktopWidgetType::LuaScript)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[wi]);
            widgetEngine_->EnsureWidgetLoaded(widgets_[wi].id, widgets_[wi].scriptPath);
            widgetEngine_->InvokeMouseEvent(widgets_[wi].id, "onMouseDown",
                pt.x - frame.left, pt.y - frame.top, 1, 0);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        WidgetContainer* wc = nullptr;
        for (auto& c : containers_)
        {
            wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[wi]) break;
            wc = nullptr;
        }
        if (!wc) continue;

        WidgetHit wh = wc->HitTestWidget(pt);
        if (wh == WidgetHit::None) continue;

        if (wh == WidgetHit::ResizeHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingResize;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::MoveHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingMove;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            RECT bounds = widgets_[wi].bounds;
            dragGroupOriginX_ = bounds.left;
            dragGroupOriginY_ = bounds.top;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::Content)
        {
            Item* memberItem = nullptr;
            RECT bodyRect = wc->GetBodyRect();
            auto& slots = wc->GetSlots();
            for (auto& slot : slots)
            {
                RECT bounds = slot->GetBounds();
                if (PtInRect(&bounds, pt) && PtInRect(&bodyRect, pt))
                {
                    memberItem = slot->GetItem();
                    break;
                }
            }

            if (memberItem)
            {
                if (ctrl)
                {
                    ClearSelectionOutsideWidget(wi);
                    if (memberItem->IsSelected())
                        pendingCtrlToggleWidgetItem_ = memberItem;
                    else
                        memberItem->SetSelected(true);
                }
                else if (!memberItem->IsSelected())
                {
                    ClearSelection();
                    memberItem->SetSelected(true);
                }
                else
                {
                    ClearSelectionOutsideWidget(wi);
                }
                mouseDownWidgetIndex_ = wi;
                mouseDownHit_ = memberItem;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }

            // Empty content selects the widget itself.
            ClearSelection();
            widgets_[wi].selected = true;
            mouseDownWidgetIndex_ = wi;
            marqueeWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::CollectionOpenBtn)
        {
            SelectWidgetOnly(wi);
            OpenCollectionPopupAt(wi, pt);
            mouseDown_ = false;
            mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            mouseDownHit_ = nullptr;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::ListToggleBtn)
        {
            if (widgets_[wi].type == DesktopWidgetType::FolderMapping ||
                widgets_[wi].type == DesktopWidgetType::FileCategories)
            {
                widgets_[wi].listMode = !widgets_[wi].listMode;
                wc->InvalidateSlots();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::OpenFolderBtn)
        {
            // FolderMapping: open source folder
            if (widgets_[wi].type == DesktopWidgetType::FolderMapping
                && !widgets_[wi].sourceFolderPath.empty())
            {
                ShellExecuteW(hwnd_, L"open", widgets_[wi].sourceFolderPath.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            return;
        }
        else if (wh == WidgetHit::CategoryTab)
        {
            if (widgets_[wi].type == DesktopWidgetType::FileCategories)
            {
                auto* fc = dynamic_cast<FileCategories*>(wc);
                std::wstring id = fc ? fc->CategoryIdAtPoint(pt) : L"";
                if (!id.empty())
                {
                    widgets_[wi].activeCategoryId = id;
                    widgets_[wi].scrollOffset = 0;
                    wc->InvalidateSlots();
                    SaveLayoutSlots();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            return;
        }
    }

    // ── Desktop icon hit-test ───────────────────────────────
    DesktopIcon* hit = HitTestIcon(pt);
    mouseDownHit_ = hit;

    // Clear widget selection when clicking desktop area
    if (!hit && mouseDownWidgetIndex_ == static_cast<size_t>(-1) && !ctrl)
    {
        for (auto& w : widgets_) w.selected = false;
    }

    if (hit)
    {
        DesktopItem* di = hit->GetDesktopItem();
        if (ctrl)
        {
            ClearSelectionOutsideDesktop();
            size_t hitIndex = (di && !di->layoutKey.empty())
                ? FindItemIndexByKey(di->layoutKey)
                : static_cast<size_t>(-1);
            if (hit->IsSelected())
                pendingCtrlToggleDesktopIndex_ = hitIndex;
            else
                hit->SetSelected(true);
        }
        else if (!di->selected)
        {
            ClearSelection();
            hit->SetSelected(true);
        }
        else
        {
            ClearSelectionOutsideDesktop();
        }
    }
    else if (!ctrl)
        ClearSelection();

    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

inline void DesktopApp::OnMouseMove(WPARAM wp, LPARAM lp)
{
    (void)wp;
    POINT current{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT oldMouse = lastMousePoint_;
    lastMousePoint_ = current;

    if (!dragSession_.IsActive() && widgetAction_ == WidgetAction::None &&
        mouseDownWidgetIndex_ < widgets_.size() &&
        widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::LuaScript &&
        widgetEngine_)
    {
        WidgetHit hit = HitTestStandaloneWidget(mouseDownWidgetIndex_, current);
        if (hit == WidgetHit::Content)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]);
            widgetEngine_->EnsureWidgetLoaded(widgets_[mouseDownWidgetIndex_].id,
                widgets_[mouseDownWidgetIndex_].scriptPath);
            widgetEngine_->InvokeMouseEvent(widgets_[mouseDownWidgetIndex_].id, "onMouseMove",
                current.x - frame.left, current.y - frame.top,
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0, 0);
        }
    }

    if (mouseDown_ && mouseDownHit_ && mouseDownHit_->IsSelected() && !dragSession_.IsActive())
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            Container* source = mouseDownHit_->GetContainer();
            std::vector<Item*> sourceItems = source ? source->GetSelectedItems() : std::vector<Item*>{};
            DragSourceList sourceList = BuildDragSourceList(sourceItems, source);
            if (sourceItems.empty())
            {
                return;
            }
            dragSession_.Begin(source, std::move(sourceItems), std::move(sourceList),
                mouseDownPoint_, current);
            pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
            pendingCtrlToggleWidgetItem_ = nullptr;
            marqueeActive_ = false;
            marqueeWidgetIndex_ = static_cast<size_t>(-1);
            if (source == GetDesktopGrid())
            {
                UpdateDragGroupOrigin();
            }
            else
            {
                RECT groupBounds{};
                bool hasBounds = false;
                for (auto* item : dragSession_.Items())
                {
                    if (!item) continue;
                    RECT bounds = item->GetBounds();
                    if (IsRectEmptyRect(bounds)) continue;
                    groupBounds = hasBounds ? UnionCopy(groupBounds, bounds) : bounds;
                    hasBounds = true;
                }
                if (hasBounds)
                {
                    dragGroupOriginX_ = groupBounds.left;
                    dragGroupOriginY_ = groupBounds.top;
                }
            }
        }
    }

    UpdateCollectionPopupDwell(current);

    if (mouseDown_ && !dragSession_.IsActive()
        && (widgetAction_ == WidgetAction::PendingMove || widgetAction_ == WidgetAction::PendingResize)
        && mouseDownWidgetIndex_ < widgets_.size()
        && (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3))
    {
        if (widgetAction_ == WidgetAction::PendingMove)
            widgetAction_ = WidgetAction::Move;
        else if (widgetAction_ == WidgetAction::PendingResize)
            widgetAction_ = WidgetAction::Resize;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Widget resize preview
    if (widgetAction_ == WidgetAction::Resize && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        const auto& widget = widgets_[mouseDownWidgetIndex_];
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (page)
        {
            int stepX = std::max(1, page->cellWidth + page->gapX);
            int stepY = std::max(1, page->cellHeight + page->gapY);
            int dCol = static_cast<int>(std::round(static_cast<double>(current.x - mouseDownPoint_.x) / static_cast<double>(stepX)));
            int dRow = static_cast<int>(std::round(static_cast<double>(current.y - mouseDownPoint_.y) / static_cast<double>(stepY)));

            GridCell cell = widgetDragOriginalCell_;
            GridSpan span = widgetDragOriginalSpan_;
            span.columns += dCol;
            span.rows += dRow;
            span.columns = std::clamp(span.columns, 1, page->columns - cell.column);
            span.rows = std::clamp(span.rows, 1, page->rows - cell.row);

            widgetPreviewCell_ = cell;
            widgetPreviewSpan_ = span;
        }
        ShowDragHintWindow(current, L"释放：调整组件大小");
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    // Widget drag preview
    if (widgetAction_ == WidgetAction::Move && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline int SlotFromCell(const std::vector<GridPage>&, const GridCell&);
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        POINT adjusted = {
            dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
            dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
        };
        GridCell cell = CellFromPoint(adjusted);
        if (!cell.pageId.empty())
        {
            const GridPage* page = FindGridPage(gridPages_, cell.pageId);
            if (page)
            {
                cell.column = std::clamp(cell.column, 0, page->columns - widgetDragOriginalSpan_.columns);
                cell.row    = std::clamp(cell.row,    0, page->rows    - widgetDragOriginalSpan_.rows);
            }
            widgetPreviewCell_ = cell;
        }
        ShowDragHintWindow(current, L"释放：移动组件");
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    if (dragSession_.IsActive() && !dragSession_.Items().empty())
    {
        dragSession_.UpdatePoint(current);
        int currentMods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) currentMods |= MK_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    currentMods |= MK_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   currentMods |= MK_SHIFT;
        dragSession_.UpdateActionFromMods(currentMods);

        POINT screenPt = current;
        ClientToScreen(hwnd_, &screenPt);
        bool overExternal = IsExternalDropWindowAt(current);

        if (overExternal)
        {
            DropPayload payload = DropPayload::From(dragSession_.Items());
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj)
            {
                auto* sourceWidget = dynamic_cast<WidgetContainer*>(dragSession_.Source());
                DesktopWidget* sourceWidgetData = sourceWidget ? sourceWidget->GetWidgetData() : nullptr;

                HideDragHintWindow();
                ReleaseCapture();
                mouseDown_ = false;
                mouseDownHit_ = nullptr;

                selfDragActive_ = true;
                selfDragReturned_ = false;
                selfDragOutKeys_.clear();
                for (const auto& item : items_)
                    if (item.selected) selfDragOutKeys_.push_back(item.layoutKey);

                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);

                DWORD oleEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                HRESULT hr = DoDragDrop(dataObj.Get(), static_cast<IDropSource*>(this), oleEffect, &oleEffect);
                selfDragActive_ = false;

                if (hr == DRAGDROP_S_DROP && oleEffect == DROPEFFECT_MOVE
                    && !selfDragReturned_ && payload.hasDesktopIcons)
                {
                    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
                    {
                        if (it->selected && !it->desktopIconClsid.empty()) continue;
                        if (!it->selected) continue;
                        wchar_t path[MAX_PATH]{};
                        if (SHGetPathFromIDList(it->absolutePidl.get(), path))
                        {
                            SHFILEOPSTRUCTW op{};
                            op.wFunc = FO_DELETE;
                            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                            wchar_t from[MAX_PATH + 2]{};
                            wcscpy_s(from, path);
                            from[wcslen(path) + 1] = L'\0';
                            op.pFrom = from;
                            SHFileOperationW(&op);
                        }
                    }
                    SaveLayoutSlots();
                }

                if (!selfDragReturned_ && sourceWidgetData
                    && sourceWidgetData->type == DesktopWidgetType::FolderMapping)
                {
                    for (size_t i = 0; i < widgets_.size(); ++i)
                    {
                        if (&widgets_[i] == sourceWidgetData)
                        {
                            RefreshFolderMappingWidget(i);
                            break;
                        }
                    }
                }

                if (!selfDragReturned_)
                {
                    ClearSelection();
                    EndDragSession();
                    ReloadItems();
                }
                else
                {
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                selfDragOutKeys_.clear();
                return;
            }
        }

        {
            RECT prevRect, nextRect;
            GetNavButtonRects(prevRect, nextRect);
            int navSide = 0;
            const bool hasPrev = pageOffset_ > 0;
            const bool hasNext = pageOffset_ < MaxPageOffset();
            if (hasPrev && PtInRect(&prevRect, current)) navSide = -1;
            else if (hasNext && PtInRect(&nextRect, current)) navSide = 1;
            navHoverSide_ = navSide;

            if (navSide != 0)
            {
                DWORD now = GetTickCount();
                if (navAutoFlipDir_ != navSide)
                {
                    navAutoFlipDir_ = navSide;
                    navAutoFlipTick_ = now;
                }
                else if (now - navAutoFlipTick_ > 500)
                {
                    int newOffset = NextNonEmptyOffset(pageOffset_, navSide);
                    if (newOffset != pageOffset_)
                    {
                        pageOffset_ = newOffset;
                        ApplyPageMapping();
                        MigrateSelectedItemsToLastMonitorPage();
                        LayoutItems();
                        InvalidateDragStaticScene();
                        UpdateDragGroupOrigin();
                        SaveLayoutSlots();
                        navAutoFlipTick_ = now;
                    }
                }
            }
            else
            {
                navAutoFlipDir_ = 0;
                navAutoFlipTick_ = 0;
            }
        }

        // OO hit testing: iterate all containers in reverse (topmost first)
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        popupDragTargetSlot_.reset();

        if (popupWidgetIndex_ < widgets_.size())
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (PtInRect(&popup, current))
            {
                WidgetContainer* popupContainer = nullptr;
                for (auto& c : containers_)
                {
                    popupContainer = dynamic_cast<WidgetContainer*>(c.get());
                    if (popupContainer && popupContainer->GetWidgetData() == &widgets_[popupWidgetIndex_])
                        break;
                    popupContainer = nullptr;
                }

                if (popupContainer)
                {
                    std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                    RECT content = GetCollectionPopupContentRect(popup);
                    size_t slotIndex = 0;
                    RECT slotBounds = content;
                    HitRegion region = HitRegion::Empty;
                    Item* handoffItem = nullptr;

                    if (!popupKeys.empty())
                    {
                        bool foundItem = false;
                        for (size_t i = 0; i < popupKeys.size(); ++i)
                        {
                            RECT itemRect = GetCollectionPopupItemRect(popup, i);
                            RECT clipped = itemRect;
                            clipped.top = std::max(clipped.top, content.top);
                            clipped.bottom = std::min(clipped.bottom, content.bottom);
                            if (clipped.bottom <= clipped.top || !PtInRect(&clipped, current))
                                continue;

                            // Check Handoff: mouse on icon area of unselected item
                            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                            if (itemIndex != static_cast<size_t>(-1) && !items_[itemIndex].selected)
                            {
                                RECT iconRect = GetItemIconRect(itemRect);
                                RECT hf = { iconRect.left - 4, iconRect.top - 2,
                                            iconRect.right + 4, iconRect.bottom + 4 };
                                if (PtInRect(&hf, current))
                                {
                                    region = HitRegion::Handoff;
                                    slotBounds = itemRect;
                                    handoffItem = popupContainer->GetMemberItem(i);
                                }
                            }

                            if (region != HitRegion::Handoff)
                            {
                                slotIndex = i;
                                slotBounds = itemRect;
                                region = current.x < itemRect.left + (itemRect.right - itemRect.left) / 2
                                    ? HitRegion::SortBefore
                                    : HitRegion::SortAfter;
                            }
                            foundItem = true;
                            break;
                        }

                        if (!foundItem)
                        {
                            slotIndex = popupKeys.size() - 1;
                            slotBounds = GetCollectionPopupItemRect(popup, slotIndex);
                            region = HitRegion::SortAfter;
                        }
                    }

                    popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
                    if (handoffItem)
                        popupDragTargetSlot_->SetItem(handoffItem);
                    targetContainer = popupContainer;
                    targetSlot = popupDragTargetSlot_.get();
                    targetRegion = region;
                }
            }
        }

        if (!targetContainer)
        {
            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                Slot* slot = nullptr;
                HitRegion region = (*it)->HitTestDrag(current, slot);
                if (region != HitRegion::None)
                {
                    targetContainer = it->get();
                    targetSlot = slot;
                    targetRegion = region;
                    break;
                }
            }
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        std::wstring hint;
        if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), currentMods);

        ShowDragHintWindow(current, hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        return;
    }

    if (mouseDown_ && !mouseDownHit_)
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            marqueeActive_ = true;
            marqueeRect_ = NormalizeRect(mouseDownPoint_, current);

            if (marqueeWidgetIndex_ < widgets_.size())
            {
                if (popupWidgetIndex_ == marqueeWidgetIndex_)
                {
                    // Marquee inside collection popup
                    RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
                    RECT content = GetCollectionPopupContentRect(popup);
                    std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                    for (size_t i = 0; i < popupKeys.size(); ++i)
                    {
                        RECT itemRect = GetCollectionPopupItemRect(popup, i);
                        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;
                        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                        if (itemIndex == static_cast<size_t>(-1)) continue;
                        items_[itemIndex].selected = RectsIntersect(itemRect, marqueeRect_);
                    }
                }
                else
                {
                    // Marquee inside a widget — select member items
                    WidgetContainer* wc = nullptr;
                    for (auto& c : containers_)
                    {
                        wc = dynamic_cast<WidgetContainer*>(c.get());
                        if (wc && wc->GetWidgetData() == &widgets_[marqueeWidgetIndex_]) break;
                        wc = nullptr;
                    }
                    if (wc)
                    {
                        auto& slots = wc->GetSlots();
                        for (auto& slot : slots)
                        {
                            if (!slot) continue;
                            RECT bounds = slot->GetBounds();
                            bool intersect = RectsIntersect(bounds, marqueeRect_);
                            if (auto* item = slot->GetItem())
                                item->SetSelected(intersect);
                        }
                    }
                }
            }
            else
            {
                for (auto& oo : items_oo_)
                {
                    auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
                    if (!icon) continue;
                    DesktopItem* di = icon->GetDesktopItem();
                    if (!di || IsItemInAnyWidget(*di) || IsRectEmptyRect(di->bounds)) continue;
                    RECT itemSelRect = GetItemSelectionRect(di->bounds, false);
                    di->selected = RectsIntersect(itemSelRect, marqueeRect_);
                }
            }
        }
    }

    {
        int oldHover = navHoverSide_;
        navHoverSide_ = 0;
        if (MaxPageOffset() > 0 || pageOffset_ > 0)
        {
            RECT prevRect, nextRect;
            GetNavButtonRects(prevRect, nextRect);
            if (pageOffset_ > 0 && PtInRect(&prevRect, current)) navHoverSide_ = -1;
            else if (pageOffset_ < MaxPageOffset() && PtInRect(&nextRect, current)) navHoverSide_ = 1;
        }
        if (navHoverSide_ != oldHover)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (oldMouse.x != current.x || oldMouse.y != current.y)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

inline void DesktopApp::OnLeftButtonUp(WPARAM wp, LPARAM lp)
{
    (void)wp;
    POINT upPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    HideDragHintWindow();

    // ── Widget action completion ────────────────────────────
    if (widgetAction_ != WidgetAction::None && mouseDownWidgetIndex_ < widgets_.size())
    {
        if (widgetAction_ == WidgetAction::Move)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, true);
        else if (widgetAction_ == WidgetAction::Resize)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, false);
        // PendingMove/PendingResize: just cancel without displacement
        widgetAction_ = WidgetAction::None;
        InvalidateDragStaticScene();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragSession_.IsActive())
    {
        if (mouseDownWidgetIndex_ < widgets_.size() &&
            widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::LuaScript &&
            HitTestStandaloneWidget(mouseDownWidgetIndex_, upPoint) == WidgetHit::Content &&
            widgetEngine_)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]);
            widgetEngine_->EnsureWidgetLoaded(widgets_[mouseDownWidgetIndex_].id,
                widgets_[mouseDownWidgetIndex_].scriptPath);
            widgetEngine_->InvokeMouseEvent(widgets_[mouseDownWidgetIndex_].id, "onMouseUp",
                upPoint.x - frame.left, upPoint.y - frame.top, 1, 0);
            widgetEngine_->InvokeClick(widgets_[mouseDownWidgetIndex_].id,
                upPoint.x - frame.left, upPoint.y - frame.top);
        }
        if (pendingCtrlToggleDesktopIndex_ < items_.size())
            items_[pendingCtrlToggleDesktopIndex_].selected = false;
        pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
        if (pendingCtrlToggleWidgetItem_)
        {
            pendingCtrlToggleWidgetItem_->SetSelected(!pendingCtrlToggleWidgetItem_->IsSelected());
            pendingCtrlToggleWidgetItem_ = nullptr;
        }
        mouseDown_ = false;
        marqueeActive_ = false;
        marqueeWidgetIndex_ = static_cast<size_t>(-1);
        navHoverSide_ = 0;
        navAutoFlipDir_ = 0;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragSession_.TargetContainer() || dragSession_.TargetRegion() == HitRegion::None)
    {
        goto cleanup;
    }

    {
        int mods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;

        if (dragSession_.TargetRegion() == HitRegion::Handoff && dragSession_.TargetSlot()
            && dragSession_.TargetSlot()->GetItem())
        {
            Item* targetItem = dragSession_.TargetSlot()->GetItem();
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj)
            {
                ComPtr<IDropTarget> dropTarget;
                if (auto* targetIcon = dynamic_cast<DesktopIcon*>(targetItem))
                {
                    DesktopItem* desktopItem = targetIcon->GetDesktopItem();
                    if (desktopItem && desktopItem->childPidl.get())
                    {
                        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(desktopItem->childPidl.get());
                        desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                            reinterpret_cast<void**>(dropTarget.GetAddressOf()));
                    }
                }
                else if (!targetItem->GetPath().empty())
                {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                        nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                    {
                        shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dropTarget));
                    }
                }

                if (dropTarget)
                {
                    POINT screen = dragSession_.CurrentPoint();
                    ClientToScreen(hwnd_, &screen);
                    POINTL spl{ screen.x, screen.y };
                    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                    DWORD keyState = MK_LBUTTON;
                    if (mods & MK_CONTROL) keyState |= MK_CONTROL;
                    if (mods & MK_ALT)     keyState |= MK_ALT;
                    if (mods & MK_SHIFT)   keyState |= MK_SHIFT;
                    if (SUCCEEDED(dropTarget->DragEnter(dataObj.Get(), keyState, spl, &effect)))
                    {
                        dropTarget->DragOver(keyState, spl, &effect);
                        dropTarget->Drop(dataObj.Get(), keyState, spl, &effect);
                    }
                }
            }
            SaveLayoutSlots();
            ClearSelection();
            EndDragSession();
            ReloadItems();
            goto cleanup;
        }

        Container* targetContainer = dragSession_.TargetContainer();
        bool needsReload = targetContainer->NeedsShellReloadAfterDrop();
        targetContainer->OnItemsDropped(dragSession_.Items(), dragSession_.Source(),
            dragSession_.TargetSlot(), dragSession_.TargetRegion(), mods);

        SaveLayoutSlots();
        ClearSelection();
        EndDragSession();
        RebuildContainersAndItems();
        if (needsReload)
            ReloadItems();
        else
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

cleanup:
    EndDragSession();
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    popupDwellWidgetIndex_ = static_cast<size_t>(-1);
    KillTimer(hwnd_, kCollectionPopupDwellTimerId);
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    ReleaseCapture();
}

inline std::vector<std::wstring> DesktopApp::GetSelectedFolderEntryPaths(size_t* firstWidgetIndex) const
{
    if (firstWidgetIndex)
        *firstWidgetIndex = static_cast<size_t>(-1);

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;

        std::vector<std::wstring> paths;
        for (const auto& entry : widget.folderEntries)
            if (entry.selected && !entry.fullPath.empty())
                paths.push_back(entry.fullPath);

        if (!paths.empty())
        {
            if (firstWidgetIndex)
                *firstWidgetIndex = i;
            return paths;
        }
    }

    return {};
}

inline size_t DesktopApp::FindFolderMappingShortcutTarget() const
{
    size_t selectedEntryWidget = static_cast<size_t>(-1);
    (void)GetSelectedFolderEntryPaths(&selectedEntryWidget);
    if (selectedEntryWidget < widgets_.size())
        return selectedEntryWidget;

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::FolderMapping || widget.sourceFolderPath.empty())
            continue;
        if (PtInRect(&widget.bounds, lastMousePoint_))
            return i;
    }

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type == DesktopWidgetType::FolderMapping &&
            widget.selected && !widget.sourceFolderPath.empty())
            return i;
    }

    return static_cast<size_t>(-1);
}

inline bool DesktopApp::CopyCutSelectedFolderEntries(bool cut)
{
    std::vector<std::wstring> paths = GetSelectedFolderEntryPaths();
    if (paths.empty()) return false;

    ComPtr<IDataObject> dataObj = CreateFileDropDataObject(paths);
    if (!dataObj) return false;

    cutPaths_.clear();
    if (cut)
    {
        CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        FORMATETC fmt{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med{};
        med.tymed = TYMED_HGLOBAL;
        med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (med.hGlobal)
        {
            *static_cast<DWORD*>(GlobalLock(med.hGlobal)) = DROPEFFECT_MOVE;
            GlobalUnlock(med.hGlobal);
            dataObj->SetData(&fmt, &med, TRUE);
        }

        for (const auto& path : paths)
            cutPaths_.insert(path);
    }

    OleSetClipboard(dataObj.Get());
    OleFlushClipboard();
    UpdateCutState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

inline bool DesktopApp::DeleteSelectedFolderEntries()
{
    std::vector<std::wstring> paths = GetSelectedFolderEntryPaths();
    if (paths.empty()) return false;

    std::wstring from;
    for (const auto& path : paths)
    {
        cutPaths_.erase(path);
        from += path;
        from.push_back(L'\0');
    }
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd_;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted)
        return true;

    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].type == DesktopWidgetType::FolderMapping)
            RefreshFolderMappingWidget(i);
    ReloadItems(false);
    return true;
}

inline bool DesktopApp::PasteClipboardToFolderMapping(size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return false;
    DesktopWidget& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::FolderMapping || widget.sourceFolderPath.empty())
        return false;

    ComPtr<IDataObject> clipObj;
    if (FAILED(OleGetClipboard(&clipObj)) || !clipObj)
        return false;

    DropAction action = DropAction::Copy;
    CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medPref{};
    if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) && medPref.hGlobal)
    {
        DWORD* pEffect = static_cast<DWORD*>(GlobalLock(medPref.hGlobal));
        if (pEffect)
        {
            if (*pEffect & DROPEFFECT_MOVE)
                action = DropAction::Move;
            else if (*pEffect & DROPEFFECT_LINK)
                action = DropAction::Link;
            GlobalUnlock(medPref.hGlobal);
        }
        ReleaseStgMedium(&medPref);
    }

    std::vector<std::wstring> paths;
    FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medDrop{};
    if (SUCCEEDED(clipObj->GetData(&fmtDrop, &medDrop)) && medDrop.hGlobal)
    {
        HDROP hDrop = static_cast<HDROP>(medDrop.hGlobal);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                paths.push_back(path);
        }
        ReleaseStgMedium(&medDrop);
    }
    if (paths.empty()) return false;

    DragSourceList sourceList;
    sourceList.hasExternalFiles = true;
    sourceList.entries.reserve(paths.size());
    for (const auto& path : paths)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = sourceList.entries.size();
        entry.filePath = path;
        entry.displayName = FileNameFromPath(path);
        sourceList.entries.push_back(std::move(entry));
    }

    if (!MaterializeFilesToFolder(sourceList, widget.sourceFolderPath, action))
        return true;

    if (action == DropAction::Move)
    {
        cutPaths_.clear();
        if (OpenClipboard(hwnd_))
        {
            EmptyClipboard();
            CloseClipboard();
        }
    }

    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].type == DesktopWidgetType::FolderMapping)
            RefreshFolderMappingWidget(i);
    ReloadItems(false);
    return true;
}

inline void DesktopApp::OnKeyDown(WPARAM key)
{
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT)
    {
        RefreshDragHintFromKeyboard();
        return;
    }

    if (renameEdit_ != nullptr) return;

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    switch (key)
    {
    case VK_F2:
    case 'R':
        if (key == 'R' && !ctrl) break;
        if (key == VK_F2 || ctrl)
            BeginRenameSelected();
        break;
    case VK_F5:
        ReloadItems();
        break;
    case VK_DELETE:
    {
        if (DeleteSelectedFolderEntries())
            break;

        cutPaths_.clear();
        for (const auto& item : items_)
        {
            if (!item.selected || !item.desktopIconClsid.empty()) continue;
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
            {
                SHFILEOPSTRUCTW op{};
                op.wFunc = FO_DELETE;
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                wchar_t from[MAX_PATH + 2]{};
                wcscpy_s(from, path);
                from[wcslen(path) + 1] = L'\0';
                op.pFrom = from;
                SHFileOperationW(&op);
            }
        }
        ReloadItems();
        break;
    }
    case 'C':
        if (!ctrl) break;
        if (CopyCutSelectedFolderEntries(false))
            break;
        InvokeSelectedShellVerb("copy");
        break;
    case 'X':
        if (!ctrl) break;
    {
        if (CopyCutSelectedFolderEntries(true))
            break;

        cutPaths_.clear();

        std::vector<PCUITEMID_CHILD> pidls;
        std::vector<size_t> selectedIndexes;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i].selected || !items_[i].desktopIconClsid.empty()) continue;
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(items_[i].childPidl.get()));
            selectedIndexes.push_back(i);
        }

        if (!pidls.empty())
        {
            ComPtr<IDataObject> dataObj;
            if (SUCCEEDED(desktopFolder_->GetUIObjectOf(hwnd_, static_cast<UINT>(pidls.size()),
                pidls.data(), IID_IDataObject, nullptr,
                reinterpret_cast<void**>(dataObj.GetAddressOf()))) && dataObj)
            {
                CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
                FORMATETC fmt{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                STGMEDIUM med{};
                med.tymed = TYMED_HGLOBAL;
                med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
                if (med.hGlobal)
                {
                    *static_cast<DWORD*>(GlobalLock(med.hGlobal)) = DROPEFFECT_MOVE;
                    GlobalUnlock(med.hGlobal);
                    dataObj->SetData(&fmt, &med, TRUE);
                }

                OleSetClipboard(dataObj.Get());
                OleFlushClipboard();
            }
        }

        for (size_t idx : selectedIndexes)
        {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(items_[idx].absolutePidl.get(), path))
                cutPaths_.insert(path);
        }

        UpdateCutState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    break;
    case 'V':
        if (!ctrl) break;
    {
        if (PasteClipboardToFolderMapping(FindFolderMappingShortcutTarget()))
            break;

        bool fromDesktop = false;
        std::unordered_set<std::wstring> clipPaths;

        ComPtr<IDataObject> clipObj;
        if (SUCCEEDED(OleGetClipboard(&clipObj)) && clipObj)
        {
            CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
            FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM medPref{};
            if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) && medPref.hGlobal)
            {
                DWORD* pEffect = static_cast<DWORD*>(GlobalLock(medPref.hGlobal));
                bool isMove = pEffect && (*pEffect & DROPEFFECT_MOVE);
                if (pEffect) GlobalUnlock(medPref.hGlobal);
                if (isMove)
                {
                    FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                    STGMEDIUM medDrop{};
                    if (SUCCEEDED(clipObj->GetData(&fmtDrop, &medDrop)) && medDrop.hGlobal)
                    {
                        HDROP hDrop = static_cast<HDROP>(medDrop.hGlobal);
                        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                        for (UINT i = 0; i < count; ++i)
                        {
                            wchar_t path[MAX_PATH]{};
                            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                                clipPaths.insert(path);
                        }
                        ReleaseStgMedium(&medDrop);
                    }
                }
                ReleaseStgMedium(&medPref);
            }
        }

        if (!clipPaths.empty())
        {
            for (const auto& item : items_)
            {
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(item.absolutePidl.get(), path) && clipPaths.contains(path))
                {
                    fromDesktop = true;
                    break;
                }
            }
        }

        if (fromDesktop)
        {
            cutPaths_.clear();
            if (OpenClipboard(hwnd_))
            {
                EmptyClipboard();
                CloseClipboard();
            }
            UpdateCutState();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else
        {
            ComPtr<IContextMenu> bgMenu;
            if (SUCCEEDED(desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu,
                reinterpret_cast<void**>(bgMenu.GetAddressOf()))) && bgMenu)
            {
                CMINVOKECOMMANDINFO info{};
                info.cbSize = sizeof(info);
                info.hwnd = hwnd_;
                info.lpVerb = "paste";
                info.nShow = SW_SHOWNORMAL;
                bgMenu->InvokeCommand(&info);
                cutPaths_.clear();
                ReloadItems();
            }
        }
    }
    break;
    case 'A':
        if (!ctrl) break;
    {
        ClearSelection();
        for (auto& oo : items_oo_)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
            if (!icon) continue;
            DesktopItem* di = icon->GetDesktopItem();
            if (!di || di->name.empty()) continue;
            di->selected = true;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    break;
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
        MoveKeyboardSelection(key);
        break;
    default:
        break;
    }
}

inline void DesktopApp::RefreshDragHintFromKeyboard()
{
    if (!dragSession_.IsActive() && !externalDragActive_ && !selfDragActive_) return;

    int mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(mods, externalDragActive_ ? DropAction::Copy : DropAction::Move);

    std::wstring hint;
    if (dragSession_.TargetContainer() && dragSession_.TargetRegion() != HitRegion::None)
    {
        hint = dragSession_.TargetContainer()->GetDragHint(dragSession_.TargetSlot(),
            dragSession_.TargetRegion(), dragSession_.Items(), dragSession_.Source(), mods);
    }

    if (externalDragActive_ || selfDragActive_)
    {
        POINT screenPoint = dragSession_.CurrentPoint();
        ClientToScreen(hwnd_, &screenPoint);
        ShowDragHintWindowScreen(screenPoint, hint);
        OnPaint();
    }
    else
    {
        ShowDragHintWindow(dragSession_.CurrentPoint(), hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

inline void DesktopApp::InvokeSelectedShellVerb(const char* verb)
{
    std::vector<PCUITEMID_CHILD> pidls;
    for (const auto& item : items_)
    {
        if (item.selected && item.childPidl.value)
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.value));
    }
    if (pidls.empty()) return;

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IContextMenu, nullptr,
        reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(hr) || !contextMenu) return;

    CMINVOKECOMMANDINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = hwnd_;
    info.lpVerb = verb;
    info.nShow = SW_SHOWNORMAL;
    if (SUCCEEDED(contextMenu->InvokeCommand(&info)))
        ReloadItems();
}

inline void DesktopApp::MoveKeyboardSelection(WPARAM arrowKey)
{
    if (items_.empty()) return;

    std::vector<size_t> visible;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        if (items_[i].selected || !IsRectEmptyRect(items_[i].bounds))
            visible.push_back(i);
    }
    if (visible.empty()) return;

    int current = -1;
    for (size_t i = 0; i < visible.size(); ++i)
    {
        if (items_[visible[i]].selected) { current = static_cast<int>(i); break; }
    }
    int delta = 0;
    switch (arrowKey)
    {
    case VK_LEFT:  delta = -1; break;
    case VK_RIGHT: delta =  1; break;
    case VK_UP:    delta = -1; break;
    case VK_DOWN:  delta =  1; break;
    }
    if (delta == 0) return;

    int next = current < 0 ? 0 : current + delta;
    next = std::clamp(next, 0, static_cast<int>(visible.size()) - 1);
    SelectOnly(static_cast<int>(visible[static_cast<size_t>(next)]));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

inline bool DesktopApp::HandlePageNavClick(POINT point)
{
    if (gridPages_.empty()) return false;
    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();
    if (!hasPrev && !hasNext) return false;

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    int delta = 0;
    if (hasPrev && PtInRect(&prevRect, point)) delta = -1;
    else if (hasNext && PtInRect(&nextRect, point)) delta = 1;
    if (delta == 0) return false;

    int newOffset = NextNonEmptyOffset(pageOffset_, delta);
    if (newOffset == pageOffset_) return false;

    bool wasDragging = dragSession_.IsActive();
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (wasDragging) MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();
    if (wasDragging) InvalidateDragStaticScene();
    if (wasDragging) UpdateDragGroupOrigin();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline void DesktopApp::OnRightButtonUp(LPARAM lp)
{
    if (renameEdit_ != nullptr) return;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT screenPt = pt;
    ClientToScreen(hwnd_, &screenPt);

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt))
        {
            std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
            RECT content = GetCollectionPopupContentRect(popup);
            for (size_t i = 0; i < popupKeys.size(); ++i)
            {
                RECT itemRect = GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top = std::max(clipped.top, content.top);
                clipped.bottom = std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;

                size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                if (itemIndex != static_cast<size_t>(-1))
                {
                    if (!items_[itemIndex].selected)
                        SelectOnly(static_cast<int>(itemIndex));
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (IsProtectedDesktopIcon(items_[itemIndex]))
                        ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
                    else
                        ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
                    return;
                }
            }
        }
    }

    // Check widget member items first; otherwise the widget frame menu steals member right-clicks.
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(it->get());
        if (!wc) continue;

        WidgetHit wh = wc->HitTestWidget(pt);
        if (wh == WidgetHit::MoveHandle || wh == WidgetHit::ResizeHandle)
            continue;

        RECT bodyRect = wc->GetBodyRect();

        auto& slots = wc->GetSlots();
        for (auto& slot : slots)
        {
            if (!slot) continue;
            RECT bounds = slot->GetBounds();
            if (!PtInRect(&bounds, pt)) continue;
            if (!PtInRect(&bodyRect, pt)) continue;

            auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem());
            DesktopItem* item = icon ? icon->GetDesktopItem() : nullptr;
            if (!item)
            {
                auto* folderIcon = dynamic_cast<FolderEntryIcon*>(slot->GetItem());
                FolderEntry* entry = folderIcon ? folderIcon->GetFolderEntry() : nullptr;
                if (!entry) break;

                auto* folderWidget = dynamic_cast<WidgetContainer*>(wc);
                DesktopWidget* data = folderWidget ? folderWidget->GetWidgetData() : nullptr;
                size_t widgetIndex = static_cast<size_t>(-1);
                size_t memberIndex = static_cast<size_t>(-1);
                for (size_t wi = 0; wi < widgets_.size(); ++wi)
                {
                    if (&widgets_[wi] != data) continue;
                    widgetIndex = wi;
                    for (size_t mi = 0; mi < widgets_[wi].folderEntries.size(); ++mi)
                    {
                        if (&widgets_[wi].folderEntries[mi] == entry)
                        {
                            memberIndex = mi;
                            break;
                        }
                    }
                    break;
                }
                if (widgetIndex == static_cast<size_t>(-1) ||
                    memberIndex == static_cast<size_t>(-1))
                    break;

                if (!entry->selected)
                {
                    ClearSelection();
                    widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                ShowFolderEntryContextMenu(screenPt, widgetIndex, memberIndex);
                return;
            }

            size_t itemIndex = FindItemIndexByKey(item->layoutKey);
            if (itemIndex == static_cast<size_t>(-1)) break;

            if (!items_[itemIndex].selected)
                SelectOnly(static_cast<int>(itemIndex));
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (IsProtectedDesktopIcon(items_[itemIndex]))
                ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
            else
                ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
            return;
        }
    }

    // Check widget hit after member items.
    size_t hitWidget = static_cast<size_t>(-1);
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgets_[wi]) continue;
            if (wc->HitTestWidget(pt) != WidgetHit::None)
            {
                hitWidget = wi;
                break;
            }
        }
        if (hitWidget != static_cast<size_t>(-1)) break;
    }

    if (hitWidget != static_cast<size_t>(-1))
    {
        // Select the widget and show its context menu
        SelectWidgetOnly(hitWidget);
        InvalidateRect(hwnd_, nullptr, FALSE);

        ShowWidgetContextMenu(screenPt, hitWidget);
        return;
    }

    size_t hitStandaloneWidget = HitTestStandaloneWidgetIndex(pt);
    if (hitStandaloneWidget != static_cast<size_t>(-1))
    {
        SelectWidgetOnly(hitStandaloneWidget);
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowWidgetContextMenu(screenPt, hitStandaloneWidget);
        return;
    }

    if (IsPointOverWidgetChrome(pt))
    {
        ClearSelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowBackgroundContextMenu(screenPt);
        return;
    }

    int hit = HitTestItem(pt);
    if (hit >= 0 && !items_[hit].selected)
        SelectOnly(hit);
    else if (hit < 0)
        ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (hit >= 0)
    {
        if (IsProtectedDesktopIcon(items_[hit]))
            ShowShellContextMenu(screenPt, hit);
        else
            ShowItemContextMenu(screenPt, hit);
    }
    else
        ShowBackgroundContextMenu(screenPt);
}

inline void DesktopApp::OnTimer(WPARAM timerId)
{
    if (timerId == kShellChangeTimerId)
    {
        KillTimer(hwnd_, kShellChangeTimerId);
        if (!mouseDown_ && !reloading_)
            ReloadItems();
    }
    else if (timerId == kRecycleBinPollTimerId)
    {
        SHQUERYRBINFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)))
        {
            if (lastRecycleBinItemCount_ >= 0 && info.i64NumItems != lastRecycleBinItemCount_)
            {
                if (!mouseDown_ && !reloading_)
                    ReloadItems();
            }
            lastRecycleBinItemCount_ = info.i64NumItems;
        }
    }
    else if (timerId == kDesktopHostWatchTimerId)
    {
        WatchDesktopHost();
    }
    else if (timerId == kWidgetRefreshTimerId)
    {
        bool hasLuaWidget = false;
        for (const auto& widget : widgets_)
        {
            if (widget.type == DesktopWidgetType::LuaScript)
            {
                hasLuaWidget = true;
                break;
            }
        }
        if (hasLuaWidget)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (timerId == kCollectionPopupDwellTimerId)
    {
        if (!dragSession_.IsActive() || popupWidgetIndex_ < widgets_.size() ||
            popupDwellWidgetIndex_ >= widgets_.size())
        {
            popupDwellWidgetIndex_ = static_cast<size_t>(-1);
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            return;
        }

        if (TryOpenDwellCollectionPopup(GetTickCount()))
        {
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            OnMouseMove(0, MAKELPARAM(lastMousePoint_.x, lastMousePoint_.y));
        }
    }
}

inline void DesktopApp::UpdateCollectionPopupDwell(POINT point)
{
    if (!dragSession_.IsActive() || popupWidgetIndex_ < widgets_.size())
    {
        popupDwellWidgetIndex_ = static_cast<size_t>(-1);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        return;
    }

    size_t hoveredCollection = static_cast<size_t>(-1);
    for (auto& c : containers_)
    {
        auto* collection = dynamic_cast<Collection*>(c.get());
        if (!collection) continue;

        RECT buttonRect = collection->GetAllButtonRect();
        if (IsRectEmptyRect(buttonRect) || !PtInRect(&buttonRect, point))
            continue;

        DesktopWidget* data = collection->GetWidgetData();
        for (size_t wi = 0; wi < widgets_.size(); ++wi)
        {
            if (&widgets_[wi] == data)
            {
                hoveredCollection = wi;
                break;
            }
        }
        break;
    }

    if (hoveredCollection == static_cast<size_t>(-1))
    {
        popupDwellWidgetIndex_ = static_cast<size_t>(-1);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        return;
    }

    DWORD now = GetTickCount();
    if (popupDwellWidgetIndex_ != hoveredCollection)
    {
        popupDwellWidgetIndex_ = hoveredCollection;
        popupDwellTick_ = now;
        SetTimer(hwnd_, kCollectionPopupDwellTimerId, kCollectionPopupDwellIntervalMs, nullptr);
        return;
    }

    TryOpenDwellCollectionPopup(now);
}

inline bool DesktopApp::TryOpenDwellCollectionPopup(DWORD now)
{
    if (popupDwellWidgetIndex_ >= widgets_.size())
        return false;
    if (now - popupDwellTick_ < kCollectionPopupDwellDelayMs)
        return false;

    size_t widgetIndex = popupDwellWidgetIndex_;
    OpenCollectionPopupAt(widgetIndex, lastMousePoint_);
    UpdateWindow(hwnd_);
    return true;
}

// ── Collection popup ─────────────────────────────────────────

inline void DesktopApp::OpenCollectionPopupAt(size_t widgetIndex, POINT anchorPoint,
    const std::wstring& categoryId)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        return;

    popupWidgetIndex_ = widgetIndex;
    popupScrollOffset_ = 0;
    popupHasAnchor_ = anchorPoint.x != LONG_MIN || anchorPoint.y != LONG_MIN;
    popupAnchorPoint_ = anchorPoint;
    popupCategoryId_ = categoryId;
    popupPageId_ = widgets_[widgetIndex].gridCell.pageId;
    popupRect_ = GetCollectionPopupRect(widgets_[widgetIndex]);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widgets_[widgetIndex], popupRect_));
    popupDwellWidgetIndex_ = static_cast<size_t>(-1);
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::CloseCollectionPopup()
{
    if (popupWidgetIndex_ == static_cast<size_t>(-1)) return;
    popupWidgetIndex_ = static_cast<size_t>(-1);
    popupScrollOffset_ = 0;
    popupHasAnchor_ = false;
    popupAnchorPoint_ = {};
    popupPageId_.clear();
    popupCategoryId_.clear();
    popupRect_ = {};
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::OnMouseWheel(WPARAM wp, LPARAM lp)
{
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd_, &pt);
    int currentMods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) currentMods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    currentMods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   currentMods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(currentMods, externalDragActive_ ? DropAction::Copy : DropAction::Move);

    size_t luaWidget = HitTestStandaloneWidgetIndex(pt);
    if (luaWidget != static_cast<size_t>(-1) &&
        widgets_[luaWidget].type == DesktopWidgetType::LuaScript &&
        HitTestStandaloneWidget(luaWidget, pt) == WidgetHit::Content &&
        widgetEngine_)
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        RECT frame = GetStandaloneWidgetFrameRect(widgets_[luaWidget]);
        widgetEngine_->EnsureWidgetLoaded(widgets_[luaWidget].id, widgets_[luaWidget].scriptPath);
        widgetEngine_->InvokeMouseEvent(widgets_[luaWidget].id, "onWheel",
            pt.x - frame.left, pt.y - frame.top, 0, delta);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    auto refreshDragAfterScroll = [&]()
    {
        if (!dragSession_.IsActive()) return;
        RefreshDragTargetAt(pt, currentMods);
        InvalidateDragStaticScene();
    };

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int maxScroll = GetCollectionPopupMaxScrollOffset(widgets_[popupWidgetIndex_], popup);
            popupScrollOffset_ = std::clamp(popupScrollOffset_ - delta / 2, 0, maxScroll);
            refreshDragAfterScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // Scroll widgets with overflow content
    for (auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc || !wc->GetWidgetData()) continue;
        RECT frame = wc->GetFrameRect();
        if (!PtInRect(&frame, pt)) continue;

        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        DesktopWidget* data = wc->GetWidgetData();

        // FileCategories: horizontal scroll for tabs area
        if (data->type == DesktopWidgetType::FileCategories)
        {
            auto* fc = dynamic_cast<FileCategories*>(wc);
            if (fc && fc->TryScrollTabs(pt, delta))
            {
                refreshDragAfterScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }

        int maxScroll = wc->GetMaxScrollOffset();
        if (maxScroll <= 0) continue;

        data->scrollOffset = std::clamp(data->scrollOffset - delta / 2, 0, maxScroll);
        wc->InvalidateSlots();
        refreshDragAfterScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

// ── Rename ──────────────────────────────────────────────────

inline RECT DesktopApp::GetVisibleCollectionItemBounds(size_t itemIndex) const
{
    if (itemIndex >= items_.size()) return {};
    std::wstring key = ToUpperInvariant(items_[itemIndex].layoutKey);

    if (popupWidgetIndex_ < widgets_.size())
    {
        const DesktopWidget& widget = widgets_[popupWidgetIndex_];
        std::vector<std::wstring> keys = GetPopupItemKeys(widget);
        RECT popup = GetCollectionPopupRect(widget);
        RECT content = GetCollectionPopupContentRect(popup);
        for (size_t i = 0; i < keys.size(); ++i)
        {
            if (ToUpperInvariant(keys[i]) != key) continue;
            RECT rect = GetCollectionPopupItemRect(popup, i);
            if (RectsIntersect(rect, content)) return rect;
        }
    }

    for (const auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc) continue;
        for (const auto& slot : wc->GetSlots())
        {
            auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem());
            if (icon && icon->GetDesktopItem() == &items_[itemIndex])
                return slot->GetBounds();
        }
    }
    return {};
}

inline bool DesktopApp::FindSingleSelectedFolderEntry(size_t& widgetIndex, size_t& memberIndex) const
{
    size_t foundWidget = static_cast<size_t>(-1);
    size_t foundMember = static_cast<size_t>(-1);
    int count = 0;
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const auto& widget = widgets_[wi];
        if (widget.type != DesktopWidgetType::FolderMapping) continue;
        for (size_t mi = 0; mi < widget.folderEntries.size(); ++mi)
        {
            if (!widget.folderEntries[mi].selected) continue;
            foundWidget = wi;
            foundMember = mi;
            ++count;
        }
    }
    if (count != 1) return false;
    widgetIndex = foundWidget;
    memberIndex = foundMember;
    return true;
}

inline RECT DesktopApp::GetFolderEntryRenameRect(size_t widgetIndex, size_t memberIndex) const
{
    if (widgetIndex >= widgets_.size() ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return {};

    for (const auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc || wc->GetWidgetData() != &widgets_[widgetIndex]) continue;
        const auto& slots = wc->GetSlots();
        if (memberIndex >= slots.size()) break;
        RECT itemRect = slots[memberIndex]->GetBounds();
        if (widgets_[widgetIndex].listMode)
        {
            const int itemH = std::max<int>(1, static_cast<int>(itemRect.bottom - itemRect.top));
            const int iconSz = std::min(32, itemH - 4);
            return MakeRect(itemRect.left + 4 + iconSz + 6, itemRect.top + 5,
                itemRect.right - 6, itemRect.bottom - 5);
        }
        return GetItemTextRect(itemRect, true);
    }
    return {};
}

inline void DesktopApp::BeginRenameFolderEntry(size_t widgetIndex, size_t memberIndex)
{
    if (renameEdit_ != nullptr ||
        widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return;

    ClearSelection();
    widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
    renamingFolderEntry_ = true;
    renameFolderWidgetIndex_ = widgetIndex;
    renameFolderEntryIndex_ = memberIndex;

    RECT rect = GetFolderEntryRenameRect(widgetIndex, memberIndex);
    if (IsRectEmptyRect(rect))
    {
        renamingFolderEntry_ = false;
        renameFolderWidgetIndex_ = static_cast<size_t>(-1);
        renameFolderEntryIndex_ = static_cast<size_t>(-1);
        return;
    }
    InflateRect(&rect, 2, 2);
    RECT screenRect = rect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    DWORD style = WS_POPUP | WS_VISIBLE | ES_AUTOVSCROLL;
    style |= widgets_[widgetIndex].listMode ? ES_LEFT : (ES_MULTILINE | ES_CENTER | ES_WANTRETURN);
    renameEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", widgets_[widgetIndex].folderEntries[memberIndex].name.c_str(), style,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);

    if (!renameEdit_)
    {
        renamingFolderEntry_ = false;
        renameFolderWidgetIndex_ = static_cast<size_t>(-1);
        renameFolderEntryIndex_ = static_cast<size_t>(-1);
        return;
    }

    if (renameFont_) DeleteObject(renameFont_);
    renameFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    SetWindowSubclass(renameEdit_, &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(renameEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
    SetFocus(renameEdit_);
}

inline bool DesktopApp::IsShellRenameCommand(IContextMenu* contextMenu, UINT commandOffset) const
{
    if (!contextMenu) return false;

    wchar_t verbW[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(commandOffset, GCS_VERBW, nullptr,
        reinterpret_cast<LPSTR>(verbW), static_cast<UINT>(_countof(verbW)))) &&
        lstrcmpiW(verbW, L"rename") == 0)
        return true;

    char verbA[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(commandOffset, GCS_VERBA, nullptr,
        verbA, static_cast<UINT>(_countof(verbA)))) &&
        lstrcmpiA(verbA, "rename") == 0)
        return true;

    return false;
}

inline void DesktopApp::ShowFolderEntryContextMenu(POINT screenPoint, size_t widgetIndex, size_t memberIndex)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return;

    const std::wstring fullPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(fullPath.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child)) || !parentFolder)
    {
        ILFree(pidl);
        return;
    }

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = parentFolder->GetUIObjectOf(hwnd_, 1, &child, IID_IContextMenu,
        nullptr, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    parentFolder->Release();
    if (FAILED(hr) || !contextMenu)
    {
        ILFree(pidl);
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        ILFree(pidl);
        return;
    }

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    hr = contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd, CMF_NORMAL | CMF_CANRENAME);
    if (FAILED(hr))
    {
        DestroyMenu(menu);
        ILFree(pidl);
        RestoreDesktopWindowLayer();
        return;
    }

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command >= kFirstCmd && command <= kLastCmd)
    {
        UINT commandOffset = command - kFirstCmd;
        wchar_t menuText[128]{};
        bool renameCommand = IsShellRenameCommand(contextMenu.Get(), commandOffset);
        if (!renameCommand &&
            GetMenuStringW(menu, command, menuText, static_cast<int>(_countof(menuText)), MF_BYCOMMAND) > 0)
        {
            renameCommand = StrStrIW(menuText, L"重命名") != nullptr ||
                StrStrIW(menuText, L"Rename") != nullptr;
        }

        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);

        if (renameCommand)
        {
            BeginRenameFolderEntry(widgetIndex, memberIndex);
            return;
        }

        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
        invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        RefreshFolderMappingWidget(widgetIndex);
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    ILFree(pidl);
}

inline void DesktopApp::CommitFolderEntryRename(const std::wstring& newName, bool cancel)
{
    size_t widgetIndex = renameFolderWidgetIndex_;
    size_t memberIndex = renameFolderEntryIndex_;
    renamingFolderEntry_ = false;
    renameFolderWidgetIndex_ = static_cast<size_t>(-1);
    renameFolderEntryIndex_ = static_cast<size_t>(-1);

    if (cancel ||
        widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size() ||
        newName.empty() ||
        newName == widgets_[widgetIndex].folderEntries[memberIndex].name)
        return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    const std::wstring oldPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
    if (FAILED(SHParseDisplayName(oldPath.c_str(), nullptr, &pidl, 0, nullptr)))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child);
    if (SUCCEEDED(hr) && parentFolder)
    {
        PITEMID_CHILD newChild = nullptr;
        hr = parentFolder->SetNameOf(hwnd_, child, newName.c_str(), SHGDN_NORMAL, &newChild);
        if (newChild) ILFree(newChild);
        parentFolder->Release();
    }
    ILFree(pidl);

    if (FAILED(hr))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    RefreshFolderMappingWidget(widgetIndex);
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::BeginRenameSelected()
{
    if (renameEdit_ != nullptr) return;

    int selectedWidgetCount = 0;
    size_t selectedWidgetIndex = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].selected)
        {
            ++selectedWidgetCount;
            selectedWidgetIndex = i;
        }
    }
    if (selectedWidgetCount == 1 && selectedWidgetIndex < widgets_.size())
    {
        renamingWidget_ = true;
        renameIndex_ = selectedWidgetIndex;

        RECT frame = widgets_[selectedWidgetIndex].bounds;
        RECT handle = frame;
        for (const auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[selectedWidgetIndex])
            {
                frame = wc->GetFrameRect();
                handle = wc->GetMoveHandleRect();
                break;
            }
        }
        const int editHeight = std::max(40, static_cast<int>(handle.bottom - handle.top) * 2);
        RECT rect = MakeRect(frame.left + 4, handle.top, frame.right - 4, handle.top + editHeight);
        InflateRect(&rect, 2, 2);
        RECT screenRect = rect;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

        renameEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"EDIT",
            widgets_[selectedWidgetIndex].title.c_str(),
            WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
            screenRect.left, screenRect.top,
            screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
            hwnd_, nullptr, instance_, nullptr);
        if (!renameEdit_)
        {
            renamingWidget_ = false;
            renameIndex_ = static_cast<size_t>(-1);
            return;
        }

        if (renameFont_) DeleteObject(renameFont_);
        renameFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(renameEdit_, WM_SETFONT,
            reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        SetWindowSubclass(renameEdit_, &DesktopApp::RenameEditSubclassProc, 1,
            reinterpret_cast<DWORD_PTR>(this));
        SetWindowPos(renameEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
            screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
        SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        SetFocus(renameEdit_);
        return;
    }

    size_t folderWidget = static_cast<size_t>(-1);
    size_t folderMember = static_cast<size_t>(-1);
    if (FindSingleSelectedFolderEntry(folderWidget, folderMember))
    {
        BeginRenameFolderEntry(folderWidget, folderMember);
        return;
    }

    int selectedCount = 0;
    int selectedIndex = -1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
    {
        if (items_[i].selected)
        {
            ++selectedCount;
            selectedIndex = i;
        }
    }
    if (selectedCount != 1 || selectedIndex < 0) return;
    if (!items_[selectedIndex].desktopIconClsid.empty()) return;

    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(items_[selectedIndex].absolutePidl.get(), path)) return;

    renameIndex_ = static_cast<size_t>(selectedIndex);
    RECT itemBounds = GetVisibleCollectionItemBounds(renameIndex_);
    if (IsRectEmptyRect(itemBounds))
        itemBounds = items_[selectedIndex].bounds;
    if (IsRectEmptyRect(itemBounds)) return;
    RECT textRect = GetItemTextRect(itemBounds, true);
    InflateRect(&textRect, 2, 2);
    RECT screenRect = textRect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT",
        items_[selectedIndex].name.c_str(),
        WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);

    if (!renameEdit_)
    {
        renameIndex_ = static_cast<size_t>(-1);
        return;
    }

    if (renameFont_) DeleteObject(renameFont_);
    renameFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    SetWindowSubclass(renameEdit_, &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(renameEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
    SetFocus(renameEdit_);
}

inline void DesktopApp::CommitRename(bool cancel)
{
    if (renameEdit_ == nullptr) return;

    HWND edit = renameEdit_;
    renameEdit_ = nullptr;
    RemoveWindowSubclass(edit, &DesktopApp::RenameEditSubclassProc, 1);

    std::wstring newName;
    if (!cancel)
    {
        int length = GetWindowTextLengthW(edit);
        if (length > 0)
        {
            std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
            GetWindowTextW(edit, buffer.data(), length + 1);
            newName.assign(buffer.data());
        }
    }

    DestroyWindow(edit);
    if (renameFont_) { DeleteObject(renameFont_); renameFont_ = nullptr; }

    if (renamingFolderEntry_)
    {
        CommitFolderEntryRename(newName, cancel);
        return;
    }

    if (renamingWidget_)
    {
        if (!cancel && renameIndex_ < widgets_.size() && !newName.empty())
        {
            widgets_[renameIndex_].title = newName;
            SaveLayoutSlots();
        }
        renamingWidget_ = false;
        renameIndex_ = static_cast<size_t>(-1);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    bool keepLayoutSlots = false;
    if (!cancel && renameIndex_ < items_.size() && !newName.empty() && newName != items_[renameIndex_].name)
    {
        std::wstring oldLayoutKey = items_[renameIndex_].layoutKey;
        PITEMID_CHILD newChild = nullptr;
        HRESULT hr = desktopFolder_->SetNameOf(hwnd_,
            reinterpret_cast<PCUITEMID_CHILD>(items_[renameIndex_].childPidl.get()),
            newName.c_str(), SHGDN_NORMAL, &newChild);
        if (SUCCEEDED(hr))
        {
            if (newChild)
            {
                PIDLIST_ABSOLUTE newAbsolute = ILCombine(desktopPidl_.get(), newChild);
                std::wstring newParsingName = StrRetToString(desktopFolder_.Get(), newChild, SHGDN_FORPARSING);
                if (newAbsolute)
                {
                    std::wstring newLayoutKey = GetStableLayoutKey(newAbsolute, newParsingName);
                    LayoutRecord record;
                    record.cell = items_[renameIndex_].gridCell;
                    record.span = items_[renameIndex_].gridSpan;
                    record.hasGrid = true;
                    record.legacySlot = items_[renameIndex_].slot;
                    layoutRecords_[newLayoutKey] = record;
                    for (auto& widget : widgets_)
                    {
                        for (auto& key : widget.itemKeys)
                        {
                            if (ToUpperInvariant(key) == ToUpperInvariant(oldLayoutKey))
                                key = newLayoutKey;
                        }
                    }
                    keepLayoutSlots = true;
                    ILFree(newAbsolute);
                }
            }
            ILFree(newChild);
        }
        else
        {
            MessageBeep(MB_ICONWARNING);
        }
    }

    renameIndex_ = static_cast<size_t>(-1);
    ReloadItems(!keepLayoutSlots);
}

inline LRESULT CALLBACK DesktopApp::RenameEditSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message)
    {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) { app->CommitRename(false); return 0; }
        if (wParam == VK_ESCAPE) { app->CommitRename(true); return 0; }
        break;
    case WM_KILLFOCUS:
        app->CommitRename(false);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

inline void DesktopApp::CommitLuaInlineTextEdit(bool cancel)
{
    if (luaInlineEdit_ == nullptr) return;

    HWND edit = luaInlineEdit_;
    luaInlineEdit_ = nullptr;
    RemoveWindowSubclass(edit, &DesktopApp::LuaInlineEditSubclassProc, 1);

    std::wstring value;
    if (!cancel)
    {
        int length = GetWindowTextLengthW(edit);
        std::vector<wchar_t> buffer(static_cast<size_t>(std::max(0, length)) + 1);
        GetWindowTextW(edit, buffer.data(), length + 1);
        value.assign(buffer.data());
    }

    DestroyWindow(edit);
    if (luaInlineEditFont_) { DeleteObject(luaInlineEditFont_); luaInlineEditFont_ = nullptr; }

    if (!cancel && widgetEngine_ && !luaInlineEditWidgetId_.empty() && !luaInlineEditStorageKey_.empty())
    {
        widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
            LuaWidgetWideToUtf8(value));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    luaInlineEditWidgetId_.clear();
    luaInlineEditStorageKey_.clear();
    luaInlineEditMultiline_ = false;
    luaInlineEditTextColor_ = RGB(0, 0, 0);
}

inline LRESULT CALLBACK DesktopApp::LuaInlineEditSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { app->CommitLuaInlineTextEdit(true); return 0; }
        if (wParam == VK_RETURN)
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!app->luaInlineEditMultiline_ || ctrl)
            {
                app->CommitLuaInlineTextEdit(false);
                return 0;
            }
        }
        break;
    case WM_KILLFOCUS:
        app->CommitLuaInlineTextEdit(false);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

inline bool DesktopApp::IsSameWindowTree(HWND parent, HWND window)
{
    return parent != nullptr && window != nullptr && (window == parent || IsChild(parent, window));
}

inline bool DesktopApp::IsKnownDesktopSurfaceWindow(HWND window) const
{
    if (!window) return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root) root = window;

    if (IsSameWindowTree(hwnd_, window) || window == hwnd_ || root == hwnd_) return true;
    if (luaInlineEdit_ && (IsSameWindowTree(luaInlineEdit_, window) || root == luaInlineEdit_)) return true;
    if (hintHwnd_ && (IsSameWindowTree(hintHwnd_, window) || root == hintHwnd_)) return true;
    if (controlHwnd_ && (IsSameWindowTree(controlHwnd_, window) || root == controlHwnd_)) return true;

    auto isSurface = [&](HWND candidate) {
        return candidate && (window == candidate || root == candidate || IsChild(candidate, window));
    };
    if (isSurface(desktopWindows_.host) || isSurface(desktopWindows_.progman) ||
        isSurface(desktopWindows_.defView) || isSurface(desktopWindows_.listView))
        return true;

    HWND desktop = GetDesktopWindow();
    return window == desktop || root == desktop;
}

inline bool DesktopApp::IsExternalDropWindowAt(POINT clientPoint) const
{
    POINT screenPoint = clientPoint;
    ClientToScreen(hwnd_, &screenPoint);
    HWND hit = WindowFromPoint(screenPoint);
    if (!hit || IsKnownDesktopSurfaceWindow(hit)) return false;
    HWND root = GetAncestor(hit, GA_ROOT);
    if (!root) root = hit;
    return IsWindowVisible(root) != FALSE;
}

inline DWORD DesktopApp::ChooseDropEffect(DWORD keyState, DWORD allowed) const
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

inline POINT DesktopApp::ScreenPointToClient(POINTL screen) const
{
    POINT pt{ screen.x, screen.y };
    if (hwnd_ && IsWindow(hwnd_))
        ScreenToClient(hwnd_, &pt);
    return pt;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::QueryInterface(REFIID riid, void** object)
{
    if (!object) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget)
    {
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IDropSource)
    {
        *object = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

inline ULONG STDMETHODCALLTYPE DesktopApp::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
}

inline ULONG STDMETHODCALLTYPE DesktopApp::Release()
{
    return static_cast<ULONG>(InterlockedDecrement(&refCount_));
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::DragEnter(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;

    if (selfDragActive_)
    {
        selfDragReturned_ = true;
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
        }

        // OO hit-test
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            Slot* slot = nullptr;
            HitRegion region = (*it)->HitTestDrag(client, slot);
            if (region != HitRegion::None)
            {
                targetContainer = it->get();
                targetSlot = slot;
                targetRegion = region;
                break;
            }
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        OnPaint();
        return S_OK;
    }

    externalDragActive_ = true;
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
        dragSession_.Begin(nullptr, {}, {}, client, client);
    else
        dragSession_.UpdatePoint(client);

    // OO hit-test for external drop
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        Slot* slot = nullptr;
        HitRegion region = (*it)->HitTestDrag(client, slot);
        if (region != HitRegion::None)
        {
            targetContainer = it->get();
            targetSlot = slot;
            targetRegion = region;
            break;
        }
    }
    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    externalDropHasShortcut_ = false;
    if (dataObject)
    {
        std::vector<std::wstring> paths = GetDropPaths(dataObject);
        externalDropFileCount_ = static_cast<int>(paths.size());
        externalDropHasShortcut_ = std::any_of(paths.begin(), paths.end(),
            [](const std::wstring& path) {
                return _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
            });
    }
    else
    {
        externalDropFileCount_ = 1;
    }

    int mods = 0;
    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
    if (keyState & MK_ALT)     mods |= MK_ALT;
    if (keyState & MK_SHIFT)   mods |= MK_SHIFT;
    dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ChooseDropEffect(keyState, *effect);
    OnPaint();
    return S_OK;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::DragOver(
    DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;

    if (selfDragActive_)
    {
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
        }

        // OO hit-test
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            Slot* slot = nullptr;
            HitRegion region = (*it)->HitTestDrag(client, slot);
            if (region != HitRegion::None)
            {
                targetContainer = it->get();
                targetSlot = slot;
                targetRegion = region;
                break;
            }
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        OnPaint();
        return S_OK;
    }

    externalDragActive_ = true;
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
        dragSession_.Begin(nullptr, {}, {}, client, client);
    else
        dragSession_.UpdatePoint(client);

    // OO hit-test for external drop
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        Slot* slot = nullptr;
        HitRegion region = (*it)->HitTestDrag(client, slot);
        if (region != HitRegion::None)
        {
            targetContainer = it->get();
            targetSlot = slot;
            targetRegion = region;
            break;
        }
    }
    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    int mods = 0;
    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
    if (keyState & MK_ALT)     mods |= MK_ALT;
    if (keyState & MK_SHIFT)   mods |= MK_SHIFT;
    dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ChooseDropEffect(keyState, *effect);
    OnPaint();
    return S_OK;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::DragLeave()
{
    if (selfDragActive_)
    {
        dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        HideDragHintWindow();
        OnPaint();
        return S_OK;
    }
    externalDragActive_ = false;
    externalDropFileCount_ = 0;
    externalDropHasShortcut_ = false;
    EndDragSession();
    HideDragHintWindow();
    OnPaint();
    return S_OK;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::Drop(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;
    HideDragHintWindow();

    POINT clientPoint = ScreenPointToClient(point);

    if (selfDragActive_)
    {
        selfDragActive_ = false;
        selfDragReturned_ = true;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        ReleaseCapture();

        if (dragSession_.TargetRegion() == HitRegion::Handoff)
        {
            // ── Shell handoff via IShellFolder::IDropTarget ────
            Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj && targetItem)
            {
                ComPtr<IDropTarget> dt;
                if (auto* icon = dynamic_cast<DesktopIcon*>(targetItem))
                {
                    DesktopItem* di = icon->GetDesktopItem();
                    if (di && di->childPidl.get())
                    {
                        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(di->childPidl.get());
                        desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                            reinterpret_cast<void**>(dt.GetAddressOf()));
                    }
                }
                if (!dt && !targetItem->GetPath().empty())
                {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                        nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                    {
                        shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt));
                    }
                }
                if (dt)
                {
                    POINTL spl{ point.x, point.y };
                    DWORD le = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                    dt->DragEnter(dataObj.Get(), keyState, spl, &le);
                    dt->DragOver(keyState, spl, &le);
                    dt->Drop(dataObj.Get(), keyState, spl, &le);
                }
            }
            ClearSelection();
            EndDragSession();
            ReloadItems();
            *effect = DROPEFFECT_MOVE;
            return S_OK;
        }

        // ── OO dispatch ────────────────────────────────────
        if (dragSession_.TargetContainer())
        {
            int mods = 0;
            if (keyState & MK_CONTROL) mods |= MK_CONTROL;
            if (keyState & MK_ALT)     mods |= MK_ALT;
            if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

            Container* targetContainer = dragSession_.TargetContainer();
            bool needsReload = targetContainer->NeedsShellReloadAfterDrop();
            targetContainer->OnItemsDropped(dragSession_.Items(), dragSession_.Source(),
                dragSession_.TargetSlot(), dragSession_.TargetRegion(), mods);

            SaveLayoutSlots();
            ClearSelection();
            EndDragSession();
            RebuildContainersAndItems();
            if (needsReload)
                ReloadItems();
            else
                InvalidateRect(hwnd_, nullptr, FALSE);
        }
        *effect = DROPEFFECT_MOVE;
        return S_OK;
    }

    // ── External drop ──────────────────────────────────────────
    externalDragActive_ = false;
    externalDropFileCount_ = 0;
    externalDropHasShortcut_ = false;

    std::vector<std::wstring> dropPaths = dataObject ? GetDropPaths(dataObject) : std::vector<std::wstring>();

    if (dragSession_.TargetRegion() == HitRegion::Handoff && dataObject)
    {
        // ── Handoff on item (desktop OR widget member) ──
        Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
        ComPtr<IDropTarget> dt;
        if (targetItem)
        {
            if (auto* icon = dynamic_cast<DesktopIcon*>(targetItem))
            {
                DesktopItem* di = icon->GetDesktopItem();
                if (di && di->childPidl.get())
                {
                    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(di->childPidl.get());
                    desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                        reinterpret_cast<void**>(dt.GetAddressOf()));
                }
            }
            if (!dt && !targetItem->GetPath().empty())
            {
                ComPtr<IShellItem> shellItem;
                if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                    nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                {
                    shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt));
                }
            }
        }

        if (dt)
        {
            DWORD le = *effect;
            POINTL spl{ point.x, point.y };
            dt->DragEnter(dataObject, keyState, spl, &le);
            dt->DragOver(keyState, spl, &le);
            dt->Drop(dataObject, keyState, spl, &le);
            *effect = le;
            EndDragSession();
            ReloadItems(false);
            return S_OK;
        }
    }

    if (dataObject && !dropPaths.empty())
    {
        std::vector<std::unique_ptr<ExternalFileItem>> externalItems;
        std::vector<Item*> sourceItems;
        for (const auto& path : dropPaths)
        {
            auto item = std::make_unique<ExternalFileItem>(path);
            sourceItems.push_back(item.get());
            externalItems.push_back(std::move(item));
        }

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        DragSourceList sourceList = BuildDragSourceList(sourceItems, nullptr);
        Container* target = dragSession_.TargetContainer() ? dragSession_.TargetContainer() : GetDesktopGrid();
        HitRegion targetRegion = dragSession_.TargetRegion() != HitRegion::None ? dragSession_.TargetRegion() : HitRegion::Empty;
        DropPreviewList preview = BuildDropPreviewList(sourceList, target,
            dragSession_.TargetContainer() ? dragSession_.TargetSlot() : nullptr, targetRegion, mods, clientPoint);
        bool executed = ExecuteDropPipeline(sourceList, preview);
        if (executed)
        {
            SaveLayoutSlots();
            EndDragSession();
            RebuildContainersAndItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            *effect = ChooseDropEffect(keyState, *effect);
            return S_OK;
        }
        selfDragOutKeys_.clear();
    }

    *effect = DROPEFFECT_NONE;
    EndDragSession();
    return S_OK;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::QueryContinueDrag(BOOL escapePressed, DWORD keyState)
{
    if (escapePressed) return DRAGDROP_S_CANCEL;
    if ((keyState & (MK_LBUTTON | MK_RBUTTON)) == 0) return DRAGDROP_S_DROP;
    return S_OK;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::GiveFeedback(DWORD)
{
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

inline std::vector<std::wstring> DesktopApp::GetDropPaths(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    FORMATETC fmt{};
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (SUCCEEDED(dataObject->GetData(&fmt, &med)))
    {
        HDROP hDrop = static_cast<HDROP>(med.hGlobal);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                paths.push_back(path);
        }
        ReleaseStgMedium(&med);
    }
    return paths;
}

inline std::wstring DesktopApp::FileNameFromPath(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

inline bool DesktopApp::MatchPendingName(const std::wstring& itemName, const std::wstring& srcFileName)
{
    static const std::wstring kShortcutSuffix = L" - 快捷方式";
    static const std::wstring kCopySuffix = L" - 副本";

    auto stripLnk = [](const std::wstring& s) -> std::wstring {
        if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".lnk") == 0)
            return s.substr(0, s.size() - 4);
        return s;
    };
    auto stripExt = [](const std::wstring& s) -> std::wstring {
        size_t dot = s.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) return s;
        return s.substr(0, dot);
    };
    auto stripShortcut = [&](const std::wstring& s) -> std::wstring {
        if (s.size() > kShortcutSuffix.size() &&
            s.compare(s.size() - kShortcutSuffix.size(), kShortcutSuffix.size(), kShortcutSuffix) == 0)
            return s.substr(0, s.size() - kShortcutSuffix.size());
        return s;
    };
    auto stripCopy = [&](const std::wstring& s) -> std::wstring {
        std::wstring value = s;
        size_t paren = value.rfind(L" (");
        if (paren != std::wstring::npos && value.ends_with(L")"))
            value = value.substr(0, paren);
        if (value.size() > kCopySuffix.size() &&
            value.compare(value.size() - kCopySuffix.size(), kCopySuffix.size(), kCopySuffix) == 0)
            return value.substr(0, value.size() - kCopySuffix.size());
        return s;
    };
    auto eqi = [](const std::wstring& a, const std::wstring& b) -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (towlower(a[i]) != towlower(b[i])) return false;
        return true;
    };

    if (eqi(itemName, srcFileName)) return true;

    std::wstring nameNoLnk = stripLnk(itemName);
    std::wstring srcNoExt = stripExt(srcFileName);

    if (eqi(nameNoLnk, srcFileName)) return true;
    if (eqi(itemName, srcNoExt)) return true;
    if (eqi(nameNoLnk, srcNoExt)) return true;

    std::wstring nameNoShortcut = stripShortcut(itemName);
    std::wstring nameNoLnkNoShortcut = stripShortcut(nameNoLnk);

    if (eqi(nameNoShortcut, srcFileName)) return true;
    if (eqi(nameNoShortcut, srcNoExt)) return true;
    if (eqi(nameNoLnkNoShortcut, srcFileName)) return true;
    if (eqi(nameNoLnkNoShortcut, srcNoExt)) return true;

    std::wstring nameNoCopy = stripCopy(itemName);
    std::wstring nameNoLnkNoCopy = stripCopy(nameNoLnk);
    std::wstring nameNoExtNoCopy = stripCopy(stripExt(itemName));
    if (eqi(nameNoCopy, srcFileName)) return true;
    if (eqi(nameNoCopy, srcNoExt)) return true;
    if (eqi(nameNoLnkNoCopy, srcFileName)) return true;
    if (eqi(nameNoLnkNoCopy, srcNoExt)) return true;
    if (eqi(nameNoExtNoCopy, srcNoExt)) return true;

    return false;
}

// ── Drag hint ────────────────────────────────────────────────

inline bool DesktopApp::EnsureDragHintWindow()
{
    if (hintHwnd_) return true;
    hintHwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kHintWindowClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
    return hintHwnd_ != nullptr;
}

inline void DesktopApp::HideDragHintWindow()
{
    if (hintHwnd_) { ShowWindow(hintHwnd_, SW_HIDE); hintTextCache_.clear(); }
}

inline void DesktopApp::DestroyDragHintWindow()
{
    if (hintHwnd_) { DestroyWindow(hintHwnd_); hintHwnd_ = nullptr; }
}

inline void DesktopApp::ShowDragHintWindow(POINT clientPoint, const std::wstring& text)
{
    if (text.empty())
    {
        HideDragHintWindow();
        return;
    }

    // Skip expensive GDI rebuild if text hasn't changed — just move the window
    if (text == hintTextCache_)
    {
        if (hintHwnd_ && IsWindowVisible(hintHwnd_))
        {
            POINT screenPoint = clientPoint;
            ClientToScreen(hwnd_, &screenPoint);
            SetWindowPos(hintHwnd_, HWND_TOPMOST,
                screenPoint.x + 48, screenPoint.y + 22,
                0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return;
    }
    hintTextCache_ = text;

    if (!EnsureDragHintWindow())
    {
        HideDragHintWindow();
        return;
    }

    POINT screenPoint = clientPoint;
    ClientToScreen(hwnd_, &screenPoint);

    HDC screenDc = GetDC(nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(screenDc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
    SelectObject(screenDc, oldFont);

    int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
    int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
    POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8,
            monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
        windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8,
            monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        HideDragHintWindow();
        return;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
        return (static_cast<std::uint32_t>(a) << 24) |
            (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
    };

    const std::uint32_t bg = argb(255, 255, 255, 255);
    const std::uint32_t bd = argb(255, 205, 211, 220);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(y * width) + x] = (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? bd : bg;

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memoryDc, bitmap);
    HGDIOBJ oldMFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(25, 32, 42));
    RECT textRect{ 10, 0, width - 10, height };
    DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (int i = 0; i < width * height; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        pixels[i] = argb(255, r, g, b);
    }

    POINT sourcePoint{ 0, 0 };
    SIZE windowSize{ width, height };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
    ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

    SelectObject(memoryDc, oldMFont);
    SelectObject(memoryDc, oldBmp);
    DeleteDC(memoryDc);
    DeleteObject(bitmap);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDc);
}

inline void DesktopApp::ShowDragHintWindowScreen(POINT screenPoint, const std::wstring& text)
{
    if (text.empty() || !EnsureDragHintWindow())
    {
        HideDragHintWindow();
        return;
    }

    HDC screenDc = GetDC(nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(screenDc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
    SelectObject(screenDc, oldFont);

    int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
    int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
    POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8,
            monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
        windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8,
            monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        HideDragHintWindow();
        return;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
        return (static_cast<std::uint32_t>(a) << 24) |
            (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
    };

    const std::uint32_t bg = argb(255, 255, 255, 255);
    const std::uint32_t bd = argb(255, 205, 211, 220);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(y * width) + x] = (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? bd : bg;

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memoryDc, bitmap);
    HGDIOBJ oldMFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(25, 32, 42));
    RECT textRect{ 10, 0, width - 10, height };
    DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (int i = 0; i < width * height; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        pixels[i] = argb(255, r, g, b);
    }

    POINT sourcePoint{ 0, 0 };
    SIZE windowSize{ width, height };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
    ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

    SelectObject(memoryDc, oldMFont);
    SelectObject(memoryDc, oldBmp);
    DeleteDC(memoryDc);
    DeleteObject(bitmap);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDc);
}

// ── Widget context menu ────────────────────────────────────

inline void DesktopApp::ShowWidgetEditorHost(size_t widgetIndex)
{
    if (!settingsWindow_ || widgetIndex >= widgets_.size()) return;
    const auto& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::LuaScript) return;
    settingsWindow_->ShowWidgetEditor(widgetIndex, widget.id.c_str(),
        widget.title.c_str(), widget.scriptPath.c_str());
}

inline void DesktopApp::ShowWidgetContextMenu(POINT screenPoint, size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return;
    ClearMenuIcons();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const auto& widget = widgets_[widgetIndex];
    std::vector<LuaWidgetMenuItem> luaMenuItems;

    if (widget.type == DesktopWidgetType::Collection)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpen, L"打开全部");
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetManualCollect, L"立即收集");
        AppendMenuW(menu, MF_STRING | (widget.autoCollect ? MF_CHECKED : 0), kContextWidgetToggleAutoCollect, L"自动收集");
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode, widget.listMode ? L"图标显示" : L"列表显示");
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpenFolder, L"打开文件夹");
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode, widget.listMode ? L"图标显示" : L"列表显示");
        AppendMenuW(menu, MF_STRING, kContextNewMenu, L"新建");
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");
    }
    else if (widget.type == DesktopWidgetType::LuaScript)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetEdit, L"编辑组件");
        if (widgetEngine_)
        {
            widgetEngine_->EnsureWidgetLoaded(widget.id, widget.scriptPath);
            luaMenuItems = widgetEngine_->GetContextMenu(widget.id);
            for (size_t i = 0; i < luaMenuItems.size() &&
                kContextLuaWidgetMenuFirst + static_cast<UINT>(i) <= kContextLuaWidgetMenuLast; ++i)
            {
                const auto& item = luaMenuItems[i];
                if (item.separator)
                {
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    continue;
                }
                UINT flags = MF_STRING | (item.enabled ? 0 : MF_GRAYED);
                AppendMenuW(menu, flags,
                    kContextLuaWidgetMenuFirst + static_cast<UINT>(i),
                    Utf8ToWide(item.label).c_str());
            }
        }
    }

    if (widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::Collection)
    {
        HMENU sortMenu = CreatePopupMenu();
        if (sortMenu)
        {
            AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByName, L"名称");
            AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByType, L"类型");
            AppendMenuW(sortMenu, MF_STRING, kContextWidgetSortByDate, L"修改日期");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式");
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextWidgetRename, L"重命名");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kContextWidgetDelete, L"删除组件");

    SetMenuItemIcon(menu, kContextWidgetOpen, L"");
    SetMenuItemIcon(menu, kContextWidgetManualCollect, L"");
    SetMenuItemIcon(menu, kContextWidgetToggleListMode, widget.listMode ? L"" : L"");
    SetMenuItemIcon(menu, kContextWidgetOpenFolder, L"");
    SetMenuItemIcon(menu, kContextNewMenu, L"");
    SetMenuItemIcon(menu, kContextMoreCommand, L"");
    SetMenuItemIcon(menu, kContextWidgetEdit, L"");
    SetMenuItemIcon(menu, kContextWidgetRename, L"");
    SetMenuItemIcon(menu, kContextWidgetDelete, L"");
    {
        MENUITEMINFOW sortMii{ sizeof(sortMii) };
        sortMii.fMask = MIIM_SUBMENU;
        for (int i = 0; i < GetMenuItemCount(menu); ++i)
        {
            if (GetMenuItemInfoW(menu, i, TRUE, &sortMii) && sortMii.hSubMenu)
            {
                wchar_t label[64]{};
                if (GetMenuStringW(menu, i, label, _countof(label), MF_BYPOSITION) && wcsstr(label, L"排序方式"))
                {
                    SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(sortMii.hSubMenu), L"");
                    break;
                }
            }
        }
    }
    {
        MENUITEMINFOW sortMii2{ sizeof(sortMii2) };
        sortMii2.fMask = MIIM_SUBMENU;
        for (int i = 0; i < GetMenuItemCount(menu); ++i)
        {
            if (GetMenuItemInfoW(menu, i, TRUE, &sortMii2) && sortMii2.hSubMenu)
            {
                wchar_t label[64]{};
                if (GetMenuStringW(menu, i, label, _countof(label), MF_BYPOSITION) && wcsstr(label, L"排序方式"))
                {
                    SetMenuItemIcon(sortMii2.hSubMenu, kContextWidgetSortByName, L"");
                    SetMenuItemIcon(sortMii2.hSubMenu, kContextWidgetSortByType, L"");
                    SetMenuItemIcon(sortMii2.hSubMenu, kContextWidgetSortByDate, L"");
                    break;
                }
            }
        }
    }

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    DestroyMenu(menu);
    ClearMenuIcons();

    if (command >= kContextLuaWidgetMenuFirst && command <= kContextLuaWidgetMenuLast)
    {
        size_t itemIndex = static_cast<size_t>(command - kContextLuaWidgetMenuFirst);
        if (itemIndex < luaMenuItems.size() && widgetEngine_)
        {
            widgetEngine_->InvokeMenu(widgets_[widgetIndex].id, luaMenuItems[itemIndex].id);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    switch (command)
    {
    case kContextWidgetOpen:
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        OpenCollectionPopupAt(widgetIndex, clientPoint, L"");
        break;
    }
    case kContextWidgetOpenFolder:
        if (!widget.sourceFolderPath.empty())
            ShellExecuteW(hwnd_, L"open", widget.sourceFolderPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kContextWidgetToggleListMode:
        widgets_[widgetIndex].listMode = !widgets_[widgetIndex].listMode;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[widgetIndex])
            { wc->InvalidateSlots(); break; }
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleAutoCollect:
        widgets_[widgetIndex].autoCollect = !widgets_[widgetIndex].autoCollect;
        if (widgets_[widgetIndex].autoCollect)
        {
            EnforceSingleAutoCollectFileCategory(widgetIndex);
            CollectFileCategoryWidget(widgetIndex, false);
        }
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetManualCollect:
        if (!CollectFileCategoryWidget(widgetIndex, true))
            MessageBeep(MB_ICONINFORMATION);
        break;
    case kContextWidgetRename:
        SelectWidgetOnly(widgetIndex);
        BeginRenameSelected();
        break;
    case kContextWidgetEdit:
        ShowWidgetEditorHost(widgetIndex);
        break;
    case kContextWidgetDelete:
    {
        if (widgets_[widgetIndex].type == DesktopWidgetType::LuaScript && widgetEngine_)
            widgetEngine_->UnloadWidget(widgets_[widgetIndex].id);
        // Move widget's itemKeys back to desktop by just removing the widget
        widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(widgetIndex));
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    }
    case kContextNewMenu:
        if (widgetIndex < widgets_.size() && widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping &&
            !widgets_[widgetIndex].sourceFolderPath.empty())
        {
            ShowNewMenuAndInvoke(screenPoint, widgets_[widgetIndex].sourceFolderPath);
            RefreshFolderMappingWidget(widgetIndex);
            RebuildContainersAndItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextMoreCommand:
        if (widgetIndex < widgets_.size() && widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping &&
            !widgets_[widgetIndex].sourceFolderPath.empty())
        {
            ShowShellContextMenuForPath(widgets_[widgetIndex].sourceFolderPath, screenPoint);
        }
        break;
    case kContextWidgetSortByName:
        SortWidgetContents(widgetIndex, 0);
        break;
    case kContextWidgetSortByType:
        SortWidgetContents(widgetIndex, 1);
        break;
    case kContextWidgetSortByDate:
        SortWidgetContents(widgetIndex, 2);
        break;
    default:
        break;
    }
}

// ── Tray ────────────────────────────────────────────────────

inline void DesktopApp::AddTrayIcon(bool force)
{
    HWND owner = controlHwnd_ ? controlHwnd_ : hwnd_;
    if (!owner || !IsWindow(owner)) return;
    if (trayIconAdded_ && !force) return;

    if (force && trayIconAdded_)
    {
        NOTIFYICONDATAW del{};
        del.cbSize = sizeof(del);
        del.hWnd = trayIconOwnerHwnd_ ? trayIconOwnerHwnd_ : owner;
        del.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &del);
        trayIconAdded_ = false;
    }

    if (!trayIcon_)
    {
        trayIcon_ = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON_SMALL),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = trayIcon_;
    wcscpy_s(data.szTip, L"SnowDesktop");
    if (Shell_NotifyIconW(NIM_ADD, &data))
    {
        trayIconAdded_ = true;
        trayIconOwnerHwnd_ = owner;
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

inline void DesktopApp::RemoveTrayIcon()
{
    if (!trayIconAdded_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = trayIconOwnerHwnd_ ? trayIconOwnerHwnd_ : hwnd_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconAdded_ = false;
}

inline void DesktopApp::OnTrayCallback(LPARAM lParam)
{
    UINT message = LOWORD(lParam);
    if (message == WM_CONTEXTMENU || message == WM_RBUTTONUP)
    {
        POINT pt{};
        GetCursorPos(&pt);
        ShowTrayMenu(pt);
    }
    else if (message == WM_LBUTTONDBLCLK)
    {
        ReloadItems();
    }
}

inline void DesktopApp::ShowTrayMenu(POINT screenPoint)
{
    ClearMenuIcons();
    HMENU menu = CreatePopupMenu();

    HMENU iconMenu = CreatePopupMenu();
    if (iconMenu)
    {
        struct IS { UINT cmd; const wchar_t* clsid; const wchar_t* label; };
        const IS items[] = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC, L"计算机" },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles, L"用户的文件" },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork, L"网络" },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel, L"控制面板" },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin, L"回收站" },
        };
        for (const auto& s : items)
        {
            UINT flags = MF_STRING;
            DWORD val = 0;
            if (TryReadDesktopIconRegistryValueAnyRoot(s.clsid, val))
            { if (val == 0) flags |= MF_CHECKED; }
            else
            {
                static const std::unordered_map<std::wstring, bool> defVis = {
                    { kDesktopIconClsidThisPC, false }, { kDesktopIconClsidUserFiles, false },
                    { kDesktopIconClsidNetwork, false }, { kDesktopIconClsidControlPanel, false },
                    { kDesktopIconClsidRecycleBin, true },
                };
                auto it = defVis.find(s.clsid);
                if (it != defVis.end() && it->second) flags |= MF_CHECKED;
            }
            AppendMenuW(iconMenu, flags, s.cmd, s.label);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), L"桌面图标设置");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTraySettingsCommand, L"设置");
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出软件");

    SetMenuItemIcon(menu, kTraySettingsCommand, L"");
    SetMenuItemIcon(menu, kTrayExitCommand, L"");

    SetForegroundWindow(controlHwnd_ ? controlHwnd_ : hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, controlHwnd_ ? controlHwnd_ : hwnd_, nullptr);

    if (iconMenu) DestroyMenu(iconMenu);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kTraySettingsCommand:
        ShowSettingsWindow();
        break;
    case kTrayExitCommand:
        if (settingsWindow_)
            settingsWindow_->ShowExitConfirm();
        else
            RequestExit();
        break;
    case kTrayDesktopIconThisPC:
    case kTrayDesktopIconUserFiles:
    case kTrayDesktopIconNetwork:
    case kTrayDesktopIconControlPanel:
    case kTrayDesktopIconRecycleBin:
    {
        struct TV { UINT cmd; const wchar_t* clsid; };
        static const TV tv[] = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin },
        };
        for (const auto& t : tv)
        {
            if (t.cmd == command)
            {
                DWORD val = 0;
                bool visible = true;
                if (TryReadDesktopIconRegistryValueAnyRoot(t.clsid, val))
                    visible = (val == 0);
                WriteDesktopIconRegistryValue(t.clsid, !visible);
                ReloadItems();
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}
