// The visual-tree matching and Gaussian composition brush in this file are
// adapted from TranslucentTB's GPLv3 ExplorerTAP implementation. See
// third_party/translucenttb-NOTICE.md.

#include "taskbar_hook_protocol.h"

#include <windows.h>
#include <commctrl.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <ocidl.h>
#include <windows.graphics.effects.interop.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <xamlOM.h>

#pragma push_macro("GetCurrentTime")
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#pragma pop_macro("GetCurrentTime")

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wf = winrt::Windows::Foundation;
namespace wfn = winrt::Windows::Foundation::Numerics;
namespace wge = winrt::Windows::Graphics::Effects;
namespace wuc = winrt::Windows::UI::Composition;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxh = winrt::Windows::UI::Xaml::Hosting;
namespace awge = ABI::Windows::Graphics::Effects;

namespace
{
using namespace snowdesktop::taskbar_hook;

constexpr CLSID kTapSiteClsid = {
    0x91dbd39f, 0x32da, 0x40a1,
    { 0xb5, 0x47, 0x81, 0x6b, 0xf9, 0xcf, 0x78, 0xa4 }
};
constexpr UINT_PTR kTaskbarSubclassId = 0x53445442;

HMODULE g_module = nullptr;
HANDLE g_mapping = nullptr;
SharedState* g_sharedState = nullptr;
UINT g_applyMessage = 0;
std::atomic<DWORD> g_watchedOwnerProcessId{0};
std::atomic_bool g_forceRestore{false};

bool OpenSharedState()
{
    if (g_sharedState)
        return true;
    g_mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kSharedStateName);
    if (!g_mapping)
        return false;
    g_sharedState = static_cast<SharedState*>(MapViewOfFile(g_mapping,
        FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!g_sharedState)
    {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }
    return g_sharedState->magic == kSharedStateMagic &&
        g_sharedState->version == kSharedStateVersion &&
        g_sharedState->size == sizeof(SharedState);
}

void SetHookStatus(LONG status, DWORD error = ERROR_SUCCESS)
{
    if (!g_sharedState)
        return;
    InterlockedExchange(&g_sharedState->lastError, static_cast<LONG>(error));
    InterlockedExchange(&g_sharedState->status, status);
}

void SignalReady()
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, kReadyEventName);
    if (event)
    {
        SetEvent(event);
        CloseHandle(event);
    }
}

bool ReadSnapshot(Snapshot& snapshot)
{
    if (!g_sharedState && !OpenSharedState())
        return false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const LONG generation = g_sharedState->generation;
        MemoryBarrier();
        snapshot.generation = generation;
        snapshot.enabled = g_sharedState->enabled != FALSE;
        snapshot.style = g_sharedState->style;
        snapshot.contentTheme = g_sharedState->contentTheme;
        snapshot.systemUsesLightTheme = g_sharedState->systemUsesLightTheme;
        snapshot.ownerProcessId = g_sharedState->ownerProcessId;
        snapshot.red = g_sharedState->red;
        snapshot.green = g_sharedState->green;
        snapshot.blue = g_sharedState->blue;
        snapshot.alpha = g_sharedState->alpha;
        snapshot.blurAmount = g_sharedState->blurAmount;
        snapshot.borderRed = g_sharedState->borderRed;
        snapshot.borderGreen = g_sharedState->borderGreen;
        snapshot.borderBlue = g_sharedState->borderBlue;
        snapshot.borderAlpha = g_sharedState->borderAlpha;
        MemoryBarrier();
        if (generation == g_sharedState->generation)
            return true;
    }
    return false;
}

BOOL CALLBACK PostApplyToTaskbar(HWND window, LPARAM)
{
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"Shell_TrayWnd") != 0 &&
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") != 0)
        return TRUE;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == GetCurrentProcessId())
        PostMessageW(window, g_applyMessage, 0, 0);
    return TRUE;
}

void BroadcastApply()
{
    if (!g_applyMessage)
        g_applyMessage = RegisterWindowMessageW(kApplyMessageName);
    EnumWindows(PostApplyToTaskbar, 0);
}

bool IsProcessAlive(DWORD processId)
{
    if (!processId)
        return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process)
        return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

