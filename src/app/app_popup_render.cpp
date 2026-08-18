#include "app.h"
#include "../item_render_layer_rules.h"

// Collection-popup rendering.

void DesktopApp::DrawCollectionPopup(
    ID2D1DeviceContext* ctx,
    bool applyAnimation)
{
    const DesktopWidget* openWidget = GetOpenPopupWidget();
    if (!ctx || !openWidget) return;

    const DesktopWidget& widget = *openWidget;
    popupRect_ = GetCollectionPopupRect(widget);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widget, popupRect_));

    const auto animation =
        popupAnimation_.GetVisual();
    if (applyAnimation && !animation.visible)
        return;
    if (applyAnimation && popupAnimationOverlay_.active)
        return;

    D2D1_MATRIX_3X2_F previousTransform{};
    const bool animationApplied =
        applyAnimation &&
        animation.progress < 1.0f;
    if (animationApplied)
    {
        ctx->GetTransform(&previousTransform);
        D2D1_POINT_2F animationOrigin =
            D2D1::Point2F(
                static_cast<float>(
                    popupRect_.left + popupRect_.right) *
                    0.5f,
                static_cast<float>(
                    popupRect_.top + popupRect_.bottom) *
                    0.5f);
        if (popupHasAnchor_)
        {
            animationOrigin.x = std::clamp(
                static_cast<float>(popupAnchorPoint_.x),
                static_cast<float>(popupRect_.left),
                static_cast<float>(popupRect_.right));
            animationOrigin.y = std::clamp(
                static_cast<float>(popupAnchorPoint_.y),
                static_cast<float>(popupRect_.top),
                static_cast<float>(popupRect_.bottom));
        }
        ctx->SetTransform(
            D2D1::Matrix3x2F::Scale(
                animation.scale,
                animation.scale,
                animationOrigin) *
            previousTransform);

        if (!IsRectEmptyRect(
                popupAnimationCacheRect_) &&
            popupAnimationRenderCache_.DrawAt(
                ctx,
                D2D1::Point2F(
                    static_cast<float>(
                        popupAnimationCacheRect_.left),
                    static_cast<float>(
                        popupAnimationCacheRect_.top)),
                D2D1_INTERPOLATION_MODE_LINEAR))
        {
            ctx->SetTransform(previousTransform);
            return;
        }
    }

    std::vector<std::wstring> popupKeys =
        GetPopupItemKeys(widget);
    DrawD2DRoundedRectangle(ctx, popupRect_, 18.0f,
        D2D1::ColorF(0.08f, 0.10f, 0.13f, 1.0f),
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f), 1.4f);

    RECT titleRect = MakeRect(popupRect_.left + 22, popupRect_.top + 18,
        popupRect_.right - 22, popupRect_.top + 44);
    if (dockFolderPopupOpen_)
        titleRect.right =
            GetDockFolderPopupSortButtonRect(
                popupRect_).left - 10;
    std::wstring title = ShouldUseDemoCollectionIdentity(&widget)
        ? GetDemoCollectionCategoryTitle(widget)
        : (widget.title.empty()
            ? _LW("app.overlay.collection_default") : widget.title);
    DrawD2DTextEllipsis(
        ctx, title, titleRect,
        itemTextFormat_.Get(),
        D2D1::ColorF(
            1.0f, 1.0f, 1.0f, 1.0f),
        DWRITE_TEXT_ALIGNMENT_LEADING,
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    if (dockFolderPopupOpen_)
    {
        const RECT sortRect =
            GetDockFolderPopupSortButtonRect(
                popupRect_);
        const bool hovered =
            popupAnimation_.IsInteractive() &&
            PtInRect(
                &sortRect,
                lastMousePoint_) != FALSE;
        DrawD2DRoundedRectangle(
            ctx, sortRect, 8.0f,
            hovered
                ? D2D1::ColorF(
                    1.0f, 1.0f, 1.0f, 0.16f)
                : D2D1::ColorF(
                    1.0f, 1.0f, 1.0f, 0.08f),
            D2D1::ColorF(
                1.0f, 1.0f, 1.0f,
                hovered ? 0.34f : 0.20f),
            1.0f);

        std::wstring sortLabel =
            _LW("app.menu.sort_by");
        switch (
            snowdesktop::folder_sort_rules::
                NormalizeMode(
                    widget.folderSortMode))
        {
        case snowdesktop::folder_sort_rules::kName:
            sortLabel =
                _LW("app.menu.sort_name");
            break;
        case snowdesktop::folder_sort_rules::kType:
            sortLabel =
                _LW("app.menu.sort_type");
            break;
        case snowdesktop::folder_sort_rules::kModified:
            sortLabel =
                _LW("app.interact.sort_date");
            break;
        default:
            break;
        }
        if (widget.folderSortMode !=
            snowdesktop::folder_sort_rules::kManual)
        {
            sortLabel +=
                widget.folderSortAscending
                    ? L" ↑" : L" ↓";
        }
        DrawD2DTextEllipsis(
            ctx, sortLabel, sortRect,
            itemTextFormat_.Get(),
            D2D1::ColorF(
                1.0f, 1.0f, 1.0f, 0.88f),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    RECT content = GetCollectionPopupContentRect(popupRect_);
    ctx->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const size_t popupItemCount = GetPopupItemCount(widget);
    for (size_t i = 0; i < popupItemCount; ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;

        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            FolderEntry& entry = dockFolderPopupWidget_.folderEntries[i];
            const bool hovered =
                popupAnimation_.IsInteractive() &&
                !entry.selected &&
                PtInRect(&itemRect, lastMousePoint_);
            FolderEntryIcon icon(
                &entry, dockFolderPopupContainer_.get(), this);
            const auto titleLayers =
                snowdesktop::item_render_layer_rules::
                    ResolveTitleLayerPlan(entry.selected);
            icon.Draw(ctx, itemRect,
                entry.selected ? 2 : (hovered ? 1 : 0),
                false, titleLayers.drawWithItem);
        }
        else
        {
            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
            if (itemIndex == static_cast<size_t>(-1)) continue;
            bool hovered =
                popupAnimation_.IsInteractive() &&
                !items_[itemIndex].selected &&
                PtInRect(&itemRect, lastMousePoint_);
            DesktopIcon icon(&items_[itemIndex], nullptr, this);
            const auto titleLayers =
                snowdesktop::item_render_layer_rules::
                    ResolveTitleLayerPlan(
                        items_[itemIndex].selected);
            icon.Draw(ctx, itemRect,
                items_[itemIndex].selected ? 2 : (hovered ? 1 : 0),
                false, titleLayers.drawWithItem, false,
                widget.type == DesktopWidgetType::Collection
                    ? &widget : nullptr);
        }
    }
    for (size_t i = 0; i < popupItemCount; ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
        if (itemRect.bottom <= content.top ||
            itemRect.top >= content.bottom)
            continue;
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            FolderEntry& entry =
                dockFolderPopupWidget_.folderEntries[i];
            if (!entry.selected) continue;
            FolderEntryIcon icon(
                &entry, dockFolderPopupContainer_.get(), this);
            icon.DrawTitle(ctx, itemRect, true);
        }
        else
        {
            const size_t itemIndex =
                FindItemIndexByKey(popupKeys[i]);
            if (itemIndex == static_cast<size_t>(-1) ||
                !items_[itemIndex].selected)
                continue;
            DesktopIcon icon(&items_[itemIndex], nullptr, this);
            icon.DrawTitle(
                ctx, itemRect, true, 1.0f, false,
                widget.type == DesktopWidgetType::Collection
                    ? &widget : nullptr);
        }
    }
    ctx->PopAxisAlignedClip();

    if (widget.type == DesktopWidgetType::FolderMapping &&
        popupItemCount == 0)
    {
        const std::wstring status = dockFolderPopupAvailable_
            ? _LW("widget.folder_mapping.empty")
            : _LW("widget.folder_mapping.unavailable");
        DrawD2DTextEllipsis(ctx, status, content, itemTextFormat_.Get(),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.68f),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }

    // Scrollbar — same style as widget content areas
    const int cellH = GetCollectionPopupCellHeight();
    int columns = std::max(1, GetCollectionPopupColumnCount(popupRect_));
    int rows = (static_cast<int>(popupItemCount) + columns - 1) / columns;
    int contentHeight = rows * cellH + std::max(0, rows - 1) * kCollectionPopupGapY;
    int visibleHeight = std::max(1, (int)(content.bottom - content.top));
    bool popupHovered =
        popupAnimation_.IsInteractive() &&
        PtInRect(&popupRect_, lastMousePoint_);
    popupHovered = popupHovered || popupScrollbarDragging_;
    DrawScrollbarAt(ctx, content, contentHeight, visibleHeight, popupScrollOffset_, popupHovered, IsLightContentTheme());

    if (animationApplied)
        ctx->SetTransform(previousTransform);
}
