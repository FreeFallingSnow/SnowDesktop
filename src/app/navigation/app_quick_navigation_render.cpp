#include "ui/render/app_font.h"
#include "app/app.h"
#include "navigation/quick_navigation_animation_rules.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/l10n.h"
#include "diagnostics/diagnostic_log.h"
#include "common/utils.h"
#include "ui/input/text_input_window.h"
#include "quick_navigation_helpers.h"
#include "navigation/quick_navigation_rules.h"
#include "quick_navigation_theme.h"
#include "dock/dock_genie_rules.h"
#include "navigation/quick_navigation_genie_rules.h"
#include "navigation/quick_navigation_attraction_rules.h"
#include "navigation/quick_navigation_attraction_renderer.h"
#include "ui/render/smooth_corner_geometry.h"

// Quick-navigation DirectWrite and DirectComposition rendering resources.

void DesktopApp::ClearQuickNavigationDeformation()
{
    quickNavigation_->backdropCompositor.ClearGenieTransform();
    if (quickNavigation_->genieStrips.empty() && !quickNavigation_->attractionSurface)
        return;
    if (quickNavigation_->dcompVisual)
    {
        quickNavigation_->dcompVisual->RemoveAllVisuals();
        quickNavigation_->dcompVisual->SetContent(quickNavigation_->dcompSurface.Get());
        quickNavigation_->dcompVisual->SetTransform(quickNavigation_->dcompScaleTransform.Get());
    }
    quickNavigation_->genieStrips.clear();
    quickNavigation_->attractionSurface.Reset();
    quickNavigation_->attractionSurfaceSize = {};
    quickNavigation_->genieStripEdge = -1;
}

