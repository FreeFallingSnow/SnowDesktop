#include "app.h"
#include "../animation_settings.h"

void DesktopApp::BeginLargeIconPlacement(size_t index, snowdesktop::LargeIconConfig config)
{
    if (!CanEditLargeIcons() || index >= items_.size()) return;
    largeIconGesture_ = LargeIconGesture{items_[index].layoutKey, std::move(config), items_[index].gridCell, true, false, false};
    HandleLargeIconPointerMove(lastMousePoint_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::CancelLargeIconGesture()
{
    if (!largeIconGesture_) return;
    largeIconGesture_.reset();
    if (GetCapture() == hwnd_) ReleaseCapture();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool DesktopApp::HandleLargeIconPointerDown(POINT point)
{
    if (largeIconGesture_)
    {
        if (!CanEditLargeIcons()) { CancelLargeIconGesture(); return true; }
        if (largeIconGesture_->creating)
        {
            const auto index = FindItemIndexByKey(largeIconGesture_->key);
            if (index < items_.size() && largeIconGesture_->valid)
            {
                const auto oldCell = items_[index].gridCell;
                items_[index].gridCell = largeIconGesture_->cell;
                const auto config = largeIconGesture_->config;
                if (SetLargeIconConfig(index, config)) { CancelLargeIconGesture(); OpenLargeIconSettings(index); }
                else items_[index].gridCell = oldCell;
            }
            return true;
        }
    }
    if (!CanEditLargeIcons() || HasActiveContextMenuSession() || IsPointOccludedByOpenPopup(point)) return false;
    const auto index = HitTestItem(point);
    if (index < 0 || static_cast<size_t>(index) >= items_.size() || !items_[index].largeIcon) return false;
    DesktopWidget geometry;
    geometry.bounds = items_[index].bounds; geometry.gridCell = items_[index].gridCell; geometry.showTitle = false;
    if (const auto* page = FindGridPage(gridPages_, geometry.gridCell.pageId)) geometry.cellScale = GetGridPageCuScale(*page);
    const auto handle = GetStandaloneWidgetResizeHandleRect(geometry);
    if (!PtInRect(&handle, point)) return false;
    largeIconGesture_ = LargeIconGesture{items_[index].layoutKey, *items_[index].largeIcon, items_[index].gridCell, false, true, true};
    SetCapture(hwnd_);
    UpdateLargeIconHover();
    return true;
}

bool DesktopApp::HandleLargeIconPointerMove(POINT point)
{
    if (!largeIconGesture_) return false;
    if (!CanEditLargeIcons()) { CancelLargeIconGesture(); return true; }
    auto& gesture = *largeIconGesture_;
    const size_t index = FindItemIndexByKey(gesture.key);
    if (index >= items_.size() || (!gesture.creating && !items_[index].largeIcon)) { CancelLargeIconGesture(); return true; }
    GridCell hovered = CellFromPointForDrag(point);
    const auto* page = FindGridPage(gridPages_, gesture.resizing ? gesture.cell.pageId : hovered.pageId);
    if (!page || (gesture.resizing && hovered.pageId != page->id))
    { gesture.valid = false; InvalidateRect(hwnd_, nullptr, FALSE); return true; }
    if (gesture.creating) gesture.cell = hovered;
    else
    {
        gesture.config.columns = std::clamp(hovered.column - gesture.cell.column + 1, 1, page->columns - gesture.cell.column);
        gesture.config.rows = std::clamp(hovered.row - gesture.cell.row + 1, 1, page->rows - gesture.cell.row);
    }
    const GridSpan span{gesture.config.columns, gesture.config.rows};
    std::unordered_set<std::wstring> occupied;
    for (size_t i = 0; i < items_.size(); ++i)
        if (i != index && !IsItemInAnyWidget(items_[i])) MarkGridArea(occupied, items_[i].gridCell, items_[i].gridSpan);
    for (const auto& widget : widgets_) if (!IsGroupedWidget(widget)) MarkGridArea(occupied, widget.gridCell, widget.gridSpan);
    gesture.valid = gesture.cell.pageId == page->id && gesture.cell.column >= 0 && gesture.cell.row >= 0 &&
        gesture.cell.column + span.columns <= page->columns && gesture.cell.row + span.rows <= page->rows &&
        !AreGridSlotsMarked(occupied, gesture.cell, span);
    SetCursor(LoadCursorW(nullptr, gesture.valid ? IDC_SIZENWSE : IDC_NO));
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool DesktopApp::HandleLargeIconPointerUp()
{
    if (!largeIconGesture_) return false;
    if (largeIconGesture_->creating) return true;
    const auto gesture = *largeIconGesture_;
    if (gesture.valid && CanEditLargeIcons()) SetLargeIconConfig(FindItemIndexByKey(gesture.key), gesture.config);
    CancelLargeIconGesture();
    return true;
}

void DesktopApp::DrawLargeIconInteractionOverlay(ID2D1RenderTarget* context)
{
    if (largeIconGesture_)
    {
        const auto& gesture = *largeIconGesture_;
        DesktopWidget geometry;
        geometry.gridCell = gesture.cell;
        geometry.bounds = GetGridRect(gridPages_, gesture.cell, {gesture.config.columns, gesture.config.rows});
        const auto rect = GetStandaloneWidgetFrameRect(geometry);
        const UINT rgb = gesture.valid ? 0x68b5ff : 0xf16d70;
        DrawD2DRoundedRectangle(context, rect, static_cast<float>(gesture.config.radius),
            D2D1::ColorF(rgb, .2f), D2D1::ColorF(rgb, .95f), 2);
        if (gesture.creating)
        {
            ComPtr<IDWriteTextFormat> format;
            ComPtr<ID2D1SolidColorBrush> brush;
            dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 14.f, L"", &format);
            context->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &brush);
            const auto* page = FindGridPage(gridPages_, gesture.cell.pageId);
            if (page && format && brush)
            {
                RECT hint = page->bounds; hint.left += 12; hint.right -= 12; hint.top = std::max(hint.top, hint.bottom - 66);
                DrawD2DRoundedRectangle(context, hint, 6, D2D1::ColorF(0x20242b, .95f), D2D1::ColorF(0xffffff, .2f));
                const std::wstring message = _LW("largeIcon.placementHint");
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                context->DrawText(message.c_str(), static_cast<UINT32>(message.size()), format.Get(), ToD2DRect(hint), brush.Get());
            }
        }
        return;
    }
    if (!CanEditLargeIcons() || dragSession_.IsActive() || HasActiveContextMenuSession()) return;
    for (const auto& item : items_)
    {
        if (!item.largeIcon || IsItemInAnyWidget(item) || IsRectEmptyRect(item.bounds)) continue;
        auto rect = GetLargeIconFrameRect(item);
        if (!item.selected && !PtInRect(&rect, lastMousePoint_)) continue;
        DesktopWidget geometry;
        geometry.bounds = item.bounds; geometry.gridCell = item.gridCell; geometry.showTitle = false;
        if (const auto* page = FindGridPage(gridPages_, geometry.gridCell.pageId)) geometry.cellScale = GetGridPageCuScale(*page);
        rect = GetStandaloneWidgetResizeHandleRect(geometry);
        DrawD2DRoundedRectangle(context, rect, 3, D2D1::ColorF(0xffffff, .75f), D2D1::ColorF(0x202020, .4f));
    }
}

void DesktopApp::UpdateLargeIconHover()
{
    if (!CanEditLargeIcons())
    {
        CancelLargeIconGesture();
        largeIconEdit_.preview.reset();
    }
    const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    bool moving = false;
    for (auto it = largeIconRuntime_.begin(); it != largeIconRuntime_.end();)
    {
        const auto index = FindItemIndexByKey(it->first);
        if (index >= items_.size() || !items_[index].largeIcon || IsItemInAnyWidget(items_[index]))
        {
            if (largeIconAssets_) largeIconAssets_->Cancel(it->first);
            if (it->second.asset) EraseD2DIconCacheForBitmap(it->second.asset->bitmap);
            it = largeIconRuntime_.erase(it); continue;
        }
        const auto& item = items_[index];
        const auto& config = EffectiveLargeIconConfig(item);
        auto& state = it->second;
        const auto frame = GetLargeIconFrameRect(item);
        const bool visible = !desktopIconsHidden_ && !IsRectEmptyRect(frame) && FindGridPage(gridPages_, item.gridCell.pageId);
        const bool interactive = !dragSession_.IsActive() && !marqueeActive_ && !largeIconGesture_ &&
            widgetAction_ == WidgetAction::None && !HasActiveContextMenuSession() && !IsPointOccludedByOpenPopup(lastMousePoint_);
        const bool hover = PtInRect(&frame, lastMousePoint_) || (keyboardNavVisualFocus_ && item.selected);
        moving = state.motion.Advance(now, hover, visible && interactive, snowdesktop::animation::RuntimeAnimationsEnabled(),
            snowdesktop::animation::RuntimeDurationScale(), config) || moving;
        ++it;
    }
    if (moving && !largeIconAnimationToken_)
        largeIconAnimationToken_ = uiAnimationScheduler_.StartAnimation(snowdesktop::UiAnimationSurface::Desktop,
            [this](double) {
                // Clear before recomputing so the update can schedule the next
                // finite transition; no animation survives a settled frame.
                largeIconAnimationToken_ = 0;
                UpdateLargeIconHover();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return false;
            });
}

void DesktopApp::TriggerLargeIconLaunch(size_t index)
{
    if (index >= items_.size() || !items_[index].largeIcon || items_[index].largeIcon->launch == 0 ||
        !snowdesktop::animation::RuntimeAnimationsEnabled()) return;
    largeIconRuntime_[items_[index].layoutKey].motion.Launch(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(),
        true, *items_[index].largeIcon);
    UpdateLargeIconHover();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
