#include "app/app.h"
#include "common/l10n.h"
#include "layout/collection_popup_layout.h"
#include "layout/folder_preview_header_layout.h"
#include "widget/runtime/widget_item_layout.h"
#include "settings/animation_settings.h"
#include "common/utils.h"
#include "layout/folder_sort_rules.h"
#include "app/dock/dock_tray_popup_state.h"
#include "popup_opacity_scope.h"
#include "popup_resize_transition.h"
#include "app/navigation/quick_navigation_theme.h"
#include "layout/item_render_layer_rules.h"
#include "layout/popup_scrollbar_layout.h"
#include "categorized_popup_scope.h"
#include "icons/icon_backdrop_geometry.h"
#include "icons/icon_bitmap_renderer.h"
#include "ui/render/app_font.h"
#include "ui/render/blur_sampling_bounds.h"
#include <d2d1effects.h>

// Collection-popup rendering.

namespace
{
ComPtr<ID2D1Geometry> CombineFanGeometry(ID2D1Factory& factory,
    ID2D1Geometry& subject, ID2D1Geometry& mask, D2D1_COMBINE_MODE mode)
{
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory.CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return {};
    const auto combined = subject.CombineWithGeometry(&mask, mode, nullptr, sink.Get());
    const auto closed = sink->Close();
    if (FAILED(combined) || FAILED(closed)) return {};
    return path;
}

bool Overlap(const D2D1_RECT_F& a, const D2D1_RECT_F& b)
{
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
}

void DesktopApp::PublishCollectionPopupFanBackdrops()
{
    auto& backdrop = collectionPopupBackdropCompositor_;
    if (!backdrop.IsAvailable() || !d2dFactory_ ||
        (popupFanBackdropCandidates_.empty() && popupFanBackdropCount_ == 0)) return;

    // Where silhouettes overlap during unfolding, display the stronger
    // sample once. Equal radii prefer the more visible icon.
    auto& candidates = popupFanBackdropCandidates_;
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.blurRadius != b.blurRadius ? a.blurRadius > b.blurRadius : a.opacity > b.opacity;
    });
    backdrop.BeginFrame(false);
    std::size_t published = 0;
    for (auto next = candidates.begin(); next != candidates.end(); ++next)
    {
        auto clip = next->silhouette;
        for (auto previous = candidates.begin(); clip && previous != next; ++previous)
            if (Overlap(next->bounds, previous->bounds))
                clip = CombineFanGeometry(*d2dFactory_.Get(), *clip.Get(),
                    *previous->silhouette.Get(), D2D1_COMBINE_MODE_EXCLUDE);
        D2D1_RECT_F visible{};
        if (!clip || FAILED(clip->GetBounds(nullptr, &visible)) ||
            visible.left >= visible.right || visible.top >= visible.bottom) continue;
        // These small keys are local to the popup compositor. Its ordinary
        // collection, Lua panel and stack indicator use object addresses.
        const auto key = static_cast<std::uintptr_t>(published + 1);
        if (!backdrop.AddClippedPanel(next->viewport, clip.Get(), next->blurRadius, key) ||
            !backdrop.SetPanelOpacity(next->viewport, next->opacity, key))
        {
            (void)backdrop.RemovePanel({}, key);
            continue;
        }
        popupFanBackdropViewport_ = next->viewport;
        ++published;
    }
    for (auto i = published; i < popupFanBackdropCount_; ++i)
        (void)backdrop.RemovePanel({}, static_cast<std::uintptr_t>(i + 1));
    popupFanBackdropCount_ = published;
    candidates.clear();
    backdrop.EndFrame(false);
    ApplyCollectionPopupBackdropAnimationFrame();
}

