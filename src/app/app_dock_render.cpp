#include "app.h"
#include "startup_diagnostics.h"
#include "dock_platform_helpers.h"

// Dock controls, entries and running-application rendering.

bool DesktopApp::DrawDockControlBackground(
    ID2D1DeviceContext* ctx, RECT rect, int state, bool forceWhiteStyle)
{
    if (!ctx || IsRectEmptyRect(rect)) return false;
    const PersonalizationSettings appearance = CurrentDockAppearance();

    const float luminance = appearance.widgetBgR * 0.2126f +
        appearance.widgetBgG * 0.7152f + appearance.widgetBgB * 0.0722f;
    const bool lightSurface = !forceWhiteStyle &&
        luminance > 0.58f && appearance.widgetAlpha > 0.10f;
    const bool active = state > 0;
    const D2D1_COLOR_F fill = forceWhiteStyle
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.18f : 0.11f)
        : (active
            ? D2D1::ColorF(0.39f, 0.66f, 1.0f, lightSurface ? 0.20f : 0.25f)
            : (lightSurface
                ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.075f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f)));
    const D2D1_COLOR_F border = forceWhiteStyle
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, active ? 0.36f : 0.20f)
        : (active
            ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.88f)
            : (lightSurface
                ? D2D1::ColorF(0.08f, 0.11f, 0.16f, 0.14f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)));
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));
    const float scale = static_cast<float>(std::min(width, height)) / 52.0f;
    DrawBeautifiedIconPlate(ctx, rect, fill, border,
        (active ? 1.6f : 1.0f) * std::max(0.75f, scale));
    return lightSurface;
}

void DesktopApp::DrawDockSelectionIndicator(
    ID2D1DeviceContext* ctx, RECT iconRect, bool lightTheme)
{
    if (!ctx || IsRectEmptyRect(iconRect))
        return;

    const float iconSize = static_cast<float>(std::max<LONG>(1, std::min(
        iconRect.right - iconRect.left,
        iconRect.bottom - iconRect.top)));
    const float halfSize = std::clamp(iconSize * 0.06f, 3.0f, 5.0f);
    const float gap = std::max(3.0f, iconSize * 0.06f);
    const float centerX = (iconRect.left + iconRect.right) * 0.5f;
    const float centerY = (iconRect.top + iconRect.bottom) * 0.5f;
    D2D1_POINT_2F tip{};
    D2D1_POINT_2F baseA{};
    D2D1_POINT_2F baseB{};
    switch (dockSettings_.position)
    {
    case DockPosition::Top:
    {
        const float y = static_cast<float>(iconRect.top) - gap;
        tip = D2D1::Point2F(centerX, y + halfSize);
        baseA = D2D1::Point2F(centerX - halfSize, y - halfSize);
        baseB = D2D1::Point2F(centerX + halfSize, y - halfSize);
        break;
    }
    case DockPosition::Left:
    {
        const float x = static_cast<float>(iconRect.left) - gap;
        tip = D2D1::Point2F(x + halfSize, centerY);
        baseA = D2D1::Point2F(x - halfSize, centerY - halfSize);
        baseB = D2D1::Point2F(x - halfSize, centerY + halfSize);
        break;
    }
    case DockPosition::Right:
    {
        const float x = static_cast<float>(iconRect.right) + gap;
        tip = D2D1::Point2F(x - halfSize, centerY);
        baseA = D2D1::Point2F(x + halfSize, centerY - halfSize);
        baseB = D2D1::Point2F(x + halfSize, centerY + halfSize);
        break;
    }
    case DockPosition::Bottom:
    default:
    {
        const float y = static_cast<float>(iconRect.bottom) + gap;
        tip = D2D1::Point2F(centerX, y - halfSize);
        baseA = D2D1::Point2F(centerX - halfSize, y + halfSize);
        baseB = D2D1::Point2F(centerX + halfSize, y + halfSize);
        break;
    }
    }

    ComPtr<ID2D1Factory> factory;
    ctx->GetFactory(&factory);
    if (!factory)
        return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink)) || !sink)
        return;
    sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(baseA);
    sink->AddLine(baseB);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close()))
        return;

    ComPtr<ID2D1SolidColorBrush> brush;
    const D2D1_COLOR_F color = lightTheme
        ? D2D1::ColorF(0.08f, 0.42f, 0.94f, 0.96f)
        : D2D1::ColorF(0.35f, 0.68f, 1.0f, 1.0f);
    if (SUCCEEDED(ctx->CreateSolidColorBrush(color, &brush)) && brush)
        ctx->FillGeometry(geometry.Get(), brush.Get());
}

