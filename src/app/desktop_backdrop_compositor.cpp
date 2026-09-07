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

#include "desktop_backdrop_update_rules.h"
#include "popup_window_pair_z_order.h"
#include "../dock_genie_rules.h"
#include "../quick_navigation_genie_rules.h"

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <DispatcherQueue.h>
#include <dwmapi.h>
#include <windows.graphics.effects.interop.h>
#include <windows.graphics.interop.h>
#include <windows.ui.composition.interop.h>

#include "l10n.h"

#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#pragma pop_macro("GetCurrentTime")

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wfn = winrt::Windows::Foundation::Numerics;
namespace wge = winrt::Windows::Graphics::Effects;
namespace ws = winrt::Windows::System;
namespace wuc = winrt::Windows::UI::Composition;
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

/** @brief CompositionPath 对不可变 D2D 路径的原生适配。 */
struct BackdropGeometrySource : winrt::implements<BackdropGeometrySource,
    winrt::Windows::Graphics::IGeometrySource2D,
    ABI::Windows::Graphics::IGeometrySource2DInterop>
{
    explicit BackdropGeometrySource(winrt::com_ptr<ID2D1PathGeometry> geometry)
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
    float width, float height, float cornerRadius, bool vertical)
{
    namespace genie = snowdesktop::dock_genie;
    const float axisLength = vertical ? height : width;
    const float crossLength = vertical ? width : height;
    const float radius = std::clamp(cornerRadius, 0.0f,
        std::min(width, height) * 0.5f);
    // Uniform angle samples preserve the rounded corners even when a whole
    // corner falls inside one strip. Cache source points until geometry changes.
    constexpr std::size_t cornerSamples = 12;
    constexpr double halfPi = 1.57079632679489661923;
    std::array<float, cornerSamples * 2> cornerAxes{};
    for (std::size_t index = 0; index < cornerSamples; ++index)
    {
        const double angle = halfPi * static_cast<double>(index + 1) /
            static_cast<double>(cornerSamples);
        const float offset = radius * static_cast<float>(1.0 - std::cos(angle));
        cornerAxes[index] = offset;
        cornerAxes[cornerAxes.size() - 1 - index] = axisLength - offset;
    }

    std::vector<GenieOutlinePoint> outline;
    outline.reserve(genie::StripCount * 4 + cornerAxes.size() * 2);
    const auto appendLowSide = [&](std::size_t strip, float axis) {
        const float distance = std::max(0.0f,
            radius - std::min(axis, axisLength - axis));
        const float inset = radius > 0.0f
            ? radius - std::sqrt(std::max(0.0f,
                radius * radius - distance * distance))
            : 0.0f;
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
    return outline;
}

using CreateDispatcherQueueControllerFn = HRESULT(WINAPI*)(DispatcherQueueOptions,
    ABI::Windows::System::IDispatcherQueueController**);

struct SharedBackdropCompositionContext
{
    ws::DispatcherQueueController dispatcherController{nullptr};
    wuc::Compositor compositor{nullptr};
};

SharedBackdropCompositionContext& SharedBackdropContext()
{
    // Every DesktopBackdropCompositor is created and used on the desktop UI
    // thread. Sharing one compositor lets desktop and popup targets change
    // panel opacity in the same transactional visual update.
    static thread_local SharedBackdropCompositionContext context;
    return context;
}

} // namespace

struct DesktopBackdropCompositor::Impl
{

    struct PanelVisual
    {
        RECT frame{};
        std::uintptr_t ownerKey = 0;
        int cornerRadius = 0;
        int blurRadius = 0;
        wuc::SpriteVisual visual{nullptr};
        wuc::CompositionRoundedRectangleGeometry geometry{nullptr};
        wuc::CompositionGeometricClip clip{nullptr};
        bool seen = false;
    };

