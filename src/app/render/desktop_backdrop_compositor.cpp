#include "app/render/desktop_backdrop_update_rules.h"
// SPDX-FileCopyrightText: TranslucentTB contributors
// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: GPL-3.0-only
//
// The graphics-effect wrapper contains modified portions derived from
// TranslucentTB's ExplorerTAP effects at commit
// 322e2b7395a51975150126276308b415970e080b.

/**
 * @file desktop_backdrop_compositor.cpp
 * @brief Win32 DesktopWindowTarget + CompositionBackdropBrush 实现。
 */
#include "desktop_backdrop_compositor.h"
#include "layout/popup_motion_curve.h"
#include "diagnostics/performance_trace.h"

#include "desktop_backdrop_update_rules.h"
#include "panel_blur_geometry.h"
#include "app/popup/popup_window_pair_z_order.h"
#include "dock/dock_genie_rules.h"
#include "navigation/quick_navigation_genie_rules.h"
#include "navigation/quick_navigation_attraction_rules.h"
#include "layout/popup_round_geometry.h"
#include "ui/render/smooth_corner_geometry.h"
#include "ui/render/blur_sampling_bounds.h"
#include "icons/large_icon_shape_geometry.h"
#include "icons/icon_backdrop_geometry.h"
#include "platform/shell_overlay_window.h"
#include "widget/view/widget_native_marquee.h"
#include "widget/view/widget_surface_renderer.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1effects.h>
#include <dcomp.h>
#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <roapi.h>
#include <windows.graphics.effects.interop.h>
#include <windows.graphics.interop.h>
#include <windows.ui.composition.interop.h>

#include "common/l10n.h"

#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Core.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include "ui/render/backdrop_srgb_effect.h"
#pragma pop_macro("GetCurrentTime")

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wfn = winrt::Windows::Foundation::Numerics;
namespace wge = winrt::Windows::Graphics::Effects;
namespace wgx = winrt::Windows::Graphics::DirectX;
namespace ws = winrt::Windows::System;
namespace wuc = winrt::Windows::UI::Composition;
namespace wucc = winrt::Windows::UI::Composition::Core;
namespace wucd = winrt::Windows::UI::Composition::Desktop;
namespace awge = ABI::Windows::Graphics::Effects;
namespace awucd = ABI::Windows::UI::Composition::Desktop;
namespace awuci = ABI::Windows::UI::Composition;

