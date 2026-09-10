#include "app.h"
#include "popup_opacity_scope.h"
#include "quick_navigation_theme.h"
#include "../item_render_layer_rules.h"
#include <d2d1effects.h>

// Collection-popup rendering.

void DesktopApp::DrawCollectionPopup(
    ID2D1DeviceContext* ctx,
    bool applyAnimation)
{
    const DesktopWidget* openWidget = GetOpenPopupWidget();
    if (!ctx || !openWidget) return;

    const DesktopWidget& widget = *openWidget;
    const bool fan = UsesCollectionPopupFan(widget);
    const auto popupMetrics =
        GetCollectionPopupLayoutMetrics(widget);
    const QuickNavTheme& popupTheme = collectionPopupLightTheme_
        ? kQuickNavLight : kQuickNavDark;
    const auto popupTextColor = [&](float alpha) {
        D2D1_COLOR_F color = popupTheme.popupTitle;
        color.a *= std::clamp(alpha, 0.0f, 1.0f);
        return color;
    };
    popupRect_ = GetCollectionPopupRect(widget);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widget, popupRect_));

    const auto animation =
        popupAnimation_.GetVisual();
    if (applyAnimation && !animation.visible)
        return;
    if (applyAnimation && popupAnimationOverlay_.active)
        return;
    PopupOpacityScope opacity(ctx, applyAnimation, animation.opacity);

    D2D1_MATRIX_3X2_F previousTransform{};
    const bool animationApplied =
        applyAnimation && !fan &&
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

    const auto& popupKeys = widget.itemKeys;
    if (!fan)
    {
        PersonalizationSettings popupBackgroundAppearance =
            collectionPopupAppearance_;
        popupBackgroundAppearance.widgetEdgeHighlightEnabled = false;
        DrawWidgetPanelBackground(
            ctx, popupRect_, 18.0f * popupMetrics.scale,
            D2D1::ColorF(
                collectionPopupAppearance_.widgetBgR,
                collectionPopupAppearance_.widgetBgG,
                collectionPopupAppearance_.widgetBgB,
                std::clamp(
                    collectionPopupAppearance_.widgetAlpha,
                    0.0f, 1.0f)),
            D2D1::ColorF(
                collectionPopupAppearance_.widgetBorderR,
                collectionPopupAppearance_.widgetBorderG,
                collectionPopupAppearance_.widgetBorderB,
                std::clamp(
                    collectionPopupAppearance_.widgetBorderAlpha,
                    0.0f, 1.0f)),
            false,
            std::clamp(collectionPopupAppearance_.widgetBorderWidth,
                kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth) *
                popupMetrics.scale,
            &popupBackgroundAppearance, false, 0, popupMetrics.scale);

        const auto headerBounds =
            snowdesktop::collection_popup_layout::
                ResolveHeaderVerticalBounds(popupMetrics.scale);
        RECT titleRect = snowdesktop::collection_popup_layout::
            ResolveTitleRect(popupRect_, popupMetrics.scale);
        if (dockFolderPopupOpen_)
            titleRect.right =
                GetDockFolderPopupSortButtonRect(
                    popupRect_).left -
                snowdesktop::collection_popup_layout::
                    ScaleDimension(10, popupMetrics.scale);
        std::wstring title = ShouldUseDemoCollectionIdentity(&widget)
            ? GetDemoCollectionCategoryTitle(widget)
            : (widget.title.empty()
                ? _LW("app.overlay.collection_default") : widget.title);
        DrawD2DTextEllipsis(
            ctx, title, titleRect,
            itemTextFormat_.Get(),
            popupTextColor(1.0f),
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
                ctx, sortRect, 8.0f * popupMetrics.scale,
                hovered
                    ? popupTextColor(
                        collectionPopupLightTheme_ ? 0.10f : 0.16f)
                    : popupTextColor(
                        collectionPopupLightTheme_ ? 0.05f : 0.08f),
                popupTextColor(
                    hovered
                        ? (collectionPopupLightTheme_ ? 0.24f : 0.34f)
                        : (collectionPopupLightTheme_ ? 0.12f : 0.20f)),
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
            RECT sortTextRect = sortRect;
            OffsetRect(
                &sortTextRect, 0,
                headerBounds.sortLabelOffsetY);
            DrawD2DTextEllipsis(
                ctx, sortLabel, sortTextRect,
                itemTextFormat_.Get(),
                popupTextColor(0.88f),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

    }

    RECT content = GetCollectionPopupContentRect(popupRect_);
    DesktopWidget popupListStyle;
    popupListStyle.type = widget.type;
    popupListStyle.bounds = popupRect_;
    popupListStyle.cellScale = popupMetrics.scale;
    popupListStyle.listMode = UsesCollectionPopupList(widget);
    popupListStyle.showDetails = widget.showDetails;
    popupListStyle.detailShowModified =
        widget.detailShowModified;
    popupListStyle.detailShowType =
        widget.detailShowType;
    popupListStyle.detailShowSize =
        widget.detailShowSize;
    popupListStyle.detailModifiedPosition =
        widget.detailModifiedPosition;
    popupListStyle.detailTypePosition =
        widget.detailTypePosition;
    popupListStyle.detailSizePosition =
        widget.detailSizePosition;
    popupListStyle.contentSortColumn =
        widget.contentSortColumn;
    popupListStyle.contentSortAscending =
        widget.contentSortAscending;
    Collection popupListRenderer(
        &popupListStyle, this);
    popupListRenderer.SetHostedFrame(&popupRect_);
    if (UsesCollectionPopupList(widget) && !fan)
    {
        popupListRenderer.DrawDetailsHeader(
            ctx, content, collectionPopupLightTheme_);
    }
    ctx->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const size_t popupItemCount = GetPopupItemCount(widget);
    const size_t fanCapacity = GetCollectionPopupFanVisibleCount(popupRect_);
    const double fanOffset = fan ? GetCollectionPopupFanScrollOffset(popupRect_) : 0;
    const auto fanRange = snowdesktop::collection_popup_layout::FanVisibleRange(
        fanOffset, fanCapacity, popupItemCount);
    const size_t firstItem = fan ? fanRange.first : 0;
    const size_t endItem = fan ? fanRange.end : popupItemCount;
    ComPtr<ID2D1DeviceContext> iconRecorder;
    ComPtr<ID2D1Effect> iconShadow;
    if (fan && d2dDevice_ && SUCCEEDED(d2dDevice_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &iconRecorder)) &&
        SUCCEEDED(ctx->CreateEffect(CLSID_D2D1Shadow, &iconShadow)))
    {
        iconRecorder->SetDpi(96, 96);
        iconShadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 2.5f * popupMetrics.scale);
        iconShadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1_VECTOR_4F{0, 0, 0, 0.65f});
        iconShadow->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_BALANCED);
    }
    const auto drawFanSlot = [&](size_t index, bool selected, bool cut, const auto& drawIcon) {
        namespace layout = snowdesktop::collection_popup_layout;
        const auto pose = GetCollectionPopupFanItem(popupRect_, index);
        const double slot = index == popupItemCount ? static_cast<double>(fanCapacity) :
            static_cast<double>(index) - fanOffset;
        const float visibility = index == popupItemCount ? 1.0f : layout::FanItemOpacity(slot, fanCapacity);
        if (visibility <= 0.001f) return;
        const bool hovered = popupAnimation_.IsInteractive() &&
            layout::FanItemContains(pose, lastMousePoint_);
        float progress = 1.0f;
        if (applyAnimation && animation.progress < 1.0f &&
            snowdesktop::animation::RuntimePopupEffect() != snowdesktop::animation::Fade)
            progress = layout::FanRevealProgress(animation.progress,
                static_cast<float>(slot / static_cast<double>(std::max<size_t>(1, fanCapacity))));
        const D2D1_POINT_2F center = D2D1::Point2F(pose.center.x, pose.center.y);
        const float originX = popupHasAnchor_ ? static_cast<float>(popupAnchorPoint_.x) : center.x;
        const float originY = popupHasAnchor_ ? static_cast<float>(popupAnchorPoint_.y) :
            static_cast<float>(CollectionPopupFanRootAbove() ? popupRect_.top : popupRect_.bottom);
        const auto unfolding = layout::FanUnfoldCenter({originX, originY}, pose.center, progress);
        D2D1_MATRIX_3X2_F transform{};
        ctx->GetTransform(&transform);
        ctx->SetTransform(D2D1::Matrix3x2F::Rotation(pose.angle * progress, center) *
            D2D1::Matrix3x2F::Translation(unfolding.x - center.x, unfolding.y - center.y) * transform);
        PopupOpacityScope slotOpacity(ctx, true, progress * visibility * (cut ? 0.5f : 1.0f));

        // Only the filename has a small backing. The arc and its gaps stay transparent.
        RECT shadow = pose.label;
        OffsetRect(&shadow, 0, layout::ScaleDimension(2, popupMetrics.scale));
        DrawD2DRoundedRectangle(ctx, shadow, 5.0f * popupMetrics.scale,
            D2D1::ColorF(0, 0, 0, 0.16f), D2D1::ColorF(0, 0, 0, 0), 0);
        const auto labelFill = selected ? D2D1::ColorF(0.16f, 0.43f, 0.82f, 0.95f) :
            (collectionPopupLightTheme_ ? D2D1::ColorF(0.96f, 0.96f, 0.96f, hovered ? 0.96f : 0.86f) :
                D2D1::ColorF(0.12f, 0.12f, 0.14f, hovered ? 0.88f : 0.72f));
        DrawD2DRoundedRectangle(ctx, pose.label, 5.0f * popupMetrics.scale,
            labelFill, D2D1::ColorF(0, 0, 0, 0), 0);
        if (hovered || selected)
            DrawD2DRoundedRectangle(ctx, pose.icon, 7.0f * popupMetrics.scale,
                D2D1::ColorF(1, 1, 1, selected ? 0.18f : 0.10f),
                D2D1::ColorF(1, 1, 1, 0.35f), popupMetrics.scale);
        bool iconDrawn = false;
        if (iconRecorder && iconShadow)
        {
            // Record the real silhouette, including placeholder/demo icons and
            // shortcut badges. Shadow and icon then share the same fan transform.
            ComPtr<ID2D1CommandList> commands;
            if (SUCCEEDED(iconRecorder->CreateCommandList(&commands)))
            {
                iconRecorder->SetTarget(commands.Get());
                iconRecorder->SetTransform(D2D1::Matrix3x2F::Identity());
                iconRecorder->BeginDraw();
                drawIcon(iconRecorder.Get(), RECT{0, 0,
                    pose.icon.right - pose.icon.left, pose.icon.bottom - pose.icon.top});
                const HRESULT recorded = iconRecorder->EndDraw();
                iconRecorder->SetTarget(nullptr);
                if (SUCCEEDED(recorded) && SUCCEEDED(commands->Close()))
                {
                    iconShadow->SetInput(0, commands.Get());
                    ComPtr<ID2D1Image> shadowImage;
                    iconShadow->GetOutput(&shadowImage);
                    D2D1_RECT_F shadowBounds{};
                    // DrawImage maps the image's bounding-box corner to its
                    // offset. Preserve the negative blur extent around origin.
                    if (SUCCEEDED(ctx->GetImageLocalBounds(shadowImage.Get(), &shadowBounds)))
                    {
                        const auto shadowOffset = D2D1::Point2F(
                            pose.icon.left + shadowBounds.left,
                            pose.icon.top + shadowBounds.top + 3.0f * popupMetrics.scale);
                        ctx->DrawImage(shadowImage.Get(), &shadowOffset, &shadowBounds);
                    }
                    const auto iconBounds = D2D1::RectF(0, 0,
                        static_cast<float>(pose.icon.right - pose.icon.left),
                        static_cast<float>(pose.icon.bottom - pose.icon.top));
                    const auto iconOffset = D2D1::Point2F(
                        static_cast<float>(pose.icon.left), static_cast<float>(pose.icon.top));
                    ctx->DrawImage(commands.Get(), &iconOffset, &iconBounds);
                    iconDrawn = true;
                }
            }
        }
        if (!iconDrawn) drawIcon(ctx, pose.icon);
        RECT textRect = pose.label;
        InflateRect(&textRect, -layout::ScaleDimension(8, popupMetrics.scale), 0);
        DrawD2DTextEllipsis(ctx, GetCollectionPopupFanLabel(index), textRect,
            itemTextFormat_.Get(), selected ? D2D1::ColorF(D2D1::ColorF::White) : popupTextColor(1.0f),
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ctx->SetTransform(transform);
    };
    const auto drawFanItem = [&](size_t index, const auto& item, const DesktopItem* desktopItem) {
        drawFanSlot(index, item.selected, item.isCut, [&](ID2D1DeviceContext* iconContext, const RECT& iconRect) {
            const bool demo = desktopItem && ShouldUseDemoCollectionIdentity(&widget);
            if (demo)
                DrawDemoCollectionIdentityIcon(iconContext, widget,
                    desktopItem->layoutKey.empty() ? desktopItem->parsingName : desktopItem->layoutKey,
                    iconRect, 1.0f);
            else if (auto* icon = GetOrCreateD2DBitmap(item.iconBitmap,
                ShouldBeautifyIconBitmap(item.iconIsMediaThumbnail)))
                DrawIconBitmap(iconContext, icon, iconRect);
            else
                DrawPlaceholderIcon(iconContext, item.sysIconIndex, iconRect, 1.0f);
            if (!demo && ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut))
                DrawShortcutArrowOverlay(iconContext, iconRect, 1.0f);
        });
    };
    for (size_t i = firstItem; i < endItem; ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;

        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            FolderEntry& entry = dockFolderPopupWidget_.folderEntries[i];
            if (fan)
            {
                drawFanItem(i, entry, nullptr);
                continue;
            }
            if (UsesCollectionPopupList(widget))
            {
                popupListRenderer.DrawListItem(
                    ctx, itemRect,
                    entry.iconBitmap,
                    entry.sysIconIndex,
                    entry.name,
                    entry.selected,
                    entry.iconIsMediaThumbnail,
                    {}, nullptr,
                    {
                        entry.typeName,
                        entry.lastWriteTime,
                        entry.fileSize,
                        entry.isDirectory,
                    },
                    collectionPopupLightTheme_);
                continue;
            }
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
                collectionPopupLightTheme_,
                titleLayers.drawWithItem);
        }
        else
        {
            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
            if (itemIndex == static_cast<size_t>(-1)) continue;
            if (fan)
            {
                const auto& item = items_[itemIndex];
                drawFanItem(i, item, &item);
                continue;
            }
            if (UsesCollectionPopupList(widget))
            {
                DesktopItem& item = items_[itemIndex];
                const bool useDemoIdentity =
                    ShouldUseDemoCollectionIdentity(
                        &widget);
                const std::wstring_view demoIdentity =
                    !useDemoIdentity
                    ? std::wstring_view{}
                    : (item.layoutKey.empty()
                        ? std::wstring_view(
                            item.parsingName)
                        : std::wstring_view(
                            item.layoutKey));
                popupListRenderer.DrawListItem(
                    ctx, itemRect,
                    item.iconBitmap,
                    item.sysIconIndex,
                    item.name,
                    item.selected,
                    item.iconIsMediaThumbnail,
                    demoIdentity,
                    &widget,
                    {
                        item.typeName,
                        item.modifiedTime,
                        item.fileSize,
                        false,
                    },
                    collectionPopupLightTheme_);
                continue;
            }
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
                collectionPopupLightTheme_,
                titleLayers.drawWithItem, false,
                widget.type == DesktopWidgetType::Collection
                    ? &widget : nullptr);
        }
    }
    for (size_t i = 0; i < popupItemCount; ++i)
    {
        if (UsesCollectionPopupList(widget) || fan) break;
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
            icon.DrawTitle(
                ctx, itemRect, true, 1.0f,
                collectionPopupLightTheme_);
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
                ctx, itemRect, true, 1.0f,
                collectionPopupLightTheme_,
                widget.type == DesktopWidgetType::Collection
                    ? &widget : nullptr);
        }
    }
    if (fan)
    {
        drawFanSlot(popupItemCount, popupFanActionFocused_, false, [&](ID2D1DeviceContext* iconContext, const RECT& iconRect) {
            const float x = (iconRect.left + iconRect.right) * 0.5f;
            const float y = (iconRect.top + iconRect.bottom) * 0.5f;
            const float radius = (iconRect.right - iconRect.left) * 0.38f;
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(iconContext->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.17f, 0.65f), &brush)))
            {
                const auto circle = D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
                iconContext->FillEllipse(circle, brush.Get());
                brush->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
                iconContext->DrawEllipse(circle, brush.Get(), 2.0f * popupMetrics.scale);
                const float arm = radius * 0.38f;
                const float stroke = 2.4f * popupMetrics.scale;
                iconContext->DrawLine(D2D1::Point2F(x - arm, y + arm),
                    D2D1::Point2F(x + arm, y - arm), brush.Get(), stroke);
                iconContext->DrawLine(D2D1::Point2F(x - arm, y - arm),
                    D2D1::Point2F(x + arm, y - arm), brush.Get(), stroke);
                iconContext->DrawLine(D2D1::Point2F(x + arm, y - arm),
                    D2D1::Point2F(x + arm, y + arm), brush.Get(), stroke);
            }
        });
    }
    ctx->PopAxisAlignedClip();

    if (widget.type == DesktopWidgetType::FolderMapping &&
        popupItemCount == 0)
    {
        const std::wstring status = dockFolderPopupAvailable_
            ? _LW("widget.folder_mapping.empty")
            : _LW("widget.folder_mapping.unavailable");
        DrawD2DTextEllipsis(ctx, status, content, itemTextFormat_.Get(),
            popupTextColor(0.68f),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }

    // Scrollbar — same style as widget content areas
    int visibleHeight = std::max(1, (int)(content.bottom - content.top));
    int contentHeight = visibleHeight +
        GetCollectionPopupMaxScrollOffset(
            widget, popupRect_);
    bool popupHovered =
        popupAnimation_.IsInteractive() &&
        PtInRect(&popupRect_, lastMousePoint_);
    popupHovered = popupHovered || popupScrollbarDragging_;
    if (!fan) DrawScrollbarAt(
        ctx, content, contentHeight, visibleHeight,
        popupScrollOffset_, popupHovered,
        collectionPopupLightTheme_);

    if (!fan) (void)DrawWidgetPanelEdgeHighlight(
        ctx, popupRect_, 18.0f * popupMetrics.scale,
        D2D1::ColorF(
            collectionPopupAppearance_.widgetBgR,
            collectionPopupAppearance_.widgetBgG,
            collectionPopupAppearance_.widgetBgB,
            std::clamp(
                collectionPopupAppearance_.widgetAlpha,
                0.0f, 1.0f)),
        &collectionPopupAppearance_, popupMetrics.scale);

    if (animationApplied)
        ctx->SetTransform(previousTransform);
}