void DesktopApp::DrawDockEntry(ID2D1DeviceContext* ctx,
    const DockEntry& entry, RECT rect, int state)
{
    if (!ctx) return;
    const int scaledSpacing = std::max(1, static_cast<int>(std::round(
        kDockSpacing * ClampDockScale(dockSettings_.thicknessScale))));
    const int iconSize = std::max(1, static_cast<int>(std::min(
        rect.right - rect.left, rect.bottom - rect.top)) - scaledSpacing);
    RECT iconRect{
        rect.left + (rect.right - rect.left - iconSize) / 2,
        rect.top + (rect.bottom - rect.top - iconSize) / 2,
        rect.left + (rect.right - rect.left + iconSize) / 2,
        rect.top + (rect.bottom - rect.top + iconSize) / 2
    };
    const RECT indicatorIconRect = iconRect;
    const bool lt = (CurrentDockAppearance().contentTheme == 1);

    auto drawDesktopItem = [&](const DesktopItem& item, RECT target,
        const DesktopWidget* demoCollection = nullptr) {
        RECT bitmapTarget = target;
        const bool recycleBin = _wcsicmp(item.desktopIconClsid.c_str(),
            kDesktopIconClsidRecycleBin) == 0;
        if (recycleBin)
        {
            DrawDockControlBackground(ctx, target, 0, !lt);
            const int shortSide = std::max(1, static_cast<int>(std::min(
                target.right - target.left, target.bottom - target.top)));
            const int inset = std::max(1, static_cast<int>(std::round(shortSide * 0.16f)));
            InflateRect(&bitmapTarget, -inset, -inset);
        }
        const float alpha = item.isCut ? 0.4f : 1.0f;
        const bool useDemoIdentity = demoCollection
            ? ShouldUseDemoCollectionIdentity(demoCollection)
            : ShouldUseDemoIdentity(item);
        const std::wstring_view demoIdentity = item.layoutKey.empty()
            ? std::wstring_view(item.parsingName)
            : std::wstring_view(item.layoutKey);
        if (useDemoIdentity)
        {
            if (demoCollection)
                DrawDemoCollectionIdentityIcon(
                    ctx, *demoCollection, demoIdentity,
                    bitmapTarget, alpha);
            else
                DrawDemoIdentityIcon(
                    ctx, demoIdentity, bitmapTarget, alpha);
        }
        else if (item.iconState == IconState::Loading)
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapTarget, alpha, !recycleBin);
        else if (ID2D1Bitmap1* bitmap = GetOrCreateD2DBitmap(
                item.iconBitmap,
                !recycleBin &&
                    ShouldBeautifyIconBitmap(item.iconIsMediaThumbnail)))
            DrawIconBitmap(ctx, bitmap, bitmapTarget, alpha);
        else
            DrawPlaceholderIcon(ctx, item.sysIconIndex, bitmapTarget, alpha, !recycleBin);
        if (!useDemoIdentity &&
            ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut) &&
            item.iconState != IconState::Loading)
            DrawShortcutArrowOverlay(ctx, bitmapTarget, alpha);
    };

    if (entry.type == DockEntryType::DesktopItem)
    {
        size_t index = FindItemIndexByKey(entry.reference);
        if (index >= items_.size())
        {
            if (initialShellReadPending_)
                DrawPlaceholderIcon(ctx, -1, iconRect, 1.0f);
            return;
        }
        const float launchOffset =
            GetDockLaunchBounceOffset(index, iconSize);
        const float launchScale = GetDockLaunchPulseScale(index);
        float launchOffsetX = 0.0f;
        float launchOffsetY = 0.0f;
        switch (dockSettings_.position)
        {
        case DockPosition::Top:
            launchOffsetY = launchOffset;
            break;
        case DockPosition::Left:
            launchOffsetX = launchOffset;
            break;
        case DockPosition::Right:
            launchOffsetX = -launchOffset;
            break;
        case DockPosition::Bottom:
        default:
            launchOffsetY = -launchOffset;
            break;
        }
        D2D1_MATRIX_3X2_F previousTransform{};
        ctx->GetTransform(&previousTransform);
        const bool launchTransformApplied =
            std::abs(launchOffsetX) > 0.001f ||
            std::abs(launchOffsetY) > 0.001f ||
            std::abs(launchScale - 1.0f) > 0.001f;
        if (launchTransformApplied)
        {
            ctx->SetTransform(
                D2D1::Matrix3x2F::Scale(
                    launchScale, launchScale,
                    D2D1::Point2F(
                        (iconRect.left + iconRect.right) * 0.5f,
                        (iconRect.top + iconRect.bottom) * 0.5f)) *
                D2D1::Matrix3x2F::Translation(
                    launchOffsetX,
                    launchOffsetY) *
                previousTransform);
        }
        drawDesktopItem(items_[index], iconRect);
        if (state == 2)
            DrawDockSelectionIndicator(ctx, iconRect, lt);
        if (launchTransformApplied)
            ctx->SetTransform(previousTransform);

        const DockWindowVisualState windowState = GetDockWindowVisualState(index);
        if (windowState != DockWindowVisualState::Closed && state != 2)
        {
            const bool minimized = windowState == DockWindowVisualState::Minimized;
            const bool foreground = windowState == DockWindowVisualState::Foreground;
            const bool verticalDock = dockSettings_.position == DockPosition::Left ||
                dockSettings_.position == DockPosition::Right;
            const float dotRadius = std::max(1.7f, iconSize * 0.038f);
            const float indicatorGap = std::max(3.0f, iconSize * 0.06f);
            D2D1_POINT_2F center{};
            switch (dockSettings_.position)
            {
            case DockPosition::Top:
                center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                    static_cast<float>(indicatorIconRect.top) - indicatorGap);
                break;
            case DockPosition::Left:
                center = D2D1::Point2F(static_cast<float>(indicatorIconRect.left) - indicatorGap,
                    (rect.top + rect.bottom) * 0.5f);
                break;
            case DockPosition::Right:
                center = D2D1::Point2F(static_cast<float>(indicatorIconRect.right) + indicatorGap,
                    (rect.top + rect.bottom) * 0.5f);
                break;
            case DockPosition::Bottom:
            default:
                center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
                    static_cast<float>(indicatorIconRect.bottom) + indicatorGap);
                break;
            }

            ComPtr<ID2D1SolidColorBrush> indicatorBrush;
            const auto indicator =
                snowdesktop::dock_window_rules::
                    ResolveDockRunningIndicatorColor(
                        lt, foreground, minimized);
            const D2D1_COLOR_F indicatorColor =
                D2D1::ColorF(
                    indicator.red,
                    indicator.green,
                    indicator.blue,
                    indicator.alpha);
            if (SUCCEEDED(ctx->CreateSolidColorBrush(indicatorColor, &indicatorBrush)) &&
                indicatorBrush)
            {
                if (minimized)
                {
                    ctx->FillEllipse(D2D1::Ellipse(
                        center, dotRadius, dotRadius), indicatorBrush.Get());
                }
                else
                {
                    const float longHalf = foreground
                        ? std::max(6.0f, iconSize * 0.14f)
                        : std::max(4.0f, iconSize * 0.09f);
                    const float shortHalf = foreground
                        ? std::max(1.45f, iconSize * 0.031f)
                        : std::max(1.2f, iconSize * 0.026f);
                    const D2D1_RECT_F bar = verticalDock
                        ? D2D1::RectF(center.x - shortHalf, center.y - longHalf,
                            center.x + shortHalf, center.y + longHalf)
                        : D2D1::RectF(center.x - longHalf, center.y - shortHalf,
                            center.x + longHalf, center.y + shortHalf);
                    ctx->FillRoundedRectangle(D2D1::RoundedRect(
                        bar, shortHalf, shortHalf), indicatorBrush.Get());
                }
            }
        }
        return;
    }

    size_t widgetIndex = FindWidgetIndexById(entry.reference);
    if (widgetIndex >= widgets_.size()) return;
    const DesktopWidget& widget = widgets_[widgetIndex];
    if (entry.type == DockEntryType::FolderMapping)
    {
        const std::wstring iconCacheKey =
            ToUpperInvariant(entry.reference + L"\n" + widget.sourceFolderPath);
        const int requestedSize = GetMaximumShellIconBitmapSize();
        const auto local = dockFolderBitmapCache_.Read(iconCacheKey, std::to_wstring(requestedSize));
        if (!local.fresh)
        {
            const auto queuedAt = GetTickCount64();
            dockIconWork_.Submit(L"dock-folder:" + iconCacheKey + L"\n" + std::to_wstring(local.ticket),
                [path = widget.sourceFolderPath, requestedSize, queuedAt] {
                    const auto started = GetTickCount64();
                    auto result = std::make_shared<snowdesktop::BackgroundBitmap>();
                    result->bitmap = GetLocalIconResourceBitmap(path, result->size, requestedSize);
                    WriteDiagnosticLogEntry((L"Dock local folder icon: queueMs=" + std::to_wstring(started - queuedAt) +
                        L" readMs=" + std::to_wstring(GetTickCount64() - started) + L" bitmap=" +
                        std::to_wstring(result->bitmap ? 1 : 0) + L" path=" + path).c_str());
                    return result;
                }, [path = widget.sourceFolderPath, requestedSize, queuedAt] {
                    const auto started = GetTickCount64();
                    auto result = std::make_shared<snowdesktop::BackgroundBitmap>();
                    result->bitmap = CreateDockShellIconBitmap(path, result->size, requestedSize);
                    WriteDiagnosticLogEntry((L"Dock Shell folder icon: queueMs=" + std::to_wstring(started - queuedAt) +
                        L" readMs=" + std::to_wstring(GetTickCount64() - started) + L" bitmap=" +
                        std::to_wstring(result->bitmap ? 1 : 0) + L" path=" + path).c_str());
                    return result;
                }, [this, iconCacheKey, ticket = local.ticket](auto result, bool refined) {
                    auto bitmap = result && result->bitmap
                        ? CreateD2DBitmapFromHBitmap(result->bitmap, iconBeautifySettings_.enabled)
                        : ComPtr<ID2D1Bitmap1>{};
                    if (bitmap)
                    {
                        if (!dockFolderBitmapCache_.Publish(iconCacheKey, ticket, std::move(bitmap), refined)) return;
                    }
                    else if (!refined || !dockFolderBitmapCache_.PublishFailure(iconCacheKey, ticket, std::chrono::seconds(5))) return;
                    InvalidateDockRects();
                }, hwnd_, kBackgroundShellReadyMessage);
        }
        if (local.value && local.value->Get())
            DrawIconBitmap(ctx, local.value->Get(), iconRect);
        else if (local.fresh)
            DrawPrivacyFaIcon(ctx, iconRect, true);
        else
            DrawPlaceholderIcon(ctx, -1, iconRect, 1.0f, true);
        if (ShouldDrawShortcutArrow(true, false))
            DrawShortcutArrowOverlay(
                ctx, iconRect, 1.0f);
        if (state == 2)
            DrawDockSelectionIndicator(
                ctx, iconRect, lt);
        return;
    }
    const auto collectionLayout =
        snowdesktop::dock_collection_icon_rules::
            CalculateLayout(iconRect);
    DrawDockControlBackground(
        ctx, collectionLayout.background,
        0, !lt);
    for (size_t i = 0; i < std::min<size_t>(4, widget.itemKeys.size()); ++i)
    {
        size_t itemIndex = FindItemIndexByKey(widget.itemKeys[i]);
        int col = static_cast<int>(i % 2);
        int row = static_cast<int>(i / 2);
        const RECT cell =
            snowdesktop::dock_collection_icon_rules::
                CellRect(
                    collectionLayout,
                    col, row);
        if (itemIndex < items_.size())
            drawDesktopItem(items_[itemIndex], cell, &widget);
        else if (initialShellReadPending_)
            DrawPlaceholderIcon(ctx, -1, cell, 1.0f);
    }
    if (state == 2)
        DrawDockSelectionIndicator(ctx, iconRect, lt);
}

