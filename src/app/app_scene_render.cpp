#include "app.h"
#include "grid_geometry.h"
#include "../item_render_layer_rules.h"
#include "../drag_visual_rules.h"
#include "../widget_composition_layer_rules.h"
#include "../widget_visibility_rules.h"
#include "../widgets/collection_group_rules.h"

// Desktop scene composition: static cache, dynamic overlays and frame orchestration.

void DesktopApp::DrawStaticBackground(
    ID2D1DeviceContext* ctx,
    const RECT* updateRect,
    bool hiddenMode)
{
    auto intersectsUpdate =
        [&](RECT bounds, int overdraw = 0) {
        if (!updateRect)
            return true;
        if (overdraw > 0)
            InflateRect(
                &bounds, overdraw, overdraw);
        RECT intersection{};
        return IntersectRect(
            &intersection,
            &bounds,
            updateRect) != FALSE;
    };

    const bool suppressDesktopWidgetTargets = SuppressDesktopWidgetDragTargets();
    const bool popupOccludesPointer =
        IsPointOccludedByOpenPopup(lastMousePoint_);
    const POINT interactionMousePoint = lastMousePoint_;
    if (suppressDesktopWidgetTargets)
        lastMousePoint_ = { LONG_MIN, LONG_MIN };
    // 集合弹窗（含开/关动画期间）遮挡指针时，被遮挡元素不得渲染 hover
    // 反馈（图标高亮、组件 chrome、Dock 放大），但弹窗自身在动态层仍可用
    // lastMousePoint_ 绘制 item hover。
    if (popupOccludesPointer)
        lastMousePoint_ = { LONG_MIN, LONG_MIN };

    // Desktop icons
    const bool mouseOverWidget = IsPointOverWidgetChrome(lastMousePoint_);
    if (!hiddenMode)
    {
        struct ForegroundTitle
        {
            DesktopIcon* icon = nullptr;
            RECT bounds{};
        };
        std::vector<ForegroundTitle>
            foregroundTitles;
        const bool desktopMarqueeActive =
            marqueeActive_ &&
            !marqueeDockFolderPopup_ &&
            marqueeWidgetIndex_ >= widgets_.size();
        for (auto& ooItem : items_oo_)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(ooItem.get());
            if (!icon) continue;
            DesktopItem* di = icon->GetDesktopItem();
            if (!di || IsRectEmptyRect(di->bounds)) continue;
            if (dragSession_.IsActive() && !dragSession_.Items().empty() &&
                dragSession_.IsMoveAction() && di->selected)
                continue;

            const bool hovered = !desktopMarqueeActive && !mouseOverWidget &&
                PtInRect(&di->bounds, lastMousePoint_) != FALSE;
            const bool selected = di->selected && !desktopMarqueeActive;
            const auto titleLayers =
                snowdesktop::item_render_layer_rules::
                    ResolveTitleLayerPlan(selected);
            if (titleLayers.drawInForeground)
            {
                foregroundTitles.push_back(
                    { icon, di->bounds });
            }
            if (!intersectsUpdate(di->bounds, 8))
                continue;
            int state = selected ? 2 : (hovered ? 1 : 0);
            icon->Draw(
                ctx, di->bounds, state,
                false,
                titleLayers.drawWithItem);
        }

        // Expanded selected titles may cross into lower grid cells. Draw them
        // only after every desktop icon so later cells cannot cover the text.
        for (const auto& title : foregroundTitles)
        {
            title.icon->DrawTitle(
                ctx, title.bounds, true);
        }
    }

    // Widgets
    for (size_t widgetIndex = 0;
         widgetIndex < widgets_.size();
         ++widgetIndex)
    {
        auto& widgetData = widgets_[widgetIndex];
        const bool hasDesktopBounds =
            !IsRectEmptyRect(widgetData.bounds);
        // Dock-exclusive and grouped collections deliberately keep an empty
        // desktop rectangle while retaining a runtime WidgetContainer for
        // popup interaction. Never let an empty rectangle reach widget
        // drawing: rounded geometry APIs can turn it into a visible 1x1
        // artifact at the desktop origin.
        const RECT widgetFrame =
            GetStandaloneWidgetFrameRect(widgetData);
        const bool popupOpen =
            popupWidgetIndex_ < widgets_.size() &&
            widgetIndex == popupWidgetIndex_;
        const bool interactionPinned =
            !interactionPinnedWidgetId_.empty() &&
            widgetData.id == interactionPinnedWidgetId_;
        const bool interactionRetained =
            popupOpen || interactionPinned ||
            snowdesktop::widget_visibility_rules::
                ShouldRetainForKeyboardNavigation(
                    keyboardNavVisualFocus_,
                    keyboardNavInsideWidget_,
                    keyboardNavWidgetIndex_,
                    widgetIndex);
        const bool interactionVisible =
            snowdesktop::widget_visibility_rules::ShouldRenderWidget(
                widgetData.showOnHoverOnly,
                dragSession_.IsActive(),
                dragDropController_.IsExternalDragActive(),
                widgetAction_ == WidgetAction::Move,
                widgetData.selected,
                HasSelectedFilesInWidget(widgetIndex),
                interactionRetained,
                PtInRect(&widgetFrame, lastMousePoint_) != FALSE);
        const bool desktopSurfaceVisible =
            snowdesktop::widget_visibility_rules::IsDesktopSurfaceVisible(
                hiddenMode, widgetData.keepWhenDesktopHidden,
                hasDesktopBounds, interactionVisible);
        const bool pageUnavailable =
            !widgetData.gridCell.pageId.empty() &&
            FindGridPage(gridPages_, widgetData.gridCell.pageId) == nullptr;
        const bool keepTopologyHiddenPageRuntimeActive =
            snowdesktop::widget_visibility_rules::
                ShouldKeepTopologyHiddenPageRuntimeActive(
                    hiddenMode, desktopSurfaceVisible, pageUnavailable,
                    displayTopologyHiddenPageIds_.contains(
                        widgetData.gridCell.pageId));
        // Runtime visibility is semantic state, not a paint-frequency signal.
        // Synchronize it before dirty-region culling because DirectComposition
        // may retain a visible widget without asking the host to redraw it.
        if (widgetEngine_ &&
            widgetData.type == DesktopWidgetType::LuaScript)
        {
            widgetEngine_->SetWidgetDesktopVisible(
                widgetData.id, desktopSurfaceVisible,
                keepTopologyHiddenPageRuntimeActive);
        }
        const bool isPreviewSource =
            mouseDownWidgetIndex_ < widgets_.size() &&
            &widgetData == &widgets_[mouseDownWidgetIndex_];
        const bool previewSourceHidden =
            snowdesktop::widget_visibility_rules::
                ShouldHideWidgetPreviewSource(
                    widgetAction_ == WidgetAction::Move,
                    widgetAction_ == WidgetAction::Resize,
                    isPreviewSource);
        const bool presentWidgetSurface =
            snowdesktop::widget_composition_layer_rules::
                ShouldPresentWidgetSurface(
                    desktopSurfaceVisible, previewSourceHidden);
        if (HasDesktopWidgetComposition(widgetData.id))
            SetDesktopWidgetCompositionVisible(
                widgetData.id, presentWidgetSurface, widgetFrame);
        if (!desktopSurfaceVisible)
            continue;
        if (!presentWidgetSurface)
            continue;

        // A standalone widget is never painted into the root surface. Create
        // a missing child even when the current root dirty rectangle is
        // elsewhere; existing children redraw only when their own bounds are
        // dirty. Any failure is recovered as one composition transaction.
        if (!HasDesktopWidgetComposition(widgetData.id) ||
            intersectsUpdate(widgetFrame, 2))
        {
            (void)QueueDesktopWidgetComposition(widgetData.id);
        }
    }

    if (suppressDesktopWidgetTargets || popupOccludesPointer)
        lastMousePoint_ = interactionMousePoint;
}