void WatchOwnerProcess(DWORD processId)
{
    if (!processId)
        return;
    const DWORD previous = g_watchedOwnerProcessId.exchange(processId);
    g_forceRestore = false;
    if (previous == processId)
        return;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process)
    {
        g_forceRestore = true;
        return;
    }
    std::thread([processId, process] {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        if (g_watchedOwnerProcessId.load() == processId)
        {
            g_forceRestore = true;
            BroadcastApply();
        }
    }).detach();
}

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
            *value = wf::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_SOFT)
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
    winrt::hstring effectName = L"SnowDesktopGaussianBlur";
};

struct FloodEffect : winrt::implements<FloodEffect, wge::IGraphicsEffect,
    wge::IGraphicsEffectSource, awge::IGraphicsEffectD2D1Interop>
{
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override
    {
        if (!id) return E_INVALIDARG;
        *id = CLSID_D2D1Flood;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index,
        awge::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override
    {
        if (!name || !index || !mapping) return E_INVALIDARG;
        if (std::wstring_view(name) == L"Color")
        {
            *index = D2D1_FLOOD_PROP_COLOR;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        return E_INVALIDARG;
    }

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
        if (index != D2D1_FLOOD_PROP_COLOR) return E_BOUNDS;
        *value = wf::PropertyValue::CreateSingleArray(
            { color.x, color.y, color.z, color.w })
            .as<ABI::Windows::Foundation::IPropertyValue>().detach();
        return S_OK;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSource(UINT,
        awge::IGraphicsEffectSource** source) noexcept override
    {
        return source ? E_BOUNDS : E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = 0;
        return S_OK;
    }

    winrt::hstring Name() const { return effectName; }
    void Name(const winrt::hstring& value) { effectName = value; }

    wfn::float4 color{0.0f, 0.0f, 0.0f, 1.0f};
    winrt::hstring effectName = L"SnowDesktopTint";
};

struct CompositeEffect : winrt::implements<CompositeEffect,
    wge::IGraphicsEffect, wge::IGraphicsEffectSource,
    awge::IGraphicsEffectD2D1Interop>
{
    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override
    {
        if (!id) return E_INVALIDARG;
        *id = CLSID_D2D1Composite;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index,
        awge::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override
    {
        if (!name || !index || !mapping) return E_INVALIDARG;
        if (std::wstring_view(name) == L"Mode")
        {
            *index = D2D1_COMPOSITE_PROP_MODE;
            *mapping = awge::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }
        return E_INVALIDARG;
    }

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
        *value = wf::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER)
            .as<ABI::Windows::Foundation::IPropertyValue>().detach();
        return S_OK;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSource(UINT index,
        awge::IGraphicsEffectSource** source) noexcept override try
    {
        if (!source) return E_INVALIDARG;
        winrt::copy_to_abi(sources.at(index), *reinterpret_cast<void**>(source));
        return S_OK;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = static_cast<UINT>(sources.size());
        return S_OK;
    }

    winrt::hstring Name() const { return effectName; }
    void Name(const winrt::hstring& value) { effectName = value; }

    std::vector<wge::IGraphicsEffectSource> sources;
    winrt::hstring effectName = L"SnowDesktopBlurAndTint";
};

class XamlBlurBrush : public wux::Media::XamlCompositionBrushBaseT<XamlBlurBrush>
{
public:
    XamlBlurBrush(wuc::Compositor compositor, float blurAmount, wfn::float4 tint)
        : compositor_(std::move(compositor)), blurAmount_(blurAmount), tint_(tint)
    {
    }

    void OnConnected()
    {
        if (CompositionBrush())
            return;
        auto backdropBrush = compositor_.CreateBackdropBrush();
        auto blur = winrt::make_self<GaussianBlurEffect>();
        blur->effectSource = wuc::CompositionEffectSourceParameter(L"backdrop");
        blur->blurAmount = blurAmount_;
        auto tint = winrt::make_self<FloodEffect>();
        tint->color = tint_;
        auto composite = winrt::make_self<CompositeEffect>();
        composite->sources.push_back(*blur);
        composite->sources.push_back(*tint);
        auto brush = compositor_.CreateEffectFactory(*composite).CreateBrush();
        brush.SetSourceParameter(L"backdrop", backdropBrush);
        CompositionBrush(brush);
    }

    void OnDisconnected()
    {
        if (auto brush = CompositionBrush())
        {
            brush.Close();
            CompositionBrush(nullptr);
        }
    }

private:
    wuc::Compositor compositor_{nullptr};
    float blurAmount_ = 24.0f;
    wfn::float4 tint_{};
};

class VisualTreeWatcher;
winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;

class VisualTreeWatcher : public winrt::implements<VisualTreeWatcher,
    IVisualTreeServiceCallback2, winrt::non_agile>
{
public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : diagnostics_(site.as<IXamlDiagnostics>())
    {
        if (g_sharedState)
            InterlockedExchange(&g_sharedState->diagnosticStage, 230);
        g_applyMessage = RegisterWindowMessageW(kApplyMessageName);
        std::thread([self = get_strong()] {
            const HRESULT result = self->diagnostics_.as<IVisualTreeService3>()
                ->AdviseVisualTreeChange(self.get());
            if (g_sharedState)
                InterlockedExchange(&g_sharedState->diagnosticStage, 240);
            if (SUCCEEDED(result))
            {
                if (g_sharedState &&
                    g_sharedState->status < kStatusConnected)
                    InterlockedExchange(&g_sharedState->status, kStatusConnected);
            }
            else
                SetHookStatus(kStatusFailed, static_cast<DWORD>(result));
            SignalReady();
        }).detach();
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation,
        VisualElement element, VisualMutationType mutationType) override try
    {
        if (mutationType == Add)
        {
            const std::wstring_view type = element.Type
                ? std::wstring_view(element.Type, SysStringLen(element.Type))
                : std::wstring_view{};
            if (type == winrt::name_of<wuxh::DesktopWindowXamlSource>())
            {
                xamlSources_.insert(element.Handle);
            }
            else if (type == L"Taskbar.TaskbarFrame")
            {
                RegisterTaskbarFrame(relation.Parent, element.Handle);
            }
            else if (type == winrt::name_of<wux::Shapes::Rectangle>())
            {
                const std::wstring_view name = element.Name
                    ? std::wstring_view(element.Name, SysStringLen(element.Name))
                    : std::wstring_view{};
                if (name == L"BackgroundFill")
                    RegisterTaskbarControl(relation.Parent, element.Handle, false);
                else if (name == L"BackgroundStroke")
                    RegisterTaskbarControl(relation.Parent, element.Handle, true);
            }
        }
        else if (mutationType == Remove)
        {
            UnregisterTaskbar(element.Handle);
            xamlSources_.erase(element.Handle);
        }
        return S_OK;
    }
    catch (...)
    {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
        VisualElementState, LPCWSTR) noexcept override
    {
        return S_OK;
    }

    void ApplyTaskbar(HWND taskbar)
    {
        Snapshot snapshot;
        if (!ReadSnapshot(snapshot))
        {
            // Shared memory is gone (process exited/crashed). Clear any
            // applied content theme and backdrop on all registered taskbars.
            for (auto& [handle, info] : taskbars_)
            {
                (void)handle;
                if (info.rootElement && info.appliedContentTheme >= 0)
                {
                    try
                    {
                        info.rootElement.ClearValue(
                            wux::FrameworkElement::RequestedThemeProperty());
                        info.appliedContentTheme = -1;
                    }
                    catch (...) {}
                }
                RestoreControl(info.background);
                RestoreControl(info.border);
            }
            return;
        }
        const bool ownerAlive = IsProcessAlive(snapshot.ownerProcessId);
        if (snapshot.enabled && ownerAlive)
            WatchOwnerProcess(snapshot.ownerProcessId);
        const bool enabled = snapshot.enabled && ownerAlive && !g_forceRestore.load();

        bool applied = false;
        for (auto& [handle, info] : taskbars_)
        {
            (void)handle;
            if (info.taskbar != taskbar)
                continue;

            // Apply content theme (light/dark text and icons) independently
            // of the backdrop. contentTheme: 0=深(白字)→Dark, 1=浅(黑字)→Light,
            // when disabled, fall back to the system theme stored in the snapshot.
            const int effectiveContentTheme = enabled
                ? snapshot.contentTheme
                : snapshot.systemUsesLightTheme;
            if (info.rootElement &&
                info.appliedContentTheme != effectiveContentTheme)
            {
                try
                {
                    info.rootElement.RequestedTheme(
                        effectiveContentTheme != 0
                            ? wux::ElementTheme::Light
                            : wux::ElementTheme::Dark);
                    info.rootElement.InvalidateArrange();
                    info.appliedContentTheme = effectiveContentTheme;
                }
                catch (...)
                {
                }
            }

            if (!info.background.control || !info.background.originalFill)
                continue;
            if (info.appliedGeneration == snapshot.generation &&
                info.appliedEnabled == enabled)
                continue;

            if (!enabled)
            {
                RestoreControl(info.background);
                RestoreControl(info.border);
            }
            else
            {
                ApplyBackdrop(info.background, snapshot);
                ApplySolidFill(info.border, snapshot.borderRed,
                    snapshot.borderGreen, snapshot.borderBlue,
                    snapshot.borderAlpha);
            }
            info.appliedGeneration = snapshot.generation;
            info.appliedEnabled = enabled;
            applied = applied || enabled;
        }
        if (applied)
            SetHookStatus(kStatusApplied);
    }

private:
    struct ControlInfo
    {
        wux::Shapes::Shape control{nullptr};
        wux::Media::Brush originalFill{nullptr};
        wux::Media::Brush appliedFill{nullptr};
        LONG appliedStyle = -1;
        float appliedRed = -1.0f;
        float appliedGreen = -1.0f;
        float appliedBlue = -1.0f;
        float appliedAlpha = -1.0f;
        float appliedBlur = -1.0f;
    };

    struct TaskbarInfo
    {
        ControlInfo background;
        ControlInfo border;
        wux::FrameworkElement rootElement{nullptr};
        HWND xamlWindow = nullptr;
        HWND taskbar = nullptr;
        LONG appliedGeneration = -1;
        LONG appliedContentTheme = -1;
        bool appliedEnabled = false;
    };

    static void ApplyBackdrop(ControlInfo& control, const Snapshot& snapshot)
    {
        if (!control.control || !control.originalFill)
            return;
        const auto currentFill = control.control.Fill();
        if (currentFill && (!control.appliedFill ||
                currentFill != control.appliedFill))
        {
            // Windows replaces its native brushes when the system theme changes.
            // Keep the latest native brush so disabling our material restores the
            // current theme rather than the one active when the hook was loaded.
            control.originalFill = currentFill;
        }
        const LONG backdropStyle = snapshot.style &
            (kStyleGlassBackdrop | kStyleAcrylicBackdrop);
        const bool parametersMatch = control.appliedStyle == backdropStyle &&
            control.appliedRed == snapshot.red &&
            control.appliedGreen == snapshot.green &&
            control.appliedBlue == snapshot.blue &&
            control.appliedAlpha == snapshot.alpha &&
            control.appliedBlur == snapshot.blurAmount;
        if (parametersMatch && control.appliedFill &&
            currentFill == control.appliedFill)
            return;

        wux::Media::Brush brush{nullptr};
        const winrt::Windows::UI::Color tint{
            static_cast<std::uint8_t>(std::clamp(snapshot.alpha, 0.0f, 1.0f) * 255.0f),
            static_cast<std::uint8_t>(std::clamp(snapshot.red, 0.0f, 1.0f) * 255.0f),
            static_cast<std::uint8_t>(std::clamp(snapshot.green, 0.0f, 1.0f) * 255.0f),
            static_cast<std::uint8_t>(std::clamp(snapshot.blue, 0.0f, 1.0f) * 255.0f)
        };
        if ((backdropStyle & kStyleAcrylicBackdrop) != 0)
        {
            // AcrylicBrush(BackgroundSource=HostBackdrop) 在 Explorer
            // XAML 树内无法正确采样桌面背景，实际会回退为纯色。统一走
            // XamlBlurBrush 提供模糊，噪点由 SnowDesktop 主进程在桌面
            // DComp 层统一叠加。
        }
        if (!brush && (backdropStyle & kStyleGlassBackdrop) != 0)
        {
            auto compositor = wuxh::ElementCompositionPreview::
                GetElementVisual(control.control).Compositor();
            brush = winrt::make<XamlBlurBrush>(std::move(compositor),
                std::clamp(snapshot.blurAmount, 0.0f, 48.0f),
                wfn::float4{
                    tint.R / 255.0f, tint.G / 255.0f,
                    tint.B / 255.0f, tint.A / 255.0f
                });
        }
        if (!brush)
        {
            wux::Media::SolidColorBrush solid;
            solid.Color(tint);
            brush = std::move(solid);
        }
        control.control.Fill(brush);
        control.appliedFill = brush;
        control.appliedStyle = backdropStyle;
        control.appliedRed = snapshot.red;
        control.appliedGreen = snapshot.green;
        control.appliedBlue = snapshot.blue;
        control.appliedAlpha = snapshot.alpha;
        control.appliedBlur = snapshot.blurAmount;
    }

    static void ApplySolidFill(ControlInfo& control, float red, float green,
        float blue, float alpha)
    {
        if (!control.control || !control.originalFill)
            return;
        const auto currentFill = control.control.Fill();
        if (currentFill && (!control.appliedFill ||
                currentFill != control.appliedFill))
        {
            control.originalFill = currentFill;
        }
        const bool parametersMatch = control.appliedStyle == 0 &&
            control.appliedRed == red && control.appliedGreen == green &&
            control.appliedBlue == blue && control.appliedAlpha == alpha &&
            control.appliedBlur == 0.0f;
        if (parametersMatch && control.appliedFill &&
            currentFill == control.appliedFill)
            return;

        wux::Media::SolidColorBrush brush;
        brush.Color(winrt::Windows::UI::Color{
            static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f),
            static_cast<std::uint8_t>(std::clamp(red, 0.0f, 1.0f) * 255.0f),
            static_cast<std::uint8_t>(std::clamp(green, 0.0f, 1.0f) * 255.0f),
            static_cast<std::uint8_t>(std::clamp(blue, 0.0f, 1.0f) * 255.0f)
        });
        control.control.Fill(brush);
        control.appliedFill = brush;
        control.appliedStyle = 0;
        control.appliedRed = red;
        control.appliedGreen = green;
        control.appliedBlue = blue;
        control.appliedAlpha = alpha;
        control.appliedBlur = 0.0f;
    }

    static void RestoreControl(ControlInfo& control)
    {
        if (control.control && control.originalFill)
            control.control.Fill(control.originalFill);
        control.appliedFill = nullptr;
        control.appliedStyle = -1;
    }

    template<typename T>
    T FromHandle(InstanceHandle handle)
    {
        wf::IInspectable inspectable{nullptr};
        winrt::check_hresult(diagnostics_->GetIInspectableFromHandle(handle,
            reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))));
        return inspectable.as<T>();
    }

    wux::FrameworkElement FindParent(std::wstring_view name,
        wux::FrameworkElement element)
    {
        while (element)
        {
            if (element.Name() == name)
                return element;
            auto parent = wux::Media::VisualTreeHelper::GetParent(element)
                .try_as<wux::FrameworkElement>();
            if (!parent)
                return nullptr;
            element = std::move(parent);
        }
        return nullptr;
    }

    void RegisterTaskbarFrame(InstanceHandle rootGridHandle,
        InstanceHandle frameHandle)
    {
        const auto rootGrid = FromHandle<wux::UIElement>(rootGridHandle);
        for (auto iterator = xamlSources_.begin(); iterator != xamlSources_.end();
            ++iterator)
        {
            const auto source = FromHandle<wuxh::DesktopWindowXamlSource>(*iterator);
            wux::UIElement content{nullptr};
            try
            {
                content = source.Content();
            }
            catch (const winrt::hresult_wrong_thread&)
            {
                continue;
            }
            if (content != rootGrid)
                continue;

            HWND xamlWindow = nullptr;
            winrt::check_hresult(source.as<IDesktopWindowXamlSourceNative>()
                ->get_WindowHandle(&xamlWindow));
            HWND taskbar = GetAncestor(xamlWindow, GA_PARENT);
            auto& info = taskbars_[frameHandle];
            info.xamlWindow = xamlWindow;
            info.taskbar = taskbar;
            info.rootElement = rootGrid.try_as<wux::FrameworkElement>();
            info.appliedContentTheme = -1;
            if (taskbar && subclassedTaskbars_.insert(taskbar).second)
                SetWindowSubclass(taskbar, TaskbarSubclassProc,
                    kTaskbarSubclassId, reinterpret_cast<DWORD_PTR>(this));
            xamlSources_.erase(iterator);
            break;
        }
    }

    void RegisterTaskbarControl(InstanceHandle parentHandle,
        InstanceHandle controlHandle, bool border)
    {
        const auto parent = FromHandle<wux::FrameworkElement>(parentHandle);
        const auto frame = FindParent(L"TaskbarFrame", parent);
        if (!frame)
            return;
        InstanceHandle frameHandle = 0;
        winrt::check_hresult(diagnostics_->GetHandleFromIInspectable(
            reinterpret_cast<::IInspectable*>(winrt::get_abi(frame)),
            &frameHandle));
        auto iterator = taskbars_.find(frameHandle);
        if (iterator == taskbars_.end())
            return;
        auto shape = FromHandle<wux::Shapes::Shape>(controlHandle);
        ControlInfo& control = border
            ? iterator->second.border : iterator->second.background;
        control.control = shape;
        control.originalFill = shape.Fill();
        iterator->second.appliedGeneration = -1;
        ApplyTaskbar(iterator->second.taskbar);
    }

    void UnregisterTaskbar(InstanceHandle handle)
    {
        const auto iterator = taskbars_.find(handle);
        if (iterator == taskbars_.end())
            return;
        const HWND taskbar = iterator->second.taskbar;
        taskbars_.erase(iterator);
        bool stillUsed = false;
        for (const auto& [otherHandle, info] : taskbars_)
        {
            (void)otherHandle;
            if (info.taskbar == taskbar)
            {
                stillUsed = true;
                break;
            }
        }
        if (!stillUsed && taskbar)
        {
            RemoveWindowSubclass(taskbar, TaskbarSubclassProc,
                kTaskbarSubclassId);
            subclassedTaskbars_.erase(taskbar);
        }
    }

    void OnTaskbarDestroyed(HWND taskbar)
    {
        subclassedTaskbars_.erase(taskbar);
    }

    static LRESULT CALLBACK TaskbarSubclassProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR reference)
    {
        auto* self = reinterpret_cast<VisualTreeWatcher*>(reference);
        if (self && message == g_applyMessage)
        {
            try
            {
                self->ApplyTaskbar(window);
            }
            catch (...)
            {
                SetHookStatus(kStatusFailed,
                    static_cast<DWORD>(winrt::to_hresult()));
            }
            return 0;
        }
        if (self && message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(window, TaskbarSubclassProc,
                kTaskbarSubclassId);
            self->OnTaskbarDestroyed(window);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    winrt::com_ptr<IXamlDiagnostics> diagnostics_;
    std::unordered_set<InstanceHandle> xamlSources_;
    std::unordered_map<InstanceHandle, TaskbarInfo> taskbars_;
    std::unordered_set<HWND> subclassedTaskbars_;
};

class TapSite : public winrt::implements<TapSite, IObjectWithSite,
    winrt::non_agile>
{
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override try
    {
        if (g_sharedState)
            InterlockedExchange(&g_sharedState->diagnosticStage, 220);
        site_.copy_from(site);
        if (site_)
            g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(site_);
        return S_OK;
    }
    catch (...)
    {
        SetHookStatus(kStatusFailed, static_cast<DWORD>(winrt::to_hresult()));
        SignalReady();
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** object) noexcept override
    {
        return site_.as(iid, object);
    }

private:
    winrt::com_ptr<IUnknown> site_;
};

template<typename T>
class SimpleFactory : public winrt::implements<SimpleFactory<T>, IClassFactory,
    winrt::non_agile>
{
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid,
        void** object) override try
    {
        if (g_sharedState)
            InterlockedExchange(&g_sharedState->diagnosticStage, 210);
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!object) return E_POINTER;
        *object = nullptr;
        return winrt::make<T>().as(iid, object);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override
    {
        return S_OK;
    }
};

