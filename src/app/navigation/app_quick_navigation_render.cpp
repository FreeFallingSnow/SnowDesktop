#include "ui/render/app_font.h"
#include "app/app.h"
#include "ui/render/rgba_color.h"
#include "navigation/quick_navigation_animation_rules.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/l10n.h"
#include "diagnostics/diagnostic_log.h"
#include "diagnostics/performance_trace.h"
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

bool snowdesktop::quick_navigation::State::PublishPrecomposedFrame(
    ID2D1Device* device, quick_navigation_animation_rules::Effect effect, float deformationAmount, float opacity)
{
    performance::Scope trace("navigation.render", "precompose");
    namespace rules = quick_navigation_animation_rules;
    namespace genie = dock_genie;
    if (!sceneBitmap) return false;
    RECT bounds{};
    if (!GetClientRect(hwnd, &bounds) || IsRectEmpty(&bounds)) return false;
    const auto key = reinterpret_cast<std::uintptr_t>(hwnd);
    const auto asRect = [](const RECT& r) {
        return genie::Rect{static_cast<double>(r.left), static_cast<double>(r.top),
            static_cast<double>(r.right), static_cast<double>(r.bottom)};
    };
    const auto panel = asRect(rect);
    const auto dock = asRect(animationDockRect);
    const auto edge = static_cast<genie::Edge>(animationDockEdge);
    const double width = panel.right - panel.left;
    const double height = panel.bottom - panel.top;
    if (width <= 0 || height <= 0) return false;
    const bool deformed = deformationAmount > 0.f && rules::IsDeformationEffect(effect);
    struct Strip
    {
        D2D1_RECT_F clip, source, destination;
        rules::GenieStripProjection projection;
    };
    std::array<Strip, genie::StripCount> strips;
    std::size_t stripCount = 0;
    std::optional<dock_attraction::Frame> mesh;
    const auto toRectangle = [](const genie::Rect& value) {
        return D2D1::RectF(static_cast<float>(value.left), static_cast<float>(value.top),
            static_cast<float>(value.right), static_cast<float>(value.bottom));
    };
    if (deformed && effect == rules::Effect::Attraction)
        mesh = dock_attraction::MeshFrame(panel, dock, deformationAmount);
    else if (deformed)
    {
        // Match the body's pixel partitions and source crops before projection.
        for (std::size_t index = 0; index < genie::StripCount; ++index)
        {
            const auto clip = rules::GenieRasterBandClip(panel, dock, edge, deformationAmount, width, height,
                index, hostRect.left, hostRect.top, bounds.right, bounds.bottom);
            if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
            const auto projection = rules::GenieProjection(panel, dock, edge, deformationAmount, width, height,
                index, hostRect.left, hostRect.top);
            const auto crop = rules::GenieSourceBandClip(projection, clip, edge, width, height);
            if (crop.right <= crop.left || crop.bottom <= crop.top) continue;
            const auto source = toRectangle(crop);
            strips[stripCount++] = {toRectangle(clip), source,
                D2D1::RectF(source.left - projection.sourceX, source.top - projection.sourceY,
                    source.right - projection.sourceX, source.bottom - projection.sourceY), projection};
        }
    }
    if (deformed)
    {
        // The native HWND retains monitor coordinates. Rasterize only the
        // fixed envelope visible through its expanded region for both effects.
        const RECT fence = WindowFence(true);
        const double clientPixels = static_cast<double>(bounds.right) * bounds.bottom;
        if (!IntersectRect(&bounds, &bounds, &fence))
            return false;
        const double pixels = static_cast<double>(bounds.right - bounds.left) *
            (bounds.bottom - bounds.top);
        performance::Value("navigation.render", "deformation_surface_pixels", L"", pixels);
        performance::Value("navigation.render", "deformation_client_pixels", L"", clientPixels);
    }
    else
    {
        bounds = rect;
        OffsetRect(&bounds, -hostRect.left, -hostRect.top);
    }
    if (!backdropCompositor.StageSrgbSceneFrame(hwnd, key, bounds, device,
            [&](ID2D1DeviceContext* target, POINT origin) {
                performance::Scope draw("navigation.render", "precompose.draw");
                auto* bitmap = sceneBitmap.Get();
                const float atlasX = static_cast<float>(origin.x - (deformed ? bounds.left : 0));
                const float atlasY = static_cast<float>(origin.y - (deformed ? bounds.top : 0));
                target->SetDpi(96.f, 96.f);
                target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
                target->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
                target->SetTransform(D2D1::Matrix3x2F::Identity());
                target->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(origin.x),
                    static_cast<float>(origin.y),
                    static_cast<float>(origin.x + bounds.right - bounds.left),
                    static_cast<float>(origin.y + bounds.bottom - bounds.top)),
                    D2D1_ANTIALIAS_MODE_ALIASED);
                target->SetTransform(D2D1::Matrix3x2F::Translation(atlasX, atlasY));
                target->Clear(D2D1::ColorF(0, 0.f));
                if (!deformed)
                {
                    const auto area = D2D1::RectF(0.f, 0.f, static_cast<float>(width), static_cast<float>(height));
                    target->DrawBitmap(bitmap, &area, 1.f,
                        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &area, nullptr);
                    target->PopAxisAlignedClip();
                    return S_OK;
                }
                if (mesh)
                {
                    const auto hr = rules::DrawAttractionMesh(target, bitmap, *mesh,
                        hostRect.left, hostRect.top, width, height);
                    target->PopAxisAlignedClip();
                    return hr;
                }

                target->SetTransform(D2D1::Matrix3x2F::Identity());
                for (const auto& strip : std::span(strips.data(), stripCount))
                {
                    const auto& p = strip.projection;
                    // Translate the projected position into the surface atlas.
                    const D2D1_MATRIX_4X4_F transform{
                        p.m11 + atlasX * p.m14, p.m12 + atlasY * p.m14, 0.f, p.m14,
                        p.m21 + atlasX * p.m24, p.m22 + atlasY * p.m24, 0.f, p.m24,
                        0.f, 0.f, 1.f, 0.f,
                        p.m41 + atlasX * p.m44, p.m42 + atlasY * p.m44, 0.f, p.m44};
                    target->PushAxisAlignedClip(D2D1::RectF(strip.clip.left + atlasX, strip.clip.top + atlasY,
                        strip.clip.right + atlasX, strip.clip.bottom + atlasY),
                        D2D1_ANTIALIAS_MODE_ALIASED);
                    target->DrawBitmap(bitmap, &strip.destination, 1.f, D2D1_INTERPOLATION_MODE_LINEAR,
                        &strip.source, &transform);
                    target->PopAxisAlignedClip();
                }
                target->PopAxisAlignedClip();
                return S_OK;
            }) || !backdropCompositor.PublishSrgbContent(key, opacity))
        return false;
    sceneDirty = false;
    sceneFrameDeformed = deformed;
    return backdropCompositor.SetSrgbContentPose(key, {}, 1.f, opacity);
}