    HWND contentWindow = nullptr;
    HWND backdropWindow = nullptr;
    ws::DispatcherQueueController dispatcherController{nullptr};
    wuc::Compositor compositor{nullptr};
    wucd::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr};
    std::unordered_map<int, wuc::CompositionEffectFactory> blurFactories;
    std::vector<PanelVisual> panels;
    wuc::SpriteVisual genieRoot{nullptr};
    wuc::SpriteVisual genieSource{nullptr};
    wuc::CompositionPathGeometry genieGeometry{nullptr};
    winrt::com_ptr<ID2D1Factory> genieGeometryFactory;
    std::vector<GenieOutlinePoint> genieOutline;
    RECT geniePanelFrame{};
    SIZE genieHostSize{};
    int genieCornerRadius = 0;
    bool genieVertical = true;
    float genieSourceOpacity = 1.0f;
    float genieOpacity = 1.0f;
    std::wstring lastError;
    bool completeCollection = true;
    bool collectingFrame = false;
    bool blurFactoriesDirty = false;
    bool available = false;
    bool popupMode = false;
    bool popupTopmost = false;
    bool visible = true;
    bool animationPathRegionExpanded = false;

    void SetError(const wchar_t* stage, HRESULT hr)
    {
        lastError = FormatHresult(stage, hr);
        available = false;
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
                shared.compositor = wuc::Compositor();
            compositor = shared.compositor;
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
            origin = { rect.left, rect.top };
            size.cx = std::max<LONG>(1, rect.right - rect.left);
            size.cy = std::max<LONG>(1, rect.bottom - rect.top);
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
            return true;
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
            flags) != FALSE;
    }

    bool SyncPanelWindowRegion()
    {
        if (!available || !backdropWindow ||
            !IsWindow(backdropWindow))
            return false;

        HRGN panelRegion = CreateRectRgn(0, 0, 0, 0);
        if (!panelRegion)
            return false;
        for (const PanelVisual& panel : panels)
        {
            // The CompositionRoundedRectangleGeometry below supplies the
            // antialiased clip. Limit this helper HWND only to each panel's
            // rectangular bounds so a binary GDI region cannot cut off the
            // partially covered pixels along the rounded edge.
            HRGN frameRegion = CreateRectRgn(
                panel.frame.left,
                panel.frame.top,
                panel.frame.right + 1,
                panel.frame.bottom + 1);
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

        HRGN currentRegion = CreateRectRgn(0, 0, 0, 0);
        const int currentRegionType = currentRegion
            ? GetWindowRgn(backdropWindow, currentRegion)
            : ERROR;
        const bool unchanged =
            currentRegionType != ERROR &&
            EqualRgn(panelRegion, currentRegion) != FALSE;
        if (currentRegion)
            DeleteObject(currentRegion);
        if (unchanged)
        {
            DeleteObject(panelRegion);
            animationPathRegionExpanded = false;
            return true;
        }

        // The helper HWND is a synchronous visibility fence for the separate
        // Windows Composition tree. Its region always matches the current
        // panel collection, so a retired SpriteVisual can never remain visible
        // after the application has removed that panel from the scene.
        if (!SetWindowRgn(backdropWindow, panelRegion, FALSE))
        {
            DeleteObject(panelRegion);
            return false;
        }
        animationPathRegionExpanded = false;
        return true;
    }

    void SetAnimationPathRegionExpanded(bool expanded)
    {
        if (!available || !backdropWindow ||
            !IsWindow(backdropWindow) ||
            animationPathRegionExpanded == expanded)
            return;
        if (expanded)
        {
            if (SetWindowRgn(backdropWindow, nullptr, FALSE))
                animationPathRegionExpanded = true;
            return;
        }
        SyncPanelWindowRegion();
    }

    void RequestCommit() noexcept
    {
        if (!available || !compositor)
            return;
        try
        {
            // Do not await this action on the UI thread. Requesting a cycle is
            // enough to submit the transactional visual changes; the helper
            // HWND region provides the synchronous visibility boundary.
            const wf::IAsyncAction pendingCommit =
                compositor.RequestCommitAsync();
            (void)pendingCommit;
        }
        catch (const winrt::hresult_error&)
        {
            // Older Windows builds can lack RequestCommitAsync. Their normal
            // implicit compositor cycle remains the fallback.
        }
    }

    bool RequestCommitAndNotify(
        HWND notifyWindow, UINT message,
        WPARAM token) noexcept
    {
        if (!available || !compositor ||
            !notifyWindow || !IsWindow(notifyWindow))
            return false;
        try
        {
            wf::IAsyncAction pendingCommit =
                compositor.RequestCommitAsync();
            pendingCommit.Completed(
                [notifyWindow, message, token](
                    const wf::IAsyncAction&,
                    wf::AsyncStatus status) noexcept {
                    if (IsWindow(notifyWindow))
                    {
                        PostMessageW(
                            notifyWindow, message, token,
                            static_cast<LPARAM>(status));
                    }
                });
            return true;
        }
        catch (const winrt::hresult_error&)
        {
            // Keep the normal implicit compositor cycle as the compatibility
            // fallback. The caller will retain the old target instead of
            // destroying it before an unavailable completion fence.
            return false;
        }
    }

    wuc::CompositionEffectFactory GetBlurFactory(int blurRadius)
    {
        const auto found = blurFactories.find(blurRadius);
        if (found != blurFactories.end())
            return found->second;

        auto blur = winrt::make_self<GaussianBlurEffect>();
        blur->effectSource = wuc::CompositionEffectSourceParameter(L"backdrop");
        blur->blurAmount = static_cast<float>(blurRadius);
        auto factory = compositor.CreateEffectFactory(*blur);
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
            genieSource.Opacity(genieSourceOpacity);
        genieRoot = nullptr;
        genieSource = nullptr;
        genieGeometry = nullptr;
        genieOutline.clear();
        geniePanelFrame = {};
        genieHostSize = {};
        genieOpacity = 1.0f;
        SetAnimationPathRegionExpanded(false);
    }

    void Reset()
    {
        available = false;
        genieOutline.clear();
        genieGeometry = nullptr;
        genieGeometryFactory = nullptr;
        genieRoot = nullptr;
        genieSource = nullptr;
        geniePanelFrame = {};
        genieHostSize = {};
        genieOpacity = 1.0f;
        panels.clear();
        blurFactories.clear();
        collectingFrame = false;
        blurFactoriesDirty = false;
        try
        {
            if (target)
                target.Root(nullptr);
        }
        catch (...)
        {
            // Destruction must continue even if the compositor target is
            // already faulted; otherwise its helper HWND can remain visible.
        }
        root = nullptr;
        target = nullptr;
        // The compositor and dispatcher queue are shared by every backdrop
        // target on this UI thread. Releasing one target must not invalidate
        // the desktop/floating counterpart during a hand-off.
        compositor = nullptr;
        dispatcherController = nullptr;
        if (backdropWindow && IsWindow(backdropWindow))
            DestroyWindow(backdropWindow);
        backdropWindow = nullptr;
        contentWindow = nullptr;
        popupMode = false;
        popupTopmost = false;
        visible = true;
        animationPathRegionExpanded = false;
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

bool DesktopBackdropCompositor::Initialize(HWND contentWindow)
{
    return InitializeInternal(
        contentWindow, false, false, true);
}

bool DesktopBackdropCompositor::InitializePopup(
    HWND contentWindow, bool topmost,
    bool initiallyVisible)
{
    return InitializeInternal(
        contentWindow, true, topmost,
        initiallyVisible);
}

void DesktopBackdropCompositor::SetPopupWindowPairZOrder(
    HWND contentWindow, HWND contentInsertAfter,
    bool topmost, HWND preserveAboveWindow)
{
    const bool contentValid =
        contentWindow && IsWindow(contentWindow);
    if (!contentValid)
        return;

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
        constexpr UINT contentFlags =
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
            SWP_NOOWNERZORDER;
        SetWindowPos(
            contentWindow, contentInsertAfter,
            0, 0, 0, 0, contentFlags);
        if (backdropWindow)
            impl_->SyncWindowPlacement();
    }
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
    Reset();
    impl_->lastError.clear();
    if (!contentWindow || !IsWindow(contentWindow))
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

    const DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT |
        (impl_->popupTopmost ? WS_EX_TOPMOST : 0);
    const DWORD windowStyle = popupMode
        ? (WS_POPUP |
            (initiallyVisible ? WS_VISIBLE : 0))
        : (WS_CHILD | WS_VISIBLE);
    impl_->backdropWindow = CreateWindowExW(
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
            impl_->Reset();
            return false;
        }
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.get_interop"), error.code());
        impl_->Reset();
        return false;
    }

    try
    {
        impl_->root = impl_->compositor.CreateContainerVisual();
        impl_->target.Root(impl_->root);
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
        impl_->Reset();
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
    const bool contentValid =
        contentWindow && IsWindow(contentWindow);
    if (!impl_)
    {
        if (contentValid)
            ShowWindow(contentWindow, SW_HIDE);
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

    const float clampedScale = std::clamp(scale, 0.01f, 1.0f);
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
    float normalizedStartSlope)
{
    if (!impl_ || !impl_->available || !impl_->root ||
        !impl_->compositor || durationMilliseconds == 0 ||
        !impl_->contentWindow || !IsWindow(impl_->contentWindow))
        return false;

    ClearGenieTransform();
    if (!impl_->available)
        return false;

    const float clampedFrom =
        std::clamp(fromScale, 0.01f, 1.0f);
    const float clampedTo =
        std::clamp(toScale, 0.01f, 1.0f);
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
            wfn::float3{ clampedFrom, clampedFrom, 1.0f });
        animation.InsertKeyFrame(
            1.0f,
            wfn::float3{ clampedTo, clampedTo, 1.0f },
            easing);
        auto opacityAnimation =
            impl_->compositor.CreateScalarKeyFrameAnimation();
        opacityAnimation.Duration(wf::TimeSpan{
            static_cast<std::int64_t>(
                durationMilliseconds) * 10'000LL });
        opacityAnimation.InsertKeyFrame(
            0.0f, clampedFromOpacity);
        opacityAnimation.InsertKeyFrame(
            1.0f, clampedToOpacity, easing);

        impl_->root.CenterPoint(wfn::float3{
            anchorX, anchorY, 0.0f });
        impl_->SetAnimationPathRegionExpanded(
            std::min(clampedFrom, clampedTo) < 0.9995f);
        // Keep final values as the base properties. Direct assignments also
        // disconnect prior animations before a rapid reversal.
        impl_->root.Scale(wfn::float3{
            clampedTo, clampedTo, 1.0f });
        impl_->root.Opacity(clampedToOpacity);
        impl_->root.StartAnimation(L"Scale", animation);
        impl_->root.StartAnimation(L"Opacity", opacityAnimation);
        impl_->RequestCommit();
        return true;
    }
    catch (...)
    {
        animationHr = winrt::to_hresult();
    }

    try
    {
        impl_->root.Scale(wfn::float3{
            clampedFrom, clampedFrom, 1.0f });
        impl_->root.Opacity(clampedFromOpacity);
        impl_->SetAnimationPathRegionExpanded(
            clampedFrom < 0.9995f);
        impl_->RequestCommit();
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
        if (panel == impl_->panels.end() || !panel->visual.Brush())
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
            glass.Brush(impl_->CreateBlurBrush(panel->blurRadius));
            auto geometry = impl_->compositor.CreatePathGeometry();
            glass.Clip(impl_->compositor.CreateGeometricClip(geometry));
            auto outline = BuildGenieSourceOutline(width, height,
                static_cast<float>(panel->cornerRadius), vertical);
            impl_->root.StopAnimation(L"Scale");
            impl_->root.StopAnimation(L"Opacity");
            impl_->root.Scale(wfn::float3{1.0f, 1.0f, 1.0f});
            impl_->root.CenterPoint(wfn::float3{});
            impl_->root.Opacity(1.0f);
            impl_->genieRoot = glass;
            impl_->genieSource = panel->visual;
            impl_->genieSourceOpacity = panel->visual.Opacity();
            impl_->genieGeometry = geometry;
            impl_->genieOutline = std::move(outline);
            impl_->geniePanelFrame = panelFrame;
            impl_->genieHostSize = {hostFrame.right, hostFrame.bottom};
            impl_->genieCornerRadius = panel->cornerRadius;
            impl_->genieVertical = vertical;
            impl_->root.Children().InsertAtTop(glass);
            panel->visual.Opacity(0.0f);
        }

        const genie::Rect source{static_cast<double>(panelFrame.left),
            static_cast<double>(panelFrame.top), static_cast<double>(panelFrame.right),
            static_cast<double>(panelFrame.bottom)};
        const genie::Rect target{static_cast<double>(dockFrame.left),
            static_cast<double>(dockFrame.top), static_cast<double>(dockFrame.right),
            static_cast<double>(dockFrame.bottom)};
        namespace navigation = snowdesktop::quick_navigation_animation_rules;
        std::array<navigation::GenieStripProjection, genie::StripCount> matrices;
        for (std::size_t index = 0; index < matrices.size(); ++index)
        {
            matrices[index] = navigation::GenieProjection(source, target, direction,
                std::clamp(collapsed, 0.0f, 1.0f), width, height, index);
        }
        const auto transformPoint = [&matrices](const GenieOutlinePoint& point) {
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
        impl_->lastError = FormatHresult(L"backdrop.genie_transform", winrt::to_hresult());
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
        // A failed restore must not strand hidden originals or animated glass.
        impl_->Reset();
    }
}

void DesktopBackdropCompositor::BeginFrame(bool completeCollection)
{
    if (!impl_->available)
        return;
    impl_->completeCollection = completeCollection;
    impl_->collectingFrame = true;
    if (completeCollection)
    {
        for (auto& panel : impl_->panels)
            panel.seen = false;
    }
    impl_->SyncWindowPlacement();
}

bool DesktopBackdropCompositor::AddPanel(
    const RECT& frame, float cornerRadius,
    float blurRadius, std::uintptr_t ownerKey)
{
    if (!impl_->available || frame.right <= frame.left || frame.bottom <= frame.top)
        return false;
    const int cornerKey = std::max(0, static_cast<int>(std::lround(cornerRadius)));
    const int blurKey = std::clamp(static_cast<int>(std::lround(blurRadius)), 0, 48);

    try
    {
        auto existing = std::find_if(impl_->panels.begin(), impl_->panels.end(),
            [&frame, ownerKey](const Impl::PanelVisual& panel) {
                return snowdesktop::desktop_backdrop_update_rules::
                    PanelIdentityMatches(
                        panel.ownerKey, panel.frame,
                        ownerKey, frame);
            });
        if (existing == impl_->panels.end())
        {
            Impl::PanelVisual panel{};
            panel.frame = frame;
            panel.ownerKey = ownerKey;
            panel.cornerRadius = cornerKey;
            panel.blurRadius = blurKey;
            panel.visual = impl_->compositor.CreateSpriteVisual();
            panel.geometry = impl_->compositor.CreateRoundedRectangleGeometry();
            panel.clip = impl_->compositor.CreateGeometricClip(panel.geometry);
            panel.visual.Clip(panel.clip);
            impl_->root.Children().InsertAtTop(panel.visual);
            impl_->panels.push_back(std::move(panel));
            existing = std::prev(impl_->panels.end());
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
        existing->frame = frame;

        if (existing->blurRadius != blurKey || !existing->visual.Brush())
        {
            impl_->blurFactoriesDirty = true;
            existing->blurRadius = blurKey;
            existing->visual.Brush(impl_->CreateBlurBrush(blurKey));
        }
        existing->cornerRadius = cornerKey;
        existing->visual.Offset(wfn::float3{
            static_cast<float>(frame.left), static_cast<float>(frame.top), 0.0f });
        const wfn::float2 panelSize{
            static_cast<float>(frame.right - frame.left),
            static_cast<float>(frame.bottom - frame.top) };
        existing->visual.Size(panelSize);
        existing->geometry.Size(panelSize);
        existing->geometry.CornerRadius(wfn::float2{
            static_cast<float>(cornerKey), static_cast<float>(cornerKey) });
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
            existing->visual.Opacity(0.0f);
        }
        else
            existing->visual.Opacity(1.0f);
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

bool DesktopBackdropCompositor::RemovePanel(const RECT& frame)
{
    if (!impl_->available || !impl_->root)
        return false;
    auto existing = std::find_if(impl_->panels.begin(), impl_->panels.end(),
        [&frame](const Impl::PanelVisual& panel) {
            return EqualRect(&panel.frame, &frame) != FALSE;
        });
    if (existing == impl_->panels.end())
        return false;

    try
    {
        if (impl_->genieSource == existing->visual)
            impl_->ClearGenieTransform();
        impl_->root.Children().Remove(existing->visual);
        existing->visual.Brush(nullptr);
        impl_->panels.erase(existing);
        impl_->blurFactoriesDirty = true;
        impl_->PruneUnusedBlurFactories();
        if (impl_->genieRoot)
            impl_->SetAnimationPathRegionExpanded(true);
        else
            impl_->SyncPanelWindowRegion();
        impl_->RequestCommit();
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.remove_panel"), error.code());
        return false;
    }
}

bool DesktopBackdropCompositor::KeepPanel(
    const RECT& frame)
{
    if (!impl_->available)
        return false;
    const auto existing = std::find_if(
        impl_->panels.begin(), impl_->panels.end(),
        [&frame](const Impl::PanelVisual& panel) {
            return EqualRect(&panel.frame, &frame) != FALSE;
        });
    if (existing == impl_->panels.end())
        return false;
    existing->seen = true;
    return true;
}

bool DesktopBackdropCompositor::SetPanelOpacity(
    const RECT& frame, float opacity)
{
    if (!impl_->available)
        return false;
    const auto existing = std::find_if(
        impl_->panels.begin(), impl_->panels.end(),
        [&frame](const Impl::PanelVisual& panel) {
            return EqualRect(&panel.frame, &frame) != FALSE;
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
            existing->visual.Opacity(clampedOpacity);
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(
            _LW("backdrop.update_panel"), error.code());
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

void DesktopBackdropCompositor::CommitVisualChanges()
{
    if (impl_)
        impl_->RequestCommit();
}

bool DesktopBackdropCompositor::
CommitVisualChangesAndNotify(
    HWND notifyWindow, UINT message, WPARAM token)
{
    return impl_ && impl_->RequestCommitAndNotify(
        notifyWindow, message, token);
}

void DesktopBackdropCompositor::EndFrame(
    bool requestCommit)
{
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
                iterator->visual.Brush(nullptr);
                iterator = impl_->panels.erase(iterator);
                impl_->blurFactoriesDirty = true;
            }
        }
        impl_->PruneUnusedBlurFactories();
        if (impl_->genieRoot)
            impl_->SetAnimationPathRegionExpanded(true);
        else
            impl_->SyncPanelWindowRegion();
        if (requestCommit)
            impl_->RequestCommit();
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(_LW("backdrop.remove_panel"), error.code());
    }
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
