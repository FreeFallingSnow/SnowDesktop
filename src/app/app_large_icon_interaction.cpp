#include "app.h"
#include "../animation_settings.h"
#include "../widgets/widget_chrome_rules.h"
#include "../large_icon_render_rules.h"

DesktopApp::LargeIconMenuScope::LargeIconMenuScope(DesktopApp& app, std::wstring key)
    : app_(app), previousKey_(std::move(app.largeIconMenuKey_))
{
    app_.largeIconMenuKey_ = std::move(key);
    app_.UpdateLargeIconHover();
    if (app_.hwnd_) InvalidateRect(app_.hwnd_, nullptr, FALSE);
}

DesktopApp::LargeIconMenuScope::~LargeIconMenuScope()
{
    app_.largeIconMenuKey_ = std::move(previousKey_);
    // Menu windows consume pointer movement. Resume from the actual pointer,
    // not the last desktop point captured before the menu opened.
    POINT point{};
    if (app_.hwnd_ && GetCursorPos(&point) && ScreenToClient(app_.hwnd_, &point))
        app_.lastMousePoint_ = point;
    app_.UpdateLargeIconHover();
    if (app_.hwnd_) InvalidateRect(app_.hwnd_, nullptr, FALSE);
}

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
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    UpdateLargeIconHover();
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
                if (SetLargeIconConfig(index, config)) CancelLargeIconGesture();
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
    InvalidateRect(hwnd_, nullptr, FALSE);
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
        DrawD2DRoundedRectangle(context, rect, static_cast<float>(snowdesktop::large_icon_render_rules::Radius(
            gesture.config, rect.right - rect.left, rect.bottom - rect.top, GetItemLayoutScale(geometry.bounds))),
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
        if (!item.largeIcon || !IsLargeIconVisible(item, lastMousePoint_, desktopIconsHidden_)) continue;
        auto rect = GetLargeIconFrameRect(item);
        if (!snowdesktop::widget_chrome_rules::ShowsResizeHandle(false, true, PtInRect(&rect, lastMousePoint_) != FALSE)) continue;
        DesktopWidget geometry;
        geometry.bounds = item.bounds; geometry.gridCell = item.gridCell; geometry.showTitle = false;
        if (const auto* page = FindGridPage(gridPages_, geometry.gridCell.pageId)) geometry.cellScale = GetGridPageCuScale(*page);
        rect = GetStandaloneWidgetResizeHandleRect(geometry);
        const float barHeight = CurrentPersonalization().barHeight;
        const int dot = ScaleWidgetCu(barHeight * .333f, geometry.cellScale);
        const int cx = rect.left + (rect.right - rect.left) / 2;
        const int cy = rect.top + (rect.bottom - rect.top) / 2;
        const RECT dotRect{cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2};
        const auto fill = item.selected ? D2D1::ColorF(.39f, .66f, 1.f, .62f) :
            (IsLightContentTheme() ? D2D1::ColorF(.06f, .08f, .12f, .34f) : D2D1::ColorF(1.f, 1.f, 1.f, .34f));
        const auto stroke = IsLightContentTheme() ? D2D1::ColorF(.06f, .08f, .12f, .5f) : D2D1::ColorF(1.f, 1.f, 1.f, .5f);
        DrawD2DRoundedRectangle(context, dotRect,
            static_cast<float>(ScaleWidgetCu(4.f * barHeight / 24.f, geometry.cellScale)), fill, stroke);
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
            desktopBackdropCompositor_.RemovePanel(it->second.backdropFrame);
            if (largeIconAssets_) largeIconAssets_->Cancel(it->first);
            if (it->second.asset) EraseD2DIconCacheForBitmap(it->second.asset->bitmap);
            it = largeIconRuntime_.erase(it); continue;
        }
        const auto& item = items_[index];
        const auto& config = EffectiveLargeIconConfig(item);
        auto& state = it->second;
        const auto frame = GetLargeIconFrameRect(item);
        const bool visible = IsLargeIconVisible(item, lastMousePoint_, desktopIconsHidden_);
        if (state.visible != visible)
        {
            state.visible = visible;
            if (!visible) { desktopBackdropCompositor_.RemovePanel(state.backdropFrame); state.backdropFrame = {}; }
            InvalidateDragStaticScene();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        const bool menuOpen = !largeIconMenuKey_.empty() || HasActiveContextMenuSession();
        const bool menuTitle = !largeIconMenuKey_.empty() && largeIconMenuKey_ == item.layoutKey &&
            config.effect == 2 && !snowdesktop::IsLargeIconFill(config);
        const bool interactive = !dragSession_.IsActive() && !marqueeActive_ && !largeIconGesture_ &&
            widgetAction_ == WidgetAction::None;
        const bool pointerInteractive = !menuOpen && !IsPointOccludedByOpenPopup(lastMousePoint_);
        const bool hover = menuTitle || PtInRect(&frame, lastMousePoint_) || (keyboardNavVisualFocus_ && item.selected);
        moving = state.motion.Advance(now, hover, visible && interactive && (pointerInteractive || menuTitle),
            snowdesktop::animation::RuntimeAnimationsEnabled(),
            snowdesktop::animation::RuntimeDurationScale(), config, menuTitle) || moving;
        const float width = static_cast<float>(std::max<LONG>(1, frame.right - frame.left));
        const float height = static_cast<float>(std::max<LONG>(1, frame.bottom - frame.top));
        moving = state.motion.AdvanceTilt(now, 2 * (lastMousePoint_.x - frame.left) / width - 1,
            2 * (lastMousePoint_.y - frame.top) / height - 1, PtInRect(&frame, lastMousePoint_) != FALSE,
            visible && interactive && pointerInteractive, snowdesktop::animation::RuntimeAnimationsEnabled(),
            snowdesktop::animation::RuntimeDurationScale(), config) || moving;
        ++it;
    }
    if (!moving && largeIconAnimationToken_)
    {
        uiAnimationScheduler_.Cancel(largeIconAnimationToken_);
        largeIconAnimationToken_ = 0;
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
