#pragma once

#include "app/render/widget_icon_backdrops.h"
#include "widget/view/widget_composition_layer_rules.h"
#include "widget/view/widget_native_marquee.h"
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using UiCompositionAnimationHost =
    snowdesktop::widget_composition_layer_rules::CompositionHost;

namespace snowdesktop::widget_runtime { struct WidgetSurfaceTarget; }

struct UiCompositionAnimationOverlay
{
    Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    Microsoft::WRL::ComPtr<IDCompositionScaleTransform> scaleTransform;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    UINT surfaceWidth = 0, surfaceHeight = 0;
    RECT bounds{};
    UiCompositionAnimationHost host =
        UiCompositionAnimationHost::Desktop;
    bool active = false;
};

struct PopupFanIconDrawing
{
    Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
    Microsoft::WRL::ComPtr<ID2D1Effect> shadow;
    Microsoft::WRL::ComPtr<ID2D1Image> shadowImage;
    D2D1_RECT_F shadowBounds{};
};

struct WidgetMarqueeCompositionItem
{
    std::wstring instanceId;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> clipVisual;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> textVisual;
    Microsoft::WRL::ComPtr<IDCompositionRectangleClip> clip;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    UINT surfaceWidth = 0;
    UINT surfaceHeight = 0;
    float cycle = 0.0f;
    float speed = 0.0f;
    float phase = 0.0f;
    float originX = 0.0f;
    float clipWidth = 0.0f;
    float clipHeight = 0.0f;
    std::chrono::steady_clock::time_point phaseTime{};
    bool scrolling = false;
    bool paused = false;
    bool visible = true;
};

struct PendingWidgetMarqueeComposition
{
    std::vector<snowdesktop::widget_runtime::NativeMarqueeText> marquees;
    bool reducedMotion = false;
};

struct DesktopWidgetCompositionItem
{
    // The unordered-map node stays in place while its glass geometry changes.
    std::uintptr_t BackdropOwnerKey() const noexcept
    {
        return reinterpret_cast<std::uintptr_t>(this);
    }

    Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
    Microsoft::WRL::ComPtr<IDCompositionRectangleClip> clip;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    std::shared_ptr<snowdesktop::widget_runtime::WidgetSurfaceTarget> renderTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> renderedVisual;
    std::wstring renderedContentId;
    bool renderedVisualAttached = false;
    bool renderFailed = false;
    bool occluded = false;
    RECT bounds{};
    UINT width = 0;
    UINT height = 0;
    std::uint64_t hiddenSince = 0;
    bool visible = false;
    bool backdropRequested = false;
    bool backdropRegistered = false;
    int backdropCornerRadius = 0;
    int backdropBlurRadius = 0;
    snowdesktop::WidgetIconBackdrops iconBackdrops;
};