IDWriteTextFormat* DesktopApp::GetCollectionPopupTextFormat(float scale, bool title)
{
    if (!dwriteFactory_) return nullptr;
    const auto revision = snowdesktop::app_fonts::revision.load();
    if (collectionPopupFontRevision_ != revision)
    {
        collectionPopupTitleTextFormat_.Reset();
        collectionPopupControlTextFormat_.Reset();
        collectionPopupFontRevision_ = revision;
    }
    auto& format = title ? collectionPopupTitleTextFormat_ : collectionPopupControlTextFormat_;
    const float size = title
        ? snowdesktop::collection_popup_layout::ResolveTitleFontSize(scale)
        : snowdesktop::collection_popup_layout::ResolveControlFontSize(scale);
    if (!format || format->GetFontSize() != size)
    {
        format.Reset();
        snowdesktop::app_fonts::CreateTextFormat(dwriteFactory_, L"Segoe UI",
            title ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
        if (!format) return nullptr;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return format.Get();
}

void DesktopApp::DrawCollectionPopup(
    ID2D1DeviceContext* ctx,
    bool applyAnimation, const RECT* preparedRect)
{
    const DesktopWidget* openWidget = GetOpenPopupWidget();
    if (!ctx || !openWidget) return;
    const auto animation = popupAnimation_.GetVisual();
    if (applyAnimation && (!animation.visible || popupAnimationOverlay_.active))
        return;

    const DesktopWidget& widget = *openWidget;
    const bool fan = UsesCollectionPopupFan(widget);
    const bool inlinePreview = folderPreview_ && folderPreview_->inlineBounds;
    const POINT pointer = lastMousePoint_;
    const bool lightTheme = inlinePreview ? IsLightContentTheme() : collectionPopupLightTheme_;
    const auto popupMetrics =
        GetCollectionPopupLayoutMetrics(widget);
    const QuickNavTheme& popupTheme = lightTheme
        ? kQuickNavLight : kQuickNavDark;
    const auto popupTextColor = [&](float alpha) {
        D2D1_COLOR_F color = popupTheme.popupTitle;
        color.a *= std::clamp(alpha, 0.0f, 1.0f);
        return color;
    };
    popupRect_ = preparedRect ? *preparedRect : GetCollectionPopupRect(widget);
    auto* offsetView = GetCategorizedPopupView();
    if (!offsetView || !offsetView->IsSearchActive())
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
            GetCollectionPopupMaxScrollOffset(widget, popupRect_));
    const auto* motion = GetCollectionPopupResizeTransition();
    const RECT presentedPanel = motion ? motion->Panel(popupRect_) : popupRect_;
    if (const auto* host = GetFolderPreviewComponent()) dockFolderPopupWidget_.privacyMode = host->privacyMode;
    const bool privatePreview = folderPreview_ && widget.privacyMode && !PtInRect(&presentedPanel, pointer) &&
        !dragSession_.IsActive() && !dragDropController_.IsExternalDragActive();

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

    const auto& popupKeys = GetPopupItemKeysView(widget);
    const auto drawPreviewMarquee = [&] {
        if (folderPreview_ && folderPreview_->keyboardFocused && marqueeActive_ && marqueeDockFolderPopup_)
            DrawD2DFilledRectangle(ctx, marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f));
    };
    if (!fan)
    {
        if (!inlinePreview)
        {
            PersonalizationSettings popupBackgroundAppearance = collectionPopupAppearance_;
            popupBackgroundAppearance.widgetEdgeHighlightEnabled = false;
            DrawWidgetPanelBackground(
                ctx, presentedPanel, 18.0f * popupMetrics.scale,
                D2D1::ColorF(
                    collectionPopupAppearance_.widgetBgR,
                    collectionPopupAppearance_.widgetBgG,
                    collectionPopupAppearance_.widgetBgB,
                    std::clamp(collectionPopupAppearance_.widgetAlpha, 0.0f, 1.0f)),
                D2D1::ColorF(
                    collectionPopupAppearance_.widgetBorderR,
                    collectionPopupAppearance_.widgetBorderG,
                    collectionPopupAppearance_.widgetBorderB,
                    std::clamp(collectionPopupAppearance_.widgetBorderAlpha, 0.0f, 1.0f)),
                false,
                std::clamp(collectionPopupAppearance_.widgetBorderWidth,
                    kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth) * popupMetrics.scale,
                &popupBackgroundAppearance, false, 0, popupMetrics.scale);
        }

        const RECT titleRect = snowdesktop::collection_popup_layout::
            ResolveTitleRect(presentedPanel, popupMetrics,
                dockFolderPopupOpen_ && !folderPreview_ ? GetDockFolderPopupSortButtonRect(popupRect_).left -
                    popupMetrics.controlGap
                    : (std::numeric_limits<LONG>::max)());
        if (folderPreview_)
        {
            const auto row = GetFolderPreviewNavigationRow(presentedPanel);
            const auto controlColor = [&](float alpha) {
                return lightTheme ? D2D1::ColorF(0.0f, 0.0f, 0.0f, alpha)
                                  : D2D1::ColorF(1.0f, 1.0f, 1.0f, alpha);
            };
            const auto highlight = [&](RECT rect) {
                InflateRect(&rect, -ScaleWidgetCu(2.0f, row.scale), -ScaleWidgetCu(3.0f, row.scale));
                if (!IsRectEmpty(&rect))
                    DrawD2DRoundedRectangle(ctx, rect, static_cast<float>(ScaleWidgetCu(5.0f, row.scale)),
                        controlColor(0.10f), controlColor(0.0f), 0.0f);
            };
            if (!IsRectEmpty(&row.bounds))
                DrawD2DRoundedRectangle(ctx, row.bounds, static_cast<float>(ScaleWidgetCu(8.0f, row.scale)),
                    controlColor(lightTheme ? 0.035f : 0.05f), controlColor(lightTheme ? 0.14f : 0.20f), 1.0f);
            const auto drawNavigationButton = [&](const RECT& bounds, bool home) {
                if (IsRectEmpty(&bounds)) return;
                const bool hovered = PtInRect(&bounds, pointer) != FALSE;
                if (hovered) highlight(bounds);
                ComPtr<ID2D1SolidColorBrush> brush;
                if (FAILED(ctx->CreateSolidColorBrush(controlColor(hovered ? 1.0f : 0.82f), &brush))) return;
                const float centerX = (bounds.left + bounds.right) * 0.5f + (home ? -2.0f : 2.0f) * row.scale;
                const float centerY = (bounds.top + bounds.bottom) * 0.5f;
                const auto line = [&](float x1, float y1, float x2, float y2) {
                    ctx->DrawLine(D2D1::Point2F(centerX + x1 * row.scale, centerY + y1 * row.scale),
                        D2D1::Point2F(centerX + x2 * row.scale, centerY + y2 * row.scale),
                        brush.Get(), std::max(1.0f, 1.25f * row.scale));
                };
                if (home)
                {
                    line(-6, 0, 0, -6);
                    line(0, -6, 6, 0);
                    line(-4.5f, -1.5f, -4.5f, 5.5f);
                    line(4.5f, -1.5f, 4.5f, 5.5f);
                    line(-4.5f, 5.5f, 4.5f, 5.5f);
                }
                else
                {
                    line(-5.5f, 0, 5.5f, 0);
                    line(-5.5f, 0, -1, -4.5f);
                    line(-5.5f, 0, -1, 4.5f);
                }
            };
            auto* textFormat = GetCollectionPopupTextFormat(row.scale, false);
            drawNavigationButton(row.back, false);
            drawNavigationButton(row.home, true);
            const LONG dividerInset = ScaleWidgetCu(8.0f, row.scale);
            if (!IsRectEmpty(&row.home) && !IsRectEmpty(&row.breadcrumb))
                DrawD2DSeparator(ctx, {row.home.right, row.home.top + dividerInset,
                    row.home.right + 1, row.home.bottom - dividerInset}, controlColor(0.16f));
            const auto crumbs = privatePreview ? std::vector<snowdesktop::folder_preview::Breadcrumb>{}
                : GetFolderPreviewBreadcrumbs(presentedPanel);
            const auto* dropSlot = dragSession_.IsActive() && dragSession_.TargetRegion() == HitRegion::Handoff &&
                dragSession_.TargetContainer() == dockFolderPopupContainer_.get()
                ? dragSession_.TargetSlot() : nullptr;
            const auto* dropFolder = dropSlot ? dynamic_cast<const ExternalFileItem*>(dropSlot->GetItem()) : nullptr;
            const std::wstring dropPath = dropFolder ? dropFolder->GetPath() : std::wstring{};
            for (size_t i = 0; i < crumbs.size(); ++i)
            {
                const auto& crumb = crumbs[i];
                RECT label = crumb.bounds;
                const LONG separator = snowdesktop::collection_popup_layout::ScaleDimension(10, row.scale);
                if (i != 0)
                {
                    const RECT chevron{label.left, label.top, std::min(label.right, label.left + separator), label.bottom};
                    DrawD2DTextEllipsis(ctx, L"›", chevron, textFormat, controlColor(0.52f),
                        DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    label.left = chevron.right;
                }
                const bool hovered = PtInRect(&crumb.bounds, pointer) != FALSE;
                if (!dropPath.empty() && crumb.path == dropPath)
                {
                    RECT dropBounds = label;
                    InflateRect(&dropBounds, -ScaleWidgetCu(2.0f, row.scale), -ScaleWidgetCu(3.0f, row.scale));
                    if (!IsRectEmpty(&dropBounds))
                        DrawD2DRoundedRectangle(ctx, dropBounds, static_cast<float>(ScaleWidgetCu(5.0f, row.scale)),
                            D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.15f), D2D1::ColorF(0.20f, 0.80f, 0.40f, 0.60f));
                }
                else if (hovered) highlight(label);
                InflateRect(&label, -snowdesktop::collection_popup_layout::ScaleDimension(3, row.scale), 0);
                DrawD2DTextEllipsis(ctx, crumb.label, label, textFormat,
                    controlColor(hovered || i + 1 == crumbs.size() ? 0.97f : 0.82f),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        }
        std::wstring title = ShouldUseDemoCollectionIdentity(&widget)
            ? GetDemoCollectionCategoryTitle(widget)
            : (widget.title.empty()
                ? _LW("app.overlay.collection_default") : widget.title);
        if (!folderPreview_) DrawD2DTextEllipsis(
            ctx, title, titleRect,
            GetCollectionPopupTextFormat(popupMetrics.scale, true),
            popupTextColor(1.0f),
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        if (dockTrayPopup_ && !inlinePreview)
        {
            DrawDockTrayPopup(ctx, popupRect_);
            if (animationApplied) ctx->SetTransform(previousTransform);
            return;
        }

        if (dockFolderPopupOpen_ && !folderPreview_)
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
            case snowdesktop::folder_sort_rules::kSize:
                sortLabel = _LW("app.menu.sort_size");
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
                GetCollectionPopupTextFormat(popupMetrics.scale, false),
                popupTextColor(0.88f),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

    }

    const RECT targetContent = GetCollectionPopupContentRect(popupRect_);
    RECT content = targetContent;
    if (motion) content = motion->Content(content, popupRect_);
    if (auto* view = GetHostedFolderMappingPopupView(popupRect_);
        view && (!folderPreview_ || (!dockFolderPopupLoading_ && dockFolderPopupAvailable_)))
    {
        if (!inlinePreview && motion) view = GetHostedFolderMappingPopupView(presentedPanel);
        const RECT frame = view->GetLayoutFrameRect();
        const snowdesktop::ScopedIconBackdropClip highlightClip(ctx, frame);
        ctx->PushAxisAlignedClip(ToD2DRect(frame), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        view->DrawContent(ctx, frame);
        if (dragSession_.TargetContainer() == view)
        {
            auto* slot = dragSession_.TargetSlot();
            const auto region = dragSession_.TargetRegion();
            if (folderPreview_ && slot && slot == popupDragTarget_.Get() &&
                (region == HitRegion::SortBefore || region == HitRegion::SortAfter))
            {
                const auto style = UsesCollectionPopupList(widget) && GetCollectionPopupColumnCount(presentedPanel) == 1
                    ? BarStyle::HBar : BarStyle::VBar;
                slot->DrawDropIndicatorWithStyle(ctx, region, style,
                    GetFolderPreviewInsertionPad(presentedPanel, slot->GetIndex(), region == HitRegion::SortAfter));
            }
            else view->DrawDropPreview(ctx, slot, region);
        }
        ctx->PopAxisAlignedClip();
        view->DrawScrollbar(ctx, PtInRect(&presentedPanel, pointer) != FALSE);
        if (inlinePreview)
        {
            ctx->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            drawPreviewMarquee();
            ctx->PopAxisAlignedClip();
        }
        if (animationApplied) ctx->SetTransform(previousTransform);
        return;
    }
    if (auto* group = GetGroupPopupView())
    {
        const RECT frame = GetCategorizedPopupFrame(presentedPanel);
        group->SetPopupFrame(&frame);
        const snowdesktop::ScopedIconBackdropClip highlightClip(ctx, frame);
        ctx->PushAxisAlignedClip(ToD2DRect(frame), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        group->DrawContent(ctx, frame);
        if (dragSession_.TargetContainer() == group)
            group->DrawDropPreview(ctx, dragSession_.TargetSlot(), dragSession_.TargetRegion());
        ctx->PopAxisAlignedClip();
        group->DrawScrollbar(ctx, PtInRect(&presentedPanel, pointer) != FALSE);
        if (animationApplied) ctx->SetTransform(previousTransform);
        return;
    }
    if (auto* view = GetCategorizedPopupView())
    {
        RECT controls = GetCollectionPopupControlsRect(presentedPanel);
        controls.bottom = std::min(controls.bottom, content.top);
        if (!IsRectEmpty(&controls))
        {
            const RECT frame = GetCategorizedPopupFrame(presentedPanel);
            CategorizedPopupScope scope(view, frame);
            const snowdesktop::ScopedIconBackdropClip highlightClip(ctx, controls);
            ctx->PushAxisAlignedClip(ToD2DRect(controls), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            view->DrawContent(ctx, frame);
            ctx->PopAxisAlignedClip();
        }
    }
    DesktopWidget popupListStyle;
    popupListStyle.type = widget.type;
    popupListStyle.bounds = popupRect_;
    popupListStyle.cellScale = popupMetrics.scale;
    popupListStyle.listMode = UsesCollectionPopupList(widget);
    popupListStyle.id = widget.id;
    popupListStyle.listColumns = snowdesktop::list_detail_rules::ListColumns(widget);
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
    popupListRenderer.SetPopupFrame(&popupRect_);
    if (UsesCollectionPopupList(widget) && !fan)
    {
        popupListRenderer.DrawDetailsHeader(
            ctx, content, collectionPopupLightTheme_);
    }
    std::optional<snowdesktop::ScrollContentClip> scrollClip;
    if (fan) ctx->PushAxisAlignedClip(ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    else scrollClip.emplace(ctx, collectionPopupScrollFadeCache_, content, popupScrollOffset_,
        GetCollectionPopupContentHeight(widget, popupRect_, targetContent), static_cast<float>(ScaleWidgetCu(16.0f, popupMetrics.scale)));
    const size_t popupItemCount = GetPopupItemCount(widget);
    const size_t fanCapacity = GetCollectionPopupFanVisibleCount(popupRect_);
    const double fanOffset = fan ? GetCollectionPopupFanScrollOffset(popupRect_) : 0;
    const auto fanRange = snowdesktop::collection_popup_layout::FanVisibleRange(
        fanOffset, fanCapacity, popupItemCount);
    size_t firstItem = fan ? fanRange.first : 0;
    size_t endItem = fan ? fanRange.end : popupItemCount;
    if (!fan)
    {
        const int visibleHeight = std::max(0, static_cast<int>(content.bottom - content.top));
        if (UsesCollectionPopupList(widget))
        {
            const auto layout = snowdesktop::widget_item_layout::ResolveList(targetContent,
                snowdesktop::collection_popup_layout::ResolveListRowHeight(popupMetrics, listItemFontSizeCu_),
                GetListSpacing(), snowdesktop::list_detail_rules::ListColumns(widget));
            std::tie(firstItem, endItem) = snowdesktop::widget_item_layout::VisibleRange(
                layout, popupItemCount, popupScrollOffset_, visibleHeight);
        }
        else
        {
            const int columns = GetCollectionPopupColumnCount(popupRect_);
            const int stride = std::max(1, popupMetrics.cellHeight + popupMetrics.gapY);
            firstItem = std::min(popupItemCount, static_cast<size_t>(popupScrollOffset_ / stride) * columns);
            endItem = std::min(popupItemCount,
                static_cast<size_t>((popupScrollOffset_ + visibleHeight + stride - 1) / stride) * columns);
        }
    }
    if (fan && !popupFanIconRecorder_ && d2dDevice_)
        (void)d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &popupFanIconRecorder_);
    const auto iconRecorder = popupFanIconRecorder_;
    ComPtr<ID2D1RectangleGeometry> fanViewportClip;
    const auto collectFanBackdrop = [&](ID2D1Bitmap* bitmap, const RECT& icon,
        const D2D1_MATRIX_3X2_F& pose, float opacity) {
        if (!applyAnimation || !renderingFloatingPopup_ || !collectionPopupBackdropCompositor_.IsAvailable() ||
            !d2dFactory_ || !bitmap || opacity <= 0.001f)
            return;
        const auto eligible = iconGlassBackdrop_.find(bitmap);
        if (eligible == iconGlassBackdrop_.end() || !eligible->second) return;
        const auto styled = iconBitmapStyles_.find(bitmap);
        const auto& style = styled == iconBitmapStyles_.end() ? iconBeautifySettings_ : styled->second;
        if (!style.enabled || !style.glassEnabled) return;
        const auto size = bitmap->GetPixelSize();
        const RECT frame = snowdesktop::icon_bitmap_renderer::Prepare(
            static_cast<int>(size.width), static_cast<int>(size.height), icon, true).bounds;
        if (IsRectEmpty(&frame)) return;

        ComPtr<ID2D1Geometry> iconShape;
        if (FAILED(snowdesktop::CreateIconBackdropGeometry(d2dFactory_.Get(), style.shape,
                frame, nullptr, iconShape))) return;
        const auto localPose = D2D1::Matrix3x2F::Translation(static_cast<float>(frame.left),
            static_cast<float>(frame.top)) * pose * D2D1::Matrix3x2F::Translation(
                static_cast<float>(-content.left), static_cast<float>(-content.top));
        ComPtr<ID2D1TransformedGeometry> transformed;
        if (FAILED(d2dFactory_->CreateTransformedGeometry(iconShape.Get(), localPose, &transformed))) return;
        const auto viewport = D2D1::RectF(0.f, 0.f, static_cast<float>(content.right - content.left),
            static_cast<float>(content.bottom - content.top));
        D2D1_RECT_F bounds{};
        if (FAILED(transformed->GetBounds(nullptr, &bounds)) || !Overlap(viewport, bounds)) return;

        ComPtr<ID2D1Geometry> silhouette = transformed;
        if (bounds.left < viewport.left || bounds.top < viewport.top ||
            bounds.right > viewport.right || bounds.bottom > viewport.bottom)
        {
            if (!fanViewportClip && FAILED(d2dFactory_->CreateRectangleGeometry(viewport, &fanViewportClip))) return;
            silhouette = CombineFanGeometry(*d2dFactory_.Get(), *transformed.Get(),
                *fanViewportClip.Get(), D2D1_COMBINE_MODE_INTERSECT);
            if (!silhouette || FAILED(silhouette->GetBounds(nullptr, &bounds))) return;
        }
        const auto radius = std::clamp(style.glassBlurRadius, 0.f,
            static_cast<float>(snowdesktop::blur_sampling::kMaximumBackdropDeviation));
        popupFanBackdropCandidates_.push_back({content, std::move(silhouette), bounds, radius, opacity});
    };
    const auto drawFanSlot = [&](size_t index, bool selected, bool cut, const auto& drawIcon,
        std::wstring_view iconIdentity = {}, ID2D1Bitmap* glassBitmap = nullptr) {
        namespace layout = snowdesktop::collection_popup_layout;
        const auto pose = GetCollectionPopupFanItem(popupRect_, index);
        const double slot = index == popupItemCount ? static_cast<double>(fanCapacity) :
            static_cast<double>(index) - fanOffset;
        const float visibility = index == popupItemCount ? 1.0f : layout::FanItemOpacity(slot, fanCapacity);
        if (visibility <= 0.001f) return;
        const bool hovered = popupItemCount > 0 && popupAnimation_.IsInteractive() &&
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
        const auto iconPose = D2D1::Matrix3x2F::Rotation(pose.angle * progress, center) *
            D2D1::Matrix3x2F::Translation(unfolding.x - center.x, unfolding.y - center.y);
        const float iconOpacity = progress * visibility * (cut ? 0.5f : 1.0f);
        D2D1_MATRIX_3X2_F transform{};
        ctx->GetTransform(&transform);
        ctx->SetTransform(iconPose * transform);
        PopupOpacityScope slotOpacity(ctx, true, iconOpacity);
        collectFanBackdrop(glassBitmap, pose.icon, iconPose, animation.opacity * iconOpacity);

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
        if (iconRecorder)
        {
            const auto cacheKey = std::to_wstring(index) + L":" + std::to_wstring(popupMetrics.scale) +
                L":" + std::to_wstring(iconAppearanceRevision_) + L":" + std::to_wstring(iconCustomizationRevision_) +
                L":" + std::wstring(iconIdentity);
            auto cached = popupFanIconDrawings_.find(cacheKey);
            if (cached == popupFanIconDrawings_.end())
            {
                if (popupFanIconDrawings_.size() >= 24)
                {
                    popupFanIconDrawings_.clear();
                    cached = popupFanIconDrawings_.end();
                }
                PopupFanIconDrawing drawing;
                if (SUCCEEDED(iconRecorder->CreateCommandList(&drawing.commands)) &&
                    SUCCEEDED(ctx->CreateEffect(CLSID_D2D1Shadow, &drawing.shadow)))
                {
                    iconRecorder->SetDpi(96, 96);
                    iconRecorder->SetTarget(drawing.commands.Get());
                    iconRecorder->SetTransform(D2D1::Matrix3x2F::Identity());
                    iconRecorder->BeginDraw();
                    drawIcon(iconRecorder.Get(), RECT{0, 0,
                        pose.icon.right - pose.icon.left, pose.icon.bottom - pose.icon.top});
                    const HRESULT recorded = iconRecorder->EndDraw();
                    iconRecorder->SetTarget(nullptr);
                    if (SUCCEEDED(recorded) && SUCCEEDED(drawing.commands->Close()))
                    {
                        drawing.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 2.5f * popupMetrics.scale);
                        drawing.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1_VECTOR_4F{0, 0, 0, 0.65f});
                        drawing.shadow->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_BALANCED);
                        drawing.shadow->SetValue(D2D1_PROPERTY_CACHED, static_cast<BOOL>(TRUE));
                        drawing.shadow->SetInput(0, drawing.commands.Get());
                        drawing.shadow->GetOutput(&drawing.shadowImage);
                        if (SUCCEEDED(ctx->GetImageLocalBounds(drawing.shadowImage.Get(), &drawing.shadowBounds)))
                            cached = popupFanIconDrawings_.emplace(cacheKey, std::move(drawing)).first;
                    }
                }
            }
            if (cached != popupFanIconDrawings_.end())
            {
                const auto& drawing = cached->second;
                const auto shadowOffset = D2D1::Point2F(pose.icon.left + drawing.shadowBounds.left,
                    pose.icon.top + drawing.shadowBounds.top + 3.0f * popupMetrics.scale);
                ctx->DrawImage(drawing.shadowImage.Get(), &shadowOffset, &drawing.shadowBounds);
                const auto iconBounds = D2D1::RectF(0, 0, static_cast<float>(pose.icon.right - pose.icon.left),
                    static_cast<float>(pose.icon.bottom - pose.icon.top));
                const auto iconOffset = D2D1::Point2F(static_cast<float>(pose.icon.left), static_cast<float>(pose.icon.top));
                ctx->DrawImage(drawing.commands.Get(), &iconOffset, &iconBounds);
                iconDrawn = true;
            }
        }
        if (!iconDrawn) drawIcon(ctx, pose.icon);
        const RECT textRect = layout::FanLabelTextRect(pose.label, popupMetrics.scale);
        const auto text = GetCollectionPopupFanLabel(index);
        auto* labelLayout = GetCollectionPopupFanTextLayout(text,
            static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left)),
            static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top)), true);
        DrawD2DTextEllipsis(ctx, text, textRect,
            fanTitleTextFormat_.Get(), selected ? D2D1::ColorF(D2D1::ColorF::White) : popupTextColor(1.0f),
            DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true, labelLayout);
        ctx->SetTransform(transform);
    };
    const auto drawFanItem = [&](size_t index, const auto& item, const DesktopItem* desktopItem) {
        const bool demo = desktopItem && ShouldUseDemoCollectionIdentity(&widget);
        const std::wstring_view identity = [&]() -> std::wstring_view {
            if constexpr (requires { item.layoutKey; }) return item.layoutKey;
            else return item.fullPath;
        }();
        const auto pose = GetCollectionPopupFanItem(popupRect_, index);
        auto* custom = demo ? nullptr : GetSmallCustomIcon(ctx, identity, pose.icon.right - pose.icon.left);
        auto* bitmap = demo || custom ? nullptr : GetTargetIconBitmap(ctx, item.iconBitmap, identity, item.iconIsMediaThumbnail);
        // Resolve asynchronous/custom icon state before consulting the recorded
        // drawing. A late icon arrival must replace a cached placeholder.
        const auto iconIdentity = std::wstring(identity) + L":" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(custom ? custom : bitmap)) + L":" +
            std::to_wstring(item.sysIconIndex) + L":" + std::to_wstring(item.isShortcut) +
            L":" + std::to_wstring(item.isApplicationShortcut) + (demo ? L":demo" : L":icon");
        drawFanSlot(index, item.selected, item.isCut, [&](ID2D1DeviceContext* iconContext, const RECT& iconRect) {
            if (demo)
                DrawDemoCollectionIdentityIcon(iconContext, widget,
                    desktopItem->layoutKey.empty() ? desktopItem->parsingName : desktopItem->layoutKey,
                    iconRect, 1.0f);
            else if (custom)
                DrawIconBitmap(iconContext, custom, iconRect);
            else if (bitmap)
                DrawIconBitmap(iconContext, bitmap, iconRect);
            else
                DrawPlaceholderIcon(iconContext, item.sysIconIndex, iconRect, 1.0f);
            if (!demo && ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut))
                DrawShortcutArrowOverlay(iconContext, iconRect, 1.0f);
        }, iconIdentity, custom ? custom : bitmap);
    };
    for (size_t i = firstItem; i < endItem; ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom) continue;

        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            FolderEntry& entry = dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(widget, i)];
            if (privatePreview)
            {
                popupListRenderer.DrawPrivacyPlaceholder(ctx, itemRect, {}, entry.isDirectory);
                continue;
            }
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
                    entry.DisplayName(),
                    entry.selected,
                    entry.iconIsMediaThumbnail,
                    {}, nullptr,
                    {
                        entry.typeName,
                        entry.lastWriteTime,
                        entry.fileSize,
                        entry.isDirectory,
                        entry.isCut,
                    },
                    lightTheme, !IsRenamingItem(&entry), entry.fullPath, content);
                continue;
            }
            const bool hovered =
                popupAnimation_.IsInteractive() &&
                !entry.selected &&
                PtInRect(&content, pointer) && PtInRect(&itemRect, pointer);
            FolderEntryIcon icon(
                &entry, dockFolderPopupContainer_.get(), this);
            const auto titleLayers =
                snowdesktop::item_render_layer_rules::
                    ResolveTitleLayerPlan(entry.selected);
            icon.Draw(ctx, itemRect,
                entry.selected ? 2 : (hovered ? 1 : 0),
                lightTheme,
                titleLayers.drawWithItem);
        }
        else
        {
            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
            if (itemIndex == static_cast<size_t>(-1))
            {
                if (initialShellReadPending_)
                {
                    if (fan)
                        drawFanSlot(i, false, false, [&](auto* target, const RECT& bounds) {
                            DrawPlaceholderIcon(target, -1, bounds, 1.0f);
                        });
                    else if (UsesCollectionPopupList(widget))
                        popupListRenderer.DrawListItem(ctx, itemRect, nullptr, -1,
                            L"", false, false, {}, nullptr, {}, collectionPopupLightTheme_);
                    else
                        DrawPlaceholderIcon(ctx, -1, GetItemIconRect(itemRect), 1.0f);
                }
                continue;
            }
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
                    collectionPopupLightTheme_, !IsRenamingItem(&item), item.layoutKey, content);
                continue;
            }
            bool hovered =
                popupAnimation_.IsInteractive() &&
                !items_[itemIndex].selected &&
                PtInRect(&itemRect, lastMousePoint_);
            DesktopIcon icon(&items_[itemIndex], nullptr, this, true);
            const auto titleLayers =
                snowdesktop::item_render_layer_rules::
                    ResolveTitleLayerPlan(
                        items_[itemIndex].selected);
            icon.Draw(ctx, itemRect,
                items_[itemIndex].selected ? 2 : (hovered ? 1 : 0),
                collectionPopupLightTheme_,
                titleLayers.drawWithItem, false,
                (widget.type == DesktopWidgetType::Collection || widget.type == DesktopWidgetType::FileCategories)
                    ? &widget : nullptr);
        }
    }
    for (size_t i = firstItem; !privatePreview && i < endItem; ++i)
    {
        if (UsesCollectionPopupList(widget) || fan) break;
        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
        if (itemRect.bottom <= content.top ||
            itemRect.top >= content.bottom)
            continue;
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            FolderEntry& entry =
                dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(widget, i)];
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
            DesktopIcon icon(&items_[itemIndex], nullptr, this, true);
            icon.DrawTitle(
                ctx, itemRect, true, 1.0f,
                collectionPopupLightTheme_,
                (widget.type == DesktopWidgetType::Collection || widget.type == DesktopWidgetType::FileCategories)
                    ? &widget : nullptr);
        }
    }
    if (fan && popupItemCount == 0)
    {
        // A noninteractive status label shares the fan pose instead of flashing
        // a rectangular panel or exposing a Show all action for a pending read.
        drawFanSlot(0, false, false, [](ID2D1DeviceContext*, const RECT&) {});
    }
    else if (fan)
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
    drawPreviewMarquee();
    if (fan) ctx->PopAxisAlignedClip();
    else scrollClip.reset();

    if (!fan && widget.type == DesktopWidgetType::FolderMapping &&
        popupItemCount == 0)
    {
        const std::wstring status = dockFolderPopupLoading_
            ? _LW("widget.folder_mapping.loading")
            : dockFolderPopupAvailable_
                ? _LW("widget.folder_mapping.empty")
                : _LW("widget.folder_mapping.unavailable");
        DrawD2DTextEllipsis(ctx, status, content,
            GetCollectionPopupTextFormat(popupMetrics.scale, false),
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
        PtInRect(&popupRect_, pointer);
    popupHovered = popupHovered || popupScrollbarDragging_;
    auto* searchPopup = GetCategorizedPopupView();
    const int contentScroll = searchPopup && searchPopup->IsSearchActive()
        ? searchPopup->GetScrollOffset() : popupScrollOffset_;
    if (!fan) DrawScrollbarAt(
        ctx, snowdesktop::popup_scrollbar_layout::Viewport(presentedPanel, content), contentHeight, visibleHeight,
        contentScroll, popupHovered,
        lightTheme, popupMetrics.scale);

    if (!fan && !inlinePreview) (void)DrawWidgetPanelEdgeHighlight(
        ctx, presentedPanel, 18.0f * popupMetrics.scale,
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