void DesktopApp::DrawDockRunningApp(ID2D1DeviceContext* ctx,
    const DockRunningAppInfo& app, RECT rect, int state)
{
    if (!ctx) return;
    const int scaledSpacing = std::max(1, static_cast<int>(std::round(
        kDockSpacing * ClampDockScale(dockSettings_.thicknessScale))));
    const int iconSize = std::max(1, static_cast<int>(std::min(
        rect.right - rect.left, rect.bottom - rect.top)) - scaledSpacing);
    RECT iconRect{
        rect.left + (rect.right - rect.left - iconSize) / 2,
        rect.top + (rect.bottom - rect.top - iconSize) / 2,
        rect.left + (rect.right - rect.left + iconSize) / 2,
        rect.top + (rect.bottom - rect.top + iconSize) / 2
    };
    const bool lt = (CurrentDockAppearance().contentTheme == 1);
    if (generalSettings_.demoModeEnabled &&
        demoIdentityAssetsAvailable_)
    {
        const std::wstring_view identity = app.identityKey.empty()
            ? std::wstring_view(app.executablePath)
            : std::wstring_view(app.identityKey);
        DrawDemoIdentityIcon(ctx, identity, iconRect);
    }
    else if (ID2D1Bitmap1* bitmap = GetOrCreateD2DBitmap(app.iconBitmap))
        DrawIconBitmap(ctx, bitmap, iconRect);
    else
        DrawPlaceholderIcon(ctx, -1, iconRect, 1.0f, true);
    if (state == 2)
    {
        DrawDockSelectionIndicator(ctx, iconRect, lt);
        return;
    }

    const bool verticalDock = dockSettings_.position == DockPosition::Left ||
        dockSettings_.position == DockPosition::Right;
    const float dotRadius = std::max(1.7f, iconSize * 0.038f);
    const float indicatorGap = std::max(3.0f, iconSize * 0.06f);
    D2D1_POINT_2F center{};
    switch (dockSettings_.position)
    {
    case DockPosition::Top:
        center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
            static_cast<float>(iconRect.top) - indicatorGap);
        break;
    case DockPosition::Left:
        center = D2D1::Point2F(static_cast<float>(iconRect.left) - indicatorGap,
            (rect.top + rect.bottom) * 0.5f);
        break;
    case DockPosition::Right:
        center = D2D1::Point2F(static_cast<float>(iconRect.right) + indicatorGap,
            (rect.top + rect.bottom) * 0.5f);
        break;
    case DockPosition::Bottom:
    default:
        center = D2D1::Point2F((rect.left + rect.right) * 0.5f,
            static_cast<float>(iconRect.bottom) + indicatorGap);
        break;
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    const auto indicator =
        snowdesktop::dock_window_rules::
            ResolveDockRunningIndicatorColor(
                lt, app.foreground, app.minimized);
    const D2D1_COLOR_F color =
        D2D1::ColorF(
            indicator.red,
            indicator.green,
            indicator.blue,
            indicator.alpha);
    if (FAILED(ctx->CreateSolidColorBrush(color, &brush)) || !brush) return;
    if (app.minimized)
    {
        ctx->FillEllipse(D2D1::Ellipse(center, dotRadius, dotRadius), brush.Get());
        return;
    }

    const float longHalf = app.foreground
        ? std::max(6.0f, iconSize * 0.14f)
        : std::max(4.0f, iconSize * 0.09f);
    const float shortHalf = app.foreground
        ? std::max(1.45f, iconSize * 0.031f)
        : std::max(1.2f, iconSize * 0.026f);
    const D2D1_RECT_F bar = verticalDock
        ? D2D1::RectF(center.x - shortHalf, center.y - longHalf,
            center.x + shortHalf, center.y + longHalf)
        : D2D1::RectF(center.x - longHalf, center.y - shortHalf,
            center.x + longHalf, center.y + shortHalf);
    ctx->FillRoundedRectangle(
        D2D1::RoundedRect(bar, shortHalf, shortHalf), brush.Get());
}