bool DesktopApp::ApplyQuickNavigationGenieFrame(float collapsed, float opacity)
{
    namespace genie = snowdesktop::dock_genie;
    namespace navigation = snowdesktop::quick_navigation_animation_rules;
    if (!quickNavigation_->dcompDevice || !quickNavigation_->dcompVisual ||
        !quickNavigation_->dcompSurface || !quickNavigation_->dcompEffect)
        return false;

    if (quickNavigation_->genieStripEdge != quickNavigation_->animationDockEdge ||
        quickNavigation_->attractionSurface)
        ClearQuickNavigationDeformation();
    const auto edge = static_cast<genie::Edge>(quickNavigation_->animationDockEdge);
    // A resize can retain a larger backing surface. Warp only the painted panel,
    // including when dismissal interrupts the transition before its final frame.
    const float width = static_cast<float>(std::max<LONG>(1,
        quickNavigation_->rect.right - quickNavigation_->rect.left));
    const float height = static_cast<float>(std::max<LONG>(1,
        quickNavigation_->rect.bottom - quickNavigation_->rect.top));
    const auto rect = [](const RECT& value) {
        return genie::Rect{static_cast<double>(value.left), static_cast<double>(value.top),
            static_cast<double>(value.right), static_cast<double>(value.bottom)};
    };
    const auto projectionFor = [&](size_t index) {
        return navigation::GenieProjection(rect(quickNavigation_->rect),
            rect(quickNavigation_->animationDockRect), edge, collapsed, width, height,
            index, quickNavigation_->hostRect.left, quickNavigation_->hostRect.top);
    };
    const auto clipFor = [&](size_t index) {
        return navigation::GenieRasterBandClip(rect(quickNavigation_->rect),
            rect(quickNavigation_->animationDockRect), edge, collapsed, width, height,
            index, quickNavigation_->hostRect.left, quickNavigation_->hostRect.top,
            quickNavigation_->hostRect.right - quickNavigation_->hostRect.left,
            quickNavigation_->hostRect.bottom - quickNavigation_->hostRect.top);
    };
    HRESULT hr = S_OK;
    if (quickNavigation_->genieStrips.empty())
    {
        // Destination clip -> projection -> cropped bitmap. The source crop
        // precedes perspective, so its intermediate bitmap never crosses w=0.
        // Reuse the same live surface and visuals on every frame.
        quickNavigation_->genieStrips.reserve(genie::StripCount * 3);
        for (size_t i = 0; i < genie::StripCount && SUCCEEDED(hr); ++i)
        {
            ComPtr<IDCompositionVisual2> strip;
            hr = quickNavigation_->dcompDevice->CreateVisual(&strip);
            if (FAILED(hr)) break;
            quickNavigation_->genieStrips.push_back(strip);
            hr = strip->SetBorderMode(DCOMPOSITION_BORDER_MODE_HARD);
            ComPtr<IDCompositionVisual2> projectionVisual;
            if (SUCCEEDED(hr)) hr = quickNavigation_->dcompDevice->CreateVisual(&projectionVisual);
            if (SUCCEEDED(hr)) quickNavigation_->genieStrips.push_back(projectionVisual);
            if (SUCCEEDED(hr)) hr = projectionVisual->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
            if (SUCCEEDED(hr)) hr = projectionVisual->SetBitmapInterpolationMode(
                DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
            ComPtr<IDCompositionVisual2> content;
            if (SUCCEEDED(hr)) hr = quickNavigation_->dcompDevice->CreateVisual(&content);
            if (SUCCEEDED(hr)) quickNavigation_->genieStrips.push_back(content);
            if (SUCCEEDED(hr)) hr = content->SetContent(quickNavigation_->dcompSurface.Get());
            const auto projection = projectionFor(i);
            if (SUCCEEDED(hr)) hr = content->SetOffsetX(-projection.sourceX);
            if (SUCCEEDED(hr)) hr = content->SetOffsetY(-projection.sourceY);
            if (SUCCEEDED(hr)) hr = content->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
            if (SUCCEEDED(hr)) hr = content->SetBitmapInterpolationMode(
                DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (SUCCEEDED(hr)) hr = projectionVisual->AddVisual(content.Get(), TRUE, nullptr);
            if (SUCCEEDED(hr)) hr = strip->AddVisual(projectionVisual.Get(), TRUE, nullptr);
            if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->AddVisual(strip.Get(), TRUE, nullptr);
        }
        if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetContent(nullptr);
        if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetTransform(D2D1::Matrix3x2F::Identity());
        if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetOffsetX(0.0f);
        if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetOffsetY(0.0f);
        if (FAILED(hr))
        {
            ClearQuickNavigationDeformation();
            return false;
        }
        quickNavigation_->genieStripEdge = quickNavigation_->animationDockEdge;
    }
    for (size_t i = 0; i < genie::StripCount && SUCCEEDED(hr); ++i)
    {
        // Clipping before perspective leaves fractional independently sampled
        // joins. Partition the final pixels on the parent instead, preserving
        // soft bitmap edges without gaps or double-blended translucent bands.
        const auto bounds = clipFor(i);
        hr = quickNavigation_->genieStrips[i * 3]->SetClip(D2D1::RectF(
            static_cast<float>(bounds.left), static_cast<float>(bounds.top),
            static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)));
        const auto matrix = projectionFor(i);
        const auto source = navigation::GenieSourceBandClip(matrix, bounds, edge, width, height);
        if (SUCCEEDED(hr)) hr = quickNavigation_->genieStrips[i * 3 + 2]->SetClip(D2D1::RectF(
            static_cast<float>(source.left), static_cast<float>(source.top),
            static_cast<float>(source.right), static_cast<float>(source.bottom)));
        ComPtr<IDCompositionVisual3> projectedStrip;
        if (SUCCEEDED(hr)) hr = quickNavigation_->genieStrips[i * 3 + 1].As(&projectedStrip);
        if (SUCCEEDED(hr)) hr = projectedStrip->SetTransform(D2D1_MATRIX_4X4_F{
            matrix.m11, matrix.m12, 0.0f, matrix.m14,
            matrix.m21, matrix.m22, 0.0f, matrix.m24,
            0.0f, 0.0f, 1.0f, 0.0f,
            matrix.m41, matrix.m42, 0.0f, matrix.m44});
    }
    if (SUCCEEDED(hr)) hr = quickNavigation_->dcompEffect->SetOpacity(opacity);
    if (FAILED(hr)) return false;
    if (quickNavigation_->glassTheme && quickNavigation_->backdropCompositor.IsAvailable())
    {
        RECT panel = quickNavigation_->rect;
        RECT dock = quickNavigation_->animationDockRect;
        OffsetRect(&panel, -quickNavigation_->hostRect.left, -quickNavigation_->hostRect.top);
        OffsetRect(&dock, -quickNavigation_->hostRect.left, -quickNavigation_->hostRect.top);
        if (!quickNavigation_->backdropCompositor.SetGenieTransform(panel, dock,
                quickNavigation_->animationDockEdge, collapsed, opacity))
            return false;
    }
    return CommitQuickNavigationCompositionFrame();
}

bool DesktopApp::ApplyQuickNavigationAttractionFrame(float collapsed, float opacity)
{
    namespace attraction = snowdesktop::dock_attraction;
    namespace navigation = snowdesktop::quick_navigation_animation_rules;
    if (!quickNavigation_->dcompDevice || !quickNavigation_->dcompVisual || !quickNavigation_->attractionBitmap ||
        !quickNavigation_->dcompEffect)
        return false;
    if (!quickNavigation_->genieStrips.empty()) ClearQuickNavigationDeformation();
    HRESULT hr = S_OK;
    const SIZE size{std::max<LONG>(1, quickNavigation_->hostRect.right - quickNavigation_->hostRect.left),
        std::max<LONG>(1, quickNavigation_->hostRect.bottom - quickNavigation_->hostRect.top)};
    if (!quickNavigation_->attractionSurface || size.cx != quickNavigation_->attractionSurfaceSize.cx ||
        size.cy != quickNavigation_->attractionSurfaceSize.cy)
    {
        quickNavigation_->attractionSurface.Reset();
        hr = quickNavigation_->dcompDevice->CreateSurface(static_cast<UINT>(size.cx), static_cast<UINT>(size.cy),
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &quickNavigation_->attractionSurface);
        if (FAILED(hr)) return false;
        quickNavigation_->attractionSurfaceSize = size;
    }
    const auto rect = [](const RECT& value) {
        return attraction::Rect{static_cast<double>(value.left), static_cast<double>(value.top),
            static_cast<double>(value.right), static_cast<double>(value.bottom)};
    };
    const auto frame = attraction::MeshFrame(rect(quickNavigation_->rect),
        rect(quickNavigation_->animationDockRect), collapsed);
    ComPtr<ID2D1DeviceContext> context;
    POINT offset{};
    hr = quickNavigation_->attractionSurface->BeginDraw(nullptr, IID_PPV_ARGS(&context), &offset);
    if (FAILED(hr)) return false;
    context->SetDpi(96, 96);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    context->Clear(D2D1::ColorF(0, 0.f));
    hr = navigation::DrawAttractionMesh(context.Get(), quickNavigation_->attractionBitmap.Get(), frame,
        quickNavigation_->hostRect.left, quickNavigation_->hostRect.top,
        quickNavigation_->rect.right - quickNavigation_->rect.left,
        quickNavigation_->rect.bottom - quickNavigation_->rect.top);
    context.Reset();
    const HRESULT endResult = quickNavigation_->attractionSurface->EndDraw();
    if (SUCCEEDED(hr)) hr = endResult;
    if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetContent(quickNavigation_->attractionSurface.Get());
    if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetTransform(D2D1::Matrix3x2F::Identity());
    if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetOffsetX(0.0f);
    if (SUCCEEDED(hr)) hr = quickNavigation_->dcompVisual->SetOffsetY(0.0f);
    if (SUCCEEDED(hr)) hr = quickNavigation_->dcompEffect->SetOpacity(opacity);
    if (FAILED(hr)) return false;
    if (quickNavigation_->glassTheme && quickNavigation_->backdropCompositor.IsAvailable())
    {
        RECT panel = quickNavigation_->rect, dock = quickNavigation_->animationDockRect;
        OffsetRect(&panel, -quickNavigation_->hostRect.left, -quickNavigation_->hostRect.top);
        OffsetRect(&dock, -quickNavigation_->hostRect.left, -quickNavigation_->hostRect.top);
        if (!quickNavigation_->backdropCompositor.SetAttractionTransform(panel, dock, collapsed, opacity))
            return false;
    }
    return CommitQuickNavigationCompositionFrame();
}

void DesktopApp::EnsureQuickNavTextFormats()
{
    if (!dwriteFactory_)
        return;

    const float tabSize = static_cast<float>(QuickNavScale(navigationSettings_.layout.fontSize));
    const float itemSize = static_cast<float>(QuickNavScale(navigationSettings_.layout.fontSize));
    const float pathSize = static_cast<float>(QuickNavScale(navigationSettings_.layout.secondaryFontSize));
    const float fluentSize = static_cast<float>(QuickNavScale(24));
    // Both content themes share regular text weight for consistent readability.
    const float itemWeightValue = 400.0f;
    const DWRITE_FONT_WEIGHT itemWeight =
        static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(itemWeightValue));

    auto createOrRecreate = [&](ComPtr<IDWriteTextFormat>& fmt, const wchar_t* family,
        float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT hAlign) {
        const bool stale = !fmt ||
            std::abs(fmt->GetFontSize() - size) > 0.01f ||
            fmt->GetFontWeight() != weight;
        if (stale)
        {
            fmt.Reset();
            snowdesktop::app_fonts::CreateTextFormat(dwriteFactory_, family, weight,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
            if (fmt)
            {
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                fmt->SetTextAlignment(hAlign);
            }
        }
    };

    createOrRecreate(quickNavigation_->tabTextFormat, L"Segoe UI", tabSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    createOrRecreate(quickNavigation_->itemTextFormat, L"Segoe UI Variable", itemSize, itemWeight, DWRITE_TEXT_ALIGNMENT_LEADING);
    createOrRecreate(quickNavigation_->searchTextFormat, L"Segoe UI", static_cast<float>(QuickNavScale(navigationSettings_.layout.searchFontSize)), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    createOrRecreate(quickNavigation_->pathTextFormat, L"Segoe UI", pathSize, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!quickNavigation_->fluentTextFormat ||
        std::abs(
            quickNavigation_->fluentTextFormat->GetFontSize() -
                fluentSize) > 0.01f)
    {
        quickNavigation_->fluentTextFormat.Reset();
        quickNavigation_->fluentTextFormat =
            ComPtr<IDWriteTextFormat>(
                CreateFluentTextFormat(
                    dwriteFactory_.Get(),
                    fluentSize));
        if (quickNavigation_->fluentTextFormat)
        {
            quickNavigation_->fluentTextFormat->
                SetTextAlignment(
                    DWRITE_TEXT_ALIGNMENT_CENTER);
            quickNavigation_->fluentTextFormat->
                SetParagraphAlignment(
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            quickNavigation_->fluentTextFormat->
                SetWordWrapping(
                    DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    ComPtr<IDWriteTextFormat3> itemFormat3;
    if (quickNavigation_->itemTextFormat && SUCCEEDED(quickNavigation_->itemTextFormat.As(&itemFormat3)) && itemFormat3)
    {
        DWRITE_FONT_AXIS_VALUE axes[] = {
            { DWRITE_FONT_AXIS_TAG_WEIGHT, itemWeightValue }
        };
        itemFormat3->SetFontAxisValues(axes, ARRAYSIZE(axes));
    }
}

/**
 * @brief 重置快捷导航 DComp 表面与渲染缓存（设备丢失或窗口销毁时调用）。
 */
void DesktopApp::ResetQuickNavCompositionResources()
{
    ClearQuickNavigationDeformation();
    StopQuickNavigationAnimationTimeline();
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    quickNavigation_->sysIconCache.clear();
    quickNavigation_->appIconCache.clear();
    if (quickNavigation_->dcompVisual)
        quickNavigation_->dcompVisual->SetContent(nullptr);
    quickNavigation_->dcompSurface.Reset();
    quickNavigation_->attractionBitmap.Reset();
    quickNavigation_->compWidth = 0;
    quickNavigation_->compHeight = 0;
    quickNavigation_->compositionCommitPending = false;
}

/**
 * @brief 快捷导航 DComp 渲染失败后重置表面并安排一次恢复重绘。
 */
void DesktopApp::RecoverQuickNavCompositionFailure(const wchar_t* stage, HRESULT hr)
{
    if (RequestGraphicsDeviceRecovery(stage, hr))
        return;
    wchar_t buf[192];
    wsprintfW(buf, L"QuickNav %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render", static_cast<unsigned>(hr));
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);

    ResetQuickNavCompositionResources();
    EnsureUiAnimationFrame();

    if (!quickNavigation_->compositionRenderRecoveryPending && quickNavigation_->hwnd && IsWindow(quickNavigation_->hwnd))
    {
        quickNavigation_->compositionRenderRecoveryPending = true;
        InvalidateRect(quickNavigation_->hwnd, nullptr, FALSE);
    }
}

/**
 * @brief 创建或调整快捷导航 DComp 表面大小。
 * @return S_OK 成功，否则为 HRESULT 错误码
 */
HRESULT DesktopApp::CreateOrResizeQuickNavCompositionSurface()
{
    if (!d2dDevice_ || !quickNavigation_->hwnd || !IsWindow(quickNavigation_->hwnd))
        return E_UNEXPECTED;

    if (!quickNavigation_->dcompDevice)
    {
        HRESULT hr = DCompositionCreateDevice2(
            d2dDevice_.Get(),
            __uuidof(IDCompositionDesktopDevice),
            reinterpret_cast<void**>(
                quickNavigation_->dcompDevice.GetAddressOf()));
        if (FAILED(hr) || !quickNavigation_->dcompDevice)
        {
            wchar_t buf[160];
            wsprintfW(
                buf,
                L"QuickNav create isolated DComp device FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);
            return FAILED(hr) ? hr : E_FAIL;
        }
    }

    RECT surfaceRect = quickNavigation_->rect;
    if (quickNavigation_->animation.Resize().IsAnimating())
    {
        // Reuse one surface for the complete transition instead of reallocating
        // a GPU surface on every frame. Transparent pixels are cleared on paint.
        const auto bounds = quickNavigation_->animation.Resize().Envelope();
        surfaceRect = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    }
    surfaceRect = QuickNavigationRetainedAllocation(
        MakeRect(0, 0, static_cast<LONG>(quickNavigation_->compWidth), static_cast<LONG>(quickNavigation_->compHeight)),
        MakeRect(0, 0, surfaceRect.right - surfaceRect.left, surfaceRect.bottom - surfaceRect.top));
    const UINT width = static_cast<UINT>(
        std::max<LONG>(
            1,
            surfaceRect.right - surfaceRect.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(
            1,
            surfaceRect.bottom - surfaceRect.top));

    if (!quickNavigation_->dcompTarget)
    {
        HRESULT hr = quickNavigation_->dcompDevice->CreateTargetForHwnd(quickNavigation_->hwnd, FALSE, &quickNavigation_->dcompTarget);
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(buf, L"QuickNav CreateTargetForHwnd FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);
            return hr;
        }
    }
    if (!quickNavigation_->dcompVisual)
    {
        HRESULT hr = quickNavigation_->dcompDevice->CreateVisual(&quickNavigation_->dcompVisual);
        if (FAILED(hr) || !quickNavigation_->dcompVisual)
        {
            wchar_t buf[128];
            wsprintfW(buf, L"QuickNav CreateVisual FAILED hr=0x%08X", static_cast<unsigned>(hr));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);
            return hr;
        }
        quickNavigation_->dcompTarget->SetRoot(quickNavigation_->dcompVisual.Get());
    }
    if (!quickNavigation_->dcompEffect)
    {
        HRESULT hr =
            quickNavigation_->dcompDevice->CreateEffectGroup(
                &quickNavigation_->dcompEffect);
        if (FAILED(hr) || !quickNavigation_->dcompEffect)
        {
            wchar_t buf[128];
            wsprintfW(
                buf,
                L"QuickNav CreateEffectGroup FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);
            return FAILED(hr) ? hr : E_FAIL;
        }
        hr = quickNavigation_->dcompVisual->SetEffect(
            quickNavigation_->dcompEffect.Get());
        if (FAILED(hr))
        {
            wchar_t buf[128];
            wsprintfW(
                buf,
                L"QuickNav SetEffect FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);
            return hr;
        }
    }
    if (!quickNavigation_->dcompScaleTransform)
    {
        HRESULT hr = quickNavigation_->dcompDevice->CreateScaleTransform(
            &quickNavigation_->dcompScaleTransform);
        if (FAILED(hr) || !quickNavigation_->dcompScaleTransform)
        {
            wchar_t buf[128];
            wsprintfW(
                buf,
                L"QuickNav CreateScaleTransform FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);
            return FAILED(hr) ? hr : E_FAIL;
        }
        hr = quickNavigation_->dcompVisual->SetTransform(
            quickNavigation_->dcompScaleTransform.Get());
        if (FAILED(hr))
        {
            quickNavigation_->dcompScaleTransform.Reset();
            return hr;
        }
    }

    if (quickNavigation_->dcompSurface && quickNavigation_->compWidth == width && quickNavigation_->compHeight == height)
        return S_OK;

    ClearQuickNavigationDeformation();
    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = quickNavigation_->dcompDevice->CreateSurface(width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    if (FAILED(hr))
    {
        wchar_t buf[128];
        wsprintfW(buf, L"QuickNav CreateSurface %ux%u FAILED hr=0x%08X", width, height, static_cast<unsigned>(hr));
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, buf, DiagnosticLogLevel::Warning);
        return hr;
    }
    // Keep the previously painted content attached until the replacement has
    // completed EndDraw. An allocation is not a presentable frame yet.
    quickNavigation_->dcompSurface = surface;
    quickNavigation_->compWidth = width;
    quickNavigation_->compHeight = height;
    return S_OK;
}

/**
 * @brief 绘制快捷导航窗口（含搜索栏、标签页、列表、滚动条）
 * @param hwnd 窗口句柄
 */

void DesktopApp::PaintQuickNavigationWindow(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc)
        return;
    // hdc 仅用于验证绘制区域；实际绘制走 DComp surface。
    (void)hdc;
    if (graphicsDeviceRecovery_.Pending())
    {
        EndPaint(hwnd, &ps);
        return;
    }
    if (quickNavigation_->compositionPaintInProgress)
    {
        EndPaint(hwnd, &ps);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    quickNavigation_->compositionPaintInProgress = true;
    struct QuickNavigationPaintScope final
    {
        bool& active;
        ~QuickNavigationPaintScope()
        {
            active = false;
        }
    } paintScope{
        quickNavigation_->compositionPaintInProgress
    };


    IDCompositionSurface* previousSurface = quickNavigation_->dcompSurface.Get();
    HRESULT hr = CreateOrResizeQuickNavCompositionSurface();
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"CreateOrResizeQuickNavCompositionSurface", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = quickNavigation_->dcompSurface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext), &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverQuickNavCompositionFailure(L"BeginDraw", hr);
        EndPaint(hwnd, &ps);
        return;
    }

    ComPtr<ID2D1DeviceContext> ctx;
    ctx.Attach(rawContext);
    ctx->SetDpi(96.0f, 96.0f);
    ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    // 内容 surface 只按目标面板大小分配；顶层宿主可以覆盖完整动画路径。
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(
            updateOffset.x -
            quickNavigation_->rect.left),
        static_cast<float>(
            updateOffset.y -
            quickNavigation_->rect.top)));
    ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // 文本抗锯齿沿用 DComp 默认（与桌面一致），避免在 alpha 表面上强制 ClearType 产生彩色毛边。
    ctx->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    brushCache_.clear();
    brushCacheContext_ = ctx.Get();

    HRESULT contentResult = S_OK;
    if (quickNavigation_->animation.GetEffect() ==
        snowdesktop::quick_navigation_animation_rules::Effect::Attraction)
    {
        // Keep one ordinary image of the panel. The animated mesh samples this
        // bitmap directly; it never samples separately clipped DComp children.
        ComPtr<ID2D1DeviceContext> sourceContext;
        contentResult = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &sourceContext);
        const auto bitmapSize = quickNavigation_->attractionBitmap ? quickNavigation_->attractionBitmap->GetPixelSize() : D2D1_SIZE_U{};
        if (SUCCEEDED(contentResult) && (!quickNavigation_->attractionBitmap ||
                bitmapSize.width != quickNavigation_->compWidth || bitmapSize.height != quickNavigation_->compHeight))
        {
            quickNavigation_->attractionBitmap.Reset();
            contentResult = sourceContext->CreateBitmap(D2D1::SizeU(quickNavigation_->compWidth, quickNavigation_->compHeight),
                nullptr, 0, D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96),
                &quickNavigation_->attractionBitmap);
        }
        if (SUCCEEDED(contentResult))
        {
            sourceContext->SetTarget(quickNavigation_->attractionBitmap.Get());
            sourceContext->SetDpi(96, 96);
            sourceContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            sourceContext->BeginDraw();
            sourceContext->Clear(D2D1::ColorF(0, 0.f));
            sourceContext->SetTransform(D2D1::Matrix3x2F::Translation(
                -static_cast<float>(quickNavigation_->rect.left), -static_cast<float>(quickNavigation_->rect.top)));
            brushCacheContext_ = sourceContext.Get();
            DrawQuickNavigationSurface(sourceContext.Get());
            contentResult = sourceContext->EndDraw();
            sourceContext->SetTarget(nullptr);
            brushCache_.clear();
            brushCacheContext_ = ctx.Get();
            if (SUCCEEDED(contentResult))
                ctx->DrawBitmap(quickNavigation_->attractionBitmap.Get(), D2D1::RectF(
                    static_cast<float>(quickNavigation_->rect.left), static_cast<float>(quickNavigation_->rect.top),
                    static_cast<float>(quickNavigation_->rect.left) + static_cast<float>(quickNavigation_->compWidth),
                    static_cast<float>(quickNavigation_->rect.top) + static_cast<float>(quickNavigation_->compHeight)));
        }
    }
    else
    {
        quickNavigation_->attractionBitmap.Reset();
        DrawQuickNavigationSurface(ctx.Get());
    }
    ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    ctx.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = quickNavigation_->dcompSurface->EndDraw();
    if (SUCCEEDED(hr)) hr = contentResult;
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"EndDraw", hr);
        EndPaint(hwnd, &ps);
        return;
    }
    if (previousSurface != quickNavigation_->dcompSurface.Get())
    {
        hr = quickNavigation_->dcompVisual->SetContent(quickNavigation_->dcompSurface.Get());
        if (FAILED(hr))
        {
            RecoverQuickNavCompositionFailure(L"Publish painted surface", hr);
            EndPaint(hwnd, &ps);
            return;
        }
    }
    // A caret/hover repaint must not replace the backdrop's running native
    // opening/closing transform with the logical timeline's static pose.
    // Positioning stops that timeline first whenever the geometry changes.
    if (!quickNavigation_->animationCompositorDriven)
        UpdateQuickNavigationBackdrop();
    ApplyQuickNavigationAnimationFrame();
    if (!CommitQuickNavigationCompositionFrame())
    {
        RecoverQuickNavCompositionFailure(
            L"Queue Paint Commit", E_FAIL);
        EndPaint(hwnd, &ps);
        return;
    }
    if (!FlushPendingQuickNavigationCompositionCommit())
    {
        EndPaint(hwnd, &ps);
        return;
    }
    quickNavigation_->backdropCompositor.CommitVisualChanges();
    quickNavigation_->compositionRenderRecoveryPending = false;

    EndPaint(hwnd, &ps);
}

void DesktopApp::DrawQuickNavigationSurface(ID2D1DeviceContext* context)
{
    ComPtr<ID2D1DeviceContext> ctx = context;
    const QuickNavTheme t = ResolveQuickNavTheme(quickNavigation_->lightTheme, navigationSettings_, quickNavigation_->glassTheme);
    const RECT& overlay = quickNavigation_->rect;
    const float windowCornerRadius =
        static_cast<float>(
            QuickNavScale(navigationSettings_.layout.cornerRadius));
    const auto windowClipGeometry = snowdesktop::smooth_corners::Geometry(
        d2dFactory_.Get(), ToD2DRect(overlay), windowCornerRadius);
    bool windowClipPushed = false;
    if (windowClipGeometry)
    {
        ctx->PushLayer(
            D2D1::LayerParameters(
                ToD2DRect(overlay),
                windowClipGeometry.Get()),
            nullptr);
        windowClipPushed = true;
    }
    const float borderAlpha = quickNavigation_->animation.IsAnimating()
        ? 0.0f : std::clamp(quickNavigation_->appearance.widgetBorderAlpha, 0.0f, 1.0f);
    auto background = quickNavigation_->appearance;
    background.widgetEdgeHighlightEnabled = false;
    background.acrylicEnabled = false;
    DrawWidgetPanelBackground(ctx.Get(), overlay, windowCornerRadius,
        D2D1::ColorF(background.widgetBgR, background.widgetBgG, background.widgetBgB, background.widgetAlpha),
        D2D1::ColorF(0, 0, 0, 0), false, background.widgetBorderWidth,
        &background, false, 0, static_cast<float>(QuickNavScale(100)) / 100.0f);
    if (quickNavigation_->appearance.glassEnabled && quickNavigation_->appearance.acrylicEnabled)
    {
        POINT screenOrigin{};
        if (!quickNavigation_->preview) ClientToScreen(quickNavigation_->hwnd, &screenOrigin);
        DrawAcrylicNoise(ctx.Get(), overlay, windowCornerRadius,
            quickNavigation_->appearance.contentTheme == 1, screenOrigin);
    }
    const RECT searchRect = GetQuickNavigationSearchRect(overlay);
    DrawD2DFilledRectangle(ctx.Get(), searchRect,
        ToD2DColor(t.searchBg), D2D1::ColorF(0,0,0,0.f));
    if (overlay.bottom > searchRect.bottom)
        DrawD2DSeparator(ctx.Get(), MakeRect(searchRect.left, searchRect.bottom - QuickNavScale(1), searchRect.right, searchRect.bottom),
            ToD2DColor(t.searchBorder));
    const float windowBorderStrokeWidth = std::clamp(quickNavigation_->appearance.widgetBorderWidth, kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    const float windowBorderInset =
        windowBorderStrokeWidth * 0.5f;
    const D2D1_RECT_F windowBorderRect =
        D2D1::RectF(
            static_cast<float>(overlay.left) +
                windowBorderInset,
            static_cast<float>(overlay.top) +
                windowBorderInset,
            static_cast<float>(overlay.right) -
                windowBorderInset,
            static_cast<float>(overlay.bottom) -
                windowBorderInset);
    DrawD2DRoundedRectangle(ctx.Get(),
        windowBorderRect,
        std::max(
            0.0f,
            windowCornerRadius - windowBorderInset),
        D2D1::ColorF(0, 0, 0, 0),
        D2D1::ColorF(quickNavigation_->appearance.widgetBorderR,
            quickNavigation_->appearance.widgetBorderG,
            quickNavigation_->appearance.widgetBorderB, borderAlpha),
        windowBorderStrokeWidth);
    (void)DrawWidgetPanelEdgeHighlight(ctx.Get(), overlay, windowCornerRadius,
        D2D1::ColorF(background.widgetBgR, background.widgetBgG, background.widgetBgB, background.widgetAlpha),
        &quickNavigation_->appearance, static_cast<float>(QuickNavScale(100)) / 100.0f);

    const bool searching = !GetQuickNavigationEffectiveSearchText().empty();
    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    QuickNavigationContentModel contentModel =
        BuildQuickNavigationContentModel();
    const std::vector<QuickNavigationEntry>& entries =
        contentModel.entries;
    quickNavigation_->hoverRegions.clear();
    auto registerHoverRegion = [this](
        RECT bounds,
        QuickNavigationPointerTargetKind kind,
        size_t index = 0) {
        if (!IsRectEmpty(&bounds))
            quickNavigation_->hoverRegions.push_back(
                { bounds, { kind, index } });
    };
    auto registerClippedHoverRegion =
        [&registerHoverRegion](
            RECT bounds, const RECT& clip,
            QuickNavigationPointerTargetKind kind,
            size_t index = 0) {
            RECT visible{};
            if (IntersectRect(
                    &visible, &bounds, &clip))
                registerHoverRegion(
                    visible, kind, index);
        };
    if (!searching && !UseQuickNavigationList())
        UpdateQuickNavTabWidths();
    snowdesktop::quick_navigation_rules::SetTabScrollOffset(
        std::clamp(quickNavigation_->tabScrollOffset, 0,
            GetQuickNavigationMaxTabScrollOffset(overlay)),
        quickNavigation_->tabScrollOffset,
        quickNavigation_->tabDragIndex != static_cast<size_t>(-1),
        quickNavigation_->tabDragStartPoint,
        quickNavigation_->tabDragDeltaX);
    quickNavigation_->scrollOffset = std::clamp(quickNavigation_->scrollOffset, 0,
        GetQuickNavigationMaxScrollOffset(overlay, contentModel));
    const std::vector<RECT> itemRects =
        GetQuickNavigationItemRects(
            overlay, contentModel);


    const RECT searchInput = GetQuickNavigationInputRect(overlay);
    snowdesktop::text_input::SetColors(quickNavigation_->searchEdit,
        {t.searchBg, t.searchText, t.searchBorder, t.searchFocus, t.selectedText, t.searchPlaceholder});
    snowdesktop::text_input::Draw(quickNavigation_->searchEdit, ctx.Get(),
        ToD2DRect(searchInput), 1.f, false);
    if (!quickNavigation_->searchEdit)
        DrawQuickNavigationCenteredText(ctx.Get(), GetQuickNavigationEffectiveSearchText().empty() ?
            _LW(quickNavigation_->searchType == QuickNavigationSearchType::All ? "app.nav.search_hint" : "quickNav.search.scopedPlaceholder") : GetQuickNavigationEffectiveSearchText(),
            searchInput, quickNavigation_->searchTextFormat.Get(), ToD2DColor(GetQuickNavigationEffectiveSearchText().empty() ? t.searchPlaceholder : t.searchText));

    if (quickNavigation_->preview && quickNavigation_->collapsed && GetQuickNavigationEffectiveSearchText().empty() && quickNavigation_->menu == QuickNavigationMenu::None)
    {
        const auto caret = snowdesktop::text_input::CenteredCaretRectangle(ToD2DRect(searchInput), static_cast<float>(searchInput.left),
            static_cast<float>(QuickNavScale(navigationSettings_.layout.searchFontSize + 2)), 1.5f * quickNavigation_->dpiScale);
        ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(ctx->CreateSolidColorBrush(ToD2DColor(t.searchText), &brush))) ctx->FillRectangle(caret, brush.Get());
    }
    if (!searching && !UseQuickNavigationList() && quickNavigation_->menu != QuickNavigationMenu::Types)
    {
        RECT tabs = GetQuickNavigationTabsRect(overlay);
        const int tabsStart =
            GetQuickNavigationTabsStart(overlay);
        const int tabClipRight = tabs.right;
        // 左侧外扩一圈避免固定标签圆角 AA 被截断；右侧与搜索框右边界对齐。
        const int clipPad = QuickNavScale(8);
        ctx->PushAxisAlignedClip(
            ToD2DRect(MakeRect(
                tabsStart - clipPad,
                tabs.top, tabClipRight,
                tabs.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const size_t tabCount = collectionIndices.size() + 2;
        const auto& tabWidths = quickNavigation_->tabWidths;
        const int gap = QuickNavScale(8);
        const int sepGap = QuickNavScale(6);
        const int fixedWidth = (tabWidths.size() >= 2 ? tabWidths[0] + gap + tabWidths[1] : 0);
        const int scrollPad = sepGap + QuickNavScale(1) + gap; // separator + gap after
        auto calcTabPosX = [&](size_t tabIdx) -> int {
            if (tabIdx == 0) return tabsStart;
            if (tabIdx == 1) return tabsStart + tabWidths[0] + gap;
            int x = tabsStart + fixedWidth + scrollPad;
            for (size_t i = 2; i < tabIdx && i < tabWidths.size(); ++i)
                x += tabWidths[i] + gap;
            return x - quickNavigation_->tabScrollOffset;
        };

        int dragTargetTab = -1;
        if (quickNavigation_->tabDragging && quickNavigation_->tabDragIndex != static_cast<size_t>(-1))
            dragTargetTab = GetQuickNavTabDragTarget(
                quickNavigation_->tabDragIndex, quickNavigation_->tabDragDeltaX);

        const int tabInsetY = QuickNavScale(3);
        auto drawTab = [&](size_t tab, int offsetX) {
            if (tab >= tabWidths.size()) return;
            int posX = calcTabPosX(tab) + offsetX;
            int tw = tabWidths[tab];
            // 按钮上下内缩，避免 AA 圆角被 tabs 裁剪边界截断，同时降低视觉高度。
            RECT tabRect = MakeRect(posX, tabs.top + tabInsetY, posX + tw, tabs.bottom - tabInsetY);
            if (tab >= 2)
            {
                int scrollStart =
                    tabsStart + fixedWidth +
                    scrollPad;
                if (tabRect.right <= scrollStart || tabRect.left >= tabClipRight) return;
            }
            else if (tab <= 1)
            {
                if (tabRect.right <= tabsStart ||
                    tabRect.left >=
                        tabsStart + fixedWidth +
                            sepGap)
                    return;
                tabRect.right = std::min<LONG>(
                    tabRect.right,
                    static_cast<LONG>(
                        tabsStart + fixedWidth +
                        sepGap));
            }
            if (!quickNavigation_->tabDragging)
                registerHoverRegion(
                    tabRect,
                    QuickNavigationPointerTargetKind::Tab,
                    tab);
            const bool active = (tab == 0 && quickNavigation_->activeWidgetIndex == static_cast<size_t>(-1))
                || (tab == 1 && quickNavigation_->activeWidgetIndex == static_cast<size_t>(-2))
                || (tab > 1 && quickNavigation_->activeWidgetIndex == collectionIndices[tab - 2]);
            bool hovered = false;
            if (!quickNavigation_->tabDragging)
                hovered = PtInRect(
                    &tabRect,
                    quickNavigation_->lastMousePoint) != FALSE;

            D2D1_COLOR_F fill, stroke;
            if (quickNavigation_->tabDragging && tab == quickNavigation_->tabDragIndex)
            {
                fill = ToD2DColor(t.tabDragFill, 0.78f);
                stroke = ToD2DColor(t.tabDragStroke, 0.82f);
            }
            else
            {
                fill = active ? ToD2DColor(t.tabActiveFill)
                    : (hovered ? ToD2DColor(t.tabHoverFill)
                               : ToD2DColor(t.tabDefaultFill));
                stroke = active ? ToD2DColor(t.tabActiveStroke)
                                : ToD2DColor(hovered ? t.tabHoverStroke : t.tabDefaultStroke);
            }
            DrawD2DRoundedRectangle(ctx.Get(), tabRect,
                static_cast<float>(QuickNavScale(navigationSettings_.layout.tabRadius)), fill, stroke);

            std::wstring label = GetQuickNavTabLabel(tab);
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DTextEllipsis(ctx.Get(), label, textRect, quickNavigation_->tabTextFormat.Get(),
                ToD2DColor(active ? t.tabActiveText : hovered ? t.tabHoverText : t.tabText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        };

        // 分隔线：固定在 "映射" 标签右侧
        int sepX =
            tabsStart + fixedWidth + sepGap;
        RECT sepRect = MakeRect(sepX, tabs.top + QuickNavScale(8),
            sepX + QuickNavScale(1), tabs.bottom - QuickNavScale(8));
        DrawD2DSeparator(ctx.Get(), sepRect, ToD2DColor(t.tabSeparator, .35f));

        // Draw fixed tabs (0, 1) and dragged tab displacement
        for (size_t tab = 0; tab < tabCount && tab < 2; ++tab)
        {
            if (quickNavigation_->tabDragging && tab == quickNavigation_->tabDragIndex)
                continue;
            int offsetX = 0;
            if (quickNavigation_->tabDragging && dragTargetTab >= 1)
            {
                size_t src = quickNavigation_->tabDragIndex;
                int dst = dragTargetTab;
                int cur = static_cast<int>(tab);
                int shift = (quickNavigation_->tabDragIndex < tabWidths.size()
                    ? tabWidths[quickNavigation_->tabDragIndex] + gap : tabWidths[0] + gap);
                if (cur > src && cur <= dst) offsetX = -shift;
                else if (cur < src && cur >= dst) offsetX = shift;
            }
            drawTab(tab, offsetX);
        }

        // Clip to scrollable area for remaining tabs
        int scrollLeft =
            tabsStart + fixedWidth + scrollPad;
        ctx->PopAxisAlignedClip();
        ctx->PushAxisAlignedClip(
            ToD2DRect(MakeRect(scrollLeft - clipPad, tabs.top, tabClipRight, tabs.bottom)),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        for (size_t tab = 2; tab < tabCount; ++tab)
        {
            if (quickNavigation_->tabDragging && tab == quickNavigation_->tabDragIndex)
                continue;
            int offsetX = 0;
            if (quickNavigation_->tabDragging && dragTargetTab >= 1)
            {
                size_t src = quickNavigation_->tabDragIndex;
                int dst = dragTargetTab;
                int cur = static_cast<int>(tab);
                int shift = (quickNavigation_->tabDragIndex < tabWidths.size()
                    ? tabWidths[quickNavigation_->tabDragIndex] + gap : tabWidths[0] + gap);
                if (cur > src && cur <= dst) offsetX = -shift;
                else if (cur < src && cur >= dst) offsetX = shift;
            }
            drawTab(tab, offsetX);
        }

        if (quickNavigation_->tabDragging && quickNavigation_->tabDragIndex != static_cast<size_t>(-1) &&
            quickNavigation_->tabDragIndex < tabWidths.size())
        {
            int dragTw = tabWidths[quickNavigation_->tabDragIndex];
            if (dragTargetTab >= 2 && static_cast<size_t>(dragTargetTab) < tabWidths.size())
            {
                const size_t targetTab = static_cast<size_t>(dragTargetTab);
                const int slotLeft = snowdesktop::quick_navigation_rules::TabDropSlotLeft(
                    quickNavigation_->tabDragIndex, targetTab, calcTabPosX(targetTab),
                    dragTw, tabWidths[targetTab]);
                const RECT slotRect = MakeRect(slotLeft, tabs.top + tabInsetY,
                    slotLeft + dragTw, tabs.bottom - tabInsetY);
                DrawD2DRoundedRectangle(ctx.Get(), slotRect,
                    static_cast<float>(QuickNavScale(navigationSettings_.layout.tabRadius)),
                    ToD2DColor(t.tabDragIndicator, 0.08f),
                    ToD2DColor(t.tabDragIndicator, 0.90f),
                    static_cast<float>(QuickNavScale(2)));
            }
            int posX = calcTabPosX(quickNavigation_->tabDragIndex) + quickNavigation_->tabDragDeltaX;
            RECT tabRect = MakeRect(posX, tabs.top + tabInsetY, posX + dragTw, tabs.bottom - tabInsetY);
            DrawD2DRoundedRectangle(ctx.Get(), tabRect,
                static_cast<float>(QuickNavScale(navigationSettings_.layout.tabRadius)),
                ToD2DColor(t.tabDragFloatFill, 0.82f),
                ToD2DColor(t.tabDragFloatStroke, 0.88f));

            std::wstring label = GetQuickNavTabLabel(quickNavigation_->tabDragIndex);
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            DrawD2DTextEllipsis(ctx.Get(), label, textRect, quickNavigation_->tabTextFormat.Get(),
                ToD2DColor(t.tabDragFloatText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        ctx->PopAxisAlignedClip();

        const RECT modeButton =
            GetQuickNavigationViewModeButtonRect(
                overlay);
        if (!IsRectEmpty(&modeButton))
        {
            registerHoverRegion(
                modeButton,
                QuickNavigationPointerTargetKind::ViewMode);
            const bool hovered =
                PtInRect(
                    &modeButton,
                    quickNavigation_->lastMousePoint) != FALSE;
            if (hovered || quickNavigation_->menu == QuickNavigationMenu::Views)
                DrawD2DRoundedRectangle(ctx.Get(), modeButton, static_cast<float>(QuickNavScale(navigationSettings_.layout.tabRadius)),
                    ToD2DColor(t.tabHoverFill), ToD2DColor(t.tabHoverStroke));
            RECT labelRect = modeButton; labelRect.left += QuickNavScale(8); labelRect.right -= QuickNavScale(20);
            RECT chevronRect = modeButton; chevronRect.left = chevronRect.right - QuickNavScale(20);
            DrawQuickNavigationCenteredText(ctx.Get(), L"\uF2A4", chevronRect, quickNavigation_->fluentTextFormat.Get(),
                ToD2DColor(t.appTypeText), static_cast<float>(QuickNavScale(12)));
            DrawQuickNavigationCenteredText(ctx.Get(), QuickNavigationViewLabel(), labelRect,
                quickNavigation_->tabTextFormat.Get(), ToD2DColor(t.tabText));
        }
    }

    RECT contentApp = GetQuickNavigationContentRect(overlay);
    const POINT contentMousePoint = quickNavigation_->menu == QuickNavigationMenu::None &&
        PtInRect(&contentApp, quickNavigation_->lastMousePoint) ? quickNavigation_->lastMousePoint : POINT{LONG_MIN, LONG_MIN};
    if (contentApp.bottom <= contentApp.top)
    {
        DrawQuickNavigationMenus(ctx.Get());
        if (windowClipPushed) ctx->PopLayer();
        return;
    }
    {
    const snowdesktop::ScrollContentClip contentClip(ctx.Get(), quickNavigation_->scrollFadeCache, contentApp,
        quickNavigation_->scrollOffset, quickNavigation_->initialJumpOpen || quickNavigation_->menu == QuickNavigationMenu::Types ? 0 :
            GetQuickNavigationContentHeight(overlay, contentModel), static_cast<float>(QuickNavScale(16)));
    if (quickNavigation_->menu == QuickNavigationMenu::Types)
    {
        // Menu drawing and keyboard selection use the same row geometry.
    }
    else if (UseQuickNavigationList())
        DrawQuickNavigationList(ctx.Get());
    else if (quickNavigation_->initialJumpOpen)
    {
        RECT titleRect = MakeRect(
            contentApp.left + QuickNavScale(8),
            contentApp.top,
            contentApp.right - QuickNavScale(96),
            contentApp.top + QuickNavScale(30));
        DrawD2DTextEllipsis(
            ctx.Get(),
            _LW("app.nav.initial_jump_title"),
            titleRect,
            quickNavigation_->tabTextFormat.Get(),
            ToD2DColor(t.headerText),
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        const RECT backRect =
            GetQuickNavigationInitialJumpBackRect(
                overlay);
        registerHoverRegion(
            backRect,
            QuickNavigationPointerTargetKind::InitialBack);
        const bool backHovered =
            PtInRect(&backRect,
                contentMousePoint) != FALSE;
        if (backHovered)
            DrawD2DRoundedRectangle(
                ctx.Get(), backRect,
                static_cast<float>(
                    QuickNavScale(10)) / 2.0f,
                ToD2DColor(
                    t.tabHoverFill, 0.72f),
                ToD2DColor(
                    t.tabDefaultStroke, 0.72f));
        DrawD2DTextEllipsis(
            ctx.Get(),
            _LW("app.nav.initial_jump_back"),
            backRect,
            quickNavigation_->tabTextFormat.Get(),
            ToD2DColor(t.tabText),
            DWRITE_TEXT_ALIGNMENT_CENTER,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        std::array<bool, 27> available{};
        for (const auto& section :
            contentModel.sections)
        {
            if (section.label.size() != 1)
                continue;
            available[
                snowdesktop::
                    quick_navigation_rules::
                        InitialJumpBucketIndex(
                            section.label.front())] =
                                true;
        }
        for (size_t bucketIndex = 0;
            bucketIndex < available.size();
            ++bucketIndex)
        {
            const RECT cell =
                GetQuickNavigationInitialJumpCellRect(
                    overlay, bucketIndex);
            if (available[bucketIndex])
                registerHoverRegion(
                    cell,
                    QuickNavigationPointerTargetKind::InitialBucket,
                    bucketIndex);
            const bool hovered =
                PtInRect(
                    &cell,
                    contentMousePoint) != FALSE;
            const bool selected =
                bucketIndex ==
                    quickNavigation_->initialJumpSelection;
            if (available[bucketIndex] &&
                (hovered || selected))
                DrawD2DRoundedRectangle(
                    ctx.Get(), cell,
                    static_cast<float>(
                        QuickNavScale(10)) / 2.0f,
                    ToD2DColor(
                        selected
                        ? t.tabActiveFill
                        : t.tabHoverFill,
                        0.78f),
                    ToD2DColor(
                        selected
                        ? t.tabActiveStroke
                        : t.tabDefaultStroke,
                        0.76f));
            const std::wstring label(
                1,
                snowdesktop::
                    quick_navigation_rules::
                        InitialJumpBucketAt(
                            bucketIndex));
            DrawD2DTextEllipsis(
                ctx.Get(), label, cell,
                quickNavigation_->tabTextFormat.Get(),
                ToD2DColor(
                    available[bucketIndex]
                    ? t.tabText
                    : t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    else if (entries.empty() &&
        std::none_of(contentModel.sections.begin(), contentModel.sections.end(),
            [](const auto& section) { return section.showWhenEmpty; }) &&
        quickNavigation_->appResultIndices.empty() &&
        (!searching || quickNavigation_->everythingResults.empty()))
    {
        RECT emptyRect = contentApp;
        emptyRect.top += QuickNavScale(28);
        if (searching && (!quickNavigation_->everythingSearchAvailable || quickNavigation_->everythingSearchPending || !quickNavigation_->appsIndexed))
            DrawD2DTextEllipsis(ctx.Get(), !quickNavigation_->appsIndexed ? _LW("quickNav.apps.loading") : quickNavigation_->everythingSearchPending ? _LW("quickNav.files.loading") : GetQuickNavigationEverythingNoticeText(),
                emptyRect, quickNavigation_->itemTextFormat.Get(), ToD2DColor(t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        else
            DrawD2DTextEllipsis(ctx.Get(),
                !searching
                ? (collectionIndices.empty() ? _LW("app.nav.empty_collection") : _LW("app.nav.empty_category"))
                : _LW("app.nav.no_results"),
                emptyRect, quickNavigation_->itemTextFormat.Get(), ToD2DColor(t.emptyText),
                DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    }
    else
    {
        if (searching && !entries.empty())
        {
            const int headerH = QuickNavScale(28);
            RECT desktopHeader = MakeRect(contentApp.left + QuickNavScale(8),
                contentApp.top - quickNavigation_->scrollOffset,
                contentApp.right - QuickNavScale(12),
                contentApp.top + headerH - quickNavigation_->scrollOffset);
            std::wstring desktopLabel = _LFW("app.nav.desktop_results",
                std::to_wstring(entries.size()));
            DrawD2DTextEllipsis(ctx.Get(), desktopLabel, desktopHeader,
                quickNavigation_->tabTextFormat.Get(), ToD2DColor(t.headerText),
                DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

            RECT desktopSep = MakeRect(desktopHeader.left,
                desktopHeader.bottom - QuickNavScale(1),
                desktopHeader.right, desktopHeader.bottom);
            DrawD2DSeparator(ctx.Get(), desktopSep, ToD2DColor(t.headerSeparator));
        }
        else if (!searching && contentModel.IsSectioned())
        {
            for (size_t sectionIndex = 0;
                sectionIndex <
                    contentModel.sections.size();
                ++sectionIndex)
            {
                const auto& section =
                    contentModel.sections[
                        sectionIndex];
                const RECT header =
                    GetQuickNavigationSectionHeaderRect(
                        overlay, sectionIndex,
                        contentModel);
                if (header.bottom <= contentApp.top ||
                    header.top >= contentApp.bottom)
                    continue;
                const bool initialJumpHeader = section.label.size() == 1 &&
                    GetQuickNavigationViewMode() ==
                        QuickNavigationDesktopViewMode::
                            Initial;
                if (initialJumpHeader)
                    registerClippedHoverRegion(
                        header, contentApp,
                        QuickNavigationPointerTargetKind::SectionHeader,
                        sectionIndex);
                if (initialJumpHeader &&
                    PtInRect(
                        &header,
                        contentMousePoint) != FALSE)
                    DrawD2DRoundedRectangle(
                        ctx.Get(), header,
                        static_cast<float>(
                            QuickNavScale(8)) / 2.0f,
                        ToD2DColor(
                            t.tabHoverFill, 0.58f),
                        ToD2DColor(
                            t.tabDefaultStroke,
                            0.62f));
                const RECT add = GetQuickNavigationSectionAddRect(
                    overlay, sectionIndex, contentModel);
                RECT labelRect = header;
                if (!IsRectEmptyRect(add))
                    labelRect.right = add.left - QuickNavScale(6);
                const std::wstring label = _LFW(
                    "app.nav.section_header",
                    section.label,
                    std::to_wstring(
                        section.entryCount));
                DrawD2DTextEllipsis(
                    ctx.Get(), label, labelRect,
                    quickNavigation_->tabTextFormat.Get(),
                    ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                if (!IsRectEmptyRect(add))
                {
                    registerClippedHoverRegion(
                        add, contentApp,
                        QuickNavigationPointerTargetKind::SectionAdd,
                        sectionIndex);
                    const bool hovered = PtInRect(
                        &add, contentMousePoint) != FALSE;
                    if (hovered)
                        DrawD2DRoundedRectangle(
                            ctx.Get(), add,
                            static_cast<float>(
                                QuickNavScale(6)),
                            ToD2DColor(t.tabHoverFill, 0.78f),
                            ToD2DColor(
                                t.tabDefaultStroke,
                                0.62f));
                    DrawQuickNavigationCenteredText(
                        ctx.Get(), L"\uF108", add,
                        quickNavigation_->fluentTextFormat.Get(),
                        ToD2DColor(
                            hovered ? t.headerText : t.emptyText),
                        static_cast<float>(QuickNavScale(16)));
                }
                const RECT separator = MakeRect(
                    header.left,
                    header.bottom -
                        QuickNavScale(1),
                    header.right,
                    header.bottom);
                DrawD2DSeparator(
                    ctx.Get(), separator,
                    ToD2DColor(
                        t.headerSeparator));
            }
        }

        // 桌面项直接走 D2D（ctx 为 ID2D1DeviceContext，与桌面共享 d2dDevice_ 缓存）
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const RECT itemRectApp = itemRects[i];
            if (itemRectApp.bottom <= contentApp.top ||
                itemRectApp.top >= contentApp.bottom)
                continue;

            registerClippedHoverRegion(
                itemRectApp, contentApp,
                QuickNavigationPointerTargetKind::Item,
                i);

            const QuickNavigationEntry& entry = entries[i];
            const bool selected = IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Item, i);
            const bool hovered = PtInRect(&itemRectApp, contentMousePoint) != FALSE;
            DrawD2DRoundedRectangle(ctx.Get(), itemRectApp,
                static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
                ToD2DColor(selected ? t.selectedFill : hovered ? t.itemHoverFill : t.resultFill),
                ToD2DColor(selected ? t.selectedBorder : hovered ? t.itemHoverStroke : t.resultBorder));
            DrawD2DRoundedRectangle(ctx.Get(), GetQuickNavItemIconRect(itemRectApp),
                static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
                ToD2DColor(t.iconPlateFill),
                ToD2DColor(t.iconPlateBorder));
            // 图标本体复用桌面绘制，标题由快捷导航自绘，避免桌面标题布局和字重互相影响。
            if (entry.kind == QuickNavigationEntry::Kind::DesktopItem &&
                entry.itemIndex < items_.size())
            {
                const DesktopWidget* demoCollection =
                    entry.demoCollectionIndex < widgets_.size()
                    ? &widgets_[entry.demoCollectionIndex] : nullptr;
                DesktopIcon icon(&items_[entry.itemIndex], nullptr, this);
                icon.Draw(ctx.Get(), itemRectApp, 0,
                    quickNavigation_->lightTheme, false, true, demoCollection);
                if (!IsRenamingItem(&items_[entry.itemIndex]))
                    DrawQuickNavItemText(ctx.Get(), itemRectApp, entry.name,
                        IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Item, i), quickNavigation_->lightTheme);
            }
            else if (entry.kind == QuickNavigationEntry::Kind::FolderEntry &&
                entry.widgetIndex < widgets_.size() &&
                entry.folderEntryIndex < widgets_[entry.widgetIndex].folderEntries.size())
            {
                FolderEntry& folderEntry =
                    widgets_[entry.widgetIndex].folderEntries[entry.folderEntryIndex];
                FolderEntryIcon icon(&folderEntry, nullptr, this);
                icon.Draw(ctx.Get(), itemRectApp, 0, quickNavigation_->lightTheme, false, true);
                if (!IsRenamingItem(&folderEntry))
                    DrawQuickNavItemText(ctx.Get(), itemRectApp, folderEntry.name,
                        IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Item, i), quickNavigation_->lightTheme);
            }
            else if (entry.kind == QuickNavigationEntry::Kind::PathFavorite)
            {
                const int iconIndex = GetQuickNavigationEverythingIconIndex(
                    entry.path, entry.isDirectory);
                const RECT iconRect = GetQuickNavItemIconRect(itemRectApp);
                if (iconIndex >= 0)
                    DrawQuickNavSysIcon(ctx.Get(), iconIndex, iconRect);
                else
                    DrawQuickNavSysIcon(
                        ctx.Get(),
                        GetQuickNavigationEverythingIconIndex(
                            entry.isDirectory ? L"folder" : L"file",
                            entry.isDirectory),
                        iconRect);
                DrawQuickNavItemText(
                    ctx.Get(), itemRectApp, entry.name,
                    IsQuickNavigationKeyboardTarget(
                        QuickNavigationKeyboardTargetKind::Item, i),
                    quickNavigation_->lightTheme);
            }
            else if (entry.kind == QuickNavigationEntry::Kind::ApplicationFavorite)
            {
                const auto* appEntry =
                    FindQuickNavigationAppEntryByParsingName(entry.path);
                if (appEntry)
                    DrawQuickNavAppIcon(
                        ctx.Get(), *appEntry,
                        GetQuickNavItemIconRect(itemRectApp));
                DrawQuickNavItemText(
                    ctx.Get(), itemRectApp, entry.name,
                    IsQuickNavigationKeyboardTarget(
                        QuickNavigationKeyboardTargetKind::Item, i),
                    quickNavigation_->lightTheme);
            }
        }

        if (searching)
        {
            const int columns = GetQuickNavigationColumnCount(overlay);
            const int desktopRows = entries.empty() ? 0 :
                (static_cast<int>(entries.size()) + columns - 1) / columns;
            const int headerH = QuickNavScale(28);
            const int gap = QuickNavScale(8);
            const int rowH = QuickNavScale(navigationSettings_.layout.resultRowHeight);
            const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
                QuickNavScale(QuickNavigationGridCellHeight()), QuickNavScale(navigationSettings_.layout.rowGap));
            const int appHeaderTop = contentApp.top +
                QuickNavigationSearchDesktopSectionHeight(desktopRows, desktopGridH, headerH, gap)
                - quickNavigation_->scrollOffset;
            int everythingHeaderTop = appHeaderTop;

            if (!quickNavigation_->appResultIndices.empty())
            {
                const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
                RECT appHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    appHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    appHeaderTop + headerH);
                std::wstring appLabel = _LFW("app.nav.app_results",
                    std::to_wstring(quickNavigation_->appResultIndices.size()));
                DrawD2DTextEllipsis(ctx.Get(), appLabel, appHeader,
                    quickNavigation_->tabTextFormat.Get(), ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                RECT appSep = MakeRect(appHeader.left, appHeader.bottom - QuickNavScale(1),
                    appHeader.right, appHeader.bottom);
                DrawD2DSeparator(ctx.Get(), appSep, ToD2DColor(t.headerSeparator));

                for (size_t i = 0; i < visibleAppCount; ++i)
                {
                    size_t appIndex = quickNavigation_->appResultIndices[i];
                    if (appIndex >= quickNavigation_->appEntries.size())
                        continue;

                    const int rowTop = appHeaderTop + headerH + gap + static_cast<int>(i) * rowH;
                    RECT rowRectApp = QuickNavigationResultRowRect(MakeRect(contentApp.left + QuickNavScale(8), rowTop,
                        contentApp.right - QuickNavScale(12), rowTop + rowH), QuickNavScale(2));
                    if (rowRectApp.bottom <= contentApp.top || rowRectApp.top >= contentApp.bottom)
                        continue;

                    registerClippedHoverRegion(
                        rowRectApp, contentApp,
                        QuickNavigationPointerTargetKind::App,
                        i);

                    const bool selected = IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::App, i);
                    const bool hovered = PtInRect(&rowRectApp, contentMousePoint) != FALSE;
                    DrawD2DRoundedRectangle(ctx.Get(), rowRectApp,
                        static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
                        ToD2DColor(selected ? t.selectedFill : hovered ? t.appRowHoverFill : t.resultFill),
                        ToD2DColor(selected ? t.selectedBorder : hovered ? t.appRowHoverStroke : t.resultBorder));

                    const QuickNavigationAppEntry& entry = quickNavigation_->appEntries[appIndex];
                    const int iconSz = QuickNavScale(28);
                    RECT iconRect = MakeRect(rowRectApp.left + QuickNavScale(12),
                        (rowRectApp.top + rowRectApp.bottom - iconSz) / 2,
                        rowRectApp.left + QuickNavScale(12) + iconSz,
                        (rowRectApp.top + rowRectApp.bottom + iconSz) / 2);
                    DrawD2DRoundedRectangle(ctx.Get(), iconRect,
                        static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
                        ToD2DColor(t.iconPlateFill),
                        ToD2DColor(t.iconPlateBorder));
                    if (!IsLuaLogicalSlotPickerOpen() &&
                        generalSettings_.demoModeEnabled &&
                        demoIdentityAssetsAvailable_)
                        DrawDemoIdentityIcon(ctx.Get(), entry.parsingName,
                            iconRect);
                    else
                        DrawQuickNavAppIcon(
                            ctx.Get(), entry, iconRect);

                    const int textLeft = iconRect.right + QuickNavScale(10);
                    RECT nameRect = rowRectApp;
                    nameRect.left = textLeft;
                    nameRect.right -= QuickNavScale(12);
                    nameRect.top += QuickNavScale(6);
                    nameRect.bottom = nameRect.top + QuickNavScale(navigationSettings_.layout.fontSize + 6);

                    RECT typeRect = rowRectApp;
                    typeRect.left = textLeft;
                    typeRect.right -= QuickNavScale(12);
                    typeRect.top = nameRect.bottom;
                    typeRect.bottom -= QuickNavScale(5);

                    const std::wstring appName =
                        !IsLuaLogicalSlotPickerOpen() &&
                        generalSettings_.demoModeEnabled &&
                        demoIdentityAssetsAvailable_
                        ? GetDemoIdentityTitle(entry.parsingName)
                        : entry.name;
                    DrawD2DTextEllipsis(ctx.Get(), appName, nameRect,
                        quickNavigation_->itemTextFormat.Get(), ToD2DColor(selected ? t.selectedText : t.appNameText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DrawD2DTextEllipsis(ctx.Get(), _LW("app.nav.app_label"), typeRect,
                        quickNavigation_->pathTextFormat.Get(), ToD2DColor(t.appTypeText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                int appRowsHeight = static_cast<int>(visibleAppCount) * rowH;
                if (HasQuickNavigationAppExpandButton())
                {
                    const int buttonTop = appHeaderTop + headerH + gap + appRowsHeight;
                    RECT buttonRectApp = QuickNavigationResultRowRect(MakeRect(contentApp.left + QuickNavScale(8), buttonTop,
                        contentApp.right - QuickNavScale(12), buttonTop + rowH), QuickNavScale(2));
                    if (buttonRectApp.bottom > contentApp.top && buttonRectApp.top < contentApp.bottom)
                    {
                        registerClippedHoverRegion(
                            buttonRectApp, contentApp,
                            QuickNavigationPointerTargetKind::ExpandApps);
                        const bool hovered = PtInRect(
                                &buttonRectApp,
                                contentMousePoint) != FALSE ||
                            IsQuickNavigationKeyboardTarget(
                                QuickNavigationKeyboardTargetKind::ExpandApps, 0);
                        std::wstring expandLabel = _LFW("app.interact.expand_apps_fmt",
                            std::to_wstring(quickNavigation_->appResultIndices.size()));
                        DrawD2DTextEllipsis(ctx.Get(), expandLabel, buttonRectApp,
                            quickNavigation_->tabTextFormat.Get(),
                            hovered ? ToD2DColor(t.expandHoverText) : ToD2DColor(t.expandDefaultText),
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                    appRowsHeight += rowH;
                }

                everythingHeaderTop = appHeaderTop + headerH + gap
                    + appRowsHeight
                    + gap;
            }

            if (!quickNavigation_->everythingSearchAvailable)
            {
                RECT noticeHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    everythingHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    everythingHeaderTop + headerH);
                DrawD2DTextEllipsis(ctx.Get(), GetQuickNavigationEverythingNoticeText(), noticeHeader,
                    quickNavigation_->tabTextFormat.Get(), ToD2DColor(t.emptyHeaderText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            else if (!quickNavigation_->everythingResults.empty() || quickNavigation_->everythingSearchPending)
            {
                RECT everythingHeader = MakeRect(contentApp.left + QuickNavScale(8),
                    everythingHeaderTop,
                    contentApp.right - QuickNavScale(12),
                    everythingHeaderTop + headerH);
                std::wstring everythingLabel = quickNavigation_->everythingSearchPending ? _LW("quickNav.files.loading") : L"Everything  " +
                    std::to_wstring(quickNavigation_->everythingResults.size()) +
                    (quickNavigation_->everythingHasMore
                        ? _LW("app.interact.plus_items")
                        : _LW("app.interact.items_suffix"));
                DrawD2DTextEllipsis(ctx.Get(), everythingLabel, everythingHeader,
                    quickNavigation_->tabTextFormat.Get(), ToD2DColor(t.headerText),
                    DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                RECT evSep = MakeRect(everythingHeader.left,
                    everythingHeader.bottom - QuickNavScale(1),
                    everythingHeader.right, everythingHeader.bottom);
                DrawD2DSeparator(ctx.Get(), evSep, ToD2DColor(t.headerSeparator));

                for (size_t i = 0; i < quickNavigation_->everythingResults.size(); ++i)
                {
                    const int rowTop = everythingHeaderTop + headerH + gap
                        + static_cast<int>(i) * rowH;
                    RECT rowRectApp = QuickNavigationResultRowRect(MakeRect(contentApp.left + QuickNavScale(8), rowTop,
                        contentApp.right - QuickNavScale(12), rowTop + rowH), QuickNavScale(2));
                    if (rowRectApp.bottom <= contentApp.top || rowRectApp.top >= contentApp.bottom)
                        continue;

                    registerClippedHoverRegion(
                        rowRectApp, contentApp,
                        QuickNavigationPointerTargetKind::Everything,
                        i);

                    const bool selected = IsQuickNavigationKeyboardTarget(QuickNavigationKeyboardTargetKind::Everything, i);
                    const bool hovered = PtInRect(&rowRectApp, contentMousePoint) != FALSE;
                    DrawD2DRoundedRectangle(ctx.Get(), rowRectApp,
                        static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
                        ToD2DColor(selected ? t.selectedFill : hovered ? t.appRowHoverFill : t.resultFill),
                        ToD2DColor(selected ? t.selectedBorder : hovered ? t.appRowHoverStroke : t.resultBorder));

                    const QuickNavigationEverythingEntry& entry = quickNavigation_->everythingResults[i];
                    const std::wstring modifiedText = entry.modifiedText.empty()
                        ? QuickNavigationFormatModifiedTime(entry.dateModified) : entry.modifiedText;
                    const auto geometry = snowdesktop::file_search_result_row::Resolve(rowRectApp,
                        {QuickNavScale(28), QuickNavScale(12), QuickNavScale(10), QuickNavScale(6),
                         QuickNavScale(navigationSettings_.layout.fontSize + 6), QuickNavScale(5),
                         QuickNavScale(118), QuickNavScale(156), QuickNavScale(8)}, !modifiedText.empty());
                    const RECT iconRect = geometry.icon, nameRect = geometry.name, pathRect = geometry.path;
                    DrawD2DRoundedRectangle(ctx.Get(), iconRect,
                        static_cast<float>(QuickNavScale(navigationSettings_.layout.itemRadius)),
                        ToD2DColor(t.iconPlateFill), ToD2DColor(t.iconPlateBorder));
                    DrawQuickNavSysIcon(ctx.Get(), entry.systemIconIndex, iconRect);

                    DrawD2DTextEllipsis(ctx.Get(),
                        entry.name.empty() ? FileNameFromPath(entry.path) : entry.name,
                        nameRect, quickNavigation_->itemTextFormat.Get(), ToD2DColor(selected ? t.selectedText : t.appNameText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

                    if (!modifiedText.empty())
                        DrawD2DTextEllipsis(ctx.Get(), modifiedText, geometry.modified,
                            quickNavigation_->pathTextFormat.Get(), ToD2DColor(t.appTypeText),
                            DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    DrawD2DTextEllipsis(ctx.Get(), entry.path, pathRect,
                        quickNavigation_->pathTextFormat.Get(), ToD2DColor(t.appTypeText),
                        DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }

                if (HasQuickNavigationEverythingLoadMoreButton())
                {
                    const int buttonTop = everythingHeaderTop + headerH + gap
                        + static_cast<int>(quickNavigation_->everythingResults.size()) * rowH;
                    RECT buttonRectApp = QuickNavigationResultRowRect(MakeRect(contentApp.left + QuickNavScale(8), buttonTop,
                        contentApp.right - QuickNavScale(12), buttonTop + rowH), QuickNavScale(2));
                    if (buttonRectApp.bottom > contentApp.top && buttonRectApp.top < contentApp.bottom)
                    {
                        registerClippedHoverRegion(
                            buttonRectApp, contentApp,
                            QuickNavigationPointerTargetKind::LoadMoreEverything);
                        const bool hovered = PtInRect(
                                &buttonRectApp,
                                contentMousePoint) != FALSE ||
                            IsQuickNavigationKeyboardTarget(
                                QuickNavigationKeyboardTargetKind::LoadMoreEverything, 0);
                        DrawD2DTextEllipsis(ctx.Get(), _LW("app.nav.load_more_everything"), buttonRectApp,
                            quickNavigation_->tabTextFormat.Get(),
                            hovered ? ToD2DColor(t.expandHoverText) : ToD2DColor(t.expandDefaultText),
                            DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                }
            }
        }
    }
    }

    RECT track{}, thumb{};
    int maxScroll = 0, contentHeight = 0;
    if (!quickNavigation_->initialJumpOpen &&
        GetQuickNavigationScrollbarGeometry(overlay, contentModel,
        track, thumb, maxScroll, contentHeight))
    {
        registerHoverRegion(
            thumb,
            QuickNavigationPointerTargetKind::Scrollbar);
        const int trackW = QuickNavScale(5);
        DrawD2DRoundedRectangle(ctx.Get(), track, static_cast<float>(trackW) / 2.0f,
            ToD2DColor(t.scrollTrack), ToD2DColor(t.scrollTrack));
        const auto thumbColor = (quickNavigation_->scrollbarDragging || quickNavigation_->scrollbarHovered)
            ? t.scrollThumbHover : t.scrollThumbDefault;
        DrawD2DRoundedRectangle(ctx.Get(), thumb, static_cast<float>(trackW) / 2.0f,
            ToD2DColor(thumbColor), ToD2DColor(thumbColor));
    }

    DrawQuickNavigationMenus(ctx.Get());
    if (windowClipPushed) ctx->PopLayer();
    quickNavigation_->pointerTarget = HitTestQuickNavigationPointerTarget(quickNavigation_->lastMousePoint);
}