namespace {

constexpr wchar_t kBackdropWindowClassName[] = L"SnowDesktopBackdropWindow";

std::wstring FormatHresult(const wchar_t* stage, HRESULT hr)
{
    wchar_t text[192]{};
    swprintf_s(text, L"%ls（0x%08X）", stage, static_cast<unsigned>(hr));
    return text;
}

LRESULT CALLBACK BackdropWindowProc(HWND window, UINT message, WPARAM wparam,
    LPARAM lparam)
{
    if (message == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (message == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterBackdropWindowClass()
{
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (GetClassInfoExW(instance, kBackdropWindowClassName, &existing))
        return true;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = BackdropWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kBackdropWindowClassName;
    return RegisterClassExW(&windowClass) != 0 ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/** @brief 可由 CompositionEffectFactory 消费的最小 D2D 高斯模糊描述。 */
struct GaussianBlurEffect : winrt::implements<GaussianBlurEffect,
    wge::IGraphicsEffect, wge::IGraphicsEffectSource,
    awge::IGraphicsEffectD2D1Interop>
{
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override
    {
        if (!id) return E_INVALIDARG;
        *id = CLSID_D2D1GaussianBlur;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index,
        awge::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override
    {
        if (!name || !index || !mapping) return E_INVALIDARG;
        const std::wstring_view property(name);
        if (property == L"BlurAmount")
        {
            *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        if (property == L"Optimization")
        {
            *index = D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        if (property == L"BorderMode")
        {
            *index = D2D1_GAUSSIANBLUR_PROP_BORDER_MODE;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        return E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = 3;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(UINT index,
        ABI::Windows::Foundation::IPropertyValue** value) noexcept override try
    {
        if (!value) return E_INVALIDARG;
        switch (index)
        {
        case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION:
            *value = wf::PropertyValue::CreateSingle(blurAmount)
                .as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION:
            *value = wf::PropertyValue::CreateUInt32(
                D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED)
                .as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE:
            *value = wf::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD)
                .as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        default:
            return E_BOUNDS;
        }
    }
    catch (...)
    {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSource(UINT index,
        awge::IGraphicsEffectSource** source) noexcept override
    {
        if (!source) return E_INVALIDARG;
        if (index != 0) return E_BOUNDS;
        winrt::copy_to_abi(effectSource, *reinterpret_cast<void**>(source));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = 1;
        return S_OK;
    }

    winrt::hstring Name() const { return effectName; }
    void Name(const winrt::hstring& value) { effectName = value; }

    wge::IGraphicsEffectSource effectSource{nullptr};
    float blurAmount = 24.0f;
    winrt::hstring effectName = L"SnowDesktopBackdropBlur";
};

/** @brief CompositionPath 对不可变 D2D 几何（路径或圆形）的原生适配。 */
struct BackdropGeometrySource : winrt::implements<BackdropGeometrySource,
    winrt::Windows::Graphics::IGeometrySource2D,
    ABI::Windows::Graphics::IGeometrySource2DInterop>
{
    explicit BackdropGeometrySource(winrt::com_ptr<ID2D1Geometry> geometry)
        : geometry_(std::move(geometry))
    {
    }

    HRESULT STDMETHODCALLTYPE GetGeometry(ID2D1Geometry** value) noexcept override
    {
        if (!value) return E_INVALIDARG;
        geometry_.copy_to(value);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE TryGetGeometryUsingFactory(
        ID2D1Factory*, ID2D1Geometry** value) noexcept override
    {
        if (!value) return E_INVALIDARG;
        *value = nullptr;
        // Composition can consume the original immutable geometry through
        // GetGeometry; no factory-specific copy is supplied by this adapter.
        return E_NOTIMPL;
    }

private:
    winrt::com_ptr<ID2D1Geometry> geometry_;
};

struct GenieOutlinePoint
{
    D2D1_POINT_2F source{};
    std::size_t strip = 0;
};

std::vector<GenieOutlinePoint> BuildGenieSourceOutline(
    float width, float height, float cornerRadius, bool vertical, bool attraction = false)
{
    namespace genie = snowdesktop::dock_genie;
    const float axisLength = vertical ? height : width;
    const float crossLength = vertical ? width : height;
    const snowdesktop::smooth_corners::Corner corner(cornerRadius, std::min(width, height)*.5f);
    // Include the smooth ramps even when a whole corner fits in one strip.
    constexpr std::size_t cornerSamples = 24;
    std::array<float, cornerSamples * 2> cornerAxes{};
    for (std::size_t index = 0; index < cornerSamples; ++index)
    {
        const float offset = corner.Point(static_cast<float>(index+1) / static_cast<float>(cornerSamples)).y;
        cornerAxes[index] = offset;
        cornerAxes[cornerAxes.size() - 1 - index] = axisLength - offset;
    }

    std::vector<GenieOutlinePoint> outline;
    outline.reserve(genie::StripCount * 4 + cornerAxes.size() * 2);
    const auto appendLowSide = [&](std::size_t strip, float axis) {
        const float inset = corner.Inset(std::min(axis, axisLength-axis));
        outline.push_back({vertical ? D2D1_POINT_2F{inset, axis}
                                   : D2D1_POINT_2F{axis, inset}, strip});
    };
    for (std::size_t index = 0; index < genie::StripCount; ++index)
    {
        const float begin = static_cast<float>(
            static_cast<double>(axisLength) * index / genie::StripCount);
        const float end = static_cast<float>(
            static_cast<double>(axisLength) * (index + 1) / genie::StripCount);
        appendLowSide(index, begin);
        for (const float axis : cornerAxes)
        {
            if (axis > begin && axis < end)
                appendLowSide(index, axis);
        }
        appendLowSide(index, end);
    }
    // Walk back along the opposite side. Adjacent projections share their
    // boundary, keeping the glass outline continuous with the live content.
    for (std::size_t remaining = outline.size(); remaining > 0; --remaining)
    {
        auto point = outline[remaining - 1];
        if (vertical)
            point.source.x = crossLength - point.source.x;
        else
            point.source.y = crossLength - point.source.y;
        outline.push_back(point);
    }
    if (!attraction) return outline;
    // Attraction also curves the horizontal caps. Sample them at mesh joins;
    // a single straight segment would leave glass outside the content warp.
    std::vector<GenieOutlinePoint> perimeter;
    for (std::size_t i = 0; i < outline.size(); ++i)
    {
        const auto point = outline[i];
        const auto next = outline[(i + 1) % outline.size()];
        perimeter.push_back(point);
        if (point.source.y != next.source.y ||
            (point.source.y != 0 && point.source.y != height)) continue;
        for (std::size_t column = 1; column < genie::StripCount; ++column)
        {
            const auto step = point.source.x < next.source.x ? column : genie::StripCount - column;
            const float x = width * static_cast<float>(step) / static_cast<float>(genie::StripCount);
            if (x > std::min(point.source.x, next.source.x) && x < std::max(point.source.x, next.source.x))
                perimeter.push_back({{x, point.source.y}, 0});
        }
    }
    return perimeter;
}

using CreateDispatcherQueueControllerFn = HRESULT(WINAPI*)(DispatcherQueueOptions,
    ABI::Windows::System::IDispatcherQueueController**);

struct BackgroundBlurFactory
{
    int radius = 0;
    std::mutex mutex;
    wuc::CompositionEffectFactory factory{nullptr};
    std::promise<bool> completion;
    std::shared_future<bool> result = completion.get_future().share();
};

winrt::com_ptr<GaussianBlurEffect> MakeBackdropBlur(int radius)
{
    auto blur = winrt::make_self<GaussianBlurEffect>();
    blur->effectSource = wuc::CompositionEffectSourceParameter(L"backdrop");
    blur->blurAmount = static_cast<float>(radius);
    return blur;
}

wuc::CompositionEffectFactory CreateBlurFactory(const wuc::Compositor& compositor, int radius)
{
    return compositor.CreateEffectFactory(*MakeBackdropBlur(radius));
}

struct BlurCompositeEffect : winrt::implements<BlurCompositeEffect,
    wge::IGraphicsEffect, wge::IGraphicsEffectSource, awge::IGraphicsEffectD2D1Interop>
{
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override
    {
        if (!id) return E_INVALIDARG;
        *id = CLSID_D2D1Composite;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*,
        awge::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UINT index,
        ABI::Windows::Foundation::IPropertyValue** value) noexcept override try
    {
        if (!value) return E_INVALIDARG;
        if (index != D2D1_COMPOSITE_PROP_MODE) return E_BOUNDS;
        *value = wf::PropertyValue::CreateUInt32(static_cast<UINT32>(mode))
            .as<ABI::Windows::Foundation::IPropertyValue>().detach();
        return S_OK;
    }
    catch (...) { return winrt::to_hresult(); }
    HRESULT STDMETHODCALLTYPE GetSource(UINT index,
        awge::IGraphicsEffectSource** source) noexcept override
    {
        if (!source) return E_INVALIDARG;
        if (index >= sources.size()) return E_BOUNDS;
        winrt::copy_to_abi(sources[index], *reinterpret_cast<void**>(source));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = static_cast<UINT>(sources.size());
        return S_OK;
    }
    winrt::hstring Name() const { return name; }
    void Name(const winrt::hstring& value) { name = value; }

    D2D1_COMPOSITE_MODE mode = D2D1_COMPOSITE_MODE_SOURCE_OVER;
    std::array<wge::IGraphicsEffectSource, 2> sources{nullptr, nullptr};
    winrt::hstring name;
};

winrt::hstring RadiusMaskName(int radius)
{
    return winrt::hstring(L"radiusMask" + std::to_wstring(radius));
}

wuc::CompositionEffectFactory CreateRegionalBlurFactory(const wuc::Compositor& compositor,
    const std::vector<int>& radii, bool iconsOnly)
{
    const auto makeBlur = [](int radius) {
        auto blur = MakeBackdropBlur(radius);
        blur->effectName = winrt::hstring(L"SnowDesktopBlurRadius" + std::to_wstring(radius));
        return blur;
    };
    wge::IGraphicsEffect result{nullptr};
    if (!iconsOnly) result = *makeBlur(radii.front());
    for (std::size_t i = 1; i < radii.size(); ++i)
    {
        // Each blur reads the same raw input. The mask selects its contribution
        // only after the filter has sampled neighbours outside the icon.
        auto masked = winrt::make_self<BlurCompositeEffect>();
        masked->mode = D2D1_COMPOSITE_MODE_DESTINATION_IN;
        masked->sources[0] = *makeBlur(radii[i]);
        masked->sources[1] = wuc::CompositionEffectSourceParameter(RadiusMaskName(radii[i]));
        if (!result)
        {
            result = *masked;
            continue;
        }
        auto composite = winrt::make_self<BlurCompositeEffect>();
        composite->sources[0] = result;
        composite->sources[1] = *masked;
        result = *composite;
    }
    auto factory = compositor.CreateEffectFactory(result);
    winrt::check_hresult(factory.ExtendedError());
    return factory;
}

struct SharedBackdropCompositionContext
{
    ws::DispatcherQueueController dispatcherController{nullptr};
    wucc::CompositorController compositorController{nullptr};
    wuc::Compositor compositor{nullptr};
    // Only this UI thread owns the slot. The worker owns a copied shared_ptr
    // and publishes an agile factory under the short result lock.
    std::shared_ptr<BackgroundBlurFactory> warmup;
};

SharedBackdropCompositionContext& SharedBackdropContext()
{
    // Window/visual-tree operations stay on the desktop UI thread. Sharing
    // one compositor keeps their updates transactional; an agile effect
    // factory can be prepared separately without editing either visual tree.
    static thread_local SharedBackdropCompositionContext context;
    return context;
}

} // namespace

struct DesktopBackdropCompositor::SrgbMarqueeScene
{
    RECT frame{};
    RECT allowed{};
    std::vector<snowdesktop::widget_runtime::NativeMarqueeText> labels;
    wuc::ContainerVisual visual{nullptr};
    std::vector<wuc::AnimationController> animations;
    bool paused = false;

    bool Matches(const RECT& bounds, const RECT& clipBounds,
        std::span<const snowdesktop::widget_runtime::NativeMarqueeText> next) const
    {
        return EqualRect(&frame, &bounds) && EqualRect(&allowed, &clipBounds) &&
            std::ranges::equal(labels, next, [](const auto& a, const auto& b) {
                return a.key == b.key && a.layout.Get() == b.layout.Get() &&
                    a.viewport.left == b.viewport.left && a.viewport.top == b.viewport.top &&
                    a.viewport.right == b.viewport.right && a.viewport.bottom == b.viewport.bottom &&
                    a.originX == b.originX && a.originY == b.originY &&
                    a.textWidth == b.textWidth && a.speed == b.speed && a.gap == b.gap &&
                    a.color == b.color && a.alpha == b.alpha && a.scrolling == b.scrolling;
            });
    }
};

struct DesktopBackdropCompositor::SrgbImageSpinScene
{
    struct Clock
    {
        explicit Clock(const wuc::Compositor& compositor) : phase(compositor.CreatePropertySet())
        { phase.InsertScalar(L"Angle", 0.f); }

        void Play(float speed)
        {
            if (speed == 0.f)
            {
                if (!paused)
                {
                    if (controller) controller.Pause();
                    else phase.StopAnimation(L"Angle");
                    paused = true;
                }
                return;
            }
            if (speed == turnSpeed && controller)
            {
                if (paused) controller.Resume();
            }
            else
            {
                const auto compositor = phase.Compositor();
                constexpr double longestSeconds = 24.0 * 24.0 * 3600.0; // WUC's maximum keyframe duration.
                const double period = 360.0 / speed;
                const auto seconds = std::chrono::duration<double>(std::min(period, longestSeconds));
                auto turn = compositor.CreateScalarKeyFrameAnimation();
                turn.Duration(std::chrono::duration_cast<wf::TimeSpan>(seconds));
                turn.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
                turn.StopBehavior(wuc::AnimationStopBehavior::LeaveCurrentValue);
                // At a speed change, hand the displayed angle to the new turn on the compositor.
                turn.InsertExpressionKeyFrame(0.f, L"Mod(this.StartingValue, 360.0)");
                turn.InsertExpressionKeyFrame(1.f, L"Mod(this.StartingValue, 360.0) + 360.0",
                    compositor.CreateLinearEasingFunction());
                phase.StartAnimation(L"Angle", turn);
                controller = nullptr;
                turnSpeed = 0.f;
                paused = false;
                controller = phase.TryGetAnimationController(L"Angle");
                if (!controller) throw winrt::hresult_error(E_UNEXPECTED);
                if (period > longestSeconds) controller.PlaybackRate(static_cast<float>(longestSeconds / period));
                turnSpeed = speed;
            }
            paused = false;
        }

        wuc::CompositionPropertySet phase{nullptr};
        wuc::AnimationController controller{nullptr};
        float turnSpeed = 0.f;
        bool paused = true;
    };

    snowdesktop::widget_runtime::WidgetImageSpin image;
    RECT frame{};
    winrt::com_ptr<ID2D1Device> device;
    wuc::SpriteVisual visual{nullptr};
    std::shared_ptr<Clock> clock;
};

struct DesktopBackdropCompositor::Impl
{
    struct PanelVisual
    {
        struct IconRegions
        {
            std::vector<snowdesktop::PanelBackdropIcon> icons;
            RECT base{}, allocation{}, visibleBounds{}, samplingFrame{}, sampleBounds{};
            RECT animationEnvelope{};
            LONG sampleMargin = 0;
            float corners = 0.0f;
            int blurRadius = 0;
            bool extendOutline = false;
            bool iconsOnly = false;
            int configuredIconRadius = 0;
            wuc::CompositionEffectBrush brush{nullptr};
            wuc::CompositionPathGeometry clip{nullptr};
            std::map<int, wuc::CompositionDrawingSurface> masks;
        };
        IconRegions iconRegions;
        bool iconRegionsApplied = false;
        bool smoothCorners = false;
        RECT frame{};
        RECT regionFrame{};
        std::uintptr_t ownerKey = 0;
        float cornerRadius = 0;
        float regionCornerRadius = 0;
        std::optional<RECT> animationFence;
        // Engaged once the popup lifecycle owns this panel's scale and opacity.
        std::optional<bool> popupTransformAnimating;
        RECT synchronizedRegionFrame{};
        float synchronizedRegionRadius = 0;
        bool regionFenceSynchronized = false;
        int blurRadius = 0;
        wuc::SpriteVisual visual{nullptr};
        wuc::SpriteVisual sample{nullptr};
        RECT sampleBounds{}, sampleLocal{};
        LONG sampleMargin = 0;
        wuc::CompositionEffectBrush baseBlur{nullptr};
        wuc::CompositionRoundedRectangleGeometry geometry{nullptr};
        wuc::CompositionPathGeometry smoothGeometry{nullptr};
        SIZE smoothSize{};
        float smoothRadius = -1.f;
        wuc::CompositionGeometricClip clip{nullptr};
        wuc::CompositionGeometry customGeometry{nullptr};
        bool seen = false;
        bool background = false;
        int iconShape = -1;
        SIZE iconSize{};
        RECT iconFrame{};
        std::optional<snowdesktop::IconBackdropClip> iconContentClip;
        int largeIconShape = -1;
        int largeIconFlagDirection = 0;
        SIZE largeIconSize{};

        // Cache only successfully submitted values. All panel property writers,
        // including animation hand-offs, use these setters so a normal paint
        // still restores the ordinary pose and visibility.
        std::optional<wfn::float4x4> submittedTransform;
        std::optional<wfn::float3> submittedOffset;
        std::optional<wfn::float2> submittedSize;
        std::optional<float> submittedRadius;
        std::optional<float> submittedOpacity;
        wuc::CompositionGeometry submittedClip{nullptr};

        const RECT& FenceFrame() const noexcept
        {
            return animationFence ? *animationFence : regionFrame;
        }
        float FenceRadius() const noexcept
        {
            // A scaled contour can intersect the corners of its resting
            // shape. Reserve the rectangle and let the visual clip shape it.
            return sampleMargin || animationFence || popupTransformAnimating.has_value() ? 0.0f : regionCornerRadius;
        }

        RECT SamplingFence() const noexcept
        {
            RECT bounds = snowdesktop::blur_sampling::SampleBounds(FenceFrame(), {}, sampleMargin);
            UnionRect(&bounds, &bounds, &sampleBounds);
            return bounds;
        }

        void SetSampleBounds(const RECT& allocation, const RECT& bounds, LONG margin)
        {
            sampleMargin = margin;
            sampleBounds = bounds;
            RECT local = bounds;
            OffsetRect(&local, -allocation.left, -allocation.top);
            if (EqualRect(&sampleLocal, &local)) return;
            sample.Offset({static_cast<float>(local.left), static_cast<float>(local.top), 0.0f});
            sample.Size({static_cast<float>(local.right - local.left),
                static_cast<float>(local.bottom - local.top)});
            sampleLocal = local;
        }

        void SetBaseSampleBounds()
        {
            const LONG margin = snowdesktop::blur_sampling::KernelMargin(static_cast<float>(blurRadius));
            SetSampleBounds(frame, snowdesktop::blur_sampling::SampleBounds(
                frame, animationFence.value_or(RECT{}), margin), margin);
        }

        void SetTransform(const wfn::float4x4& value)
        {
            if (submittedTransform && *submittedTransform == value) return;
            visual.TransformMatrix(value);
            submittedTransform = value;
            snowdesktop::performance::Value("render", "backdrop.panel.transform", {}, 1, ownerKey);
        }
        void SetOffset(const wfn::float3& value)
        {
            if (submittedOffset && *submittedOffset == value) return;
            visual.Offset(value);
            submittedOffset = value;
            snowdesktop::performance::Value("render", "backdrop.panel.offset", {}, 1, ownerKey);
        }
        void SetSize(const wfn::float2& value)
        {
            if (submittedSize && *submittedSize == value) return;
            visual.Size(value);
            geometry.Size(value);
            submittedSize = value;
            snowdesktop::performance::Value("render", "backdrop.panel.size", {}, 1, ownerKey);
        }
        void SetRadius(float value)
        {
            if (submittedRadius && *submittedRadius == value) return;
            geometry.CornerRadius(wfn::float2{value, value});
            submittedRadius = value;
            snowdesktop::performance::Value("render", "backdrop.panel.radius", {}, 1, ownerKey);
        }
        void SetClip(const wuc::CompositionGeometry& value)
        {
            if (submittedClip == value) return;
            clip.Geometry(value);
            submittedClip = value;
            snowdesktop::performance::Value("render", "backdrop.panel.clip", {}, 1, ownerKey);
        }
        void SetOpacity(float value)
        {
            if (submittedOpacity && *submittedOpacity == value) return;
            visual.Opacity(value);
            submittedOpacity = value;
            snowdesktop::performance::Value("render", "backdrop.panel.opacity", {}, value, ownerKey);
        }
    };

    // Values only: collecting a candidate must not touch the shared compositor.
    struct StagedPanel
    {
        enum class Kind { Rounded, Icon, LargeIcon } kind = Kind::Rounded;
        RECT frame{};
        float cornerRadius = 0, blurRadius = 0, opacity = 1;
        bool smoothCorners = false;
        std::uintptr_t ownerKey = 0;
        int shape = 0, flagDirection = 0;
        bool transformed = false;
        D2D1_MATRIX_4X4_F transform{};
        RECT projectedFrame{};
        std::optional<snowdesktop::IconBackdropClip> iconContentClip;
        std::vector<snowdesktop::PanelBackdropIcon> panelIcons;
    };
    std::vector<StagedPanel> stagedPanels;
    std::unordered_map<std::uintptr_t, std::size_t> stagedPanelOwners;
    auto FindStagedPanel(const RECT& frame, std::uintptr_t ownerKey)
    {
        if (ownerKey)
        {
            const auto owner = stagedPanelOwners.find(ownerKey);
            return owner == stagedPanelOwners.end() ? stagedPanels.end() :
                stagedPanels.begin() + static_cast<std::ptrdiff_t>(owner->second);
        }
        return std::find_if(stagedPanels.begin(), stagedPanels.end(), [&](const auto& panel) {
            return snowdesktop::desktop_backdrop_update_rules::PanelIdentityMatches(
                panel.ownerKey, panel.frame, ownerKey, frame);
        });
    }
    void RebuildStagedPanelOwners()
    {
        stagedPanelOwners.clear();
        for (std::size_t index = 0; index < stagedPanels.size(); ++index)
            if (stagedPanels[index].ownerKey) stagedPanelOwners.emplace(stagedPanels[index].ownerKey, index);
    }
    bool stagingFrame = false;
    RECT stagedOcclusionRect{};

    HWND contentWindow = nullptr;
    HWND backdropWindow = nullptr;
    ws::DispatcherQueueController dispatcherController{nullptr};
    wucc::CompositorController compositorController{nullptr};
    wuc::Compositor compositor{nullptr};
    wucd::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual placementRoot{nullptr};
    wuc::ContainerVisual root{nullptr};
    std::unordered_map<int, wuc::CompositionEffectFactory> blurFactories;
    wuc::CompositionEffectFactory precomposedSceneFactory{nullptr};
    struct SrgbRetirement { bool waiting = false; };
    struct SrgbContent
    {
        struct Buffer
        {
            wuc::CompositionDrawingSurface surface{nullptr};
            SIZE size{};
        };
        Buffer active, candidate;
        wuc::CompositionSurfaceBrush sceneBrush{nullptr};
        wuc::CompositionEffectBrush sceneEffect{nullptr};
        PreparedMarquees marquees;
        PreparedImageSpin spin;
        wuc::ContainerVisual layers{nullptr};
        wuc::SpriteVisual body{nullptr};
        wuc::CompositionVisualSurface layersSurface{nullptr};
        wuc::CompositionSurfaceBrush flattened{nullptr};
        RECT stagedFrame{};
        bool staged = false;
        wuc::SpriteVisual visual{nullptr};
        std::shared_ptr<SrgbRetirement> retirement;
    };
    HWND srgbContentWindow = nullptr;
    wucd::DesktopWindowTarget srgbContentTarget{nullptr};
    wuc::ContainerVisual srgbContentRoot{nullptr};
    bool srgbRootOnPopup = false;
    bool popupPresentationViewport = false;
    std::unordered_map<std::uintptr_t, SrgbContent> srgbContent;
    std::vector<std::uintptr_t> srgbContentOrder;
    bool srgbContentOrderDirty = false;
    bool srgbContentCommitPending = false;
    bool srgbPublicationFailed = false;
    bool srgbRetirementPending = false;
    std::shared_ptr<int> srgbLifetime = std::make_shared<int>(0);
    std::map<std::pair<bool, std::vector<int>>, wuc::CompositionEffectFactory> regionalFactories;
    winrt::com_ptr<ID2D1Device> maskDrawDevice;
    wuc::CompositionGraphicsDevice maskGraphicsDevice{nullptr};
    std::vector<PanelVisual> panels;
    std::unordered_map<std::uintptr_t, std::size_t> panelOwners;
    struct ChangedIconPanel { RECT frame; std::uintptr_t ownerKey; bool redrawing; };
    std::vector<ChangedIconPanel> changedIconPanels;

    auto FindPanel(const RECT& frame, std::uintptr_t ownerKey, bool background = false)
    {
        if (ownerKey)
        {
            const auto owner = panelOwners.find(ownerKey);
            return owner == panelOwners.end() ? panels.end() :
                panels.begin() + static_cast<std::ptrdiff_t>(owner->second);
        }
        return std::find_if(panels.begin(), panels.end(), [&](const auto& panel) {
            return panel.background == background &&
                snowdesktop::desktop_backdrop_update_rules::PanelIdentityMatches(
                    panel.ownerKey, panel.frame, ownerKey, frame);
        });
    }

    auto ErasePanel(std::vector<PanelVisual>::iterator panel)
    {
        panelOwners.erase(panel->ownerKey);
        auto next = panels.erase(panel);
        // Keep vector order (and therefore native stacking order) unchanged.
        for (auto moved = next; moved != panels.end(); ++moved)
            if (moved->ownerKey) panelOwners.at(moved->ownerKey) = static_cast<std::size_t>(moved - panels.begin());
        return next;
    }
    wuc::SpriteVisual genieRoot{nullptr};
    wuc::SpriteVisual genieSource{nullptr};
    wuc::CompositionPathGeometry genieGeometry{nullptr};
    winrt::com_ptr<ID2D1Factory> genieGeometryFactory;
    std::vector<GenieOutlinePoint> genieOutline;
    RECT geniePanelFrame{};
    SIZE genieHostSize{};
    float genieCornerRadius = 0;
    bool genieVertical = true;
    bool genieAttraction = false;
    float genieSourceOpacity = 1.0f;
    float genieOpacity = 1.0f;
    std::wstring lastError;
    bool completeCollection = true;
    bool collectingFrame = false;
    bool frameCommitPending = false;
    bool blurFactoriesDirty = false;
    bool available = false;
    bool transformedRegionDirty = false;
    bool popupMode = false;
    bool popupTopmost = false;
    RECT popupPadding{};
    RECT popupDisplayBounds{};
    RECT submittedPopupDisplayClip{};
    wuc::CompositionRoundedRectangleGeometry popupDisplayClip{nullptr};
    bool visible = true;
    bool animationPathRegionExpanded = false;
    bool rootTranslationOwnsGlass = false;
    bool stableWindowRegion = false;
    RECT occlusionRect{};
    bool panelRegionSnapshotValid = false;
    bool synchronizedStableWindowRegion = false;
    std::size_t synchronizedPanelCount = 0;
    RECT synchronizedOcclusionRect{};
    SIZE synchronizedWindowSize{};
    UINT synchronizedWindowDpi = 0;

    void SetError(const wchar_t* stage, HRESULT hr)
    {
        lastError = FormatHresult(stage, hr);
        available = false;
    }

    void BeginCollection(bool complete, bool stableRegion = false)
    {
        completeCollection = complete;
        stableWindowRegion = stableRegion;
        collectingFrame = true;
        if (complete)
            for (auto& panel : panels) panel.seen = false;
        SyncWindowPlacement();
    }

    bool EnsureDispatcherQueue()
    {
        SharedBackdropCompositionContext& shared =
            SharedBackdropContext();
        if (!ws::DispatcherQueue::GetForCurrentThread())
        {
            HMODULE coreMessaging = LoadLibraryW(L"CoreMessaging.dll");
            if (!coreMessaging)
            {
                SetError(_LW("backdrop.load_core_msg"),
                    HRESULT_FROM_WIN32(GetLastError()));
                return false;
            }
            const auto createController =
                reinterpret_cast<CreateDispatcherQueueControllerFn>(
                    GetProcAddress(coreMessaging,
                        "CreateDispatcherQueueController"));
            if (!createController)
            {
                const HRESULT hr =
                    HRESULT_FROM_WIN32(GetLastError());
                FreeLibrary(coreMessaging);
                SetError(_LW("backdrop.find_dispatch"), hr);
                return false;
            }

            DispatcherQueueOptions options{};
            options.dwSize = sizeof(options);
            options.threadType = DQTYPE_THREAD_CURRENT;
            options.apartmentType = DQTAT_COM_STA;
            ABI::Windows::System::IDispatcherQueueController*
                rawController = nullptr;
            const HRESULT hr = createController(
                options, &rawController);
            FreeLibrary(coreMessaging);
            if (FAILED(hr) || !rawController)
            {
                SetError(_LW("backdrop.create_dispatch"),
                    FAILED(hr) ? hr : E_FAIL);
                return false;
            }
            shared.dispatcherController =
                ws::DispatcherQueueController{
                    rawController,
                    winrt::take_ownership_from_abi };
        }

        try
        {
            if (!shared.compositor)
            {
                wucc::CompositorController controller;
                controller.CommitNeeded([](const auto& sender, const auto&) noexcept {
                    try { sender.Commit(); }
                    catch (...) { /* Explicit frame commits retain error handling. */ }
                });
                shared.compositor = controller.Compositor();
                shared.compositorController = std::move(controller);
            }
            compositor = shared.compositor;
            compositorController = shared.compositorController;
            dispatcherController =
                shared.dispatcherController;
        }
        catch (const winrt::hresult_error& error)
        {
            SetError(_LW("backdrop.activate_compositor"),
                error.code());
            return false;
        }
        return true;
    }

    bool QueryContentPlacement(HWND parent, POINT& origin, SIZE& size) const
    {
        if (!contentWindow || !IsWindow(contentWindow))
            return false;
        RECT rect{};
        if (!GetWindowRect(contentWindow, &rect))
            return false;
        if (popupMode)
        {
            const LONG margin = PlacementMargin();
            origin = { rect.left - popupPadding.left - margin, rect.top - popupPadding.top - margin };
            size.cx = std::max<LONG>(1, rect.right + popupPadding.right + margin - origin.x);
            size.cy = std::max<LONG>(1, rect.bottom + popupPadding.bottom + margin - origin.y);
            return true;
        }
        if (!parent || !IsWindow(parent))
            return false;
        POINT points[2] = {
            { rect.left, rect.top },
            { rect.right, rect.bottom },
        };
        MapWindowPoints(nullptr, parent, points, 2);
        origin = points[0];
        size.cx = std::max<LONG>(1, points[1].x - points[0].x);
        size.cy = std::max<LONG>(1, points[1].y - points[0].y);
        return true;
    }

    LONG PlacementMargin() const noexcept
    {
        return popupMode ? snowdesktop::blur_sampling::kMaximumBackdropMargin : 0;
    }

    bool SyncPopupDisplayClip(POINT screenOrigin)
    {
        // The animation moves root inside a fixed display clip on
        // placementRoot. The native window still reserves the blur halo.
        RECT clip = popupDisplayBounds;
        const bool enabled = !IsRectEmpty(&clip);
        const LONG margin = PlacementMargin();
        if (enabled) OffsetRect(&clip, -screenOrigin.x - margin, -screenOrigin.y - margin);
        if (!enabled && !popupDisplayClip) return true;
        if (enabled && popupDisplayClip && EqualRect(&submittedPopupDisplayClip, &clip)) return true;
        try
        {
            if (enabled)
            {
                auto geometry = popupDisplayClip;
                if (!geometry)
                {
                    // CompositionRectangleGeometry has a known issue on Windows 10 2004.
                    geometry = compositor.CreateRoundedRectangleGeometry();
                    geometry.CornerRadius({0.f, 0.f});
                }
                geometry.Offset({static_cast<float>(clip.left), static_cast<float>(clip.top)});
                geometry.Size({static_cast<float>(clip.right - clip.left), static_cast<float>(clip.bottom - clip.top)});
                if (!popupDisplayClip) placementRoot.Clip(compositor.CreateGeometricClip(geometry));
                popupDisplayClip = std::move(geometry);
            }
            else
            {
                placementRoot.Clip(nullptr);
                popupDisplayClip = nullptr;
            }
            submittedPopupDisplayClip = clip;
            frameCommitPending = true;
            return true;
        }
        catch (const winrt::hresult_error& error)
        {
            SetError(L"backdrop.popup_display_clip", error.code());
            return false;
        }
    }

    bool SyncWindowPlacement()
    {
        if (!available || !backdropWindow || !IsWindow(backdropWindow) ||
            !contentWindow || !IsWindow(contentWindow))
            return false;
        HWND parent = popupMode ? nullptr : GetParent(contentWindow);
        if (!popupMode)
        {
            if (!parent || !IsWindow(parent))
                return false;
            if (GetParent(backdropWindow) != parent)
                SetParent(backdropWindow, parent);
        }

        POINT origin{};
        SIZE size{};
        if (!QueryContentPlacement(parent, origin, size))
            return false;

        POINT screenOrigin = origin;
        SetLastError(ERROR_SUCCESS);
        if (!popupMode &&
            MapWindowPoints(
                parent, nullptr, &screenOrigin, 1) == 0 &&
            GetLastError() != ERROR_SUCCESS)
        {
            return false;
        }
        const RECT expectedRect{
            screenOrigin.x,
            screenOrigin.y,
            screenOrigin.x + size.cx,
            screenOrigin.y + size.cy,
        };
        RECT currentRect{};
        const bool placementMatches =
            GetWindowRect(backdropWindow, &currentRect) &&
            EqualRect(&currentRect, &expectedRect) != FALSE;
        const bool pairedZOrder =
            GetWindow(backdropWindow, GW_HWNDPREV) ==
                contentWindow;
        const bool currentlyVisible =
            (GetWindowLongPtrW(
                backdropWindow, GWL_STYLE) & WS_VISIBLE) != 0;
        const bool visibilityMatches =
            currentlyVisible == visible;
        if (placementMatches && pairedZOrder &&
            visibilityMatches)
        {
            // Ordinary panel paints do not own HWND placement. In
            // particular, a Dock demotion has already moved the content and
            // glass pair through the coordinated Z-order policy; issuing another
            // helper-only SetWindowPos/SWP_SHOWWINDOW here would make DWM
            // re-evaluate the BackdropBrush source for an extra frame.
            return SyncPopupDisplayClip(screenOrigin);
        }

        UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (placementMatches)
            flags |= SWP_NOMOVE | SWP_NOSIZE;
        if (pairedZOrder)
            flags |= SWP_NOZORDER;
        if (!visibilityMatches)
            flags |= visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        return SetWindowPos(
            backdropWindow,
            pairedZOrder ? nullptr : contentWindow,
            origin.x, origin.y, size.cx, size.cy,
            flags) != FALSE && SyncPopupDisplayClip(screenOrigin);
    }

    // A deferred frame may retire old pixels before it publishes new ones.
    // A shrink-only update returns true only if the final region is in place.
    bool SyncPanelWindowRegion(bool shrinkOnly = false)
    {
        if (!available || !backdropWindow ||
            !IsWindow(backdropWindow))
            return false;
        // While the root moves, the visual clips own each card contour. Keep
        // the whole viewport available while still subtracting any guide HWND.
        const bool fenceAllocation = stableWindowRegion || rootTranslationOwnsGlass || popupPresentationViewport;

        // The owned HWND fence depends on the final collection, not on the
        // individual setters used while collecting it. In particular, a card
        // can restore its ordinary pose and reapply the same projection in one
        // frame. Compare exact physical-pixel inputs, without hash collisions
        // or another per-frame snapshot allocation. Opacity and Composition
        // clips deliberately retain the existing, conservative HWND semantics.
        RECT windowFrame{};
        const UINT windowDpi = GetDpiForWindow(backdropWindow);
        const bool windowGeometryKnown = GetWindowRect(backdropWindow, &windowFrame) && windowDpi != 0;
        const SIZE windowSize{windowFrame.right - windowFrame.left, windowFrame.bottom - windowFrame.top};
        if (fenceAllocation && !windowGeometryKnown) return false;
        bool inputsMatch = panelRegionSnapshotValid && !animationPathRegionExpanded &&
            synchronizedStableWindowRegion == fenceAllocation &&
            windowGeometryKnown && synchronizedPanelCount == panels.size() &&
            EqualRect(&synchronizedOcclusionRect, &occlusionRect) &&
            synchronizedWindowSize.cx == windowSize.cx && synchronizedWindowSize.cy == windowSize.cy &&
            synchronizedWindowDpi == windowDpi;
        if (inputsMatch && !fenceAllocation)
        {
            for (const auto& panel : panels)
            {
                const RECT samplingFence = panel.SamplingFence();
                if (!panel.regionFenceSynchronized ||
                    !EqualRect(&panel.synchronizedRegionFrame, &samplingFence) ||
                    panel.synchronizedRegionRadius != panel.FenceRadius())
                {
                    inputsMatch = false;
                    break;
                }
            }
        }
        if (inputsMatch)
            return true;

        // A failed allocation/combine/publication must remain retryable even
        // when the next commit supplies exactly the same input geometry.
        panelRegionSnapshotValid = false;
        const auto rememberSynchronizedRegion = [&] {
            animationPathRegionExpanded = false;
            if (!windowGeometryKnown) return;
            for (auto& panel : panels)
            {
                panel.synchronizedRegionFrame = panel.SamplingFence();
                panel.synchronizedRegionRadius = panel.FenceRadius();
                panel.regionFenceSynchronized = true;
            }
            synchronizedPanelCount = panels.size();
            synchronizedStableWindowRegion = fenceAllocation;
            synchronizedOcclusionRect = occlusionRect;
            synchronizedWindowSize = windowSize;
            synchronizedWindowDpi = windowDpi;
            panelRegionSnapshotValid = true;
        };

        // Staged Dock frames move inside one stable allocation. SetWindowRgn
        // takes effect independently of the Composition commit: a tight new
        // fence can cut away the still-displayed old glass. Keep this HWND
        // fence stable and let the visual clips move with their own pixels.
        // An empty collection still retires all glass synchronously.
        HRGN panelRegion = fenceAllocation && (!panels.empty() || popupPresentationViewport)
            ? CreateRectRgn(0, 0, windowSize.cx, windowSize.cy)
            : CreateRectRgn(0, 0, 0, 0);
        if (!panelRegion)
            return false;
        if (!fenceAllocation)
        {
            for (const PanelVisual& panel : panels)
            {
                // This native region also reserves the filter's input. Only
                // the visual clip chooses which blurred pixels are displayed.
                RECT samplingFence = panel.SamplingFence();
                const LONG margin = PlacementMargin();
                OffsetRect(&samplingFence, margin, margin);
                HRGN frameRegion = snowdesktop::popup_round_geometry::CreateWindowFence(
                    samplingFence, panel.FenceRadius());
                if (!frameRegion)
                {
                    DeleteObject(panelRegion);
                    return false;
                }
                const int combineResult = CombineRgn(
                    panelRegion, panelRegion,
                    frameRegion, RGN_OR);
                DeleteObject(frameRegion);
                if (combineResult == ERROR)
                {
                    DeleteObject(panelRegion);
                    return false;
                }
            }
        }

        if (!IsRectEmpty(&occlusionRect))
        {
            RECT exclusion = occlusionRect;
            const LONG margin = PlacementMargin();
            OffsetRect(&exclusion, margin, margin);
            HRGN excluded = CreateRectRgnIndirect(&exclusion);
            const bool ok = excluded && CombineRgn(panelRegion, panelRegion, excluded, RGN_DIFF) != ERROR;
            if (excluded) DeleteObject(excluded);
            if (!ok) { DeleteObject(panelRegion); return false; }
        }
        HRGN currentRegion = CreateRectRgn(0, 0, 0, 0);
        const int currentRegionType = currentRegion
            ? GetWindowRgn(backdropWindow, currentRegion)
            : ERROR;
        const bool unchanged =
            currentRegionType != ERROR &&
            EqualRgn(panelRegion, currentRegion) != FALSE;
        bool appliedFinalRegion = true;
        if (shrinkOnly && !unchanged)
        {
            // Intersect the existing fence with the new one. Removed areas
            // disappear now, while additions wait for the body and glass commits.
            const HRGN intersection = currentRegionType != ERROR
                ? CreateRectRgn(0, 0, 0, 0) : nullptr;
            if (!intersection ||
                CombineRgn(intersection, currentRegion, panelRegion, RGN_AND) == ERROR ||
                EqualRgn(intersection, currentRegion))
            {
                if (intersection) DeleteObject(intersection);
                if (currentRegion) DeleteObject(currentRegion);
                DeleteObject(panelRegion);
                return false;
            }
            appliedFinalRegion = EqualRgn(intersection, panelRegion) != FALSE;
            DeleteObject(panelRegion);
            panelRegion = intersection;
        }
        if (currentRegion)
            DeleteObject(currentRegion);
        if (unchanged)
        {
            DeleteObject(panelRegion);
            rememberSynchronizedRegion();
            return true;
        }

        // The helper HWND is a synchronous visibility fence for the separate
        // Windows Composition tree. Ordinary collections fence each panel;
        // staged Dock collections fence the allocation and guide exclusion.
        if (!SetWindowRgn(backdropWindow, panelRegion, FALSE))
        {
            DeleteObject(panelRegion);
            return false;
        }
        if (appliedFinalRegion)
            rememberSynchronizedRegion();
        return appliedFinalRegion;
    }

    void SetAnimationPathRegionExpanded(bool expanded)
    {
        if (!available || !backdropWindow ||
            !IsWindow(backdropWindow) ||
            animationPathRegionExpanded == expanded)
            return;
        if (expanded && IsRectEmpty(&occlusionRect))
        {
            if (SetWindowRgn(backdropWindow, nullptr, FALSE))
            {
                animationPathRegionExpanded = true;
                panelRegionSnapshotValid = false;
            }
            return;
        }
        SyncPanelWindowRegion();
    }

    using PendingRetirement = std::pair<std::uintptr_t, std::weak_ptr<SrgbRetirement>>;
    static winrt::fire_and_forget WaitForDirectContent(
        DesktopBackdropCompositor* owner, std::weak_ptr<int> lifetime,
        winrt::com_ptr<IDCompositionDevice2> device, ws::DispatcherQueue queue,
        std::vector<PendingRetirement> pending)
    {
        // A WinRT visual may remain above the replacement for one refresh.
        // Waiting off the UI thread keeps moving desktop tiles responsive.
        co_await winrt::resume_background();
        const HRESULT result = device->WaitForCommitCompletion();
        try
        {
            queue.TryEnqueue([owner, lifetime, pending = std::move(pending), result] {
                if (lifetime.expired())
                    return;
                bool removed = false;
                for (const auto& [key, item] : pending)
                {
                    const auto retirement = item.lock();
                    if (!retirement)
                        continue;
                    if (SUCCEEDED(result))
                    {
                        owner->RemoveSrgbContent(key);
                        removed = true;
                    }
                    else
                    {
                        retirement->waiting = false;
                        owner->impl_->srgbRetirementPending = true;
                    }
                }
                if (removed)
                    (void)owner->CommitSrgbContent();
            });
        }
        catch (...) { /* The old sRGB content remains attached. */ }
    }

    bool RequestCommit() noexcept
    {
        if (!available || !compositorController)
            return false;
        try
        {
            // Submit now, like the content's IDCompositionDevice::Commit.
            // RequestCommitAsync only schedules a cycle: a drop can present
            // the resized content and then save layout while glass is still
            // waiting for the UI dispatcher. Do not wait for GPU completion.
            {
                snowdesktop::performance::Scope trace("composition.srgb", "commit");
                compositorController.Commit();
            }
            frameCommitPending = false;
            // The glass and sRGB visuals share this compositor and were
            // submitted together by this commit.
            srgbContentCommitPending = false;
            if (srgbContent.empty())
                srgbPublicationFailed = false;
            return true;
        }
        catch (const winrt::hresult_error& error)
        {
            SetError(_LW("backdrop.update_panel"), error.code());
            return false;
        }
    }

    wuc::CompositionEffectFactory GetBlurFactory(int blurRadius)
    {
        const auto found = blurFactories.find(blurRadius);
        if (found != blurFactories.end())
            return found->second;

        wuc::CompositionEffectFactory factory{nullptr};
        auto& shared = SharedBackdropContext();
        if (const auto ready = shared.warmup; ready && ready->radius == blurRadius)
        {
            std::unique_lock lock(ready->mutex, std::try_to_lock);
            if (lock.owns_lock() && ready->factory &&
                winrt::get_abi(ready->factory.Compositor()) == winrt::get_abi(compositor))
            {
                factory = ready->factory;
                snowdesktop::performance::Value("popup", "backdrop.warmup.reused", {}, 1);
            }
        }
        // Opening never waits for a queued/in-flight warmup.
        if (!factory) factory = CreateBlurFactory(compositor, blurRadius);
        blurFactories.emplace(blurRadius, factory);
        blurFactoriesDirty = true;
        return factory;
    }

    wuc::CompositionEffectBrush CreateBlurBrush(int blurRadius)
    {
        // A backdrop effect brush is sized for the SpriteVisual that consumes
        // it. Sharing one brush between differently-sized panels can retain
        // the shorter effect surface and stretch its final row over a taller
        // panel. Factories are safe to cache, but every panel needs its own
        // effect brush and backdrop source.
        auto brush = GetBlurFactory(blurRadius).CreateBrush();
        brush.SetSourceParameter(
            L"backdrop", compositor.CreateBackdropBrush());
        return brush;
    }

    wuc::CompositionDrawingSurface CreateRadiusMask(const SIZE& size)
    {
        return maskGraphicsDevice.CreateDrawingSurface(wf::Size{
            static_cast<float>(size.cx), static_cast<float>(size.cy)},
            wgx::DirectXPixelFormat::B8G8R8A8UIntNormalized, wgx::DirectXAlphaMode::Premultiplied);
    }

    void PaintRadiusMask(const wuc::CompositionDrawingSurface& surface, ID2D1Geometry* geometry,
        POINT inset = {})
    {
        const auto interop = surface.as<awuci::ICompositionDrawingSurfaceInterop>();
        POINT offset{};
        winrt::com_ptr<ID2D1DeviceContext> context;
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
            context.put_void(), &offset));
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x + inset.x), static_cast<float>(offset.y + inset.y)));
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->Clear(D2D1::ColorF(0, 0.0f));
        winrt::com_ptr<ID2D1SolidColorBrush> ink;
        HRESULT draw = S_OK;
        if (geometry)
        {
            draw = context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), ink.put());
            if (SUCCEEDED(draw)) context->FillGeometry(geometry, ink.get());
        }
        const HRESULT finish = interop->EndDraw();
        winrt::check_hresult(draw);
        winrt::check_hresult(finish);
    }

    wuc::CompositionSurfaceBrush MakeRadiusMaskBrush(const wuc::CompositionDrawingSurface& surface)
    {
        const auto mask = compositor.CreateSurfaceBrush(surface);
        mask.Stretch(wuc::CompositionStretch::None);
        mask.HorizontalAlignmentRatio(0.0f);
        mask.VerticalAlignmentRatio(0.0f);
        return mask;
    }

    void EnsureMaskGraphicsDevice(ID2D1Device* device)
    {
        if (maskDrawDevice.get() != device)
        {
            wuc::CompositionGraphicsDevice graphics{nullptr};
            winrt::check_hresult(compositor.as<awuci::ICompositorInterop>()->CreateGraphicsDevice(
                device, reinterpret_cast<awuci::ICompositionGraphicsDevice**>(winrt::put_abi(graphics))));
            maskGraphicsDevice = std::move(graphics);
            maskDrawDevice.copy_from(device);
        }
    }

    // The popup helper is immediately below the input HWND. Its placement root
    // already aligns client coordinates and applies the stationary viewport.
    // Move the existing scene above glass, retaining the brush, live backdrop
    // input, and pixel order. Both target changes belong to this controller.
    void SetSrgbRootOnPopup(bool onPopup)
    {
        if (srgbRootOnPopup == onPopup)
        {
            // Recover a target detached before an unsuccessful insertion.
            if (!onPopup && popupPresentationViewport && srgbContentTarget &&
                srgbContentRoot && !srgbContentTarget.Root())
            {
                srgbContentTarget.Root(srgbContentRoot);
                srgbContentCommitPending = true;
            }
            return;
        }
        auto children = placementRoot.Children();
        if (onPopup)
        {
            srgbContentTarget.Root(nullptr);
            try { children.InsertAtTop(srgbContentRoot); }
            catch (...) { srgbContentTarget.Root(srgbContentRoot); throw; }
        }
        else
        {
            children.Remove(srgbContentRoot);
            try { srgbContentTarget.Root(srgbContentRoot); }
            catch (...) { children.InsertAtTop(srgbContentRoot); throw; }
        }
        srgbRootOnPopup = onPopup;
        srgbContentCommitPending = true;
    }

    void ResetSrgbContent()
    {
        if (srgbRootOnPopup)
        {
            placementRoot.Children().Remove(srgbContentRoot);
            srgbRootOnPopup = false;
        }
        if (srgbContentTarget) srgbContentTarget.Root(nullptr);
        srgbContent.clear();
        srgbContentOrder.clear();
        srgbContentOrderDirty = false;
        srgbContentCommitPending = false;
        srgbPublicationFailed = false;
        srgbRetirementPending = false;
        srgbContentRoot = nullptr;
        srgbContentTarget = nullptr;
        srgbContentWindow = nullptr;
    }

    void EnsureSrgbContentTarget(HWND window)
    {
        if (srgbContentWindow == window && srgbContentTarget) return;
        ResetSrgbContent();
        const auto interop = compositor.as<awucd::ICompositorDesktopInterop>();
        wucd::DesktopWindowTarget frameTarget{nullptr};
        winrt::check_hresult(interop->CreateDesktopWindowTarget(window, TRUE,
            reinterpret_cast<awucd::IDesktopWindowTarget**>(winrt::put_abi(frameTarget))));
        auto frameRoot = compositor.CreateContainerVisual();
        frameTarget.Root(frameRoot);
        srgbContentWindow = window;
        srgbContentTarget = std::move(frameTarget);
        srgbContentRoot = std::move(frameRoot);
    }

    SrgbContent::Buffer CreateSrgbContentBuffer(SIZE size)
    {
        SrgbContent::Buffer buffer;
        buffer.size = size;
        const wf::Size extent{static_cast<float>(size.cx), static_cast<float>(size.cy)};
        buffer.surface = maskGraphicsDevice.CreateDrawingSurface(extent,
            wgx::DirectXPixelFormat::B8G8R8A8UIntNormalized, wgx::DirectXAlphaMode::Premultiplied);
        return buffer;
    }

    // All moving children are sampled together before the single scene/backdrop effect.
    void EnsureSrgbLayers(std::uintptr_t key)
    {
        auto& frame = srgbContent[key];
        if (frame.layers) return;
        auto layers = compositor.CreateContainerVisual();
        auto body = compositor.CreateSpriteVisual();
        layers.Children().InsertAtBottom(body);
        auto surface = compositor.CreateVisualSurface();
        surface.SourceVisual(layers);
        auto brush = compositor.CreateSurfaceBrush(surface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0.f);
        brush.VerticalAlignmentRatio(0.f);
        brush.BitmapInterpolationMode(wuc::CompositionBitmapInterpolationMode::NearestNeighbor);
        frame.body = std::move(body);
        frame.layersSurface = std::move(surface);
        frame.flattened = std::move(brush);
        frame.layers = std::move(layers);
    }

    static void BindSrgbLayers(SrgbContent& frame, const PreparedImageSpin& spin,
        const PreparedMarquees& marquees)
    {
        const auto children = frame.layers.Children();
        children.RemoveAll();
        if (spin || marquees)
        {
            frame.body.Brush(frame.sceneBrush);
            children.InsertAtBottom(frame.body);
            if (spin) children.InsertAtTop(spin->visual);
            if (marquees) children.InsertAtTop(marquees->visual);
            frame.sceneEffect.SetSourceParameter(L"scene", frame.flattened);
        }
        else frame.sceneEffect.SetSourceParameter(L"scene", frame.sceneBrush);
    }

    static void PaintSrgbSurface(const wuc::CompositionDrawingSurface& surface,
        const SrgbSceneDraw& draw, const RECT* damage = nullptr)
    {
        auto interop = surface.as<awuci::ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> context;
        POINT origin{};
        {
            snowdesktop::performance::Scope trace("composition.srgb", "begin_draw");
            winrt::check_hresult(interop->BeginDraw(damage, __uuidof(ID2D1DeviceContext),
                context.put_void(), &origin));
        }
        HRESULT painted;
        try
        {
            painted = context ? draw(context.get(), origin) : E_POINTER;
        }
        catch (...)
        {
            (void)interop->EndDraw();
            throw;
        }
        HRESULT finished;
        {
            snowdesktop::performance::Scope trace("composition.srgb", "end_draw");
            finished = interop->EndDraw();
        }
        winrt::check_hresult(painted);
        winrt::check_hresult(finished);
    }

    void SetRegionalBlurBrush(ID2D1Device* device, const snowdesktop::panel_blur::Geometry& geometry,
        PanelVisual::IconRegions& next, const PanelVisual::IconRegions& previous)
    {
        const bool reuse = !next.iconsOnly && !previous.iconsOnly && maskDrawDevice.get() == device &&
            previous.brush && previous.blurRadius == next.blurRadius &&
            EqualRect(&previous.sampleBounds, &next.sampleBounds) &&
            std::equal(previous.masks.begin(), previous.masks.end(),
                geometry.strongerMasks.begin(), geometry.strongerMasks.end(),
                [](const auto& a, const auto& b) { return a.first == b.first; });
        const POINT inset{next.allocation.left - next.sampleBounds.left,
            next.allocation.top - next.sampleBounds.top};
        if (reuse)
        {
            next.masks = previous.masks;
            for (const auto& [radius, mask] : geometry.strongerMasks)
                PaintRadiusMask(next.masks.at(radius), mask.Get(), inset);
            next.brush = previous.brush;
            return;
        }

        EnsureMaskGraphicsDevice(device);
        std::vector<int> radii{next.blurRadius};
        for (const auto& entry : geometry.strongerMasks)
            radii.push_back(entry.first);
        auto key = std::make_pair(next.iconsOnly, std::move(radii));
        auto found = regionalFactories.find(key);
        if (found == regionalFactories.end())
        {
            if (regionalFactories.size() >= 16) regionalFactories.clear();
            auto factory = CreateRegionalBlurFactory(compositor, key.second, next.iconsOnly);
            found = regionalFactories.emplace(std::move(key), std::move(factory)).first;
        }
        auto brush = found->second.CreateBrush();
        brush.SetSourceParameter(L"backdrop", compositor.CreateBackdropBrush());
        const SIZE size{next.sampleBounds.right - next.sampleBounds.left,
            next.sampleBounds.bottom - next.sampleBounds.top};
        for (const auto& [radius, mask] : geometry.strongerMasks)
        {
            const auto surface = CreateRadiusMask(size);
            PaintRadiusMask(surface, mask.Get(), inset);
            brush.SetSourceParameter(RadiusMaskName(radius), MakeRadiusMaskBrush(surface));
            next.masks.emplace(radius, surface);
        }
        next.brush = std::move(brush);
    }

    void PruneUnusedBlurFactories()
    {
        if (collectingFrame || !blurFactoriesDirty)
            return;
        snowdesktop::desktop_backdrop_update_rules::PruneUnusedBlurFactories(
            blurFactories, panels);
        blurFactoriesDirty = false;
    }

    void ClearGenieTransform()
    {
        if (!genieRoot)
            return;
        if (root)
        {
            root.StopAnimation(L"Scale");
            root.StopAnimation(L"Opacity");
            root.Scale(wfn::float3{1.0f, 1.0f, 1.0f});
            root.CenterPoint(wfn::float3{});
            root.Opacity(1.0f);
            root.Children().Remove(genieRoot);
        }
        if (genieSource)
        {
            bool restored = false;
            for (auto& panel : panels)
                if (panel.visual == genieSource)
                {
                    panel.SetOpacity(genieSourceOpacity);
                    restored = true;
                    break;
                }
            // Icon retirement may have already detached the source visual.
            if (!restored) genieSource.Opacity(genieSourceOpacity);
        }
        genieRoot = nullptr;
        genieSource = nullptr;
        genieGeometry = nullptr;
        genieOutline.clear();
        geniePanelFrame = {};
        genieHostSize = {};
        genieOpacity = 1.0f;
        SetAnimationPathRegionExpanded(false);
    }

    void Reset(bool preserveSrgbContent = false)
    {
        available = false;
        panelRegionSnapshotValid = false;
        genieOutline.clear();
        genieGeometry = nullptr;
        genieGeometryFactory = nullptr;
        genieRoot = nullptr;
        genieSource = nullptr;
        geniePanelFrame = {};
        genieHostSize = {};
        genieOpacity = 1.0f;
        panels.clear();
        panelOwners.clear();
        changedIconPanels.clear();
        blurFactories.clear();
        if (!preserveSrgbContent)
        {
            precomposedSceneFactory = nullptr;
        }
        regionalFactories.clear();
        if (!preserveSrgbContent)
        {
            maskGraphicsDevice = nullptr;
            maskDrawDevice = nullptr;
        }
        collectingFrame = false;
        frameCommitPending = false;
        stagingFrame = false;
        stagedPanels.clear();
        stagedPanelOwners.clear();
        blurFactoriesDirty = false;
        try
        {
            if (preserveSrgbContent && srgbRootOnPopup)
            {
                try { SetSrgbRootOnPopup(false); }
                catch (...) { ResetSrgbContent(); }
            }
            if (!preserveSrgbContent)
                ResetSrgbContent();
            if (target)
                target.Root(nullptr);
        }
        catch (...)
        {
            // Destruction must continue even if the compositor target is
            // already faulted; otherwise its helper HWND can remain visible.
        }
        root = nullptr;
        placementRoot = nullptr;
        popupDisplayClip = nullptr;
        target = nullptr;
        // The controller is shared across backdrop instances on this thread.
        // Keep this instance attached while its sRGB content is still live.
        if (!preserveSrgbContent)
        {
            compositor = nullptr;
            compositorController = nullptr;
            dispatcherController = nullptr;
        }
        if (backdropWindow && IsWindow(backdropWindow))
            DestroyWindow(backdropWindow);
        backdropWindow = nullptr;
        contentWindow = nullptr;
        popupMode = false;
        popupTopmost = false;
        popupPadding = {};
        popupDisplayBounds = {};
        submittedPopupDisplayClip = {};
        visible = true;
        animationPathRegionExpanded = false;
        stableWindowRegion = false;
        rootTranslationOwnsGlass = false;
        srgbRootOnPopup = false;
        popupPresentationViewport = false;
    }
};

DesktopBackdropCompositor::DesktopBackdropCompositor()
    : impl_(std::make_unique<Impl>())
{
}

DesktopBackdropCompositor::~DesktopBackdropCompositor()
{
    Reset();
}

bool DesktopBackdropCompositor::Initialize(
    HWND contentWindow, bool initiallyVisible)
{
    return InitializeInternal(
        contentWindow, false, false, initiallyVisible);
}

bool DesktopBackdropCompositor::InitializePopup(
    HWND contentWindow, bool topmost,
    bool initiallyVisible)
{
    return InitializeInternal(
        contentWindow, true, topmost,
        initiallyVisible);
}

bool DesktopBackdropCompositor::SetExcludedFromPeek(bool excluded)
{
    excludedFromPeek_ = excluded;
    return impl_ && snowdesktop::popup_window_pair_z_order::SetExcludedFromPeek(
        impl_->contentWindow, impl_->backdropWindow, excluded);
}

HWND DesktopBackdropCompositor::PreparePeekInsertBefore(HWND contentWindow)
{
    if (!contentWindow || !IsWindow(contentWindow)) return nullptr;
    if (!impl_ || !impl_->available || !impl_->popupMode || impl_->contentWindow != contentWindow)
        return contentWindow;
    POINT origin{};
    SIZE size{};
    if (!impl_->QueryContentPlacement(nullptr, origin, size)) return nullptr;
    return snowdesktop::popup_window_pair_z_order::PreparePeekInsertBefore(
        contentWindow, impl_->backdropWindow, origin, size);
}

std::shared_future<bool> DesktopBackdropCompositor::PrepareBlurFactoryInBackground(float blurRadius)
{
    if (!impl_ || !std::isfinite(blurRadius) || !impl_->EnsureDispatcherQueue()) return {};
    auto& shared = SharedBackdropContext();
    const int radius = std::clamp(static_cast<int>(std::lround(blurRadius)), 0, 48);
    if (shared.warmup)
    {
        if (shared.warmup->radius == radius) return shared.warmup->result;
        if (shared.warmup->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return {}; // At most one background factory build per UI compositor.
    }
    const auto work = std::make_shared<BackgroundBlurFactory>();
    work->radius = radius;
    shared.warmup = work;
    try
    {
        // Compositor and CompositionEffectFactory are Agile/Both. The worker
        // never touches an HWND, visual tree, D2D context, or DesktopApp state.
        std::thread([work, compositor = shared.compositor]() mutable {
            const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
            const bool background = SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE;
            const bool lowerPriority = background ||
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) != FALSE;
            bool prepared = false;
            {
                snowdesktop::performance::Scope scope("popup", "backdrop.warmup.background");
                if (SUCCEEDED(apartment) && lowerPriority)
                {
                    try
                    {
                        auto factory = CreateBlurFactory(compositor, work->radius);
                        std::lock_guard lock(work->mutex);
                        work->factory = std::move(factory);
                        prepared = true;
                    }
                    catch (...) { /* A failed warmup leaves the normal path available. */ }
                }
                compositor = nullptr;
            }
            if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            if (SUCCEEDED(apartment)) RoUninitialize();
            work->completion.set_value(prepared);
        }).detach();
    }
    catch (...)
    {
        work->completion.set_value(false);
    }
    return work->result;
}

bool DesktopBackdropCompositor::SetPopupWindowPairZOrder(
    HWND contentWindow, HWND contentInsertAfter,
    bool topmost, HWND preserveAboveWindow)
{
    const bool contentValid =
        contentWindow && IsWindow(contentWindow);
    if (!contentValid)
        return false;

    HWND backdropWindow = nullptr;
    if (impl_ && impl_->popupMode &&
        impl_->available &&
        impl_->contentWindow == contentWindow &&
        impl_->backdropWindow &&
        IsWindow(impl_->backdropWindow))
    {
        backdropWindow = impl_->backdropWindow;
        impl_->popupTopmost = topmost;
    }

    bool positionedTogether = false;
    POINT origin{};
    SIZE size{};
    if (backdropWindow)
    {
        if (impl_->QueryContentPlacement(
                nullptr, origin, size))
        {
            positionedTogether =
                snowdesktop::popup_window_pair_z_order::Apply(
                    contentWindow, backdropWindow,
                    contentInsertAfter, topmost,
                    origin, size, preserveAboveWindow);
        }
    }
    else
    {
        positionedTogether =
            snowdesktop::popup_window_pair_z_order::Apply(
                contentWindow, nullptr,
                contentInsertAfter, topmost,
                origin, size, preserveAboveWindow);
    }

    if (!positionedTogether)
    {
        // A concrete anchor can disappear or change bands during placement.
        // Recover the requested band instead of repeating the rejected anchor;
        // retain the same menu protection as the coordinated path.
        snowdesktop::popup_window_pair_z_order::Apply(
            contentWindow, nullptr,
            topmost ? HWND_TOPMOST : HWND_NOTOPMOST, topmost,
            POINT{}, SIZE{}, preserveAboveWindow);
        if (backdropWindow)
            impl_->SyncWindowPlacement();
    }
    // A repaired band is useful, but does not complete a requested concrete
    // placement. Callers restoring the desktop layer must retain retry ownership.
    const bool concreteAnchor = contentInsertAfter != HWND_TOP &&
        contentInsertAfter != HWND_BOTTOM && contentInsertAfter != HWND_TOPMOST &&
        contentInsertAfter != HWND_NOTOPMOST;
    return positionedTogether && (!concreteAnchor ||
        (IsWindow(contentInsertAfter) &&
         GetWindow(contentWindow, GW_HWNDPREV) == contentInsertAfter));
}

void DesktopBackdropCompositor::SetPopupTopmost(
    bool topmost)
{
    if (!impl_ || !impl_->popupMode)
        return;
    if (!impl_->available || !impl_->backdropWindow ||
        !IsWindow(impl_->backdropWindow))
    {
        impl_->popupTopmost = topmost;
        return;
    }

    const bool isTopmost =
        (GetWindowLongPtrW(
            impl_->backdropWindow,
            GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (impl_->popupTopmost == topmost &&
        isTopmost == topmost)
        return;

    impl_->popupTopmost = topmost;
    SetWindowPos(
        impl_->backdropWindow,
        topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    impl_->SyncWindowPlacement();
}

bool DesktopBackdropCompositor::InitializeInternal(
    HWND contentWindow, bool popupMode,
    bool popupTopmost,
    bool initiallyVisible)
{
    // The helper HWND can be rebuilt while the same content HWND still shows
    // its WinRT body. Detach that body only when changing content windows.
    const bool validContent = contentWindow && IsWindow(contentWindow);
    const bool preserveSrgbContent = validContent && impl_->srgbContentWindow == contentWindow;
    impl_->Reset(preserveSrgbContent);
    impl_->lastError.clear();
    if (!validContent)
    {
        impl_->lastError = _LW("backdrop.content_invalid");
        return false;
    }
    HWND parent = popupMode ? nullptr : GetParent(contentWindow);
    if (!popupMode && (!parent || !IsWindow(parent)))
    {
        impl_->lastError = _LW("backdrop.host_invalid");
        return false;
    }
    if (!impl_->EnsureDispatcherQueue() || !RegisterBackdropWindowClass())
    {
        if (impl_->lastError.empty())
            impl_->lastError = FormatHresult(_LW("backdrop.register_class"),
                HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    POINT origin{};
    SIZE size{};
    impl_->contentWindow = contentWindow;
    impl_->popupMode = popupMode;
    impl_->popupTopmost =
        popupMode && popupTopmost;
    impl_->visible = initiallyVisible;
    if (!impl_->QueryContentPlacement(parent, origin, size))
    {
        impl_->lastError = _LW("backdrop.read_position");
        impl_->contentWindow = nullptr;
        return false;
    }

    // A popup backdrop can cover transparent space outside its content HWND.
    // Layered transparency passes cross-thread hit tests through to the real
    // drop target while the Composition visual supplies the popup's pixels.
    const DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT |
        (popupMode ? WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP : 0) |
        (impl_->popupTopmost ? WS_EX_TOPMOST : 0);
    const DWORD windowStyle = popupMode ? WS_POPUP : WS_CHILD;
    impl_->backdropWindow = snowdesktop::CreateShellOverlayWindowEx(
        extendedStyle,
        kBackdropWindowClassName, L"SnowDesktopBackdrop",
        windowStyle,
        origin.x, origin.y, size.cx, size.cy,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!impl_->backdropWindow)
    {
        impl_->lastError = FormatHresult(_LW("backdrop.create_window"),
            HRESULT_FROM_WIN32(GetLastError()));
        impl_->contentWindow = nullptr;
        return false;
    }
    const BOOL disableTransitions = TRUE;
    if (popupMode && excludedFromPeek_)
        snowdesktop::popup_window_pair_z_order::SetExcludedFromPeek(contentWindow, impl_->backdropWindow, true);
    DwmSetWindowAttribute(
        impl_->backdropWindow,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions, sizeof(disableTransitions));

    try
    {
        auto interop = impl_->compositor.as<awucd::ICompositorDesktopInterop>();
        const HRESULT hr = interop->CreateDesktopWindowTarget(
            impl_->backdropWindow, FALSE,
            reinterpret_cast<awucd::IDesktopWindowTarget**>(
                winrt::put_abi(impl_->target)));
        if (FAILED(hr))
        {
            impl_->SetError(_LW("backdrop.create_target"), hr);
            impl_->Reset(preserveSrgbContent);
            return false;
        }
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.get_interop"), error.code());
        impl_->Reset(preserveSrgbContent);
        return false;
    }

    try
    {
        impl_->root = impl_->compositor.CreateContainerVisual();
        impl_->placementRoot = impl_->compositor.CreateContainerVisual();
        const auto margin = static_cast<float>(impl_->PlacementMargin());
        impl_->placementRoot.Offset({margin, margin, 0.0f});
        impl_->placementRoot.Children().InsertAtTop(impl_->root);
        impl_->target.Root(impl_->placementRoot);
        impl_->available = true;
        impl_->SyncWindowPlacement();
        impl_->SyncPanelWindowRegion();
        impl_->RequestCommit();
        if (initiallyVisible)
            ShowWindow(
                impl_->backdropWindow,
                SW_SHOWNOACTIVATE);
        impl_->lastError.clear();
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.set_root"), error.code());
        impl_->Reset(preserveSrgbContent);
        return false;
    }
}

void DesktopBackdropCompositor::Reattach(HWND contentWindow)
{
    if (!impl_->available || !contentWindow || !IsWindow(contentWindow))
        return;
    impl_->contentWindow = contentWindow;
    impl_->SyncWindowPlacement();
}

void DesktopBackdropCompositor::SetVisible(bool visible)
{
    if (!impl_)
        return;
    if (impl_->visible == visible)
        return;
    impl_->visible = visible;
    if (!impl_->available || !impl_->backdropWindow ||
        !IsWindow(impl_->backdropWindow))
        return;
    if (visible)
        impl_->SyncWindowPlacement();
    else
        ShowWindow(impl_->backdropWindow, SW_HIDE);
}

void DesktopBackdropCompositor::ShowPopupWindowPair(
    HWND contentWindow)
{
    const bool contentValid =
        contentWindow && IsWindow(contentWindow);
    if (!impl_)
    {
        if (contentValid)
            ShowWindow(contentWindow, SW_SHOWNOACTIVATE);
        return;
    }

    HWND backdropWindow =
        impl_->popupMode &&
        impl_->backdropWindow &&
        IsWindow(impl_->backdropWindow)
            ? impl_->backdropWindow
            : nullptr;
    impl_->visible = true;
    bool shownTogether = false;
    if (contentValid && backdropWindow &&
        impl_->contentWindow == contentWindow)
    {
        POINT origin{};
        SIZE size{};
        if (impl_->QueryContentPlacement(nullptr, origin, size))
        {
            // Establish the helper immediately behind the still-hidden
            // content window, then reveal both HWNDs in one User32 batch.
            SetWindowPos(
                backdropWindow, contentWindow,
                origin.x, origin.y, size.cx, size.cy,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            constexpr UINT showFlags =
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
            HDWP deferred = BeginDeferWindowPos(2);
            if (deferred)
            {
                deferred = DeferWindowPos(
                    deferred, backdropWindow, nullptr,
                    0, 0, 0, 0, showFlags);
            }
            if (deferred)
            {
                deferred = DeferWindowPos(
                    deferred, contentWindow, nullptr,
                    0, 0, 0, 0, showFlags);
            }
            if (deferred)
                shownTogether = EndDeferWindowPos(deferred) != FALSE;
        }
    }

    if (!shownTogether)
    {
        if (backdropWindow)
            impl_->SyncWindowPlacement();
        if (contentValid)
            ShowWindow(contentWindow, SW_SHOWNOACTIVATE);
    }
}

void DesktopBackdropCompositor::HidePopupWindowPair(
    HWND contentWindow)
{
    // Hiding a window does not release capture or keyboard focus. Retire only
    // input owned by this hidden popup/its children; another host may already
    // own the next gesture. Capture/focus callbacks may reset the compositor.
    const auto releaseHiddenInput = [contentWindow] {
        if (!contentWindow || !IsWindow(contentWindow) ||
            IsWindowVisible(contentWindow))
            return;
        const HWND focus = GetFocus();
        const bool ownedFocus = focus &&
            (focus == contentWindow || IsChild(contentWindow, focus));
        const HWND capture = GetCapture();
        if (capture && (capture == contentWindow || IsChild(contentWindow, capture)))
            ReleaseCapture();
        // A capture-loss handler can show another surface or transfer focus.
        // Do not clear the replacement, even when it belongs to this thread.
        if (ownedFocus && GetFocus() == focus && IsWindow(contentWindow) &&
            !IsWindowVisible(contentWindow))
            SetFocus(nullptr);
    };
    const bool contentValid =
        contentWindow && IsWindow(contentWindow);
    if (!impl_)
    {
        if (contentValid)
            ShowWindow(contentWindow, SW_HIDE);
        releaseHiddenInput();
        return;
    }

    HWND backdropWindow =
        impl_->popupMode &&
        impl_->backdropWindow &&
        IsWindow(impl_->backdropWindow)
            ? impl_->backdropWindow
            : nullptr;
    bool hiddenTogether = false;
    if (contentValid && backdropWindow &&
        impl_->contentWindow == contentWindow)
    {
        constexpr UINT hideFlags =
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_HIDEWINDOW;
        HDWP deferred = BeginDeferWindowPos(2);
        if (deferred)
        {
            deferred = DeferWindowPos(
                deferred, contentWindow, nullptr,
                0, 0, 0, 0, hideFlags);
        }
        if (deferred)
        {
            deferred = DeferWindowPos(
                deferred, backdropWindow, nullptr,
                0, 0, 0, 0, hideFlags);
        }
        if (deferred)
        {
            hiddenTogether =
                EndDeferWindowPos(deferred) != FALSE;
        }
    }

    if (!hiddenTogether)
    {
        if (contentValid)
            ShowWindow(contentWindow, SW_HIDE);
        if (backdropWindow)
            ShowWindow(backdropWindow, SW_HIDE);
    }

    if (impl_->popupMode)
    {
        impl_->visible = false;
        // A scale animation temporarily expands the helper HWND region. Both
        // popup-owned windows are hidden now, so restoring the cached panel
        // region cannot expose an intermediate glass frame.
        impl_->SetAnimationPathRegionExpanded(false);
    }
    // Input-loss callbacks are synchronous and may close/reset the owner.
    // Keep this last: no access to this/impl_ after retiring owned input.
    releaseHiddenInput();
}

void DesktopBackdropCompositor::SetVisualTransform(
    float scale, float opacity,
    float anchorX, float anchorY)
{
    if (!impl_ || !impl_->available || !impl_->root ||
        !impl_->contentWindow || !IsWindow(impl_->contentWindow))
        return;

    ClearGenieTransform();
    if (!impl_->available)
        return;

    const float clampedScale = std::clamp(scale, 0.0f, 1.0f);
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);

    try
    {
        impl_->root.CenterPoint(wfn::float3{
            anchorX, anchorY, 0.0f });
        impl_->root.Scale(wfn::float3{
            clampedScale, clampedScale, 1.0f });
        impl_->root.Opacity(clampedOpacity);
        // Keep the whole animation path available without rebuilding a native
        // HWND region every frame. The transition happens only when entering
        // or leaving the transformed state.
        impl_->SetAnimationPathRegionExpanded(
            clampedScale < 0.9995f);
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
    }
}

void DesktopBackdropCompositor::SetPopupPanelTransform(
    std::uintptr_t ownerKey, float scale, float opacity,
    POINT anchor, bool preserveAnimation)
{
    if (!impl_->available || !ownerKey)
        return;
    const auto panel = impl_->FindPanel({}, ownerKey);
    if (panel == impl_->panels.end() ||
        (preserveAnimation && panel->popupTransformAnimating.value_or(false)))
        return;

    try
    {
        panel->visual.CenterPoint(wfn::float3{
            static_cast<float>(anchor.x - panel->frame.left),
            static_cast<float>(anchor.y - panel->frame.top), 0.0f});
        panel->visual.Scale(wfn::float3{scale, scale, 1.0f});
        panel->visual.Opacity(std::clamp(opacity, 0.0f, 1.0f));
        if (!panel->popupTransformAnimating.has_value())
            impl_->transformedRegionDirty = true;
        panel->popupTransformAnimating = false;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
    }
}

bool DesktopBackdropCompositor::StartVisualScaleAnimation(
    float fromScale, float toScale, float opacity,
    float anchorX, float anchorY,
    std::uint32_t durationMilliseconds,
    float normalizedStartSlope)
{
    return StartVisualTransformAnimation(
        fromScale, toScale, opacity, opacity,
        anchorX, anchorY, durationMilliseconds,
        normalizedStartSlope);
}

bool DesktopBackdropCompositor::StartVisualTransformAnimation(
    float fromScale, float toScale,
    float fromOpacity, float toOpacity,
    float anchorX, float anchorY,
    std::uint32_t durationMilliseconds,
    float normalizedStartSlope,
    const snowdesktop::popup_motion_curve::Timeline* springTimeline,
    const snowdesktop::popup_motion_curve::Timeline* opacityTimeline,
    std::uintptr_t panelOwnerKey, std::uintptr_t contentKey)
{
    if (!impl_ || !impl_->compositor || durationMilliseconds == 0 ||
        (contentKey && panelOwnerKey) ||
        (!contentKey && (!impl_->available || !impl_->root ||
            !impl_->contentWindow || !IsWindow(impl_->contentWindow))))
        return false;

    const auto panel = panelOwnerKey ? impl_->FindPanel({}, panelOwnerKey) : impl_->panels.end();
    if (panelOwnerKey && panel == impl_->panels.end())
        return false;
    wuc::Visual target = impl_->root;
    if (contentKey)
    {
        const auto content = impl_->srgbContent.find(contentKey);
        if (content == impl_->srgbContent.end() || !content->second.visual) return false;
        target = content->second.visual;
        anchorX -= static_cast<float>(content->second.stagedFrame.left);
        anchorY -= static_cast<float>(content->second.stagedFrame.top);
    }
    else if (panelOwnerKey)
    {
        target = panel->visual;
        anchorX -= static_cast<float>(panel->frame.left);
        anchorY -= static_cast<float>(panel->frame.top);
    }
    else
    {
        ClearGenieTransform();
        if (!impl_->available)
            return false;
    }

    const float initialScale =
        panelOwnerKey ? fromScale : std::clamp(fromScale, 0.0f, 1.0f);
    const float clampedTo =
        std::clamp(toScale, 0.0f, 1.0f);
    const float clampedFromOpacity =
        std::clamp(fromOpacity, 0.0f, 1.0f);
    const float clampedToOpacity =
        std::clamp(toOpacity, 0.0f, 1.0f);
    const float clampedStartSlope =
        std::clamp(normalizedStartSlope, 0.0f, 2.0f);

    HRESULT animationHr = E_UNEXPECTED;
    try
    {
        const auto easing =
            impl_->compositor.CreateCubicBezierEasingFunction(
                wfn::float2{
                    1.0f / 3.0f,
                    clampedStartSlope / 3.0f },
                wfn::float2{ 2.0f / 3.0f, 1.0f });
        auto animation =
            impl_->compositor.CreateVector3KeyFrameAnimation();
        animation.Duration(wf::TimeSpan{
            static_cast<std::int64_t>(
                durationMilliseconds) * 10'000LL });
        animation.InsertKeyFrame(
            0.0f,
            wfn::float3{ initialScale, initialScale, 1.0f });
        if (springTimeline)
        {
            if (springTimeline->segments.empty()) return false;
            for (const auto& segment : springTimeline->segments)
            {
                const auto segmentEasing = impl_->compositor.CreateCubicBezierEasingFunction(
                    wfn::float2{1.0f / 3.0f, static_cast<float>(segment.StartControl())},
                    wfn::float2{2.0f / 3.0f, static_cast<float>(segment.EndControl())});
                const float time = std::clamp(static_cast<float>((segment.startSeconds + segment.durationSeconds) *
                    1000.0 / springTimeline->durationMilliseconds), 0.0f, 1.0f);
                const float value = static_cast<float>(segment.EvaluateLocal(segment.durationSeconds).position);
                animation.InsertKeyFrame(time, wfn::float3{value, value, 1.0f}, segmentEasing);
            }
        }
        else animation.InsertKeyFrame(1.0f, wfn::float3{clampedTo, clampedTo, 1.0f}, easing);
        auto opacityAnimation =
            impl_->compositor.CreateScalarKeyFrameAnimation();
        opacityAnimation.Duration(wf::TimeSpan{
            static_cast<std::int64_t>(
                durationMilliseconds) * 10'000LL });
        opacityAnimation.InsertKeyFrame(
            0.0f, clampedFromOpacity);
        if (opacityTimeline)
        {
            if (opacityTimeline->segments.empty()) return false;
            for (const auto& segment : opacityTimeline->segments)
            {
                const auto segmentEasing = impl_->compositor.CreateCubicBezierEasingFunction(
                    wfn::float2{1.0f / 3.0f, static_cast<float>(segment.StartControl())},
                    wfn::float2{2.0f / 3.0f, static_cast<float>(segment.EndControl())});
                const float time = std::clamp(static_cast<float>((segment.startSeconds + segment.durationSeconds) *
                    1000.0 / opacityTimeline->durationMilliseconds), 0.0f, 1.0f);
                opacityAnimation.InsertKeyFrame(time,
                    static_cast<float>(segment.EvaluateLocal(segment.durationSeconds).position), segmentEasing);
            }
        }
        else opacityAnimation.InsertKeyFrame(1.0f, clampedToOpacity, easing);

        target.CenterPoint(wfn::float3{
            anchorX, anchorY, 0.0f });
        if (panelOwnerKey)
        {
            if (!panel->popupTransformAnimating.has_value())
                impl_->transformedRegionDirty = true;
            panel->popupTransformAnimating = false;
            if (impl_->transformedRegionDirty)
                impl_->transformedRegionDirty = !impl_->SyncPanelWindowRegion();
        }
        else if (!contentKey)
            impl_->SetAnimationPathRegionExpanded(std::min(initialScale, clampedTo) < 0.9995f);
        // Keep final values as the base properties. Direct assignments also
        // disconnect prior animations before a rapid reversal.
        target.Scale(wfn::float3{
            clampedTo, clampedTo, 1.0f });
        target.Opacity(clampedToOpacity);
        target.StartAnimation(L"Scale", animation);
        target.StartAnimation(L"Opacity", opacityAnimation);
        if (panelOwnerKey)
        {
            if (!impl_->RequestCommit())
                return false;
            panel->popupTransformAnimating = true;
        }
        else if (contentKey)
            impl_->srgbContentCommitPending = true;
        else
            impl_->RequestCommit();
        return true;
    }
    catch (...)
    {
        animationHr = winrt::to_hresult();
    }

    try
    {
        target.Scale(wfn::float3{
            initialScale, initialScale, 1.0f });
        target.Opacity(clampedFromOpacity);
        if (panelOwnerKey && panel->popupTransformAnimating.has_value())
            panel->popupTransformAnimating = false;
        if (contentKey) impl_->srgbContentCommitPending = true;
        else if (!panelOwnerKey)
            impl_->SetAnimationPathRegionExpanded(initialScale < 0.9995f);
        if (!contentKey) impl_->RequestCommit();
    }
    catch (...)
    {
        impl_->SetError(
            L"backdrop.restore_scale_animation",
            winrt::to_hresult());
        impl_->Reset();
        return false;
    }
    impl_->lastError = FormatHresult(
        L"backdrop.animate_scale", animationHr);
    return false;
}

bool DesktopBackdropCompositor::SetGenieTransform(
    const RECT& panelFrame, const RECT& dockFrame,
    int edge, float collapsed, float opacity)
{
    return SetDeformationTransform(panelFrame, dockFrame, edge, collapsed, opacity, false);
}

bool DesktopBackdropCompositor::SetAttractionTransform(
    const RECT& panelFrame, const RECT& dockFrame, float collapsed, float opacity)
{
    return SetDeformationTransform(panelFrame, dockFrame, 0, collapsed, opacity, true);
}

bool DesktopBackdropCompositor::SetDeformationTransform(
    const RECT& panelFrame, const RECT& dockFrame,
    int edge, float collapsed, float opacity, bool attraction)
{
    if (!impl_ || !impl_->available || !impl_->root || !impl_->compositor ||
        !impl_->contentWindow || !IsWindow(impl_->contentWindow) ||
        panelFrame.right <= panelFrame.left || panelFrame.bottom <= panelFrame.top ||
        dockFrame.right <= dockFrame.left || dockFrame.bottom <= dockFrame.top ||
        !std::isfinite(collapsed) || !std::isfinite(opacity))
    {
        ClearGenieTransform();
        return false;
    }

    namespace genie = snowdesktop::dock_genie;
    const auto direction = static_cast<genie::Edge>(std::clamp(edge, 0, 3));
    const bool vertical = genie::Vertical(direction);
    try
    {
        const auto panel = std::find_if(impl_->panels.begin(), impl_->panels.end(),
            [&panelFrame](const Impl::PanelVisual& item) {
                return EqualRect(&item.frame, &panelFrame) != FALSE;
            });
        if (panel == impl_->panels.end() || !panel->sample.Brush())
        {
            impl_->ClearGenieTransform();
            return false;
        }
        const float width = static_cast<float>(panelFrame.right - panelFrame.left);
        const float height = static_cast<float>(panelFrame.bottom - panelFrame.top);
        RECT hostFrame{};
        if (!GetClientRect(impl_->contentWindow, &hostFrame) ||
            hostFrame.right <= 0 || hostFrame.bottom <= 0)
        {
            impl_->ClearGenieTransform();
            return false;
        }
        if (!impl_->genieRoot || impl_->genieSource != panel->visual ||
            impl_->genieVertical != vertical ||
            impl_->genieAttraction != attraction ||
            impl_->genieCornerRadius != panel->cornerRadius ||
            impl_->genieHostSize.cx != hostFrame.right ||
            impl_->genieHostSize.cy != hostFrame.bottom ||
            !EqualRect(&impl_->geniePanelFrame, &panelFrame))
        {
            impl_->ClearGenieTransform();
            if (!impl_->genieGeometryFactory)
            {
                winrt::check_hresult(D2D1CreateFactory(
                    D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    impl_->genieGeometryFactory.put()));
            }
            auto glass = impl_->compositor.CreateSpriteVisual();
            glass.Size(wfn::float2{static_cast<float>(hostFrame.right),
                static_cast<float>(hostFrame.bottom)});
            // Backdrop brushes sample the region behind each consuming visual.
            // One host-sized visual avoids repeated blur evaluation and does
            // not resize/share the ordinary panel's smaller effect brush.
            auto sample = impl_->compositor.CreateSpriteVisual();
            const auto margin = static_cast<float>(snowdesktop::blur_sampling::KernelMargin(
                static_cast<float>(panel->blurRadius)));
            sample.Offset({-margin, -margin, 0.0f});
            sample.Size({static_cast<float>(hostFrame.right) + 2.0f * margin,
                static_cast<float>(hostFrame.bottom) + 2.0f * margin});
            sample.Brush(impl_->CreateBlurBrush(panel->blurRadius));
            glass.Children().InsertAtBottom(sample);
            auto geometry = impl_->compositor.CreatePathGeometry();
            glass.Clip(impl_->compositor.CreateGeometricClip(geometry));
            auto outline = BuildGenieSourceOutline(width, height,
                static_cast<float>(panel->cornerRadius), vertical, attraction);
            impl_->root.StopAnimation(L"Scale");
            impl_->root.StopAnimation(L"Opacity");
            impl_->root.Scale(wfn::float3{1.0f, 1.0f, 1.0f});
            impl_->root.CenterPoint(wfn::float3{});
            impl_->root.Opacity(1.0f);
            impl_->genieRoot = glass;
            impl_->genieSource = panel->visual;
            impl_->genieSourceOpacity = panel->submittedOpacity.value_or(1.0f);
            impl_->genieGeometry = geometry;
            impl_->genieOutline = std::move(outline);
            impl_->geniePanelFrame = panelFrame;
            impl_->genieHostSize = {hostFrame.right, hostFrame.bottom};
            impl_->genieCornerRadius = panel->cornerRadius;
            impl_->genieVertical = vertical;
            impl_->genieAttraction = attraction;
            impl_->root.Children().InsertAtTop(glass);
            panel->SetOpacity(0.0f);
        }

        const genie::Rect source{static_cast<double>(panelFrame.left),
            static_cast<double>(panelFrame.top), static_cast<double>(panelFrame.right),
            static_cast<double>(panelFrame.bottom)};
        const genie::Rect target{static_cast<double>(dockFrame.left),
            static_cast<double>(dockFrame.top), static_cast<double>(dockFrame.right),
            static_cast<double>(dockFrame.bottom)};
        namespace navigation = snowdesktop::quick_navigation_animation_rules;
        std::array<navigation::GenieStripProjection, genie::StripCount> matrices{};
        if (!attraction)
        {
            for (std::size_t index = 0; index < matrices.size(); ++index)
                matrices[index] = navigation::GenieProjection(source, target, direction,
                    std::clamp(collapsed, 0.0f, 1.0f), width, height, index);
        }
        const auto mesh = attraction ? snowdesktop::dock_attraction::MeshFrame(source, target,
            std::clamp(collapsed, 0.0f, 1.0f)) : snowdesktop::dock_attraction::Frame{};
        const auto transformPoint = [&](const GenieOutlinePoint& point) {
            if (attraction)
            {
                const auto mapped = navigation::AttractionMeshPoint(mesh,
                    point.source.x / width, point.source.y / height);
                return D2D1_POINT_2F{static_cast<float>(mapped.x), static_cast<float>(mapped.y)};
            }
            const auto mapped = matrices[point.strip].Map(point.source.x, point.source.y);
            return D2D1_POINT_2F{static_cast<float>(mapped.x), static_cast<float>(mapped.y)};
        };
        // D2D paths are immutable once closed. Replace only the path value;
        // retain the Composition geometry, clip, visual and brush throughout
        // the animation. A single outline has no internal antialiased seams.
        winrt::com_ptr<ID2D1PathGeometry> path;
        winrt::check_hresult(impl_->genieGeometryFactory->CreatePathGeometry(path.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(path->Open(sink.put()));
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        sink->BeginFigure(transformPoint(impl_->genieOutline.front()),
            D2D1_FIGURE_BEGIN_FILLED);
        for (std::size_t index = 1; index < impl_->genieOutline.size(); ++index)
            sink->AddLine(transformPoint(impl_->genieOutline[index]));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        winrt::check_hresult(sink->Close());
        impl_->genieGeometry.Path(wuc::CompositionPath(
            winrt::make<BackdropGeometrySource>(std::move(path))));
        impl_->genieOpacity = std::clamp(opacity, 0.0f, 1.0f);
        impl_->genieRoot.Opacity(impl_->genieOpacity *
            impl_->genieSourceOpacity);
        impl_->SetAnimationPathRegionExpanded(true);
        if (!impl_->animationPathRegionExpanded)
        {
            impl_->ClearGenieTransform();
            return false;
        }
        return true;
    }
    catch (...)
    {
        impl_->lastError = FormatHresult(L"backdrop.deformation_transform", winrt::to_hresult());
        ClearGenieTransform();
        return false;
    }
}

void DesktopBackdropCompositor::ClearGenieTransform()
{
    if (!impl_ || !impl_->genieRoot)
        return;
    try
    {
        impl_->ClearGenieTransform();
    }
    catch (...)
    {
        impl_->lastError = FormatHresult(L"backdrop.clear_genie", winrt::to_hresult());
        // Discard the failed glass transform without dropping the content HWND's body.
        impl_->Reset(true);
    }
}

void DesktopBackdropCompositor::QueueIconPanelRecollection(const RECT& iconFrame, std::uintptr_t ownerKey)
{
    if (!impl_->available || impl_->panels.empty()) return;
    // Shell placeholders use the exact square destination as their anonymous
    // identity. Styled file bitmaps use their item owner and may be letterboxed.
    for (const auto key : {ownerKey, std::uintptr_t{0}})
    {
        const auto panel = impl_->FindPanel(iconFrame, key);
        if (panel == impl_->panels.end() || panel->iconShape < 0 ||
            !snowdesktop::desktop_backdrop_update_rules::CoversClientArea(&iconFrame, panel->frame)) continue;
        const auto pending = std::find_if(impl_->changedIconPanels.begin(), impl_->changedIconPanels.end(),
            [&](const auto& value) { return value.ownerKey == key && EqualRect(&value.frame, &panel->frame); });
        if (pending == impl_->changedIconPanels.end())
            impl_->changedIconPanels.push_back({panel->frame, key, false});
        else
            pending->redrawing = false; // A reentrant delivery belongs to the next content paint.
    }
}

void DesktopBackdropCompositor::BeginFrame(bool completeCollection, const RECT* desktopIconDamage)
{
    DiscardStagedFrame();
    if (!impl_->available)
        return;
    impl_->BeginCollection(completeCollection);
    if (completeCollection) impl_->changedIconPanels.clear();
    else for (auto& changed : impl_->changedIconPanels)
    {
        changed.redrawing = desktopIconDamage &&
            snowdesktop::desktop_backdrop_update_rules::CoversClientArea(desktopIconDamage, changed.frame);
        if (changed.redrawing)
        {
            const auto panel = impl_->FindPanel(changed.frame, changed.ownerKey);
            if (panel != impl_->panels.end() && EqualRect(&panel->frame, &changed.frame)) panel->seen = false;
        }
    }
}

bool DesktopBackdropCompositor::BeginStagedFrame()
{
    DiscardStagedFrame();
    if (!impl_->available || impl_->collectingFrame) return false;
    impl_->stagingFrame = true;
    impl_->stagedOcclusionRect = impl_->occlusionRect;
    return true;
}

void DesktopBackdropCompositor::DiscardStagedFrame()
{
    impl_->stagingFrame = false;
    impl_->stagedPanels.clear();
    impl_->stagedPanelOwners.clear();
}

bool DesktopBackdropCompositor::CollectStagedPanelIcon(std::uintptr_t ownerKey,
    snowdesktop::PanelBackdropIcon icon)
{
    if (!impl_->stagingFrame || !ownerKey) return false;
    const auto panel = impl_->FindStagedPanel({}, ownerKey);
    if (panel == impl_->stagedPanels.end() || panel->kind != Impl::StagedPanel::Kind::Rounded)
        return false;
    RECT visible = icon.frame;
    if (icon.clip && !IntersectRect(&visible, &icon.frame, &icon.clip->bounds)) return true;
    if (snowdesktop::panel_blur::RadiusKey(icon.blurRadius) >
        snowdesktop::panel_blur::RadiusKey(panel->blurRadius) ||
        !snowdesktop::panel_blur::Covered(panel->frame, panel->cornerRadius, visible))
        panel->panelIcons.push_back(std::move(icon));
    return true;
}

bool DesktopBackdropCompositor::ApplyStagedFrame(ID2D1Device* drawDevice)
{
    if (!impl_->available || !impl_->stagingFrame) return false;
    const RECT occlusion = impl_->stagedOcclusionRect;
    impl_->stagingFrame = false;
    impl_->BeginCollection(true, true);
    struct ClearCandidate
    {
        std::vector<Impl::StagedPanel>& panels;
        std::unordered_map<std::uintptr_t, std::size_t>& owners;
        ~ClearCandidate() { panels.clear(); owners.clear(); }
    } clearCandidate{impl_->stagedPanels, impl_->stagedPanelOwners};
    // EndFrame owns the region update, after every panel has been applied.
    impl_->occlusionRect = occlusion;
    for (const auto& panel : impl_->stagedPanels)
    {
        bool applied = false;
        switch (panel.kind)
        {
        case Impl::StagedPanel::Kind::Rounded:
        {
            const bool dockSampling = drawDevice && panel.ownerKey;
            applied = UpdatePanel(panel.frame, panel.cornerRadius, panel.blurRadius,
                panel.ownerKey, panel.smoothCorners, dockSampling);
            if (applied && dockSampling &&
                !SetPanelIconRegions(panel.ownerKey, drawDevice, panel.panelIcons, true))
                applied = AddPanel(panel.frame, panel.cornerRadius, panel.blurRadius,
                    panel.ownerKey, panel.smoothCorners);
            break;
        }
        case Impl::StagedPanel::Kind::Icon:
            applied = AddIconPanel(panel.frame, static_cast<snowdesktop::IconBeautifyShape>(panel.shape),
                panel.blurRadius, panel.ownerKey,
                panel.iconContentClip ? &*panel.iconContentClip : nullptr);
            break;
        case Impl::StagedPanel::Kind::LargeIcon:
            applied = AddLargeIconPanel(panel.frame, panel.shape, panel.cornerRadius,
                panel.blurRadius, panel.ownerKey, panel.flagDirection);
            break;
        }
        if (applied && panel.opacity != 1.0f)
            applied = SetPanelOpacity(panel.frame, panel.opacity, panel.ownerKey);
        if (applied && panel.transformed)
            applied = SetPanelTransform(panel.ownerKey, panel.transform, panel.projectedFrame);
        if (!applied)
        {
            // Do not retire old panels on a partially applied candidate.
            // Resource/commit failures still require the caller's recovery;
            // Windows Composition offers no rollback across native targets.
            impl_->collectingFrame = false;
            return false;
        }
    }
    EndFrame();
    return impl_->available && !impl_->transformedRegionDirty;
}

bool DesktopBackdropCompositor::AddPanel(
    const RECT& frame, float cornerRadius,
    float blurRadius, std::uintptr_t ownerKey, bool smoothCorners)
{
    return UpdatePanel(frame, cornerRadius, blurRadius, ownerKey, smoothCorners, false);
}

bool DesktopBackdropCompositor::SetPanelIconRegions(std::uintptr_t ownerKey,
    ID2D1Device* drawDevice, std::span<const snowdesktop::PanelBackdropIcon> icons,
    bool extendOutline, float configuredIconBlur, bool iconsOnly)
{
    if (!impl_->available || impl_->stagingFrame || !ownerKey) return false;
    const auto panel = impl_->FindPanel({}, ownerKey);
    if (panel == impl_->panels.end()) return false;
    namespace blur = snowdesktop::panel_blur;
    // The HWND allocation is reserved for the Dock's motion envelope. Keep
    // sampling in its pixel coordinates while the animated silhouettes move.
    RECT samplingFrame{};
    if (extendOutline && impl_->stableWindowRegion && GetClientRect(impl_->backdropWindow, &samplingFrame))
    {
        // The HWND reserves a maximum halo around its logical allocation.
        // The sprite below adds only the halo needed by this actual brush.
        const LONG margin = impl_->PlacementMargin();
        samplingFrame.right -= margin * 2;
        samplingFrame.bottom -= margin * 2;
    }
    const auto needsRegion = [&](const snowdesktop::PanelBackdropIcon& icon) {
        return blur::RadiusKey(icon.blurRadius) > panel->blurRadius ||
            (extendOutline && !blur::Covered(panel->frame, panel->cornerRadius, icon.frame));
    };
    const int configuredRadius = blur::RadiusKey(configuredIconBlur);
    const bool reserveIconMask = configuredRadius > panel->blurRadius;
    if (!drawDevice || (!reserveIconMask && IsRectEmpty(&samplingFrame) &&
        std::none_of(icons.begin(), icons.end(), needsRegion)))
    {
        if (iconsOnly || panel->iconRegionsApplied)
        {
            // Restore the clip and sampling footprint without changing the
            // opacity or transform owned by the panel's presentation.
            try
            {
                if (iconsOnly) panel->sample.Brush(nullptr);
                else if (panel->iconRegions.brush) panel->sample.Brush(panel->baseBlur);
                if (panel->iconRegions.clip)
                {
                    const RECT& frame = panel->frame;
                    panel->SetOffset({static_cast<float>(frame.left), static_cast<float>(frame.top), 0.0f});
                    panel->SetSize({static_cast<float>(frame.right - frame.left),
                        static_cast<float>(frame.bottom - frame.top)});
                    if (panel->smoothCorners) panel->SetClip(panel->smoothGeometry);
                    else panel->SetClip(panel->geometry);
                    panel->regionFrame = frame;
                    panel->regionCornerRadius = panel->cornerRadius;
                    impl_->transformedRegionDirty = true;
                }
                panel->iconRegionsApplied = false;
                panel->SetBaseSampleBounds();
                impl_->transformedRegionDirty = true;
                impl_->frameCommitPending = true;
            }
            catch (const winrt::hresult_error& error)
            {
                impl_->SetError(L"backdrop.icon_regions", error.code());
                return false;
            }
        }
        return true;
    }

    try
    {
        auto& cached = panel->iconRegions;
        const RECT animationEnvelope = panel->animationFence.value_or(RECT{});
        const bool changed = !EqualRect(&cached.base, &panel->frame) ||
            !EqualRect(&cached.samplingFrame, &samplingFrame) ||
            !EqualRect(&cached.animationEnvelope, &animationEnvelope) ||
            cached.corners != panel->cornerRadius || cached.blurRadius != panel->blurRadius ||
            cached.extendOutline != extendOutline || cached.iconsOnly != iconsOnly ||
            cached.configuredIconRadius != configuredRadius ||
            !std::equal(cached.icons.begin(), cached.icons.end(), icons.begin(), icons.end(), blur::SameIcon);
        if (!changed && panel->iconRegionsApplied) return true;
        if (changed)
        {
            Impl::PanelVisual::IconRegions next;
            next.icons.assign(icons.begin(), icons.end());
            next.base = panel->frame;
            next.samplingFrame = samplingFrame;
            next.animationEnvelope = animationEnvelope;
            next.corners = panel->cornerRadius;
            next.blurRadius = panel->blurRadius;
            next.extendOutline = extendOutline;
            next.iconsOnly = iconsOnly;
            next.configuredIconRadius = configuredRadius;
            winrt::com_ptr<ID2D1Factory> factory;
            drawDevice->GetFactory(factory.put());
            blur::Geometry geometry;
            winrt::check_hresult(blur::Build(*factory.get(), next.base, next.corners,
                next.blurRadius, icons, extendOutline, samplingFrame, geometry));
            // An empty tab uses the same sample origin, kernel padding and
            // effect as a populated tab. Only the stronger-radius mask clears.
            if (reserveIconMask) geometry.strongerMasks.try_emplace(configuredRadius);
            next.allocation = geometry.allocation;
            next.visibleBounds = geometry.visibleBounds;
            const int maximumRadius = geometry.strongerMasks.empty() ? next.blurRadius :
                geometry.strongerMasks.rbegin()->first;
            next.sampleMargin = snowdesktop::blur_sampling::KernelMargin(static_cast<float>(maximumRadius));
            RECT displayEnvelope{};
            UnionRect(&displayEnvelope, &next.visibleBounds, &animationEnvelope);
            next.sampleBounds = snowdesktop::blur_sampling::SampleBounds(
                next.allocation, displayEnvelope, next.sampleMargin);
            if (geometry.outline)
            {
                winrt::com_ptr<ID2D1Geometry> outline;
                outline.copy_from(geometry.outline.Get());
                next.clip = cached.clip ? cached.clip : impl_->compositor.CreatePathGeometry();
                next.clip.Path(wuc::CompositionPath(winrt::make<BackdropGeometrySource>(std::move(outline))));
            }
            if (!geometry.strongerMasks.empty())
            {
                try
                {
                    impl_->SetRegionalBlurBrush(drawDevice, geometry, next, cached);
                }
                catch (const winrt::hresult_error& error)
                {
                    // Keep one uniform parent blur and the correct outline if
                    // an effect or mask surface is rejected by the backend.
                    if (iconsOnly) throw;
                    impl_->lastError = FormatHresult(L"backdrop.icon_radius_mask", error.code());
                }
            }
            cached = std::move(next);
        }
        auto brush = cached.brush;
        if (!brush && !iconsOnly) brush = panel->baseBlur;
        if (panel->sample.Brush() != brush)
            panel->sample.Brush(brush);
        if (cached.clip)
        {
            const RECT& area = cached.allocation;
            panel->SetOffset({static_cast<float>(area.left), static_cast<float>(area.top), 0.0f});
            panel->SetSize({static_cast<float>(area.right - area.left), static_cast<float>(area.bottom - area.top)});
            panel->SetClip(cached.clip);
            panel->regionFrame = cached.visibleBounds;
            panel->regionCornerRadius = 0.0f;
            impl_->transformedRegionDirty = true;
        }
        panel->SetSampleBounds(cached.allocation, cached.sampleBounds, cached.sampleMargin);
        impl_->transformedRegionDirty = true;
        panel->iconRegionsApplied = true;
        impl_->frameCommitPending = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.icon_regions", error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::SetPopupWindowPadding(const RECT& padding, const RECT& displayBounds)
{
    if (!impl_->available || !impl_->popupMode) return false;
    if (EqualRect(&impl_->popupPadding, &padding) && EqualRect(&impl_->popupDisplayBounds, &displayBounds))
        return true;
    const RECT previousPadding = impl_->popupPadding;
    const RECT previousDisplayBounds = impl_->popupDisplayBounds;
    impl_->popupPadding = padding;
    impl_->popupDisplayBounds = displayBounds;
    if (impl_->SyncWindowPlacement()) return true;
    impl_->popupPadding = previousPadding;
    impl_->popupDisplayBounds = previousDisplayBounds;
    (void)impl_->SyncWindowPlacement();
    return false;
}

void DesktopBackdropCompositor::SetBackgroundPanels(
    std::span<const RECT> frames, float blurRadius)
{
    if (!impl_->available || !impl_->collectingFrame) return;
    try
    {
        for (auto panel = impl_->panels.begin(); panel != impl_->panels.end();)
        {
            if (!panel->background || std::any_of(frames.begin(), frames.end(),
                [&](const RECT& frame) { return EqualRect(&panel->frame, &frame) != FALSE; }))
            {
                ++panel;
                continue;
            }
            impl_->root.Children().Remove(panel->visual);
            panel->sample.Brush(nullptr);
            panel = impl_->ErasePanel(panel);
            impl_->blurFactoriesDirty = true;
        }
        for (const RECT& frame : frames)
            UpdatePanel(frame, 0.0f, blurRadius, 0, false, false, true);
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
    }
}

bool DesktopBackdropCompositor::UpdatePanel(
    const RECT& frame, float cornerRadius,
    float blurRadius, std::uintptr_t ownerKey, bool smoothCorners, bool customClip, bool background)
{
    if (!impl_->available || frame.right <= frame.left || frame.bottom <= frame.top)
        return false;
    if (impl_->stagingFrame)
    {
        Impl::StagedPanel candidate{};
        candidate.frame = frame;
        candidate.cornerRadius = cornerRadius;
        candidate.blurRadius = blurRadius;
        candidate.ownerKey = ownerKey;
        candidate.smoothCorners = smoothCorners;
        auto existing = impl_->FindStagedPanel(frame, ownerKey);
        if (existing == impl_->stagedPanels.end())
        {
            impl_->stagedPanels.push_back(candidate);
            if (ownerKey) impl_->stagedPanelOwners.emplace(ownerKey, impl_->stagedPanels.size() - 1);
        }
        else *existing = candidate;
        return true;
    }
    const auto rounded = snowdesktop::popup_round_geometry::Resolve(frame, cornerRadius);
    const float resolvedRadius = rounded.radiusX;
    const int blurKey = std::clamp(static_cast<int>(std::lround(blurRadius)), 0, 48);

    try
    {
        auto existing = impl_->FindPanel(frame, ownerKey, background);
        if (existing == impl_->panels.end())
        {
            Impl::PanelVisual panel{};
            panel.frame = frame;
            panel.ownerKey = ownerKey;
            panel.background = background;
            panel.cornerRadius = resolvedRadius;
            panel.blurRadius = blurKey;
            panel.visual = impl_->compositor.CreateSpriteVisual();
            panel.sample = impl_->compositor.CreateSpriteVisual();
            panel.visual.Children().InsertAtBottom(panel.sample);
            panel.geometry = impl_->compositor.CreateRoundedRectangleGeometry();
            panel.clip = impl_->compositor.CreateGeometricClip(panel.geometry);
            panel.submittedClip = panel.geometry;
            panel.visual.Clip(panel.clip);
            if (background)
                impl_->root.Children().InsertAtBottom(panel.visual);
            else
                impl_->root.Children().InsertAtTop(panel.visual);
            impl_->panels.push_back(std::move(panel));
            existing = std::prev(impl_->panels.end());
            if (ownerKey) impl_->panelOwners.emplace(ownerKey, impl_->panels.size() - 1);
        }

        // A moving Dock keeps one SpriteVisual and changes its geometry in
        // place. Treating every magnified RECT as a new identity leaves all
        // earlier rectangles alive during partial frames, causing the native
        // backdrop region and per-move work to grow without bound.
        if (impl_->genieSource == existing->visual &&
            (!EqualRect(&existing->frame, &frame) || existing->blurRadius != blurKey))
        {
            impl_->ClearGenieTransform();
        }
        const bool keepDockSampling = customClip && existing->iconRegionsApplied &&
            !IsRectEmpty(&existing->iconRegions.samplingFrame);
        existing->frame = frame;
        existing->SetTransform(wfn::float4x4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1});

        if (existing->blurRadius != blurKey || !existing->baseBlur)
        {
            impl_->blurFactoriesDirty = true;
            existing->blurRadius = blurKey;
            existing->baseBlur = impl_->CreateBlurBrush(blurKey);
        }
        existing->smoothCorners = smoothCorners;
        existing->cornerRadius = resolvedRadius;
        const wfn::float2 panelSize{
            static_cast<float>(frame.right - frame.left),
            static_cast<float>(frame.bottom - frame.top) };
        if (!keepDockSampling)
        {
            if (existing->sample.Brush() != existing->baseBlur)
                existing->sample.Brush(existing->baseBlur);
            existing->iconRegionsApplied = false;
            existing->regionFrame = frame;
            existing->regionCornerRadius = resolvedRadius;
            existing->SetOffset({static_cast<float>(frame.left), static_cast<float>(frame.top), 0.0f});
            existing->SetSize(panelSize);
            existing->SetRadius(resolvedRadius);
            existing->SetBaseSampleBounds();
        }
        if (!keepDockSampling && smoothCorners)
        {
            const SIZE size{frame.right-frame.left, frame.bottom-frame.top};
            if (!existing->smoothGeometry || existing->smoothRadius != resolvedRadius ||
                existing->smoothSize.cx != size.cx || existing->smoothSize.cy != size.cy)
            {
                if (!impl_->genieGeometryFactory)
                    winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                        impl_->genieGeometryFactory.put()));
                const auto path = snowdesktop::smooth_corners::Geometry(impl_->genieGeometryFactory.get(),
                    D2D1::RectF(0, 0, panelSize.x, panelSize.y), resolvedRadius);
                if (!path) return false;
                winrt::com_ptr<ID2D1Geometry> source;
                source.copy_from(path.Get());
                if (!existing->smoothGeometry) existing->smoothGeometry = impl_->compositor.CreatePathGeometry();
                existing->smoothGeometry.Path(wuc::CompositionPath(winrt::make<BackdropGeometrySource>(std::move(source))));
                existing->smoothSize = size;
                existing->smoothRadius = resolvedRadius;
            }
            if (!customClip) existing->SetClip(existing->smoothGeometry);
        }
        else if (!keepDockSampling && !customClip) existing->SetClip(existing->geometry);
        // AddPanel represents an ordinarily rendered, visible panel. A
        // floating-Dock hand-off can temporarily set an existing panel to
        // zero opacity; reusing that same rectangle on the next desktop paint
        // must retire the staging state even when hover geometry did not
        // change. Handoff callers that need an invisible target explicitly
        // call SetPanelOpacity(0) before EndFrame, so no intermediate visible
        // commit is introduced here.
        if (impl_->genieSource == existing->visual)
        {
            impl_->genieSourceOpacity = 1.0f;
            impl_->genieRoot.Opacity(impl_->genieOpacity);
            existing->SetOpacity(0.0f);
        }
        // Popup geometry can be rebased mid-animation. Its opacity is owned
        // by the popup lifecycle and a direct write would cancel the track.
        else if (!existing->popupTransformAnimating.has_value())
            existing->SetOpacity(1.0f);
        existing->seen = true;
        impl_->PruneUnusedBlurFactories();
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::HasPanelContaining(const RECT& frame) const
{
    if (impl_->stagingFrame)
        return std::any_of(impl_->stagedPanels.begin(), impl_->stagedPanels.end(), [&](const auto& panel) {
            return (panel.kind == Impl::StagedPanel::Kind::Rounded ||
                (panel.kind == Impl::StagedPanel::Kind::LargeIcon && panel.shape <= 1)) &&
                frame.left >= panel.frame.left && frame.top >= panel.frame.top &&
                frame.right <= panel.frame.right && frame.bottom <= panel.frame.bottom;
        });
    return std::any_of(impl_->panels.begin(), impl_->panels.end(), [&](const auto& panel) {
        return !panel.background && panel.iconShape < 0 && panel.largeIconShape < 0 && panel.seen &&
            frame.left >= panel.frame.left && frame.top >= panel.frame.top &&
            frame.right <= panel.frame.right && frame.bottom <= panel.frame.bottom;
    });
}

bool DesktopBackdropCompositor::RemoveIconPanel(const RECT& frame, std::uintptr_t ownerKey)
{
    if (impl_->stagingFrame)
    {
        const bool removed = std::erase_if(impl_->stagedPanels, [&](const auto& panel) {
            return panel.kind == Impl::StagedPanel::Kind::Icon &&
                snowdesktop::desktop_backdrop_update_rules::PanelIdentityMatches(
                    panel.ownerKey, panel.frame, ownerKey, frame);
        }) != 0;
        if (removed) impl_->RebuildStagedPanelOwners();
        return removed;
    }
    const auto found = impl_->FindPanel(frame, ownerKey);
    if (found == impl_->panels.end() || found->iconShape < 0) return false;
    try
    {
        impl_->root.Children().Remove(found->visual);
        found->sample.Brush(nullptr);
        impl_->ErasePanel(found);
        impl_->blurFactoriesDirty = true;
        // The enclosing EndFrame submits geometry and retirement together.
        // Visibility/deletion can also retire a child outside a paint frame.
        if (!impl_->collectingFrame)
        {
            impl_->PruneUnusedBlurFactories();
            impl_->transformedRegionDirty = !impl_->SyncPanelWindowRegion();
            impl_->RequestCommit();
        }
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.remove_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::AddIconPanel(const RECT& frame,
    snowdesktop::IconBeautifyShape shape, float blurRadius, std::uintptr_t ownerKey,
    const snowdesktop::IconBackdropClip* contentClip)
{
    RECT visibleFrame = frame;
    if (contentClip && !IntersectRect(&visibleFrame, &frame, &contentClip->bounds))
    {
        (void)RemoveIconPanel(frame, ownerKey);
        return impl_->available;
    }
    if (!UpdatePanel(frame, 0.0f, blurRadius, ownerKey, true, true)) return false;
    if (impl_->stagingFrame)
    {
        auto panel = impl_->FindStagedPanel(frame, ownerKey);
        panel->kind = Impl::StagedPanel::Kind::Icon;
        panel->shape = static_cast<int>(shape);
        panel->iconContentClip = contentClip
            ? std::optional{*contentClip} : std::nullopt;
        return true;
    }
    try
    {
        auto panel = impl_->FindPanel(frame, ownerKey);
        if (panel == impl_->panels.end()) return false;
        const SIZE size{frame.right - frame.left, frame.bottom - frame.top};
        const auto requestedClip = contentClip
            ? std::optional{*contentClip} : std::nullopt;
        if (panel->iconShape != static_cast<int>(shape) ||
            panel->iconSize.cx != size.cx || panel->iconSize.cy != size.cy ||
            !snowdesktop::SameIconBackdropClip(panel->iconContentClip, requestedClip) ||
            (contentClip && !EqualRect(&panel->iconFrame, &frame)))
        {
            if (!impl_->genieGeometryFactory)
                winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                    impl_->genieGeometryFactory.put()));
            Microsoft::WRL::ComPtr<ID2D1Geometry> path;
            winrt::check_hresult(snowdesktop::CreateIconBackdropGeometry(
                impl_->genieGeometryFactory.get(), shape, frame, contentClip, path));
            winrt::com_ptr<ID2D1Geometry> source;
            source.copy_from(path.Get());
            auto geometry = impl_->compositor.CreatePathGeometry();
            geometry.Path(wuc::CompositionPath(winrt::make<BackdropGeometrySource>(std::move(source))));
            panel->customGeometry = geometry;
            panel->SetClip(geometry);
            panel->iconShape = static_cast<int>(shape); panel->iconSize = size;
            panel->iconFrame = frame;
            panel->iconContentClip = requestedClip;
        }
        else panel->SetClip(panel->customGeometry);
        panel->regionFrame = visibleFrame;
        panel->regionCornerRadius = 0;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::AddClippedPanel(const RECT& envelope,
    ID2D1Geometry* clip, float blurRadius, std::uintptr_t ownerKey)
{
    if (!clip || !ownerKey || impl_->stagingFrame ||
        !UpdatePanel(envelope, 0.f, blurRadius, ownerKey, false, true)) return false;
    const auto panel = impl_->FindPanel(envelope, ownerKey);
    if (panel == impl_->panels.end()) return false;
    try
    {
        auto geometry = panel->customGeometry.try_as<wuc::CompositionPathGeometry>();
        if (!geometry) geometry = impl_->compositor.CreatePathGeometry();
        winrt::com_ptr<ID2D1Geometry> source;
        source.copy_from(clip);
        geometry.Path(wuc::CompositionPath(winrt::make<BackdropGeometrySource>(std::move(source))));
        panel->customGeometry = geometry;
        panel->SetClip(geometry);
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(L"backdrop.clipped_panel", error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::AddLargeIconPanel(const RECT& frame,
    int shape, float cornerRadius, float blurRadius, std::uintptr_t ownerKey, int flagDirection)
{
    if (!UpdatePanel(frame, cornerRadius, blurRadius, ownerKey, true, shape > 1)) return false;
    if (impl_->stagingFrame)
    {
        auto panel = impl_->FindStagedPanel(frame, ownerKey);
        panel->kind = Impl::StagedPanel::Kind::LargeIcon;
        panel->shape = shape;
        panel->flagDirection = flagDirection;
        return true;
    }
    try
    {
        auto panel = impl_->FindPanel(frame, ownerKey);
        if (panel == impl_->panels.end()) return false;
        if (shape <= 1)
        {
            panel->largeIconShape = -1;
            return true;
        }
        const SIZE size{frame.right - frame.left, frame.bottom - frame.top};
        if (panel->largeIconShape == shape && panel->largeIconFlagDirection == flagDirection &&
            panel->largeIconSize.cx == size.cx && panel->largeIconSize.cy == size.cy)
        { panel->SetClip(panel->customGeometry); return true; }
        if (!impl_->genieGeometryFactory)
            winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, impl_->genieGeometryFactory.put()));
        const auto mask = snowdesktop::large_icon_shape::Geometry(impl_->genieGeometryFactory.get(), shape,
            D2D1::RectF(0, 0, static_cast<float>(size.cx), static_cast<float>(size.cy)), cornerRadius, flagDirection);
        if (!mask)
        {
            // AddPanel used to select this ordinary clip before attempting
            // the custom outline. Preserve that fallback on shape failure.
            panel->SetClip(panel->smoothGeometry);
            return false;
        }
        winrt::com_ptr<ID2D1Geometry> source; source.copy_from(mask.Get());
        auto geometry = impl_->compositor.CreatePathGeometry();
        geometry.Path(wuc::CompositionPath(winrt::make<BackdropGeometrySource>(std::move(source))));
        panel->customGeometry = geometry;
        panel->SetClip(geometry);
        panel->largeIconShape = shape; panel->largeIconFlagDirection = flagDirection; panel->largeIconSize = size;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
        return false;
    }
}

namespace
{
void ReplayScene(ID2D1DeviceContext* target, ID2D1CommandList* scene,
    const RECT& frame, POINT sourceOrigin, POINT destinationOrigin)
{
    const auto source = D2D1::RectF(static_cast<float>(sourceOrigin.x), static_cast<float>(sourceOrigin.y),
        static_cast<float>(sourceOrigin.x + frame.right - frame.left),
        static_cast<float>(sourceOrigin.y + frame.bottom - frame.top));
    const auto destination = D2D1::Point2F(static_cast<float>(destinationOrigin.x),
        static_cast<float>(destinationOrigin.y));
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->DrawImage(scene, &destination, &source, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}
}

DesktopBackdropCompositor::SrgbSceneRecording::SrgbSceneRecording(
    ID2D1DeviceContext* context)
{
    if (!context || FAILED(context->CreateCommandList(&scene_))) return;
    context->GetTarget(&target_);
    if (!target_)
    {
        scene_.Reset();
        return;
    }
    context_ = context;
    context_->SetTarget(scene_.Get());
}

DesktopBackdropCompositor::SrgbSceneRecording::~SrgbSceneRecording()
{
    if (context_) context_->SetTarget(target_.Get());
}

HRESULT DesktopBackdropCompositor::SrgbSceneRecording::Finish()
{
    if (!context_) return E_FAIL;
    auto* context = std::exchange(context_, nullptr);
    context->SetTarget(target_.Get());
    const HRESULT result = context->Flush(nullptr, nullptr);
    return FAILED(result) ? result : scene_->Close();
}

bool DesktopBackdropCompositor::StageSrgbScene(HWND window, std::uintptr_t key,
    const RECT& clientFrame, ID2D1DeviceContext* context, POINT atlasOffset, ID2D1CommandList* scene,
    const RECT* paintRect)
{
    if (!context || !scene || atlasOffset.x < 0 || atlasOffset.y < 0) return false;
    winrt::com_ptr<ID2D1Device> device;
    context->GetDevice(device.put());
    return StageSrgbSceneFrame(window, key, clientFrame, device.get(),
        [&](ID2D1DeviceContext* target, POINT origin) -> HRESULT {
            target->SetDpi(96.f, 96.f);
            target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            target->Clear(D2D1::ColorF(0, 0.f));
            const RECT& source = paintRect ? *paintRect : clientFrame;
            const POINT destination{origin.x + source.left - clientFrame.left,
                origin.y + source.top - clientFrame.top};
            ReplayScene(target, scene, source, atlasOffset, destination);
            return S_OK;
        });
}

bool DesktopBackdropCompositor::StageSrgbSceneFrame(HWND window, std::uintptr_t key,
    const RECT& clientFrame, ID2D1Device* device, const SrgbSceneDraw& draw)
{
    if (const auto existing = impl_->srgbContent.find(key); existing != impl_->srgbContent.end())
        existing->second.staged = false;
    if (!window || !key || !device || !draw || IsRectEmpty(&clientFrame))
        return false;
    const SIZE size{clientFrame.right - clientFrame.left, clientFrame.bottom - clientFrame.top};

    try
    {
        if (!impl_->compositor && !impl_->EnsureDispatcherQueue()) return false;
        impl_->EnsureSrgbContentTarget(window);
        impl_->EnsureMaskGraphicsDevice(device);

        auto& frame = impl_->srgbContent[key];
        auto& candidate = frame.candidate;
        if (!candidate.surface || candidate.size.cx != size.cx || candidate.size.cy != size.cy)
        {
            snowdesktop::performance::Scope trace("composition.srgb", "create_buffer");
            candidate = impl_->CreateSrgbContentBuffer(size);
        }
        Impl::PaintSrgbSurface(candidate.surface, draw);
        frame.stagedFrame = clientFrame;
        frame.staged = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_copy", error.code());
        return false;
    }
}

DesktopBackdropCompositor::PreparedMarquees DesktopBackdropCompositor::PrepareSrgbMarquees(
    HWND window, std::uintptr_t key, ID2D1Device* device, const RECT& clientFrame, const RECT& allowed,
    std::span<const snowdesktop::widget_runtime::NativeMarqueeText> marquees)
{
    if (!window || !key || !device || marquees.empty() || IsRectEmpty(&clientFrame) || IsRectEmpty(&allowed))
        return {};
    if (window == impl_->srgbContentWindow && device == impl_->maskDrawDevice.get())
    {
        const auto existing = impl_->srgbContent.find(key);
        if (existing != impl_->srgbContent.end() && existing->second.marquees &&
            existing->second.marquees->Matches(clientFrame, allowed, marquees))
        {
            snowdesktop::performance::Value("composition.srgb", "marquee_scene_reused", {}, 1);
            return existing->second.marquees;
        }
    }
    snowdesktop::performance::Scope trace("composition.srgb", "prepare_marquee");
    const auto reject = [](std::string_view reason) {
        snowdesktop::performance::Value("composition.srgb", reason, {}, 1);
        return PreparedMarquees{};
    };
    try
    {
        if (!impl_->compositor && !impl_->EnsureDispatcherQueue()) return {};
        impl_->EnsureSrgbContentTarget(window);
        impl_->EnsureMaskGraphicsDevice(device);
        const auto& compositor = impl_->compositor;
        impl_->EnsureSrgbLayers(key);
        auto result = std::make_shared<SrgbMarqueeScene>();
        auto root = compositor.CreateContainerVisual();
        const wfn::float2 size{static_cast<float>(clientFrame.right - clientFrame.left),
            static_cast<float>(clientFrame.bottom - clientFrame.top)};
        root.Size(size);
        for (const auto& marquee : marquees)
        {
            const auto& box = marquee.viewport;
            const float cycle = marquee.textWidth + marquee.gap;
            if (!marquee.layout || !std::isfinite(cycle) || cycle <= 0 ||
                !std::isfinite(marquee.speed) || marquee.speed <= 0 ||
                !std::isfinite(marquee.offset) || !std::isfinite(marquee.originX) ||
                !std::isfinite(marquee.originY) || !std::isfinite(marquee.alpha))
                return reject("marquee_invalid_descriptor");
            if (!(box.left >= allowed.left && box.right <= allowed.right &&
                box.top >= allowed.top && box.bottom <= allowed.bottom &&
                box.right > box.left && box.bottom > box.top))
                return reject("marquee_outside_body");

            DWRITE_OVERHANG_METRICS ink{};
            winrt::check_hresult(marquee.layout->GetOverhangMetrics(&ink));
            const float guard = std::ceil(std::max(2.0f, ink.left + 1.0f));
            const int copies = marquee.scrolling ? 3 : 1;
            const float width = std::ceil(guard + (copies - 1) * cycle + marquee.textWidth +
                std::max(2.0f, ink.right + 1.0f));
            const float top = std::floor(box.top);
            const float height = std::ceil(box.bottom - top);
            if (!(width > 0 && width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
                height > 0 && height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION))
                return reject("marquee_texture_bounds");
            const auto duration = std::chrono::duration<double>(cycle / marquee.speed);
            if (marquee.scrolling &&
                (duration < std::chrono::milliseconds(1) || duration > std::chrono::hours(24)))
                return reject("marquee_duration_bounds");

            const auto strip = impl_->CreateSrgbContentBuffer({static_cast<LONG>(width), static_cast<LONG>(height)});
            Impl::PaintSrgbSurface(strip.surface, [&](ID2D1DeviceContext* target, POINT origin) {
                target->SetDpi(96.f, 96.f);
                target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
                target->SetTransform(D2D1::Matrix3x2F::Identity());
                target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                target->Clear(D2D1::ColorF(0, 0.f));
                const auto color = D2D1::ColorF(static_cast<UINT32>(marquee.color),
                    std::clamp(marquee.alpha, 0.0f, 1.0f));
                winrt::com_ptr<ID2D1SolidColorBrush> inkBrush;
                const auto hr = target->CreateSolidColorBrush(color, inkBrush.put());
                if (FAILED(hr)) return hr;
                for (int repeat = 0; repeat != copies; ++repeat)
                    target->DrawTextLayout({origin.x + guard + repeat * cycle, origin.y + marquee.originY - top},
                        marquee.layout.Get(), inkBrush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                return S_OK;
            });
            auto clipped = compositor.CreateContainerVisual();
            clipped.Offset({box.left - clientFrame.left, box.top - clientFrame.top, 0.f});
            clipped.Size({box.right - box.left, box.bottom - box.top});
            clipped.Clip(compositor.CreateInsetClip());
            auto glyphs = compositor.CreateSpriteVisual();
            auto brush = compositor.CreateSurfaceBrush(strip.surface);
            brush.Stretch(wuc::CompositionStretch::None);
            brush.HorizontalAlignmentRatio(0.f);
            brush.VerticalAlignmentRatio(0.f);
            brush.BitmapInterpolationMode(wuc::CompositionBitmapInterpolationMode::Linear);
            glyphs.Brush(brush);
            glyphs.Size({width, height});
            // Start at the saved phase. Three copies let a full revolution
            // wrap from that phase without a host update or an exposed gap.
            const float phase = marquee.scrolling ? std::fmod(std::max(0.0f, marquee.offset), cycle) : 0.f;
            const float start = marquee.originX - box.left - guard - phase;
            glyphs.Offset({start, top - box.top, 0.f});
            if (marquee.scrolling)
            {
                auto slide = compositor.CreateScalarKeyFrameAnimation();
                slide.Duration(std::chrono::duration_cast<wf::TimeSpan>(duration));
                slide.IterationBehavior(wuc::AnimationIterationBehavior::Forever);
                slide.InsertKeyFrame(0.f, start);
                slide.InsertKeyFrame(1.f, start - cycle, compositor.CreateLinearEasingFunction());
                glyphs.StartAnimation(L"Offset.X", slide);
                auto animation = glyphs.TryGetAnimationController(L"Offset.X");
                if (!animation) return reject("marquee_controller_unavailable");
                result->animations.push_back(std::move(animation));
            }
            clipped.Children().InsertAtTop(glyphs);
            root.Children().InsertAtTop(clipped);
        }

        result->visual = std::move(root);
        result->frame = clientFrame;
        result->allowed = allowed;
        result->labels.assign(marquees.begin(), marquees.end());
        return result;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_marquee", error.code());
        snowdesktop::performance::Value("composition.srgb", "marquee_prepare_hresult", {},
            static_cast<double>(error.code().value));
        return {};
    }
}

DesktopBackdropCompositor::PreparedImageSpin DesktopBackdropCompositor::PrepareSrgbImageSpin(
    HWND window, std::uintptr_t key, ID2D1Device* device, const RECT& clientFrame,
    const snowdesktop::widget_runtime::WidgetImageSpin& image)
{
    if (!window || !key || !device || !image.bitmap || IsRectEmpty(&clientFrame)) return {};
    const auto& dest = image.destination;
    const float edge = dest.right - dest.left;
    const float height = dest.bottom - dest.top;
    if (!(edge > 0.f && height > 0.f && edge <= 16382.f && std::abs(edge - height) < 1.f) ||
        !(image.degreesPerSecond >= 0.f && image.degreesPerSecond <= 360.f)) return {};
    try
    {
        if (!impl_->compositor && !impl_->EnsureDispatcherQueue()) return {};
        impl_->EnsureSrgbContentTarget(window);
        impl_->EnsureMaskGraphicsDevice(device);
        impl_->EnsureSrgbLayers(key);
        const auto& old = impl_->srgbContent.at(key).spin;
        const auto sameRect = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        };
        if (old && old->device.get() == device && old->image.bitmap.Get() == image.bitmap.Get() &&
            sameRect(old->image.source, image.source) && sameRect(old->image.destination, dest) &&
            EqualRect(&old->frame, &clientFrame) && old->image.interpolation == image.interpolation &&
            old->image.opacity == image.opacity)
        {
            if (old->image.degreesPerSecond == image.degreesPerSecond) return old;
            // Publish the speed change with the body, without replacing its pixels or clock.
            auto next = std::make_shared<SrgbImageSpinScene>(*old);
            next->image.degreesPerSecond = image.degreesPerSecond;
            return next;
        }
        auto result = std::make_shared<SrgbImageSpinScene>();
        result->frame = clientFrame;
        result->device.copy_from(device);
        result->image = image;
        const LONG extent = static_cast<LONG>(std::ceil(edge)) + 2;
        snowdesktop::performance::Scope raster("composition.srgb", "image_spin.raster");
        const auto buffer = impl_->CreateSrgbContentBuffer({extent, extent});
        Impl::PaintSrgbSurface(buffer.surface, [&](ID2D1DeviceContext* ctx, POINT offset) {
            ctx->SetDpi(96.f, 96.f);
            ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            ctx->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
            ctx->Clear(D2D1::ColorF(0, 0.f));
            const auto rect = D2D1::RectF(1.f, 1.f, edge + 1.f, edge + 1.f);
            winrt::com_ptr<ID2D1Factory> factory;
            ctx->GetFactory(factory.put());
            winrt::com_ptr<ID2D1EllipseGeometry> clip;
            const auto hr = factory->CreateEllipseGeometry(D2D1::Ellipse(
                {edge * .5f + 1.f, edge * .5f + 1.f}, edge * .5f, edge * .5f), clip.put());
            if (FAILED(hr)) return hr;
            ctx->PushLayer(D2D1::LayerParameters(rect, clip.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
            ctx->DrawBitmap(image.bitmap.Get(), rect, image.opacity, image.interpolation, image.source);
            ctx->PopLayer();
            return S_OK;
        });
        const auto& compositor = impl_->compositor;
        if (old && old->device.get() == device)
        {
            // A replacement cover or geometry gets a new raster but shares the current rotation.
            result->clock = old->clock;
        }
        else
        {
            result->clock = std::make_shared<SrgbImageSpinScene::Clock>(compositor);
            snowdesktop::performance::Value("composition.srgb", "image_spin.new_clock", {}, 1);
        }
        auto disc = compositor.CreateSpriteVisual();
        auto brush = compositor.CreateSurfaceBrush(buffer.surface);
        brush.Stretch(wuc::CompositionStretch::None);
        brush.BitmapInterpolationMode(wuc::CompositionBitmapInterpolationMode::Linear);
        disc.Brush(brush);
        disc.Size({static_cast<float>(extent), static_cast<float>(extent)});
        disc.Offset({dest.left - clientFrame.left - 1.f, dest.top - clientFrame.top - 1.f, 0.f});
        disc.CenterPoint({edge * .5f + 1.f, edge * .5f + 1.f, 0.f});
        auto angle = compositor.CreateExpressionAnimation(L"clock.Angle");
        angle.SetReferenceParameter(L"clock", result->clock->phase);
        disc.StartAnimation(L"RotationAngleInDegrees", angle);
        result->visual = std::move(disc);
        return result;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.prepare_image_spin", error.code());
        return {};
    }
}

bool DesktopBackdropCompositor::MatchesSrgbImageSpin(std::uintptr_t key, const PreparedImageSpin& spin) const noexcept
{
    const auto item = impl_->srgbContent.find(key);
    if (item == impl_->srgbContent.end()) return !spin;
    return item->second.spin == spin && (!spin || !spin->clock->paused || spin->image.degreesPerSecond == 0.f);
}

bool DesktopBackdropCompositor::PauseSrgbImageSpin(std::uintptr_t key)
{
    const auto it = impl_->srgbContent.find(key);
    if (it == impl_->srgbContent.end() || !it->second.spin || it->second.spin->clock->paused) return false;
    try
    {
        it->second.spin->clock->Play(0.f);
        impl_->srgbContentCommitPending = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.pause_image_spin", error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::PauseSrgbMarquees(std::uintptr_t key)
{
    const auto item = impl_->srgbContent.find(key);
    if (item == impl_->srgbContent.end() || !item->second.marquees || item->second.marquees->paused)
        return false;
    try
    {
        for (const auto& animation : item->second.marquees->animations)
            animation.Pause();
        item->second.marquees->paused = true;
        impl_->srgbContentCommitPending = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.pause_srgb_marquee", error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::PublishSrgbScene(HWND window, std::uintptr_t key,
    const RECT& clientFrame, ID2D1DeviceContext* context, POINT atlasOffset, ID2D1CommandList* scene,
    float opacity, PreparedMarquees marquees, PreparedImageSpin spin)
{
    return StageSrgbScene(window, key, clientFrame, context, atlasOffset, scene) &&
        PublishSrgbContent(key, opacity, std::move(marquees), std::move(spin));
}

HRESULT DesktopBackdropCompositor::UpdateSrgbScene(std::uintptr_t key, const RECT& damage,
    const SrgbSceneDraw& draw)
{
    const auto entry = impl_->srgbContent.find(key);
    if (entry == impl_->srgbContent.end() || !entry->second.active.surface)
        return E_INVALIDARG;
    try
    {
        Impl::PaintSrgbSurface(entry->second.active.surface, draw, &damage);
        impl_->srgbContentCommitPending = true;
        return S_OK;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_update", error.code());
        return error.code();
    }
}

HRESULT DesktopBackdropCompositor::UpdateSrgbScene(std::uintptr_t key, const RECT& damage,
    ID2D1CommandList* scene, POINT atlasOffset)
{
    return UpdateSrgbScene(key, damage, [&](ID2D1DeviceContext* target, POINT origin) {
        target->SetDpi(96.f, 96.f);
        target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(origin.x),
            static_cast<float>(origin.y), static_cast<float>(origin.x + damage.right - damage.left),
            static_cast<float>(origin.y + damage.bottom - damage.top)), D2D1_ANTIALIAS_MODE_ALIASED);
        target->Clear(D2D1::ColorF(0, 0.f));
        ReplayScene(target, scene, damage, atlasOffset, origin);
        target->PopAxisAlignedClip();
        return S_OK;
    });
}

bool DesktopBackdropCompositor::PresentPremultipliedPixels(HWND window, SIZE size, const std::uint32_t* pixels)
{
    if (!window || !pixels || size.cx <= 0 || size.cy <= 0) return false;
    static thread_local winrt::com_ptr<ID2D1Device> device;
    if (!device)
    {
        winrt::com_ptr<ID3D11Device> d3d;
        auto hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr);
        if (FAILED(hr)) return false;
        const auto dxgi = d3d.try_as<IDXGIDevice>();
        if (!dxgi || FAILED(D2D1CreateDevice(dxgi.get(), nullptr, device.put()))) return false;
    }
    const RECT frame{0, 0, size.cx, size.cy};
    const auto key = reinterpret_cast<std::uintptr_t>(window);
    return StageSrgbSceneFrame(window, key, frame, device.get(),
        [&](ID2D1DeviceContext* context, POINT offset) -> HRESULT {
            context->SetDpi(96.f, 96.f);
            context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
            const auto pixelSize = D2D1::SizeU(static_cast<UINT>(size.cx), static_cast<UINT>(size.cy));
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::com_ptr<ID2D1Bitmap1> bitmap;
            const auto stride = pixelSize.width * static_cast<UINT>(sizeof(std::uint32_t));
            const auto result = context->CreateBitmap(pixelSize, pixels, stride, &properties, bitmap.put());
            if (SUCCEEDED(result))
            {
                const auto dest = D2D1::RectF(0.f, 0.f, static_cast<float>(size.cx), static_cast<float>(size.cy));
                context->DrawBitmap(bitmap.get(), &dest, 1.f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            return result;
        }) && PublishSrgbContent(key) && CommitSrgbContent();
}

bool DesktopBackdropCompositor::PublishSrgbContent(std::uintptr_t key, float opacity,
    PreparedMarquees marquees, PreparedImageSpin spin)
{
    const auto entry = impl_->srgbContent.find(key);
    if (entry == impl_->srgbContent.end() || !entry->second.staged) return false;
    auto& frame = entry->second;
    const float previousSpeed = frame.spin && !frame.spin->clock->paused ? frame.spin->image.degreesPerSecond : 0.f;
    try
    {
        if (!frame.visual)
        {
            auto visual = impl_->compositor.CreateSpriteVisual();
            auto sceneBrush = impl_->compositor.CreateSurfaceBrush(frame.candidate.surface);
            sceneBrush.Stretch(wuc::CompositionStretch::None);
            sceneBrush.HorizontalAlignmentRatio(0.f);
            sceneBrush.VerticalAlignmentRatio(0.f);
            sceneBrush.BitmapInterpolationMode(wuc::CompositionBitmapInterpolationMode::NearestNeighbor);
            if (!impl_->precomposedSceneFactory)
                impl_->precomposedSceneFactory = snowdesktop::backdrop_srgb::CreateSceneFactory(impl_->compositor);
            auto effect = impl_->precomposedSceneFactory.CreateBrush();
            effect.SetSourceParameter(L"backdrop", impl_->compositor.CreateBackdropBrush());
            effect.SetSourceParameter(L"scene", sceneBrush);
            visual.Brush(effect);
            impl_->srgbContentRoot.Children().InsertAtTop(visual);
            frame.sceneBrush = std::move(sceneBrush);
            frame.sceneEffect = std::move(effect);
            frame.visual = std::move(visual);
            impl_->srgbContentOrderDirty = true;
        }
        frame.visual.Offset({static_cast<float>(frame.stagedFrame.left),
            static_cast<float>(frame.stagedFrame.top), 0.f});
        const wfn::float2 size{static_cast<float>(frame.candidate.size.cx),
            static_cast<float>(frame.candidate.size.cy)};
        frame.visual.Size(size);
        if (opacity >= 0.f) frame.visual.Opacity(std::clamp(opacity, 0.f, 1.f));
        // Keep the backdrop input attached to the same visual across content changes.
        // The back buffer becomes visible only after its complete draw succeeded.
        frame.sceneBrush.Surface(frame.candidate.surface);
        if (spin || marquees)
        {
            // The root remains the same when the cover, scrolling text, or body changes.
            if ((!frame.spin && !frame.marquees) || frame.active.size.cx != frame.candidate.size.cx ||
                frame.active.size.cy != frame.candidate.size.cy)
            {
                frame.layers.Size(size);
                frame.body.Size(size);
                frame.layersSurface.SourceSize(size);
            }
            const auto oldDisc = frame.spin ? frame.spin->visual : nullptr;
            const auto newDisc = spin ? spin->visual : nullptr;
            if (oldDisc != newDisc || frame.marquees != marquees)
                Impl::BindSrgbLayers(frame, spin, marquees);
            if (marquees && marquees->paused)
            {
                for (const auto& animation : marquees->animations) animation.Resume();
                marquees->paused = false;
            }
            if (spin)
            {
                const bool wasPaused = spin->clock->paused;
                const float speed = spin->image.degreesPerSecond;
                spin->clock->Play(speed);
                if (wasPaused && speed > 0.f)
                    snowdesktop::performance::Value("composition.srgb", "image_spin.resume_degrees_per_second", {}, speed);
            }
        }
        else if (frame.spin || frame.marquees)
            Impl::BindSrgbLayers(frame, {}, {});
        frame.marquees = std::move(marquees);
        frame.spin = std::move(spin);
        std::swap(frame.active, frame.candidate);
        frame.staged = false;
        frame.retirement.reset();
        impl_->srgbContentCommitPending = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        // A static retry can follow in this transaction. Undo partially installed
        // children before exposing the previous pixels or accepting the retry.
        if (frame.layers && frame.sceneEffect)
        {
            try
            {
                frame.sceneBrush.Surface(frame.active.surface);
                Impl::BindSrgbLayers(frame, frame.spin, frame.marquees);
                if (frame.spin) frame.spin->clock->Play(previousSpeed);
            }
            catch (const winrt::hresult_error&) { /* Device recovery owns an unavailable compositor. */ }
        }
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_publish", error.code());
        impl_->srgbPublicationFailed = true;
        if (frame.active.surface && IsWindow(impl_->srgbContentWindow))
            InvalidateRect(impl_->srgbContentWindow, nullptr, FALSE);
        return false;
    }
}

bool DesktopBackdropCompositor::HasSrgbContent(std::uintptr_t key) const noexcept
{
    const auto item = impl_->srgbContent.find(key);
    return item != impl_->srgbContent.end() && item->second.visual && item->second.active.surface;
}

HRESULT DesktopBackdropCompositor::SetDirectContentForSrgb(std::uintptr_t key,
    bool srgbPublished, IDCompositionDevice2* device, IDCompositionVisual* visual, IUnknown* surface)
{
    if (!device || !visual)
        return E_POINTER;
    const auto entry = impl_->srgbContent.find(key);
    if (!srgbPublished || entry == impl_->srgbContent.end())
    {
        const auto hr = visual->SetContent(surface);
        if (SUCCEEDED(hr))
            RetireSrgbAfterDirectCommit(key);
        return hr;
    }

    return visual->SetContent(surface);
}

void DesktopBackdropCompositor::RetireSrgbAfterDirectCommit(std::uintptr_t key)
{
    const auto found = impl_->srgbContent.find(key);
    if (found == impl_->srgbContent.end())
        return;
    auto& frame = found->second;
    if (!frame.retirement)
    {
        frame.retirement = std::make_shared<Impl::SrgbRetirement>();
        impl_->srgbRetirementPending = true;
    }
}

void DesktopBackdropCompositor::OnDirectContentCommitted(IDCompositionDevice2* device)
{
    if (!device || !impl_->srgbRetirementPending)
        return;
    std::vector<Impl::PendingRetirement> pending;
    for (const auto& [key, frame] : impl_->srgbContent)
        if (frame.retirement && !frame.retirement->waiting)
            pending.emplace_back(key, frame.retirement);
    if (pending.empty())
    {
        impl_->srgbRetirementPending = false;
        return;
    }

    const auto queue = ws::DispatcherQueue::GetForCurrentThread();
    if (!queue)
        return;
    for (const auto& [key, entry] : pending)
        if (const auto retirement = entry.lock()) retirement->waiting = true;
    impl_->srgbRetirementPending = false;
    winrt::com_ptr<IDCompositionDevice2> direct;
    direct.copy_from(device);
    Impl::WaitForDirectContent(this, impl_->srgbLifetime, std::move(direct), queue, std::move(pending));
}

void DesktopBackdropCompositor::RemoveSrgbContent(std::uintptr_t key)
{
    const auto item = impl_->srgbContent.find(key);
    if (item == impl_->srgbContent.end()) return;
    if (item->second.visual)
    {
        try
        {
            impl_->srgbContentRoot.Children().Remove(item->second.visual);
            impl_->srgbContentCommitPending = true;
        }
        catch (const winrt::hresult_error& error)
        {
            impl_->lastError = FormatHresult(L"backdrop.srgb_content_remove", error.code());
        }
    }
    impl_->srgbContent.erase(item);
}

void DesktopBackdropCompositor::SetSrgbContentOrder(std::span<const std::uintptr_t> bottomToTop)
{
    if (!impl_->srgbContentRoot || (!impl_->srgbContentOrderDirty &&
        std::equal(bottomToTop.begin(), bottomToTop.end(), impl_->srgbContentOrder.begin(),
            impl_->srgbContentOrder.end()))) return;
    try
    {
        const auto children = impl_->srgbContentRoot.Children();
        for (const auto key : bottomToTop)
        {
            const auto found = impl_->srgbContent.find(key);
            if (found == impl_->srgbContent.end() || !found->second.visual) continue;
            children.Remove(found->second.visual);
            children.InsertAtTop(found->second.visual);
        }
        impl_->srgbContentOrder.assign(bottomToTop.begin(), bottomToTop.end());
        impl_->srgbContentOrderDirty = false;
        impl_->srgbContentCommitPending = true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_order", error.code());
    }
}

void DesktopBackdropCompositor::SetSrgbContentOpacity(std::uintptr_t key, float opacity)
{
    const auto found = impl_->srgbContent.find(key);
    if (found == impl_->srgbContent.end() || !found->second.visual) return;
    try
    {
        const float clamped = std::clamp(opacity, 0.f, 1.f);
        if (found->second.visual.Opacity() == clamped) return;
        found->second.visual.Opacity(clamped);
        impl_->srgbContentCommitPending = true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_opacity", error.code());
    }
}

bool DesktopBackdropCompositor::CommitSrgbContent()
{
    if (!impl_->srgbContentCommitPending) return true;
    try
    {
        impl_->compositorController.Commit();
        impl_->srgbContentCommitPending = false;
        if (impl_->srgbContent.empty())
            impl_->srgbPublicationFailed = false;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_commit", error.code());
        impl_->srgbPublicationFailed = true;
        return false;
    }
}

bool DesktopBackdropCompositor::SetSrgbContentPose(std::uintptr_t key, POINT anchor,
    float scale, float opacity)
{
    const auto entry = impl_->srgbContent.find(key);
    if (entry == impl_->srgbContent.end() || !entry->second.visual) return false;
    const auto& frame = entry->second;
    try
    {
        frame.visual.StopAnimation(L"Scale");
        frame.visual.StopAnimation(L"Opacity");
        frame.visual.CenterPoint({static_cast<float>(anchor.x - frame.stagedFrame.left),
            static_cast<float>(anchor.y - frame.stagedFrame.top), 0.f});
        frame.visual.Scale({scale, scale, 1.f});
        frame.visual.Opacity(std::clamp(opacity, 0.f, 1.f));
        impl_->srgbContentCommitPending = true;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_pose", error.code());
        return false;
    }
}

void DesktopBackdropCompositor::SetSrgbRootPose(float opacity, float offsetX, float offsetY)
{
    if (!impl_->srgbContentRoot) return;
    try
    {
        const float clamped = std::clamp(opacity, 0.f, 1.f);
        const wfn::float3 offset{offsetX, offsetY, 0.f};
        if (impl_->srgbContentRoot.Opacity() == clamped && impl_->srgbContentRoot.Offset() == offset)
            return;
        impl_->srgbContentRoot.Opacity(clamped);
        impl_->srgbContentRoot.Offset(offset);
        impl_->srgbContentCommitPending = true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_root_pose", error.code());
    }
}

bool DesktopBackdropCompositor::SetPanelTransform(std::uintptr_t ownerKey,
    const D2D1_MATRIX_4X4_F& matrix, const RECT& projectedFrame)
{
    if (!impl_->available || !ownerKey || IsRectEmpty(&projectedFrame)) return false;
    if (impl_->stagingFrame)
    {
        auto panel = impl_->FindStagedPanel({}, ownerKey);
        if (panel == impl_->stagedPanels.end()) return false;
        panel->transformed = true;
        panel->transform = matrix;
        panel->projectedFrame = projectedFrame;
        return true;
    }
    const auto found = impl_->FindPanel({}, ownerKey);
    if (found == impl_->panels.end()) return false;
    try
    {
        found->SetTransform(wfn::float4x4{
            matrix._11, matrix._12, matrix._13, matrix._14,
            matrix._21, matrix._22, matrix._23, matrix._24,
            matrix._31, matrix._32, matrix._33, matrix._34,
            matrix._41, matrix._42, matrix._43, matrix._44 });
        // A projected/folded quad is not an axis-aligned rounded rectangle.
        // Use its conservative rectangle until the ordinary pose returns.
        const bool translated = matrix._11 == 1 && matrix._22 == 1 && matrix._33 == 1 && matrix._44 == 1 &&
            matrix._12 == 0 && matrix._13 == 0 && matrix._14 == 0 && matrix._21 == 0 && matrix._23 == 0 &&
            matrix._24 == 0 && matrix._31 == 0 && matrix._32 == 0 && matrix._34 == 0 && matrix._43 == 0;
        const float regionRadius = translated ? found->cornerRadius : 0.f;
        if (!EqualRect(&found->regionFrame, &projectedFrame) || found->regionCornerRadius != regionRadius)
            impl_->transformedRegionDirty = true;
        found->regionCornerRadius = regionRadius;
        found->regionFrame = projectedFrame;
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::StartPanelHeightAnimation(
    std::uintptr_t ownerKey, float collapsedHeight, float radius,
    const snowdesktop::popup_motion_curve::Timeline& timeline)
{
    if (!impl_->available || !impl_->collectingFrame || !ownerKey ||
        timeline.segments.empty() || timeline.durationMilliseconds <= 0.0)
        return false;
    const auto panel = impl_->FindPanel({}, ownerKey);
    if (panel == impl_->panels.end()) return false;
    const auto width = static_cast<float>(panel->frame.right - panel->frame.left);
    const auto travel = static_cast<float>(panel->frame.bottom - panel->frame.top) - collapsedHeight;

    try
    {
        auto animation = impl_->compositor.CreateVector2KeyFrameAnimation();
        animation.Duration(wf::TimeSpan{std::llround(timeline.durationMilliseconds * 10'000.0)});
        const auto size = [width, collapsedHeight, travel](double progress) {
            return wfn::float2{width, static_cast<float>(collapsedHeight + travel * progress)};
        };
        animation.InsertKeyFrame(0.0f, size(timeline.segments.front().constant));
        for (const auto& segment : timeline.segments)
        {
            const auto easing = impl_->compositor.CreateCubicBezierEasingFunction(
                wfn::float2{1.0f / 3.0f, static_cast<float>(segment.StartControl())},
                wfn::float2{2.0f / 3.0f, static_cast<float>(segment.EndControl())});
            const float time = std::clamp(static_cast<float>(
                (segment.startSeconds + segment.durationSeconds) * 1000.0 /
                timeline.durationMilliseconds), 0.0f, 1.0f);
            animation.InsertKeyFrame(time,
                size(segment.EvaluateLocal(segment.durationSeconds).position), easing);
        }

        auto geometry = impl_->compositor.CreateRoundedRectangleGeometry();
        geometry.CornerRadius(wfn::float2{radius, radius});
        geometry.Size(size(timeline.target));
        geometry.StartAnimation(L"Size", animation);
        panel->SetClip(geometry);
        const auto content = impl_->srgbContent.find(ownerKey);
        if (content != impl_->srgbContent.end() && content->second.visual)
        {
            auto contentClip = impl_->compositor.CreateRoundedRectangleGeometry();
            contentClip.Offset({static_cast<float>(panel->frame.left - content->second.stagedFrame.left),
                static_cast<float>(panel->frame.top - content->second.stagedFrame.top)});
            contentClip.CornerRadius({radius, radius});
            contentClip.Size(size(timeline.target));
            contentClip.StartAnimation(L"Size", animation);
            content->second.visual.Clip(impl_->compositor.CreateGeometricClip(contentClip));
            impl_->srgbContentCommitPending = true;
        }
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(L"backdrop.animate_height", error.code());
        return false;
    }
}

void DesktopBackdropCompositor::SetSrgbContentRollClip(std::uintptr_t ownerKey,
    const RECT& bounds, float radius)
{
    const auto found = impl_->srgbContent.find(ownerKey);
    if (found == impl_->srgbContent.end() || !found->second.visual) return;
    auto& content = found->second;
    try
    {
        if (IsRectEmpty(&bounds))
        {
            if (!content.visual.Clip()) return;
            content.visual.Clip(nullptr);
        }
        else
        {
            const float width = static_cast<float>(bounds.right - bounds.left);
            const float height = static_cast<float>(bounds.bottom - bounds.top);
            radius = std::min({radius, width * .5f, height * .5f});
            auto geometry = impl_->compositor.CreateRoundedRectangleGeometry();
            geometry.Offset({static_cast<float>(bounds.left - content.stagedFrame.left),
                static_cast<float>(bounds.top - content.stagedFrame.top)});
            geometry.Size({width, height});
            geometry.CornerRadius({radius, radius});
            content.visual.Clip(impl_->compositor.CreateGeometricClip(geometry));
        }
        impl_->srgbContentCommitPending = true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.srgb_content_roll_clip", error.code());
    }
}

void DesktopBackdropCompositor::SetPanelAnimationFence(
    std::uintptr_t ownerKey, const RECT& frame)
{
    if (!impl_->available || !impl_->collectingFrame || !ownerKey)
        return;
    const auto panel = impl_->FindPanel({}, ownerKey);
    if (panel == impl_->panels.end()) return;

    // Reserve the whole roll envelope for both native coverage and sampling.
    // The displayed height can change without moving the edge of the filter input.
    const RECT& visible = panel->regionFrame;
    const RECT previous = panel->animationFence.value_or(RECT{});
    if (!IsRectEmpty(&frame) && frame.left == visible.left &&
        frame.top == visible.top && frame.right == visible.right &&
        frame.bottom >= visible.bottom)
        panel->animationFence = frame;
    else
        panel->animationFence.reset();
    const RECT current = panel->animationFence.value_or(RECT{});
    if (!EqualRect(&previous, &current))
    {
        if (!panel->iconRegionsApplied) panel->SetBaseSampleBounds();
        // Regional brushes are prepared with their matching masks by the
        // component's icon collection before committing this frame.
        impl_->transformedRegionDirty = true;
    }
}

bool DesktopBackdropCompositor::RemovePanel(const RECT& frame, std::uintptr_t ownerKey)
{
    const auto matches = [&frame, ownerKey](const auto& panel) {
        return ownerKey != 0 ? panel.ownerKey == ownerKey : EqualRect(&panel.frame, &frame) != FALSE;
    };
    if (impl_->stagingFrame)
    {
        const bool removed = std::erase_if(impl_->stagedPanels, matches) != 0;
        if (removed) impl_->RebuildStagedPanelOwners();
        return removed;
    }
    if (!impl_->available || !impl_->root)
        return false;
    auto existing = ownerKey ? impl_->FindPanel(frame, ownerKey) :
        std::find_if(impl_->panels.begin(), impl_->panels.end(), [&](const auto& panel) {
            return !panel.background && matches(panel);
        });
    if (existing == impl_->panels.end())
        return false;

    try
    {
        if (impl_->genieSource == existing->visual)
            impl_->ClearGenieTransform();
        impl_->root.Children().Remove(existing->visual);
        existing->sample.Brush(nullptr);
        impl_->ErasePanel(existing);
        impl_->blurFactoriesDirty = true;
        if (!impl_->collectingFrame)
        {
            impl_->PruneUnusedBlurFactories();
            if (impl_->genieRoot)
                impl_->SetAnimationPathRegionExpanded(true);
            else
                impl_->SyncPanelWindowRegion();
            impl_->RequestCommit();
        }
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.remove_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::KeepPanel(
    const RECT& frame, std::uintptr_t ownerKey)
{
    // Staged frames are full redraws, not retained/partial collections.
    if (impl_->stagingFrame) return false;
    if (!impl_->available)
        return false;
    const auto existing = std::find_if(
        impl_->panels.begin(), impl_->panels.end(),
        [&frame, ownerKey](const Impl::PanelVisual& panel) {
            return !panel.background && (ownerKey == 0 || panel.ownerKey == ownerKey) &&
                EqualRect(&panel.frame, &frame) != FALSE;
        });
    if (existing == impl_->panels.end())
        return false;
    existing->seen = true;
    return true;
}

bool DesktopBackdropCompositor::SetPanelOpacity(
    const RECT& frame, float opacity, std::uintptr_t ownerKey)
{
    if (impl_->stagingFrame)
    {
        auto panel = ownerKey ? impl_->FindStagedPanel(frame, ownerKey) :
            std::find_if(impl_->stagedPanels.begin(), impl_->stagedPanels.end(),
            [&](const auto& value) { return EqualRect(&value.frame, &frame) != FALSE; });
        if (panel == impl_->stagedPanels.end()) return false;
        panel->opacity = std::clamp(opacity, 0.0f, 1.0f);
        return true;
    }
    if (!impl_->available)
        return false;
    const auto existing = ownerKey ? impl_->FindPanel(frame, ownerKey) : std::find_if(
        impl_->panels.begin(), impl_->panels.end(),
        [&frame](const Impl::PanelVisual& panel) {
            return !panel.background && EqualRect(&panel.frame, &frame) != FALSE;
        });
    if (existing == impl_->panels.end())
        return false;
    try
    {
        const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
        if (impl_->genieSource == existing->visual)
        {
            impl_->genieSourceOpacity = clampedOpacity;
            impl_->genieRoot.Opacity(impl_->genieOpacity * clampedOpacity);
        }
        else
            existing->SetOpacity(clampedOpacity);
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(
            _LW("backdrop.update_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::StartOwnerFadeOut(std::uintptr_t ownerKey,
    float fromOpacity, UINT durationMilliseconds, float normalizedStartSlope)
{
    if (impl_->stagingFrame || !ownerKey || !durationMilliseconds) return false;
    const auto panel = impl_->FindPanel({}, ownerKey);
    const auto content = impl_->srgbContent.find(ownerKey);
    const bool hasContent = content != impl_->srgbContent.end() && content->second.visual;
    if (panel == impl_->panels.end() && !hasContent) return true;
    if (panel != impl_->panels.end() && impl_->genieSource == panel->visual) return false;
    try
    {
        auto animation = impl_->compositor.CreateScalarKeyFrameAnimation();
        animation.Duration(wf::TimeSpan{static_cast<std::int64_t>(durationMilliseconds) * 10'000});
        const auto easing = impl_->compositor.CreateCubicBezierEasingFunction(
            {1.f / 3.f, normalizedStartSlope / 3.f}, {2.f / 3.f, 1.f});
        animation.InsertKeyFrame(0.f, fromOpacity);
        animation.InsertKeyFrame(1.f, 0.f, easing);
        if (panel != impl_->panels.end())
        {
            panel->visual.Opacity(0.f);
            panel->visual.StartAnimation(L"Opacity", animation);
            panel->submittedOpacity.reset();
        }
        if (hasContent)
        {
            content->second.visual.Opacity(0.f);
            content->second.visual.StartAnimation(L"Opacity", animation);
            impl_->srgbContentCommitPending = true;
        }
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::SetVisualOpacity(
    float opacity)
{
    if (!impl_->available || !impl_->root)
        return false;
    try
    {
        impl_->root.Opacity(
            std::clamp(opacity, 0.0f, 1.0f));
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(
            _LW("backdrop.update_panel"), error.code());
        return false;
    }
}

void DesktopBackdropCompositor::SetVisualTranslation(float x, float y)
{
    if (!impl_ || !impl_->available || !impl_->root) return;
    try
    {
        impl_->root.Offset(wfn::float3{x, y, 0.f});
        impl_->SetAnimationPathRegionExpanded(std::abs(x) > 0.001f || std::abs(y) > 0.001f);
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.update_panel"), error.code());
    }
}

bool DesktopBackdropCompositor::StartRootTranslationAnimation(
    const snowdesktop::popup_motion_curve::Timeline& progress,
    float baseX, float baseY, float factorX, float factorY, bool includeGlass, bool presentOnBackdrop)
{
    if (progress.segments.empty() || progress.durationMilliseconds <= 0.0 ||
        ((includeGlass || presentOnBackdrop) && (!impl_->available || !impl_->root)) ||
        (presentOnBackdrop && (!impl_->popupMode || !impl_->srgbContentTarget || !impl_->srgbContentRoot)))
        return false;
    if (!includeGlass && !impl_->srgbContentRoot) return true;

    const auto offset = [&](double position) {
        return wfn::float3{static_cast<float>(baseX + factorX * position),
            static_cast<float>(baseY + factorY * position), 0.f};
    };
    try
    {
        auto animation = impl_->compositor.CreateVector3KeyFrameAnimation();
        animation.Duration(wf::TimeSpan{std::llround(progress.durationMilliseconds * 10'000.0)});
        animation.InsertKeyFrame(0.f, offset(progress.segments.front().constant));
        for (const auto& segment : progress.segments)
        {
            const float fraction = std::clamp(static_cast<float>(
                (segment.startSeconds + segment.durationSeconds) * 1000.0 / progress.durationMilliseconds), 0.f, 1.f);
            const auto easing = impl_->compositor.CreateCubicBezierEasingFunction(
                {1.f / 3.f, static_cast<float>(segment.StartControl())},
                {2.f / 3.f, static_cast<float>(segment.EndControl())});
            animation.InsertKeyFrame(fraction, offset(segment.EvaluateLocal(segment.durationSeconds).position), easing);
        }

        // Both roots belong to the same controller. Publish their curves together,
        // including the conservative helper region needed until the motion ends.
        if (presentOnBackdrop)
        {
            // Keep the allocation through the return handoff. Shrinking an
            // HWND region synchronously could trim the compositor's last frame.
            if (!std::exchange(impl_->popupPresentationViewport, true))
                impl_->panelRegionSnapshotValid = false;
            if (!impl_->SyncPanelWindowRegion()) return false;
            if (IsWindowVisible(impl_->contentWindow)) impl_->visible = true;
            if (!impl_->SyncWindowPlacement()) return false;
            impl_->SetSrgbRootOnPopup(true);
        }
        if (includeGlass)
        {
            impl_->rootTranslationOwnsGlass = true;
            if (!impl_->SyncPanelWindowRegion()) return false;
            impl_->root.Offset(offset(progress.target));
            impl_->root.StartAnimation(L"Offset", animation);
        }
        if (impl_->srgbContentRoot)
        {
            impl_->srgbContentRoot.Offset(offset(progress.target));
            impl_->srgbContentRoot.StartAnimation(L"Offset", animation);
            impl_->srgbContentCommitPending = true;
        }
        return includeGlass ? impl_->RequestCommit() : CommitSrgbContent();
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.start_root_translation", error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::StopRootTranslationAnimation(float x, float y)
{
    const bool hadGlass = std::exchange(impl_->rootTranslationOwnsGlass, false);
    try
    {
        const wfn::float3 offset{x, y, 0.f};
        impl_->SetSrgbRootOnPopup(false);
        if (impl_->srgbContentRoot)
        {
            impl_->srgbContentRoot.StopAnimation(L"Offset");
            impl_->srgbContentRoot.Offset(offset);
            impl_->srgbContentCommitPending = true;
        }
        if (hadGlass && impl_->available && impl_->root)
        {
            impl_->root.StopAnimation(L"Offset");
            impl_->root.Offset(offset);
            if (std::abs(x) > 0.001f || std::abs(y) > 0.001f)
                impl_->SetAnimationPathRegionExpanded(true);
            else
                impl_->transformedRegionDirty = !impl_->SyncPanelWindowRegion();
            return impl_->RequestCommit();
        }
        return CommitSrgbContent();
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->lastError = FormatHresult(L"backdrop.stop_root_translation", error.code());
        return false;
    }
}

void DesktopBackdropCompositor::CommitVisualChanges()
{
    if (impl_)
    {
        if (impl_->transformedRegionDirty)
        {
            impl_->transformedRegionDirty = !impl_->SyncPanelWindowRegion();
        }
        impl_->RequestCommit();
    }
}

void DesktopBackdropCompositor::CommitDeferredFrame()
{
    if (!impl_ || !impl_->frameCommitPending || impl_->collectingFrame)
        return;
    if (impl_->popupMode)
        CommitVisualChanges();
    else if (impl_->RequestCommit() && impl_->transformedRegionDirty)
    {
        // This HWND fence can expose pixels immediately. Submit both the
        // desktop body and the matching glass before changing its region.
        impl_->transformedRegionDirty = !impl_->SyncPanelWindowRegion();
    }
}

void DesktopBackdropCompositor::EndFrame(
    bool requestCommit)
{
    if (impl_->stagingFrame) return;
    // Retire the replaced file's old glass in the transaction that drew its
    // new pixels. A retained widget or a foreground-only frame cannot consume it.
    std::erase_if(impl_->changedIconPanels, [&](const auto& changed) {
        if (!changed.redrawing) return false;
        const auto panel = impl_->FindPanel(changed.frame, changed.ownerKey);
        if (panel != impl_->panels.end() && !panel->seen && EqualRect(&panel->frame, &changed.frame))
            (void)RemoveIconPanel(changed.frame, changed.ownerKey);
        return true;
    });
    impl_->collectingFrame = false;
    if (!impl_->available)
        return;
    try
    {
        if (impl_->completeCollection)
        {
            auto children = impl_->root.Children();
            for (auto iterator = impl_->panels.begin();
                 iterator != impl_->panels.end();)
            {
                if (iterator->seen)
                {
                    ++iterator;
                    continue;
                }
                if (impl_->genieSource == iterator->visual)
                    impl_->ClearGenieTransform();
                children.Remove(iterator->visual);
                iterator->sample.Brush(nullptr);
                iterator = impl_->ErasePanel(iterator);
                impl_->blurFactoriesDirty = true;
            }
        }
        impl_->PruneUnusedBlurFactories();
        if (impl_->genieRoot)
            impl_->SetAnimationPathRegionExpanded(true);
        else if (requestCommit || impl_->popupMode)
            impl_->transformedRegionDirty = !impl_->SyncPanelWindowRegion();
        else
            impl_->transformedRegionDirty = !impl_->SyncPanelWindowRegion(true);
        impl_->frameCommitPending = true;
        if (requestCommit)
            impl_->RequestCommit();
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.remove_panel"), error.code());
    }
}

void DesktopBackdropCompositor::ResetBackdrop()
{
    if (impl_ && impl_->backdropWindow)
        impl_->Reset(true);
}

void DesktopBackdropCompositor::Reset()
{
    if (impl_)
        impl_->Reset();
}

bool DesktopBackdropCompositor::IsAvailable() const
{
    return impl_ && impl_->available;
}

bool DesktopBackdropCompositor::IsBackdropWindow(HWND window) const
{
    return impl_ && window && impl_->backdropWindow == window;
}

std::size_t DesktopBackdropCompositor::PanelCount() const
{
    return impl_ ? impl_->panels.size() : 0;
}

std::size_t DesktopBackdropCompositor::BlurFactoryCount() const
{
    return impl_ ? impl_->blurFactories.size() : 0;
}

const std::wstring& DesktopBackdropCompositor::LastError() const
{
    return impl_->lastError;
}

void DesktopBackdropCompositor::SetOcclusionRect(const RECT& bounds)
{
    if (impl_->stagingFrame)
    {
        impl_->stagedOcclusionRect = bounds;
        return;
    }
    if (!impl_ || EqualRect(&impl_->occlusionRect, &bounds)) return;
    impl_->occlusionRect = bounds;
    impl_->SyncPanelWindowRegion();
}
