#pragma once
// Inline implementations for DesktopApp — Interaction & Tray.
// This file is included by app.h after the class definition.

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
        RECT selRect = GetItemSelectionRect(di->bounds, di->selected);
        if (PtInRect(&selRect, pt)) return icon;
    }
    return nullptr;
}

inline void DesktopApp::ClearSelection()
{
    for (auto& oo : items_oo_)
    {
        auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
        if (icon) icon->SetSelected(false);
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
    SetFocus(hwnd_);
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    mouseDown_ = true;
    mouseDownPoint_ = pt;
    marqueeActive_ = false;
    marqueeRect_ = MakeRect(pt.x, pt.y, pt.x, pt.y);

    if (HandlePageNavClick(pt)) return;

    bool ctrl = (wp & MK_CONTROL) != 0;

    // ── Widget hit-test ─────────────────────────────────────
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    widgetAction_ = WidgetAction::None;
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
            widgetAction_ = WidgetAction::Resize;
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
            widgetAction_ = WidgetAction::Move;
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
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
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
            hit->SetSelected(!hit->IsSelected());
        else if (!di->selected)
        {
            ClearSelection();
            hit->SetSelected(true);
        }
    }
    else if (!ctrl)
        ClearSelection();

    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

inline void DesktopApp::OnMouseMove(WPARAM wp, LPARAM lp)
{
    POINT current{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT oldMouse = lastMousePoint_;
    lastMousePoint_ = current;

    if (mouseDown_ && mouseDownHit_ && mouseDownHit_->IsSelected() && !draggingItems_)
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            draggingItems_ = true;
            marqueeActive_ = false;
            UpdateDragGroupOrigin();
            dragCurrentPoint_ = current;
        }
    }

    if (mouseDown_ && !draggingItems_ && widgetAction_ == WidgetAction::None
        && mouseDownWidgetIndex_ < widgets_.size() && widgets_[mouseDownWidgetIndex_].selected
        && (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3))
    {
        // First significant move: activate the action
        if (mouseDownWidgetIndex_ < widgets_.size())
        {
            dragCurrentPoint_ = current;
            // Only set action based on hit type if not already set
            // (action was set by OnLeftButtonDown, but we confirm it after threshold here)
        }
    }

    // Widget resize preview
    if (widgetAction_ == WidgetAction::Resize && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        const auto& wp = widgets_[mouseDownWidgetIndex_];
        const GridPage* page = FindGridPage(gridPages_, wp.gridCell.pageId);
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
        const auto& wp = widgets_[mouseDownWidgetIndex_];
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

    if (draggingItems_)
    {
        dragCurrentPoint_ = current;
        dragCopyMode_ = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        dragLinkMode_ = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        POINT screenPt = current;
        ClientToScreen(hwnd_, &screenPt);
        bool overExternal = IsExternalDropWindowAt(current);

        if (overExternal)
        {
            ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
            if (dataObj)
            {
                HideDragHintWindow();
                ReleaseCapture();
                mouseDown_ = false;
                mouseDownHit_ = nullptr;
                draggingItems_ = false;
                dragCopyMode_ = false;
                dragLinkMode_ = false;

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
                draggingItems_ = false;

                if (hr == DRAGDROP_S_DROP && oleEffect == DROPEFFECT_MOVE && !selfDragReturned_)
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

                if (!selfDragReturned_)
                {
                    ClearSelection();
                    ReloadItems();
                }
                else
                {
                    SaveLayoutSlots();
                    ClearSelection();
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

        // OO Slot-based hit testing: widgets first (on top), DesktopGrid last
        dragTargetContainer_ = nullptr;
        dragTargetRegion_ = HitRegion::None;
        Slot* desktopSlot = nullptr;

        // Check widget containers first (reverse draw order = topmost first)
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(it->get());
            if (!wc) continue;

            // Also test: is the mouse inside the widget's frame at all?
            RECT frame = wc->GetFrameRect();
            if (!PtInRect(&frame, current)) continue;

            auto& wslots = wc->GetSlots();
            for (size_t i = 0; i < wslots.size(); ++i)
            {
                HitRegion region = wslots[i]->HitTest(current);
                if (region != HitRegion::None)
                {
                    dragTargetContainer_ = wc;
                    dragTargetSlotIndex_ = i;
                    dragTargetRegion_ = region;
                    goto show_hint;
                }
            }
            // Mouse in frame but not on any slot → sort at end or into empty area
            if (wslots.empty())
                dragTargetSlotIndex_ = 0;
            else
                dragTargetSlotIndex_ = wslots.size();
            dragTargetContainer_ = wc;
            dragTargetRegion_ = HitRegion::SortAfter;
            goto show_hint;
        }

        // DesktopGrid as fallback
        for (auto& c : containers_)
        {
            if (auto* grid = dynamic_cast<DesktopGrid*>(c.get()))
            {
                HitRegion region = grid->HitTestAtPoint(current, desktopSlot);
                if (region != HitRegion::None)
                {
                    dragTargetContainer_ = grid;
                    dragTargetRegion_ = region;
                    dragTargetSlotIndex_ = desktopSlot ? desktopSlot->GetIndex() : 0;

                    if (region == HitRegion::SortBefore)
                    {
                        int hit = HitTestItem(current);
                        if (hit >= 0 && !items_[hit].selected)
                        {
                            RECT iconRect = GetItemIconRect(items_[hit].bounds);
                            RECT hf = { iconRect.left - 4, iconRect.top - 2,
                                        iconRect.right + 4, iconRect.bottom + 4 };
                            if (PtInRect(&hf, current))
                                dragTargetRegion_ = HitRegion::Handoff;
                        }
                    }
                    if (dragTargetRegion_ == HitRegion::Handoff) break;
                }
                break; // only one DesktopGrid
            }
        }
    show_hint:
        // Desktop hint uses existing grid-aware logic (modifier keys etc.)
        std::wstring hint = MakeDragHint(current);

        // Only override with OO hint for non-desktop containers
        if (dragTargetContainer_ && dragTargetRegion_ != HitRegion::None
            && !dynamic_cast<DesktopGrid*>(dragTargetContainer_))
        {
            auto& slots = dragTargetContainer_->GetSlots();
            if (dragTargetSlotIndex_ < slots.size())
            {
                std::wstring ooHint = slots[dragTargetSlotIndex_]->GetDropHint(dragTargetRegion_, {});
                if (!ooHint.empty()) hint = ooHint;
            }
        }
        ShowDragHintWindow(current, hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (mouseDown_ && !mouseDownHit_)
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            marqueeActive_ = true;
            marqueeRect_ = NormalizeRect(mouseDownPoint_, current);

            for (auto& oo : items_oo_)
            {
                auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
                if (!icon) continue;
                DesktopItem* di = icon->GetDesktopItem();
                if (!di || IsRectEmptyRect(di->bounds)) continue;
                RECT itemSelRect = GetItemSelectionRect(di->bounds, false);
                di->selected = RectsIntersect(itemSelRect, marqueeRect_);
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
    (void)wp; (void)lp;
    HideDragHintWindow();

    // ── Widget action completion ────────────────────────────
    if (widgetAction_ != WidgetAction::None && mouseDownWidgetIndex_ < widgets_.size())
    {
        if (widgetAction_ == WidgetAction::Resize)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_);
        else if (widgetAction_ == WidgetAction::Move)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_);
        widgetAction_ = WidgetAction::None;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!draggingItems_)
    {
        mouseDown_ = false;
        marqueeActive_ = false;
        navHoverSide_ = 0;
        navAutoFlipDir_ = 0;
        mouseDownHit_ = nullptr;
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragTargetContainer_ || dragTargetRegion_ == HitRegion::None)
    {
        goto cleanup;
    }

    {
        int mods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;

        bool onDesktop = dynamic_cast<DesktopGrid*>(dragTargetContainer_) != nullptr;

        if (dragTargetRegion_ == HitRegion::Handoff && onDesktop)
        {
            // Use HitTestItem for reliable handoff target detection
            int hit = HitTestItem(dragCurrentPoint_);
            if (hit >= 0 && !items_[hit].selected)
                DropSelectedItemsOnTarget(hit);
            SaveLayoutSlots();
            ClearSelection();
            ReloadItems();
        }
        else if (onDesktop)
        {
            // ── Desktop drop: exact same math as preview ────────
            POINT adjusted = GetDragTargetPoint(dragCurrentPoint_);
            GridCell tc = FindBestDropCell(CellFromPoint(adjusted));

            if (mods & MK_ALT)      CreateShortcutSelectedItemsToCell(tc);
            else if (mods & MK_CONTROL) CopySelectedItemsToCell(tc);
            else                     MoveSelectedItemsToCell(tc);

            SaveLayoutSlots();
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else
        {
            // ── Widget container drop ─────────────────────────
            auto& slots = dragTargetContainer_->GetSlots();
            Slot* ts = (dragTargetSlotIndex_ < slots.size()) ? slots[dragTargetSlotIndex_].get() : nullptr;

            // Build source items from selected DesktopItems in items_[] (not items_oo_,
            // which skips collected items). Create temporary DesktopIcon wrappers.
            std::vector<std::unique_ptr<DesktopIcon>> tempIcons;
            std::vector<Item*> src;
            Container* origin = GetDesktopGrid();
            for (auto& di : items_)
            {
                if (di.selected && !di.name.empty())
                {
                    auto tmp = std::make_unique<DesktopIcon>(&di, origin);
                    src.push_back(tmp.get());
                    tempIcons.push_back(std::move(tmp));
                }
            }
            if (ts && !src.empty())
                ts->ExecuteDrop(dragTargetRegion_, src, origin, mods);

            SaveLayoutSlots();
            ClearSelection();
            ReloadItems();
        }
    }

cleanup:
    draggingItems_ = false;
    dragCopyMode_ = false;
    dragLinkMode_ = false;
    dragTargetContainer_ = nullptr;
    dragTargetRegion_ = HitRegion::None;
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    ReleaseCapture();
}

inline void DesktopApp::OnKeyDown(WPARAM key)
{
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
        InvokeSelectedShellVerb("copy");
        break;
    case 'X':
        if (!ctrl) break;
    {
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

    bool wasDragging = draggingItems_;
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (wasDragging) MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();
    if (wasDragging) UpdateDragGroupOrigin();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline void DesktopApp::OnRightButtonUp(LPARAM lp)
{
    if (renameEdit_ != nullptr) return;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int hit = HitTestItem(pt);
    if (hit >= 0 && !items_[hit].selected)
        SelectOnly(hit);
    else if (hit < 0)
        ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);

    POINT screenPt = pt;
    ClientToScreen(hwnd_, &screenPt);
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
        DesktopWindows current = FindDesktopWindows();
        if (current.host != desktopWindows_.host || !IsWindow(desktopWindows_.host))
        {
            desktopWindows_ = current;
            if (hwnd_ && IsWindow(hwnd_))
            {
                HWND parent = desktopWindows_.host ? desktopWindows_.host : GetDesktopWindow();
                SetParent(hwnd_, parent);
                POINT origin{ virtualLeft_, virtualTop_ };
                ScreenToClient(parent, &origin);
                SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
            }
        }
    }
}

// ── Rename ──────────────────────────────────────────────────

inline void DesktopApp::BeginRenameSelected()
{
    if (renameEdit_ != nullptr) return;

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
    RECT textRect = GetItemTextRect(items_[selectedIndex].bounds, true);
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
        draggingItems_ = true;
        POINT client = ScreenPointToClient(point);
        dragCurrentPoint_ = client;

        // OO hit-test for hint
        HitRegion tmp;
        Slot* s;
        for (auto& c : containers_)
        {
            if (auto* grid = dynamic_cast<DesktopGrid*>(c.get()))
            {
                tmp = grid->HitTestAtPoint(client, s);
                if (tmp != HitRegion::None)
                {
                    int hit = HitTestItem(client);
                    if (hit >= 0 && !items_[hit].selected)
                    {
                        RECT ir = GetItemIconRect(items_[hit].bounds);
                        RECT hf = { ir.left - 4, ir.top - 2, ir.right + 4, ir.bottom + 4 };
                        if (PtInRect(&hf, client)) tmp = HitRegion::Handoff;
                    }
                    break;
                }
            }
        }
        std::wstring hint = MakeDragHint(client);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return S_OK;
    }

    externalDragActive_ = true;
    externalDragPoint_ = ScreenPointToClient(point);

    // OO hit-test for external hint
    HitTestExternalAt(externalDragPoint_);

    if (dataObject)
        externalDropFileCount_ = static_cast<int>(GetDropPaths(dataObject).size());
    else
        externalDropFileCount_ = 1;

    std::wstring hint = MakeExternalDragHint(externalDragPoint_);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ChooseDropEffect(keyState, *effect);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return S_OK;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::DragOver(
    DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;

    if (selfDragActive_)
    {
        POINT client = ScreenPointToClient(point);
        dragCurrentPoint_ = client;
        std::wstring hint = MakeDragHint(client);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return S_OK;
    }

    externalDragActive_ = true;
    externalDragPoint_ = ScreenPointToClient(point);
    HitTestExternalAt(externalDragPoint_);
    std::wstring hint = MakeExternalDragHint(externalDragPoint_);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ChooseDropEffect(keyState, *effect);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return S_OK;
}

inline HRESULT STDMETHODCALLTYPE DesktopApp::DragLeave()
{
    if (selfDragActive_)
    {
        draggingItems_ = false;
        HideDragHintWindow();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return S_OK;
    }
    externalDragActive_ = false;
    externalDropFileCount_ = 0;
    HideDragHintWindow();
    InvalidateRect(hwnd_, nullptr, FALSE);
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
        draggingItems_ = false;

        if (dragTargetRegion_ == HitRegion::Handoff)
        {
            // ── Shell handoff via IShellFolder::IDropTarget ────
            int hit = HitTestItem(clientPoint);
            if (hit >= 0 && !items_[hit].selected)
            {
                ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
                if (dataObj)
                {
                    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(items_[hit].childPidl.get());
                    ComPtr<IDropTarget> dt;
                    if (SUCCEEDED(desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                        reinterpret_cast<void**>(dt.GetAddressOf()))) && dt)
                    {
                        POINTL spl{ point.x, point.y };
                        DWORD le = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                        dt->DragEnter(dataObj.Get(), keyState, spl, &le);
                        dt->DragOver(keyState, spl, &le);
                        dt->Drop(dataObj.Get(), keyState, spl, &le);
                    }
                }
            }
            ClearSelection();
            ReloadItems();
            *effect = DROPEFFECT_MOVE;
            return S_OK;
        }

        // ── Desktop drop: reuse same math as OnLeftButtonUp ────
        POINT adjusted = GetDragTargetPoint(clientPoint);
        GridCell tc = FindBestDropCell(CellFromPoint(adjusted));
        MoveSelectedItemsToCell(tc);
        SaveLayoutSlots();
        ClearSelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        *effect = DROPEFFECT_MOVE;
        return S_OK;
    }

    // ── External drop ──────────────────────────────────────────
    externalDragActive_ = false;
    externalDropFileCount_ = 0;

    std::vector<std::wstring> dropPaths = dataObject ? GetDropPaths(dataObject) : std::vector<std::wstring>();

    if (dragTargetRegion_ == HitRegion::Handoff && dataObject)
    {
        // ── Handoff on specific item ──
        int hit = HitTestItem(clientPoint);
        if (hit >= 0)
        {
            std::unordered_set<std::wstring> existingKeys;
            for (const auto& item : items_)
                if (!item.layoutKey.empty()) existingKeys.insert(item.layoutKey);

            PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(items_[hit].childPidl.get());
            ComPtr<IDropTarget> dt;
            if (SUCCEEDED(desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                reinterpret_cast<void**>(dt.GetAddressOf()))) && dt)
            {
                DWORD le = *effect;
                POINTL spl{ point.x, point.y };
                dt->DragEnter(dataObject, keyState, spl, &le);
                dt->DragOver(keyState, spl, &le);
                dt->Drop(dataObject, keyState, spl, &le);
                *effect = le;

                if (!dropPaths.empty())
                {
                    pendingPlaceNames_ = dropPaths;
                    pendingPlaceCell_ = CellFromPoint(clientPoint);
                    hasPendingPlace_ = true;
                    pendingPlaceTick_ = GetTickCount();
                }
                ReloadItems(false);
                PlaceNewItemsAtDropPoint(existingKeys, CellFromPoint(clientPoint));
                return S_OK;
            }
        }
    }

    // ── Non-handoff external drop: delegate to shell desktop ───
    if (dataObject)
    {
        std::unordered_set<std::wstring> existingKeys;
        for (const auto& item : items_)
            if (!item.layoutKey.empty()) existingKeys.insert(item.layoutKey);

        // Self-drag-out recognition
        if (!selfDragOutKeys_.empty())
        {
            std::vector<std::wstring> paths = GetDropPaths(dataObject);
            std::unordered_set<std::wstring> pathSet(paths.begin(), paths.end());
            bool allCached = true;
            for (const auto& k : selfDragOutKeys_)
            {
                size_t idx = FindItemIndexByKey(k);
                if (idx == static_cast<size_t>(-1)) { allCached = false; break; }
                if (!pathSet.contains(items_[idx].parsingName)) { allCached = false; break; }
            }
            if (allCached)
            {
                MoveSelectedItemsToCell(FindBestDropCell(CellFromPoint(clientPoint)));
                LayoutItems();
                SaveLayoutSlots();
                selfDragOutKeys_.clear();
                *effect = DROPEFFECT_MOVE;
                ClearSelection();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return S_OK;
            }
        }
        selfDragOutKeys_.clear();

        // Delegate to shell desktop folder's drop target
        ComPtr<IDropTarget> desktopTarget;
        if (SUCCEEDED(desktopFolder_->CreateViewObject(hwnd_, IID_IDropTarget,
            reinterpret_cast<void**>(desktopTarget.GetAddressOf()))) && desktopTarget)
        {
            POINTL spl{ point.x, point.y };
            DWORD adjustedKS = keyState;
            if (*effect == DROPEFFECT_MOVE)
                adjustedKS = (keyState & ~MK_CONTROL & ~MK_ALT) | MK_SHIFT;
            else if (*effect == DROPEFFECT_COPY)
                adjustedKS = (keyState & ~MK_SHIFT & ~MK_ALT) | MK_CONTROL;
            else if (*effect == DROPEFFECT_LINK)
                adjustedKS = (keyState & ~MK_SHIFT & ~MK_CONTROL) | MK_ALT;

            DWORD le = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
            desktopTarget->DragEnter(dataObject, adjustedKS, spl, &le);
            desktopTarget->DragOver(adjustedKS, spl, &le);
            desktopTarget->Drop(dataObject, adjustedKS, spl, &le);
            *effect = le;

            if (!dropPaths.empty())
            {
                pendingPlaceNames_ = dropPaths;
                pendingPlaceCell_ = CellFromPoint(clientPoint);
                hasPendingPlace_ = true;
                pendingPlaceTick_ = GetTickCount();
            }
            ReloadItems(false);
            PlaceNewItemsAtDropPoint(existingKeys, CellFromPoint(clientPoint));
            return S_OK;
        }
    }

    *effect = DROPEFFECT_NONE;
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
    if (hintHwnd_) ShowWindow(hintHwnd_, SW_HIDE);
}

inline void DesktopApp::DestroyDragHintWindow()
{
    if (hintHwnd_) { DestroyWindow(hintHwnd_); hintHwnd_ = nullptr; }
}

inline void DesktopApp::ShowDragHintWindow(POINT clientPoint, const std::wstring& text)
{
    if (text.empty() || !EnsureDragHintWindow())
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

inline void DesktopApp::HitTestExternalAt(POINT pt)
{
    dragTargetContainer_ = nullptr;
    dragTargetRegion_ = HitRegion::None;
    for (auto& c : containers_)
    {
        if (auto* grid = dynamic_cast<DesktopGrid*>(c.get()))
        {
            Slot* s;
            HitRegion r = grid->HitTestAtPoint(pt, s);
            if (r != HitRegion::None)
            {
                dragTargetContainer_ = grid;
                dragTargetRegion_ = r;
                dragTargetSlotIndex_ = s ? s->GetIndex() : 0;
                // Check handoff override
                int hit = HitTestItem(pt);
                if (hit >= 0)
                {
                    RECT ir = GetItemIconRect(items_[hit].bounds);
                    RECT hf = { ir.left - 4, ir.top - 2, ir.right + 4, ir.bottom + 4 };
                    if (PtInRect(&hf, pt))
                        dragTargetRegion_ = HitRegion::Handoff;
                }
            }
            break;
        }
    }
}

inline std::wstring DesktopApp::MakeExternalDragHint(POINT point) const
{
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    if (dragTargetRegion_ == HitRegion::Handoff)
    {
        int hit = HitTestItem(point);
        if (hit >= 0)
            return L"释放：拖入「" + items_[hit].name + L"」";
    }

    if (altDown) return L"释放：创建快捷方式";
    if (shiftDown) return L"释放：移动到桌面";
    if (ctrlDown) return L"释放：复制到桌面";
    return L"释放：放置到桌面";
}

inline std::wstring DesktopApp::MakeDragHint(POINT point) const
{
    bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

    if (dragTargetRegion_ == HitRegion::Handoff)
    {
        int hit = HitTestItem(point);
        if (hit >= 0 && !items_[hit].selected)
            return L"释放：交给「" + items_[hit].name + L"」处理";
    }

    GridCell bestCell = FindBestDropCell(CellFromPoint(GetDragTargetPoint(point)));
    if (BuildSelectedMove(bestCell).empty())
        return L"释放：当前位置已有图标";

    if (altDown) return L"释放：创建快捷方式到此空位";
    if (ctrlDown) return L"释放：复制到此空位";
    return L"释放：移动到此空位";
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
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTSIZE));
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = trayIcon_;
    wcscpy_s(data.szTip, L"SnowDesktopOO");
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
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出软件");

    SetForegroundWindow(controlHwnd_ ? controlHwnd_ : hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, controlHwnd_ ? controlHwnd_ : hwnd_, nullptr);

    if (iconMenu) DestroyMenu(iconMenu);
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kTrayExitCommand:
        DestroyWindow(hwnd_);
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