#include <ole2.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>
#include "native_tooltip.h"
#include "native_tooltip_content.h"
#include "app/desktop_backdrop_compositor.h"
#include "diagnostic_log.h"
#include <dcomp.h>
#include <d2d1_1.h>
#include <shellscalingapi.h>
#include <atomic>
#include <mutex>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT_PTR kHoverTimer = 1;
constexpr unsigned kHoverDelay = 400;
bool HighContrast()
{
    HIGHCONTRASTW value{sizeof(value)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
        (value.dwFlags & HCF_HIGHCONTRASTON);
}
D2D1_COLOR_F SystemColor(int index)
{
    const auto color = GetSysColor(index);
    return D2D1::ColorF(GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f);
}
struct AccessibleState
{
    std::mutex mutex;
    HWND window = nullptr;
    std::wstring name;
    bool visible = false;
};
// UIA clients may retain this provider after its popup closes. Its snapshot
// never owns or calls back into the host, including from a UIA worker thread.
class TooltipProvider final : public IRawElementProviderSimple
{
public:
    explicit TooltipProvider(std::shared_ptr<AccessibleState> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IRawElementProviderSimple)) return E_NOINTERFACE;
        *value = static_cast<IRawElementProviderSimple*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* value) override
    {
        if (!value) return E_POINTER;
        *value = ProviderOptions_ServerSideProvider; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** value) override
    { if (!value) return E_POINTER; *value = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* value) override
    {
        if (!value) return E_POINTER;
        VariantInit(value);
        std::lock_guard guard(state_->mutex);
        if (!state_->window) return UIA_E_ELEMENTNOTAVAILABLE;
        if (property == UIA_ControlTypePropertyId)
        { value->vt = VT_I4; value->lVal = UIA_ToolTipControlTypeId; }
        else if (property == UIA_NamePropertyId)
        {
            value->vt = VT_BSTR; value->bstrVal = SysAllocString(state_->name.c_str());
            if (!value->bstrVal) return E_OUTOFMEMORY;
        }
        else if (property == UIA_IsKeyboardFocusablePropertyId || property == UIA_HasKeyboardFocusPropertyId ||
            property == UIA_IsOffscreenPropertyId || property == UIA_IsControlElementPropertyId ||
            property == UIA_IsContentElementPropertyId || property == UIA_IsEnabledPropertyId)
        {
            value->vt = VT_BOOL;
            const bool enabled = property == UIA_IsControlElementPropertyId ||
                property == UIA_IsContentElementPropertyId || property == UIA_IsEnabledPropertyId ||
                (property == UIA_IsOffscreenPropertyId && !state_->visible);
            value->boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** value) override
    {
        if (!value) return E_POINTER;
        HWND window = nullptr;
        { std::lock_guard guard(state_->mutex); window = state_->window; }
        *value = nullptr;
        return window ? UiaHostProviderFromHwnd(window, value) : UIA_E_ELEMENTNOTAVAILABLE;
    }
private:
    std::atomic<ULONG> references_{1};
    std::shared_ptr<AccessibleState> state_;
};
}

struct NativeTooltip::Impl
{
    HWND owner = nullptr, window = nullptr;
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDWriteFactory> text;
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionSurface> surface;
    DesktopBackdropCompositor backdrop;
    PersonalizationSettings appearance;
    DrawBackground drawBackground;
    NativeTooltipState state;
    NativeTooltipTextLayout measured;
    RECT anchor{}, bounds{};
    NativeTooltipPlacement placement = NativeTooltipPlacement::Below;
    UINT dpi = 96;
    UINT width = 0, height = 0;
    float measuredWidth = 0, measuredHeight = 0;
    bool contentDirty = true, paintDirty = true, highContrast = false, visible = false;
    HRESULT lastError = S_OK;
    std::shared_ptr<AccessibleState> accessible = std::make_shared<AccessibleState>();
    ComPtr<IRawElementProviderSimple> provider;

