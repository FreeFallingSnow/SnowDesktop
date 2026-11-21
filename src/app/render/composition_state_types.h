#pragma once

#include "app/render/widget_icon_backdrops.h"
#include "layout/popup_animation_rules.h"
#include "widget/view/widget_composition_layer_rules.h"
#include "widget/view/widget_native_marquee.h"
#include "widget/view/widget_surface_renderer.h"
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using UiCompositionAnimationHost =
    snowdesktop::widget_composition_layer_rules::CompositionHost;

namespace snowdesktop::widget_runtime { struct WidgetSurfaceTarget; }
class DesktopBackdropCompositor;

struct UiCompositionAnimationOverlay
{
    Microsoft::WRL::ComPtr<IDCompositionVisual2> visual;
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    Microsoft::WRL::ComPtr<IDCompositionScaleTransform> scaleTransform;
    Microsoft::WRL::ComPtr<IDCompositionTranslateTransform> placementTransform;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    UINT surfaceWidth = 0, surfaceHeight = 0;
    RECT bounds{};
    UiCompositionAnimationHost host =
        UiCompositionAnimationHost::Desktop;
    bool active = false;
    bool presentingHighlights = false;
    std::optional<RECT> srgbRetirementBounds;

    HRESULT SetPlacementOffset(POINT offset) const;
    void FinishSrgbRetirement(DesktopBackdropCompositor& compositor, UiCompositionAnimationHost paintHost);
};

struct PopupFanBackdrop
{
    RECT viewport{};
    Microsoft::WRL::ComPtr<ID2D1Geometry> silhouette;
    D2D1_RECT_F bounds{};
    float blurRadius = 0.f, opacity = 0.f;
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
    Microsoft::WRL::ComPtr<IDCompositionEffectGroup> opacityEffect;
    Microsoft::WRL::ComPtr<IDCompositionRectangleClip> clip;
    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    std::shared_ptr<snowdesktop::widget_runtime::WidgetSurfaceTarget> renderTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> renderedVisual;
    std::wstring renderedContentId;
    snowdesktop::widget_runtime::WidgetSurfacePaint precomposedPaint;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> precomposedRecorder;
    bool renderedVisualAttached = false;
    bool renderFailed = false;
    bool occluded = false;
    RECT bounds{};
    UINT width = 0;
    UINT height = 0;
    std::uint64_t hiddenSince = 0;
    bool visible = false;
    // The body has finished drawing and its clip is open for this frame.
    bool contentVisible = false;
    struct HoverFade
    {
        snowdesktop::popup_animation_rules::State motion;
        bool waitingForPixels = false;
        bool compositorFadingOut = false;
    };
    std::optional<HoverFade> hoverFade;
    // Present only while the content and backdrop own matching native clips.
    std::optional<bool> hoverRollOpening;
    bool hoverRollUnavailable = false;
    float submittedOpacity = 1.0f;
    float HoverOpacity() const
    {
        return hoverFade ? hoverFade->motion.GetVisual().opacity : 1.0f;
    }
    bool backdropRequested = false;
    bool backdropRegistered = false;
    bool presentingSrgb = false;
    bool marqueeDamageUnavailable = false;
    int backdropCornerRadius = 0;
    int backdropBlurRadius = 0;
    snowdesktop::WidgetIconBackdrops iconBackdrops;
};