void DesktopApp::ClearQuickNavigationDeformation()
{
    quickNavigation_->backdropCompositor.ClearGenieTransform();
}

bool DesktopApp::ApplyQuickNavigationGenieFrame(float collapsed, float opacity)
{
    auto& nav = *quickNavigation_;
    if (!nav.PublishPrecomposedFrame(d2dDevice_.Get(),
        snowdesktop::quick_navigation_animation_rules::Effect::Genie, collapsed, opacity))
        return false;
    if (!nav.glassTheme || !nav.backdropCompositor.IsAvailable()) return true;
    RECT panel = nav.rect, dock = nav.animationDockRect;
    OffsetRect(&panel, -nav.hostRect.left, -nav.hostRect.top);
    OffsetRect(&dock, -nav.hostRect.left, -nav.hostRect.top);
    snowdesktop::performance::Scope trace("navigation.render", "genie.backdrop");
    return nav.backdropCompositor.SetGenieTransform(panel, dock, nav.animationDockEdge, collapsed, opacity);
}

bool DesktopApp::ApplyQuickNavigationAttractionFrame(float collapsed, float opacity)
{
    auto& nav = *quickNavigation_;
    if (!nav.PublishPrecomposedFrame(d2dDevice_.Get(),
        snowdesktop::quick_navigation_animation_rules::Effect::Attraction, collapsed, opacity))
        return false;
    if (!nav.glassTheme || !nav.backdropCompositor.IsAvailable()) return true;
    RECT panel = nav.rect, dock = nav.animationDockRect;
    OffsetRect(&panel, -nav.hostRect.left, -nav.hostRect.top);
    OffsetRect(&dock, -nav.hostRect.left, -nav.hostRect.top);
    snowdesktop::performance::Scope trace("navigation.render", "attraction.backdrop");
    return nav.backdropCompositor.SetAttractionTransform(panel, dock, collapsed, opacity);
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
    drawing_.Reset();
    quickNavigation_->sysIconCache.clear();
    quickNavigation_->appIconCache.clear();
    if (quickNavigation_->dcompVisual)
        quickNavigation_->dcompVisual->SetContent(nullptr);
    quickNavigation_->sceneBitmap.Reset();
    quickNavigation_->sceneContext.Reset();
    quickNavigation_->sceneDirty = false;
    quickNavigation_->sceneFrameDeformed = false;
    quickNavigation_->backdropCompositor.RemoveSrgbContent(reinterpret_cast<std::uintptr_t>(quickNavigation_->hwnd));
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

    if (quickNavigation_->sceneBitmap && quickNavigation_->compWidth == width && quickNavigation_->compHeight == height)
        return S_OK;

    ClearQuickNavigationDeformation();
    auto& nav = *quickNavigation_;
    if (!nav.sceneContext)
    {
        const auto created = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &nav.sceneContext);
        if (FAILED(created)) return created;
    }
    ComPtr<ID2D1Bitmap1> bitmap;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    const auto created = nav.sceneContext->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &props, &bitmap);
    if (FAILED(created)) return created;
    nav.sceneBitmap = std::move(bitmap);
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
    snowdesktop::performance::Scope presentScope("navigation", "paint");
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

    // Initialize the backdrop before deciding how to draw the first hidden
    // frame. Otherwise a cold open starts with an ordinary rim.
    if (!quickNavigation_->animationCompositorDriven)
        UpdateQuickNavigationBackdrop();

    HRESULT hr = CreateOrResizeQuickNavCompositionSurface();
    if (SUCCEEDED(hr))
    {
        auto& nav = *quickNavigation_;
        auto* context = nav.sceneContext.Get();
        context->SetTarget(nav.sceneBitmap.Get());
        context->SetDpi(96.f, 96.f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            -static_cast<float>(nav.rect.left), -static_cast<float>(nav.rect.top)));
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0u, 0.f));
        drawing_.Reset();
        DrawQuickNavigationSurface(context);
        hr = context->EndDraw();
        context->SetTarget(nullptr);
        drawing_.Reset();
        nav.sceneDirty = SUCCEEDED(hr);
    }
    if (FAILED(hr))
    {
        RecoverQuickNavCompositionFailure(L"Draw precomposed scene", hr);
        EndPaint(hwnd, &ps);
        return;
    }
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
    if (!quickNavigation_->backdropCompositor.CommitSrgbContent())
    {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
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
    const snowdesktop::ScopedIconBackdropClip highlightClip(ctx.Get(), overlay, windowClipGeometry.Get());
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
    const float borderAlpha =
        std::clamp(quickNavigation_->appearance.widgetBorderAlpha, 0.0f, 1.0f);
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
    {
        (void)DrawWidgetPanelEdgeHighlight(ctx.Get(), overlay, windowCornerRadius,
            D2D1::ColorF(background.widgetBgR, background.widgetBgG, background.widgetBgB, background.widgetAlpha),
            &quickNavigation_->appearance, static_cast<float>(QuickNavScale(100)) / 100.0f);
    }

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

            std::wstring label = GetQuickNavTabLabel(tab, collectionIndices);
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

            std::wstring label = GetQuickNavTabLabel(quickNavigation_->tabDragIndex, collectionIndices);
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
                    {
                        const auto drawIcon = [&](auto* drawing) {
                            DrawDemoIdentityIcon(drawing, entry.parsingName, iconRect);
                        };
                        DrawIconWithShadows(ctx.Get(), iconRect, QuickNavScale(100) / 100.0f,
                            iconShadows_.NavigationStrength(), drawIcon);
                    }
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