    bool EnsureWindow()
    {
        if (window) return true;
        if (!owner || !composition || !text) return false;
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = Procedure;
        cls.lpszClassName = L"SnowDesktop.NativeTooltip";
        RegisterClassExW(&cls);
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST,
            cls.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, cls.hInstance, this);
        if (!window) return false;
        provider.Attach(new TooltipProvider(accessible));
        { std::lock_guard guard(accessible->mutex); accessible->window = window; }
        return true;
    }
    void ReleaseGraphics()
    {
        surface.Reset(); visual.Reset(); target.Reset();
        width = height = 0; paintDirty = true;
        backdrop.Reset();
    }
    void HideWindow()
    {
        if (window) KillTimer(window, kHoverTimer);
        if (visible)
        {
            backdrop.HidePopupWindowPair(window);
            visible = false;
            { std::lock_guard guard(accessible->mutex); accessible->visible = false; }
            if (provider && UiaClientsAreListening()) UiaRaiseAutomationEvent(provider.Get(), UIA_ToolTipClosedEventId);
        }
        // No hidden popup keeps a glass composition target alive.
        backdrop.Reset();
    }
    void Hide() { HideWindow(); state.Leave(); }
    void Close()
    {
        Hide();
        if (window)
        {
            UiaReturnRawElementProvider(window, 0, 0, nullptr);
            { std::lock_guard guard(accessible->mutex); accessible->window = nullptr; }
            DestroyWindow(window); window = nullptr;
        }
        provider.Reset(); ReleaseGraphics(); measured = {}; format.Reset();
        composition.Reset(); text.Reset(); owner = nullptr;
    }
    void Error(HRESULT result)
    {
        if (lastError != result)
        {
            wchar_t message[160]{};
            swprintf_s(message, L"NativeTooltip drawing failed HRESULT=0x%08X", static_cast<unsigned>(result));
            WriteDiagnosticLogEntry(message); lastError = result;
        }
        Hide(); ReleaseGraphics();
    }
    void PublishName()
    {
        const auto name = state.title.empty() ? state.text : state.title + L"\n" + state.text;
        std::wstring previous;
        { std::lock_guard guard(accessible->mutex); previous = std::exchange(accessible->name, name); }
        if (previous == name) return;
        SetWindowTextW(window, name.c_str());
        if (visible && provider && UiaClientsAreListening())
        {
            VARIANT oldValue{}, newValue{};
            oldValue.vt = newValue.vt = VT_BSTR;
            oldValue.bstrVal = SysAllocString(previous.c_str()); newValue.bstrVal = SysAllocString(name.c_str());
            UiaRaiseAutomationPropertyChangedEvent(provider.Get(), UIA_NamePropertyId, oldValue, newValue);
            VariantClear(&oldValue); VariantClear(&newValue);
        }
    }
    HRESULT Paint()
    {
        const UINT w = static_cast<UINT>(bounds.right - bounds.left), h = static_cast<UINT>(bounds.bottom - bounds.top);
        HRESULT result = S_OK;
        if (!target && FAILED(result = composition->CreateTargetForHwnd(window, FALSE, &target))) return result;
        if (!visual)
        {
            if (FAILED(result = composition->CreateVisual(&visual))) return result;
            if (FAILED(result = target->SetRoot(visual.Get()))) return result;
        }
        if (!surface || width != w || height != h)
        {
            surface.Reset();
            if (FAILED(result = composition->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED, &surface))) return result;
            width = w; height = h;
        }
        POINT origin{};
        ComPtr<ID2D1DeviceContext> context;
        if (FAILED(result = surface->BeginDraw(nullptr, IID_PPV_ARGS(&context), &origin))) return result;
        const float scale = static_cast<float>(dpi) / 96.f;
        const float pixelWidth = static_cast<float>(w), pixelHeight = static_cast<float>(h);
        const auto translation = D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y));
        context->SetDpi(96, 96); context->SetTransform(translation);
        context->Clear(D2D1::ColorF(0, 0.f));
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ComPtr<ID2D1SolidColorBrush> brush;
        result = context->CreateSolidColorBrush(D2D1::ColorF(0), &brush);
        if (SUCCEEDED(result))
        {
            const RECT frame{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
            if (!highContrast && drawBackground) drawBackground(context.Get(), frame, appearance, scale);
            else
            {
                const float radius = std::min({appearance.cornerRadius * scale, pixelWidth * .5f, pixelHeight * .5f});
                const auto shape = D2D1::RoundedRect(D2D1::RectF(.5f, .5f, pixelWidth - .5f, pixelHeight - .5f), radius, radius);
                brush->SetColor(highContrast ? SystemColor(COLOR_INFOBK) : D2D1::ColorF(
                    appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, 1.f));
                context->FillRoundedRectangle(shape, brush.Get());
                brush->SetColor(highContrast ? SystemColor(COLOR_INFOTEXT) : D2D1::ColorF(
                    appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha));
                context->DrawRoundedRectangle(shape, brush.Get(), highContrast ? 1.f : appearance.widgetBorderWidth * scale);
            }
            // Background callbacks share application brushes and can alter the
            // context. Restore the tooltip text contract before drawing it.
            context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * translation);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            brush->SetColor(highContrast ? SystemColor(COLOR_INFOTEXT) :
                D2D1::ColorF(appearance.contentTheme == 1 ? 0x202020 : 0xf4f4f4));
            context->PushAxisAlignedClip(D2D1::RectF(8, 6, std::max(8.f, measured.width - 8),
                std::max(6.f, measured.height - 6)), D2D1_ANTIALIAS_MODE_ALIASED);
            context->DrawTextLayout(D2D1::Point2F(8, 6), measured.layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            context->PopAxisAlignedClip();
        }
        context.Reset();
        const HRESULT ended = surface->EndDraw();
        if (FAILED(result)) return result;
        if (FAILED(ended)) return ended;
        if (FAILED(result = visual->SetContent(surface.Get()))) return result;
        return composition->Commit();
    }
    void Sync(bool show)
    {
        GUITHREADINFO input{sizeof(input)};
        const bool inMenu = GetGUIThreadInfo(GetCurrentThreadId(), &input) &&
            (input.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE));
        if (state.key.empty() || state.text.empty() || !owner || !IsWindowVisible(owner) || !composition || !text)
        { Hide(); return; }
        if (GetCapture() || inMenu) { Hide(); return; }
        if (!EnsureWindow()) return;
        MONITORINFO monitor{sizeof(monitor)};
        const auto handle = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        if (!GetMonitorInfoW(handle, &monitor)) { Hide(); return; }
        UINT xDpi = 96, yDpi = 96;
        if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &xDpi, &yDpi)) || !xDpi) xDpi = 96;
        const bool hc = HighContrast();
        const bool environmentChanged = dpi != xDpi || highContrast != hc;
        dpi = xDpi; highContrast = hc;
        const float scale = static_cast<float>(dpi) / 96.f;
        const auto work = monitor.rcWork;
        const float maximumWidth = std::max(1.f, std::min(280.f, static_cast<float>(work.right - work.left) / scale - 8.f));
        const float maximumHeight = std::max(1.f, std::min(192.f, static_cast<float>(work.bottom - work.top) / scale - 8.f));
        if (!format)
        {
            const auto result = text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"", &format);
            if (FAILED(result)) { Error(result); return; }
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        }
        if (contentDirty || maximumWidth != measuredWidth || maximumHeight != measuredHeight || !measured.layout)
        {
            const auto result = MeasureNativeTooltip(text.Get(), format.Get(), state.title, state.text,
                maximumWidth, maximumHeight, measured);
            if (FAILED(result)) { Error(result); return; }
            measuredWidth = maximumWidth; measuredHeight = maximumHeight;
            contentDirty = false; paintDirty = true;
        }
        const auto next = PlaceNativeTooltip(anchor,
            {static_cast<LONG>(std::ceil(measured.width * scale)), static_cast<LONG>(std::ceil(measured.height * scale))},
            work, placement, static_cast<LONG>(std::ceil(6 * scale)), static_cast<LONG>(std::ceil(4 * scale)));
        const bool moved = !EqualRect(&next, &bounds);
        if (moved)
        {
            bounds = next; paintDirty = true;
            SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                bounds.bottom - bounds.top, SWP_NOACTIVATE | SWP_NOZORDER);
        }
        const bool glass = appearance.glassEnabled && !highContrast && static_cast<bool>(drawBackground);
        if (!glass) backdrop.Reset();
        else if (moved || environmentChanged || paintDirty || !backdrop.IsAvailable())
        {
            if (!backdrop.IsAvailable()) backdrop.InitializePopup(window, true, false);
            backdrop.Reattach(window); backdrop.BeginFrame(true);
            const RECT frame{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
            backdrop.AddPanel(frame, std::min({appearance.cornerRadius * scale,
                static_cast<float>(frame.right - frame.left) * .5f, static_cast<float>(frame.bottom - frame.top) * .5f}),
                appearance.glassBlurRadius * scale, reinterpret_cast<std::uintptr_t>(this));
            backdrop.EndFrame();
        }
        if (paintDirty || environmentChanged)
        {
            const auto result = Paint();
            if (FAILED(result)) { Error(result); return; }
            paintDirty = false; lastError = S_OK;
        }
        PublishName();
        if (show && !visible)
        {
            KillTimer(window, kHoverTimer);
            backdrop.SetPopupWindowPairZOrder(window, HWND_TOPMOST, true);
            backdrop.ShowPopupWindowPair(window);
            visible = IsWindowVisible(window) != FALSE;
            { std::lock_guard guard(accessible->mutex); accessible->visible = visible; }
            if (visible && provider && UiaClientsAreListening()) UiaRaiseAutomationEvent(provider.Get(), UIA_ToolTipOpenedEventId);
        }
        else if (visible && glass) backdrop.SetVisible(true);
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        switch (message)
        {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint); EndPaint(window, &paint); return 0;
        }
        case WM_GETOBJECT:
            if (static_cast<LONG>(lp) == UiaRootObjectId && self->provider)
                return UiaReturnRawElementProvider(window, wp, lp, self->provider.Get());
            break;
        case WM_TIMER:
            if (wp == kHoverTimer) { KillTimer(window, kHoverTimer); self->Sync(true); }
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            self->paintDirty = true;
            if (self->visible) self->Sync(true);
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};