using InitializeXamlDiagnosticsExProc = decltype(&InitializeXamlDiagnosticsEx);

DWORD WINAPI InstallTaskbarTap(void*)
{
    HMODULE selfReference = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&InstallTaskbarTap), &selfReference);
    const auto finish = [selfReference](DWORD result, bool keepLoaded) -> DWORD {
        if (selfReference && !keepLoaded)
            FreeLibraryAndExitThread(selfReference, result);
        return result;
    };

    if (!OpenSharedState())
    {
        SignalReady();
        return finish(ERROR_FILE_NOT_FOUND, false);
    }
    g_sharedState->explorerProcessId = GetCurrentProcessId();
    SetHookStatus(kStatusInjecting);

    // XAML Diagnostics can only own a taskbar visual tree once. Refuse to
    // compete with an already loaded TranslucentTB TAP; the settings UI tells
    // the user not to run both tools at the same time.
    if (GetModuleHandleW(L"ExplorerTAP.dll"))
    {
        SetHookStatus(kStatusFailed, ERROR_ALREADY_EXISTS);
        SignalReady();
        return finish(ERROR_ALREADY_EXISTS, false);
    }

    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsExProc>(
        GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
    if (!initialize)
    {
        const DWORD error = GetLastError();
        SetHookStatus(kStatusFailed, error);
        SignalReady();
        return finish(error, false);
    }

    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(g_module, modulePath,
        static_cast<DWORD>(std::size(modulePath)));
    if (!length || length >= std::size(modulePath))
    {
        const DWORD error = GetLastError();
        SetHookStatus(kStatusFailed, error);
        SignalReady();
        return finish(error, false);
    }

    HRESULT result = E_FAIL;
    for (unsigned attempt = 1; attempt <= 60 && FAILED(result); ++attempt)
    {
        wchar_t connection[64]{};
        swprintf_s(connection, L"VisualDiagConnection%u", attempt);
        InterlockedExchange(&g_sharedState->diagnosticStage,
            static_cast<LONG>(100 + attempt));
        std::thread([&, connectionName = std::wstring(connection)] {
            result = initialize(connectionName.c_str(), GetCurrentProcessId(),
                nullptr, modulePath, kTapSiteClsid, nullptr);
        }).join();
        if (FAILED(result))
            Sleep(500);
    }
    if (FAILED(result))
    {
        SetHookStatus(kStatusFailed, static_cast<DWORD>(result));
        SignalReady();
        return finish(static_cast<DWORD>(result), false);
    }
    // InitializeXamlDiagnosticsEx pins the TAP module. Keep our explicit
    // reference too so a hook timeout can never unmap code still used by the
    // visual-tree callback or its owner-process watcher.
    return finish(static_cast<DWORD>(result), true);
}

bool IsExplorerProcess()
{
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path))))
        return false;
    const wchar_t* fileName = path;
    for (const wchar_t* cursor = path; *cursor; ++cursor)
    {
        if (*cursor == L'\\' || *cursor == L'/')
            fileName = cursor + 1;
    }
    return _wcsicmp(fileName, L"explorer.exe") == 0;
}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK
SnowDesktopTaskbarHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

_Use_decl_annotations_ STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid,
    LPVOID* object) try
{
    if (g_sharedState)
        InterlockedExchange(&g_sharedState->diagnosticStage, 200);
    if (clsid != kTapSiteClsid)
        return CLASS_E_CLASSNOTAVAILABLE;
    if (!object)
        return E_POINTER;
    *object = nullptr;
    return winrt::make<SimpleFactory<TapSite>>().as(iid, object);
}
catch (...)
{
    return winrt::to_hresult();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        if (IsExplorerProcess())
        {
            HANDLE thread = CreateThread(nullptr, 0, InstallTaskbarTap,
                nullptr, 0, nullptr);
            if (!thread)
                return FALSE;
            CloseHandle(thread);
        }
    }
    return TRUE;
}