void DesktopApp::DrawDesktopForeground(
    ID2D1DeviceContext* ctx,
    bool hiddenMode)
{
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock ||
            (hiddenMode && !dockSettings_.keepWhenDesktopHidden) ||
            IsDockHostedByPersistentHost(dock))
        {
            continue;
        }
        dock->DrawChrome(ctx, lastMousePoint_);
        dock->DrawContents(ctx);
    }

    DrawDynamicOverlays(ctx, hiddenMode);
    if (desktopIconsHidden_ && showHiddenHint_)
        DrawHiddenHintOverlay(ctx);
    if (showWidgetAddedHint_)
        DrawWidgetAddedHintOverlay(ctx);
}

// ── Dynamic overlays (drag preview, dragged items, marquee, nav) ──

void DesktopApp::DrawDynamicOverlays(
    ID2D1DeviceContext* ctx,
    bool hiddenMode)
{
    auto beginPopupAnimationTransform =
        [&](const RECT& popup,
            D2D1_MATRIX_3X2_F& previousTransform) {
        const auto animation =
            popupAnimation_.GetVisual();
        if (!animation.visible ||
            animation.progress >= 1.0f)
            return false;

        ctx->GetTransform(&previousTransform);
        D2D1_POINT_2F origin =
            D2D1::Point2F(
                static_cast<float>(
                    popup.left + popup.right) * 0.5f,
                static_cast<float>(
                    popup.top + popup.bottom) * 0.5f);
        if (popupHasAnchor_)
        {
            origin.x = std::clamp(
                static_cast<float>(
                    popupAnchorPoint_.x),
                static_cast<float>(popup.left),
                static_cast<float>(popup.right));
            origin.y = std::clamp(
                static_cast<float>(
                    popupAnchorPoint_.y),
                static_cast<float>(popup.top),
                static_cast<float>(popup.bottom));
        }
        ctx->SetTransform(
            D2D1::Matrix3x2F::Scale(
                animation.scale,
                animation.scale,
                origin) *
            previousTransform);
        return true;
    };
    auto endPopupAnimationTransform =
        [&](bool applied,
            const D2D1_MATRIX_3X2_F&
                previousTransform) {
        if (!applied)
            return;
        ctx->SetTransform(previousTransform);
    };

    // Widget drag/resize preview
    if (!renderingFloatingPopup_ &&
        (widgetAction_ == WidgetAction::Move ||
         widgetAction_ == WidgetAction::Resize) &&
        mouseDownWidgetIndex_ < widgets_.size())
    {
        if (widgetAction_ == WidgetAction::Move &&
            widgetCollectionGroupTargetIndex_ <
                widgets_.size() &&
            (widgets_[widgetCollectionGroupTargetIndex_].type ==
                 DesktopWidgetType::CollectionGroup ||
             widgets_[widgetCollectionGroupTargetIndex_].type ==
                 DesktopWidgetType::FileGroup))
        {
            RECT target =
                widgets_[widgetCollectionGroupTargetIndex_].bounds;
            for (const auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<WidgetContainer*>(
                        container.get());
                if (group &&
                    group->GetWidgetData() ==
                        &widgets_[widgetCollectionGroupTargetIndex_])
                {
                    target = group->GetFrameRect();
                    break;
                }
            }
            const float cellScale =
                widgets_[widgetCollectionGroupTargetIndex_]
                    .cellScale;
            const int targetPadding =
                ScaleWidgetCu(3.0f, cellScale);
            InflateRect(
                &target, targetPadding, targetPadding);
            DrawD2DRoundedRectangle(
                ctx, target,
                static_cast<float>(
                    ScaleWidgetCu(10.0f, cellScale)),
                D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.14f),
                D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.92f),
                static_cast<float>(
                    ScaleWidgetCu(2.5f, cellScale)));
        }
        else if (widgetDockTarget_)
        {
            if (widgetDockTargetContainer_)
                widgetDockTargetContainer_->DrawInsertionPreview(
                    ctx, widgetDockInsertIndex_);
        }
        else
        {
        GridCell cell = widgetPreviewCell_;
        GridSpan span = widgetPreviewSpan_;
        RECT previewBounds = GetGridRect(gridPages_, cell, span);

        bool widgetConflict = false;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (i == mouseDownWidgetIndex_) continue;
            const auto& ow = widgets_[i];
            if (!snowdesktop::collection_group_rules::
                    ShouldOccupyDesktopGrid(
                        IsGroupedWidget(ow)))
                continue;
            if (ow.gridCell.pageId != cell.pageId) continue;
            if (cell.column + span.columns <= ow.gridCell.column) continue;
            if (ow.gridCell.column + ow.gridSpan.columns <= cell.column) continue;
            if (cell.row + span.rows <= ow.gridCell.row) continue;
            if (ow.gridCell.row + ow.gridSpan.rows <= cell.row) continue;
            widgetConflict = true; break;
        }

        bool ok = !widgetConflict && !cell.pageId.empty();
        float radius = 8.0f;
        D2D1::ColorF fill = ok ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.15f)
                              : D2D1::ColorF(1.0f, 0.30f, 0.30f, 0.18f);
        D2D1::ColorF border = ok ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.75f)
                                 : D2D1::ColorF(1.0f, 0.25f, 0.25f, 0.85f);

        DrawD2DRoundedRectangle(ctx, previewBounds, radius, fill, border, 2.0f);
        }
    }

    // Drop previews are split around the popup draw. Desktop/widget targets
    // belong below foreground popups; only a popup's own target indicator is
    // allowed above its contents.
    auto drawDropPreviewLayer = [&](bool popupLayer) {
        Container* targetContainer =
            dragSession_.TargetContainer();
        Slot* targetSlot =
            dragSession_.TargetSlot();
        HitRegion targetRegion =
            dragSession_.TargetRegion();
        if (!(dragSession_.IsActive() ||
                dragDropController_.IsExternalDragActive()) ||
            !targetContainer ||
            (hiddenMode &&
                !IsRetainedContainer(targetContainer)) ||
            targetRegion == HitRegion::None)
            return;

        if (!popupLayer)
        {
            const auto* targetDock =
                dynamic_cast<const DockContainer*>(
                    targetContainer);
            const bool targetHostedByDockHost =
                targetDock &&
                IsDockHostedByPersistentHost(targetDock);
            const bool targetIsFloatingDock =
                renderingPersistentDockHost_ &&
                targetContainer ==
                    renderingPersistentDockHost_->container;
            if (!snowdesktop::drag_visual_rules::
                    DropPreviewBelongsToRenderSurface(
                        renderingFloatingDock_,
                        targetHostedByDockHost,
                        targetIsFloatingDock))
            {
                return;
            }
        }

        RECT clipViewport{};
        RECT popupTargetRect{};
        auto* wc = dynamic_cast<WidgetContainer*>(targetContainer);
        const DesktopWidget* openPopupWidget =
            GetOpenPopupWidget();
        const bool popupTarget = wc && openPopupWidget &&
            wc->GetWidgetData() == openPopupWidget &&
            targetSlot == popupDragTarget_.Get();
        const bool targetUsesPopupLayer =
            snowdesktop::popup_drag_rules::
                ResolveDropPreviewLayer(popupTarget) ==
            snowdesktop::popup_drag_rules::
                DropPreviewLayer::Popup;
        if (targetUsesPopupLayer != popupLayer)
            return;
        const bool groupEntryTarget =
            wc &&
            ((dragSession_.SourceList().
                    hasCollectionGroupEntries &&
                dynamic_cast<CollectionGroup*>(wc)) ||
             (dragSession_.SourceList().
                    UsesFileGroupSourceInsertion() &&
                dynamic_cast<FileGroup*>(wc)));
        if (wc && !groupEntryTarget)
        {
            RECT bodyRect = wc->GetBodyRect();
            if (popupTarget)
            {
                RECT popup =
                    GetCollectionPopupRect(
                        *openPopupWidget);
                popupTargetRect = popup;
                const RECT content =
                    GetCollectionPopupContentRect(popup);
                clipViewport = openPopupWidget->listMode
                    ? snowdesktop::popup_drag_rules::
                        ExpandInsertionClipVertically(
                            content, popup,
                            kCollectionPopupGapY / 2 + 2)
                    : snowdesktop::popup_drag_rules::
                        ExpandInsertionClipHorizontally(
                            content, popup,
                            kCollectionPopupGapX / 2 + 2);
            }
            else
            {
                DesktopWidget* wd = wc->GetWidgetData();
                if (wd && wd->type == DesktopWidgetType::Collection && !wd->scrollContainerMode)
                {
                }
                else
                {
                    clipViewport = wc->GetContentViewportRect();
                    clipViewport.left = bodyRect.left;
                    clipViewport.right = bodyRect.right;
                }
            }
        }
        D2D1_MATRIX_3X2_F
            popupTargetPreviousTransform{};
        const bool popupTargetAnimationApplied =
            popupTarget &&
            beginPopupAnimationTransform(
                popupTargetRect,
                popupTargetPreviousTransform);
        bool clipped = false;
        if (!IsRectEmptyRect(clipViewport))
        {
            ctx->PushAxisAlignedClip(ToD2DRect(clipViewport), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            clipped = true;
        }
        if (targetRegion == HitRegion::Handoff && targetSlot)
        {
            RECT bounds = targetSlot->GetBounds();
            DrawD2DRoundedRectangle(ctx, bounds, 6.0f,
                D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.15f),
                D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.60f), 2.0f);
        }
        else
        {
            if (popupTarget && targetSlot &&
                (targetRegion == HitRegion::SortBefore ||
                 targetRegion == HitRegion::SortAfter))
            {
                targetSlot->DrawDropIndicator(ctx, targetRegion,
                    static_cast<float>(kCollectionPopupGapX) * 0.5f);
            }
            else
            {
                targetContainer->DrawDropPreview(ctx, targetSlot, targetRegion);
            }
        }
        if (clipped) ctx->PopAxisAlignedClip();
        endPopupAnimationTransform(
            popupTargetAnimationApplied,
            popupTargetPreviousTransform);
    };

    // Blue insertion bars and green handoff boxes for the desktop, widgets and
    // Dock are part of the background interaction layer.
    if (!renderingFloatingPopup_)
        drawDropPreviewLayer(false);

    const bool collectionHostedByFloatingPopup =
        IsCollectionPopupHostedByFloatingWindow();
    const bool popupBelongsToCurrentSurface =
        renderingFloatingPopup_
            ? collectionHostedByFloatingPopup
            : renderingFloatingDock_
            ? !collectionHostedByFloatingPopup &&
                popupAnchoredToDock_ &&
                floatingDockVisible_
            : !collectionHostedByFloatingPopup &&
                !(popupAnchoredToDock_ &&
                    persistentDockHostOwnsVisual_);
    if (popupBelongsToCurrentSurface &&
        (!hiddenMode || IsOpenPopupRetained()) &&
        GetOpenPopupWidget())
    {
        const bool suppressPopupHover =
            SuppressDesktopWidgetDragTargets();
        const POINT interactionPoint =
            lastMousePoint_;
        if (suppressPopupHover)
            lastMousePoint_ = {
                LONG_MIN, LONG_MIN };
        DrawCollectionPopup(ctx);
        if (suppressPopupHover)
            lastMousePoint_ =
                interactionPoint;
    }

    // A target resolved inside the open popup must remain visible on top of
    // that popup, while every other target stays covered by it.
    if (popupBelongsToCurrentSurface)
        drawDropPreviewLayer(true);

    const bool luaPanelBelongsToCurrentSurface =
        renderingFloatingPopup_
            ? IsLuaPanelHostedByFloatingWindow()
            : !renderingFloatingDock_ &&
                !IsLuaPanelHostedByFloatingWindow();
    if (luaPanelBelongsToCurrentSurface &&
        !luaWidgetPanelRequest_.widgetId.empty())
    {
        bool renderLuaPanel = !hiddenMode;
        if (hiddenMode)
        {
            const auto source = std::find_if(
                widgets_.begin(), widgets_.end(),
                [&](const DesktopWidget& widget) {
                    return widget.id ==
                        luaWidgetPanelRequest_.widgetId;
                });
            renderLuaPanel = source != widgets_.end() &&
                source->keepWhenDesktopHidden;
        }
        if (renderLuaPanel)
        {
            if (luaWidgetPanelRequest_.modal)
            {
                RECT scrim{};
                GetClientRect(hwnd_, &scrim);
                DrawD2DSeparator(ctx, scrim,
                    D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.38f));
            }
            DrawLuaWidgetPanel(ctx);
        }
    }

    const bool popupMarquee = marqueeDockFolderPopup_ ||
        (marqueeWidgetIndex_ < widgets_.size() &&
         marqueeWidgetIndex_ == popupWidgetIndex_);
    const bool marqueeBelongsToCurrentSurface = popupMarquee
        ? popupBelongsToCurrentSurface
        : !renderingFloatingPopup_;
    if (marqueeActive_ && marqueeBelongsToCurrentSurface)
    {
        if (!marqueeDockFolderPopup_ &&
            marqueeWidgetIndex_ >= widgets_.size())
        {
            for (auto& ooItem : items_oo_)
            {
                auto* icon = dynamic_cast<DesktopIcon*>(ooItem.get());
                if (!icon) continue;
                DesktopItem* item = icon->GetDesktopItem();
                if (!item || !item->selected || IsRectEmptyRect(item->bounds)) continue;
                icon->Draw(ctx, item->bounds, 2);
            }
        }
        if (marqueeDockFolderPopup_ ||
            marqueeWidgetIndex_ < widgets_.size())
        {
            RECT viewport = GetMarqueeViewportRect();
            D2D1_MATRIX_3X2_F
                marqueePreviousTransform{};
            const bool marqueeAnimationApplied =
                popupMarquee &&
                beginPopupAnimationTransform(
                    popupRect_,
                    marqueePreviousTransform);
            ctx->PushAxisAlignedClip(ToD2DRect(viewport),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            DrawD2DFilledRectangle(ctx, marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
            ctx->PopAxisAlignedClip();
            endPopupAnimationTransform(
                marqueeAnimationApplied,
                marqueePreviousTransform);
        }
        else
        {
            DrawD2DFilledRectangle(ctx, marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
        }
    }

    if (!hiddenMode && !renderingFloatingDock_ &&
        !renderingFloatingPopup_)
    {
        DrawPageNavHotEdgeHint(ctx);
        DrawPageNotify(ctx);
    }
}

void DesktopApp::RenderFrame(
    ID2D1DeviceContext* ctx,
    const RECT* updateRect,
    bool hiddenMode)
{
    if (ctx != brushCacheContext_ || brushCache_.size() >= 512)
    {
        brushCache_.clear();
        brushCacheContext_ = ctx;
    }
    // The root surface is the stable desktop background. Widgets and all
    // foreground interaction content are owned by higher DComp layers.
    DrawStaticBackground(ctx, updateRect, hiddenMode);
}