NativeTooltip::NativeTooltip() : impl_(std::make_unique<Impl>()) {}
NativeTooltip::~NativeTooltip() { Close(); }
void NativeTooltip::Configure(HWND owner, IDCompositionDesktopDevice* composition,
    IDWriteFactory* text, const PersonalizationSettings& appearance, DrawBackground drawBackground)
{
    auto& self = *impl_;
    if (self.owner != owner) self.Close();
    if (self.composition.Get() != composition) self.ReleaseGraphics();
    if (self.text.Get() != text) { self.format.Reset(); self.measured = {}; self.contentDirty = true; }
    if (self.appearance != appearance) self.paintDirty = true;
    self.owner = owner; self.composition = composition; self.text = text;
    self.appearance = appearance; self.drawBackground = std::move(drawBackground);
    if (self.visible) self.Sync(true);
}
void NativeTooltip::SetTarget(std::string key, std::wstring body, RECT screenAnchor,
    NativeTooltipPlacement placement, bool immediate, std::wstring title)
{
    auto& self = *impl_;
    if (key.empty() || body.empty() || IsRectEmpty(&screenAnchor)) { self.Hide(); return; }
    const bool contentChanged = self.state.text != body || self.state.title != title;
    const bool targetChanged = self.state.Enter(std::move(key), std::move(body), std::move(title), GetTickCount64(), kHoverDelay);
    if (targetChanged) self.HideWindow();
    self.contentDirty = self.contentDirty || contentChanged;
    self.anchor = screenAnchor; self.placement = placement;
    if (!self.EnsureWindow()) return;
    const auto now = GetTickCount64();
    if (immediate || self.visible || now >= self.state.readyAt) self.Sync(true);
    else if (targetChanged)
        SetTimer(self.window, kHoverTimer, static_cast<UINT>(std::max<std::uint64_t>(1, self.state.readyAt - now)), nullptr);
}
void NativeTooltip::Hide() { impl_->Hide(); }
void NativeTooltip::Close() { impl_->Close(); }
bool NativeTooltip::Visible() const { return impl_->visible; }
}
