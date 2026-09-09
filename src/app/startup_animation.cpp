#include "startup_animation.h"
#include "../diagnostic_log.h"
#include "../resource.h"

#include <commctrl.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <vector>

namespace snowdesktop
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr wchar_t kWindowClass[] = L"SnowDesktopStartupAnimation";
constexpr wchar_t kActionWindowClass[] = L"SnowDesktopStartupAction";
constexpr int kCancelButtonId = 1;

struct RenderFailure { HRESULT result; };
void Require(HRESULT result)
{
    if (FAILED(result)) throw RenderFailure{ result };
}

struct WindowOwner
{
    HWND value = nullptr;
    ~WindowOwner() { if (value && IsWindow(value)) DestroyWindow(value); }
};

LRESULT DrawCancelButton(const NMCUSTOMDRAW& draw) noexcept
{
    // Customize the native control's paint only: Windows still supplies its
    // hover/pressed state, capture, click notification, and accessibility.
    if (draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HIGHCONTRASTW contrast{ sizeof(contrast) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) return CDRF_DODEFAULT;

    const bool pressed = (draw.uItemState & CDIS_SELECTED) != 0;
    const bool hovered = (draw.uItemState & CDIS_HOT) != 0;
    const bool focused = (draw.uItemState & CDIS_FOCUS) != 0;
    const float scale = GetDpiForWindow(draw.hdr.hwndFrom) / 96.0f;
    const COLORREF fill = pressed ? RGB(32, 32, 32) :
        hovered ? RGB(54, 54, 54) : RGB(39, 39, 39);
    const COLORREF edge = focused ? RGB(190, 190, 190) :
        hovered ? RGB(108, 108, 108) : RGB(76, 76, 76);
    const int saved = SaveDC(draw.hdc);
    if (!saved) return CDRF_DODEFAULT;
    HBRUSH background = CreateSolidBrush(fill);
    HPEN border = CreatePen(PS_SOLID, std::max(1, static_cast<int>(std::lround(scale))), edge);
    if (!background || !border)
    {
        if (background) DeleteObject(background);
        if (border) DeleteObject(border);
        RestoreDC(draw.hdc, saved);
        return CDRF_DODEFAULT;
    }
    FillRect(draw.hdc, &draw.rc, background);
    SelectObject(draw.hdc, background);
    SelectObject(draw.hdc, border);
    const int diameter = static_cast<int>(std::lround(16 * scale));
    RoundRect(draw.hdc, draw.rc.left, draw.rc.top,
        draw.rc.right, draw.rc.bottom, diameter, diameter);
    const auto font = reinterpret_cast<HFONT>(
        SendMessageW(draw.hdr.hwndFrom, WM_GETFONT, 0, 0));
    if (font) SelectObject(draw.hdc, font);
    SetBkMode(draw.hdc, TRANSPARENT);
    SetTextColor(draw.hdc, (draw.uItemState & CDIS_DISABLED) ?
        RGB(140, 140, 140) : RGB(232, 232, 232));
    wchar_t label[256]{};
    GetWindowTextW(draw.hdr.hwndFrom, label, static_cast<int>(std::size(label)));
    RECT textBounds = draw.rc;
    DrawTextW(draw.hdc, label, -1, &textBounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(draw.hdc, saved);
    DeleteObject(border);
    DeleteObject(background);
    return CDRF_SKIPDEFAULT;
}

LRESULT CALLBACK ActionWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
    {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
            reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    }
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NOTIFY && lp)
    {
        const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
        if (draw->hdr.code == NM_CUSTOMDRAW &&
            draw->hdr.hwndFrom == GetDlgItem(window, kCancelButtonId))
            return DrawCancelButton(*draw);
    }
    if (message == WM_COMMAND && LOWORD(wp) == kCancelButtonId &&
        HIWORD(wp) == BN_CLICKED &&
        reinterpret_cast<HWND>(lp) == GetDlgItem(window, kCancelButtonId))
    {
        auto* cancellation = reinterpret_cast<StartupCancellation*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (cancellation) (void)cancellation->TerminateStartup();
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

struct CancelButton
{
    HWND host = nullptr;
    HFONT font = nullptr;
    ~CancelButton()
    {
        if (host && IsWindow(host)) DestroyWindow(host);
        if (font) DeleteObject(font);
    }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message)
    {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
    case WM_DISPLAYCHANGE:
        // A changed desktop topology retires this optional presentation; it
        // must never leave a stale full-screen surface on a removed monitor.
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

struct Monitor
{
    RECT bounds{};
    float scale = 1.0f;
};

struct Layout
{
    float left = 0;
    float top = 0;
    float scale = 1;
};

D2D1_COLOR_F SystemColor(int index)
{
    const COLORREF color = GetSysColor(index);
    return D2D1::ColorF(GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(parameter);
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    UINT x = 96, y = 96;
    (void)GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y);
    monitors.push_back({ info.rcWork, std::max(1.0f, x / 96.0f) });
    return TRUE;
}

class Scene
{
public:
    void Initialize(HWND window, HINSTANCE instance, RECT desktop,
        const std::vector<Layout>& layouts, const std::wstring& startingText,
        bool animate, double durationScale, bool highContrast)
    {
        ComPtr<ID3D11Device> d3d;
        HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
        if (FAILED(result))
        {
            Require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &d3d, nullptr, nullptr));
        }
        ComPtr<IDXGIDevice> dxgi;
        Require(d3d.As(&dxgi));
        ComPtr<ID2D1Factory1> factory;
        Require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            IID_PPV_ARGS(&factory)));
        ComPtr<ID2D1Device> d2d;
        Require(factory->CreateDevice(dxgi.Get(), &d2d));
        Require(DCompositionCreateDevice2(d2d.Get(),
            IID_PPV_ARGS(&device_)));
        Require(device_->CreateTargetForHwnd(window, FALSE, &target_));
        Require(device_->CreateVisual(&root_));
        Require(device_->CreateEffectGroup(&opacity_));
        Require(root_->SetEffect(opacity_.Get()));
        Require(target_->SetRoot(root_.Get()));

        // A stretched one-pixel wash covers every monitor without a full-screen
        // bitmap allocation, readback, or per-frame CPU rasterization.
        auto wash = Surface(1, 1, 1.0f, [&](ID2D1DeviceContext* context) {
            context->Clear(highContrast ? SystemColor(COLOR_WINDOW) :
                D2D1::ColorF(0x151515, 0.72f));
        });
        auto washVisual = AddVisual(wash.Get(), 0, 0);
        ComPtr<IDCompositionScaleTransform> stretch;
        Require(device_->CreateScaleTransform(&stretch));
        Require(stretch->SetScaleX(static_cast<float>(desktop.right - desktop.left)));
        Require(stretch->SetScaleY(static_cast<float>(desktop.bottom - desktop.top)));
        Require(washVisual->SetTransform(stretch.Get()));

        ComPtr<IWICImagingFactory> wic;
        Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
        HICON icon = static_cast<HICON>(LoadImageW(instance,
            MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 256, 256, 0));
        if (!icon) throw RenderFailure{ E_FAIL };
        ComPtr<IWICBitmap> iconBitmap;
        result = wic->CreateBitmapFromHICON(icon, &iconBitmap);
        DestroyIcon(icon);
        Require(result);
        ComPtr<IWICFormatConverter> iconPixels;
        Require(wic->CreateFormatConverter(&iconPixels));
        Require(iconPixels->Initialize(iconBitmap.Get(),
            GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0, WICBitmapPaletteTypeCustom));
        ComPtr<IDWriteFactory> write;
        Require(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write.GetAddressOf())));
        ComPtr<IDWriteTextFormat> text;
        Require(write->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 24, L"", &text));
        Require(text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        Require(text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
        ComPtr<IDWriteTextFormat> statusText;
        Require(write->CreateTextFormat(L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 14, L"", &statusText));
        Require(statusText->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER));
        Require(statusText->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));

        const auto foreground = highContrast ? SystemColor(COLOR_WINDOWTEXT) :
            D2D1::ColorF(0xf0f0f0);
        for (const auto& layout : layouts)
        {
            const float scale = layout.scale;
            const float left = layout.left;
            const float top = layout.top;
            auto emblem = Surface(320, 270, scale, [&](ID2D1DeviceContext* context) {
                ComPtr<ID2D1SolidColorBrush> brush;
                Require(context->CreateSolidColorBrush(foreground, &brush));
                ComPtr<ID2D1Bitmap> bitmap;
                Require(context->CreateBitmapFromWicBitmap(iconPixels.Get(), nullptr, &bitmap));
                context->DrawBitmap(bitmap.Get(), D2D1::RectF(112, 28, 208, 124));
                context->DrawText(L"SnowDesktop", 11, text.Get(),
                    D2D1::RectF(0, 144, 320, 184), brush.Get());
                context->DrawText(startingText.c_str(),
                    static_cast<UINT32>(startingText.size()), statusText.Get(),
                    D2D1::RectF(0, 232, 320, 260), brush.Get());
            });
            AddVisual(emblem.Get(), left, top);

            // Five small dots below the title. Independent opacity curves run
            // on the compositor; busy Shell initialization cannot stall them.
            for (int dot = 0; dot < 5; ++dot)
            {
                auto pixels = Surface(8, 8, scale, [&](ID2D1DeviceContext* context) {
                    ComPtr<ID2D1SolidColorBrush> brush;
                    Require(context->CreateSolidColorBrush(foreground, &brush));
                    context->FillEllipse(D2D1::Ellipse(
                        D2D1::Point2F(4, 4), 3, 3), brush.Get());
                });
                auto visual = AddVisual(pixels.Get(),
                    left + (124 + 16 * dot) * scale, top + 204 * scale);
                if (animate)
                {
                    const double period = 1.4 * durationScale;
                    const double delay = dot * 0.12 * durationScale;
                    const double rise = 0.18 * durationScale;
                    const double fall = 0.36 * durationScale;
                    ComPtr<IDCompositionAnimation> pulse;
                    Require(device_->CreateAnimation(&pulse));
                    if (delay > 0) Require(pulse->AddCubic(0, 0.25f, 0, 0, 0));
                    Require(pulse->AddCubic(delay, 0.25f,
                        static_cast<float>(0.75 / rise), 0, 0));
                    Require(pulse->AddCubic(delay + rise, 1,
                        static_cast<float>(-0.75 / fall), 0, 0));
                    Require(pulse->AddCubic(delay + rise + fall, 0.25f, 0, 0, 0));
                    Require(pulse->AddRepeat(period, period));
                    ComPtr<IDCompositionEffectGroup> effect;
                    Require(device_->CreateEffectGroup(&effect));
                    Require(effect->SetOpacity(pulse.Get()));
                    Require(visual->SetEffect(effect.Get()));
                }
            }
        }
        if (animate) Fade(0, 1, 0.16 * durationScale);
        else Require(opacity_->SetOpacity(1.0f));
        Require(device_->Commit());
    }

    void Fade(float from, float to, double duration)
    {
        ComPtr<IDCompositionAnimation> fade;
        Require(device_->CreateAnimation(&fade));
        Require(fade->AddCubic(0, from,
            static_cast<float>((to - from) / duration), 0, 0));
        Require(fade->End(duration, to));
        Require(opacity_->SetOpacity(fade.Get()));
        Require(device_->Commit());
    }

private:
    template<class Draw>
    ComPtr<IDCompositionSurface> Surface(UINT width, UINT height,
        float scale, Draw draw)
    {
        ComPtr<IDCompositionSurface> surface;
        Require(device_->CreateSurface(static_cast<UINT>(std::ceil(width * scale)),
            static_cast<UINT>(std::ceil(height * scale)),
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface));
        ComPtr<ID2D1DeviceContext> context;
        POINT offset{};
        Require(surface->BeginDraw(nullptr, IID_PPV_ARGS(&context), &offset));
        context->SetDpi(96, 96);
        context->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
            D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
        context->Clear(D2D1::ColorF(0, 0, 0, 0));
        try { draw(context.Get()); }
        catch (...) { context.Reset(); (void)surface->EndDraw(); throw; }
        context.Reset();
        Require(surface->EndDraw());
        return surface;
    }

    ComPtr<IDCompositionVisual2> AddVisual(IDCompositionSurface* surface, float x, float y)
    {
        ComPtr<IDCompositionVisual2> visual;
        Require(device_->CreateVisual(&visual));
        Require(visual->SetContent(surface));
        Require(visual->SetOffsetX(x));
        Require(visual->SetOffsetY(y));
        // FALSE with no reference places the new visual ABOVE all siblings.
        // The wash is added first and must stay behind the logo/text/dots.
        Require(root_->AddVisual(visual.Get(), FALSE, nullptr));
        return visual;
    }

    ComPtr<IDCompositionDesktopDevice> device_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual2> root_;
    ComPtr<IDCompositionEffectGroup> opacity_;
};

std::unique_ptr<CancelButton> CreateCancelButton(HINSTANCE instance, HWND owner,
    RECT desktop, const Layout& layout, const std::wstring& label,
    StartupCancellation& cancellation)
{
    auto button = std::make_unique<CancelButton>();
    const float scale = layout.scale;
    button->font = CreateFontW(-static_cast<int>(std::lround(14 * scale)),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    SIZE textSize{};
    if (HDC dc = GetDC(nullptr))
    {
        const auto previous = button->font ? SelectObject(dc, button->font) : nullptr;
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &textSize);
        if (previous) SelectObject(dc, previous);
        ReleaseDC(nullptr, dc);
    }
    const int width = static_cast<int>(std::ceil(std::clamp(
        textSize.cx + 32 * scale, 132 * scale, 288 * scale)));
    const int height = static_cast<int>(std::ceil(36 * scale));
    const int x = desktop.left + static_cast<int>(std::lround(
        layout.left + 160 * scale - width / 2.0f));
    const int y = desktop.top + static_cast<int>(std::lround(layout.top + 282 * scale));
    // The full-screen wash stays click-through. Only this small owned popup
    // receives input, on the same independent UI thread as its native button.
    button->host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kActionWindowClass, L"SnowDesktop", WS_POPUP,
        x, y, width, height, owner, nullptr, instance, &cancellation);
    if (!button->host) throw RenderFailure{ E_FAIL };
    HWND control = CreateWindowExW(0, L"BUTTON", label.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, width, height, button->host,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancelButtonId)), instance, nullptr);
    if (!control) throw RenderFailure{ E_FAIL };
    if (button->font)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(button->font), TRUE);
    // Clip the small input popup too, so its square corners do not cover the
    // desktop around the rounded button. Native hit testing/capture and the
    // accessible BUTTON role remain supplied by the system control.
    const int diameter = static_cast<int>(std::lround(16 * scale));
    HRGN rounded = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
    if (rounded && !SetWindowRgn(button->host, rounded, FALSE)) DeleteObject(rounded);
    const BOOL noWindowTransition = TRUE;
    (void)DwmSetWindowAttribute(button->host, DWMWA_TRANSITIONS_FORCEDISABLED,
        &noWindowTransition, sizeof(noWindowTransition));
    return button;
}

void RunAnimation(HINSTANCE instance, HWND desktopHost, bool animate,
    double durationScale, const std::wstring& startingText,
    const std::wstring& cancelText, StartupCancellation& cancellation,
    HANDLE stop, HANDLE finish, HANDLE handoff)
{
    // No parent/owner or AttachThreadInput: this remains independent of both
    // Explorer and the application's potentially busy initialization thread.
    const HWND desktopRoot = GetAncestor(desktopHost, GA_ROOT);
    if (!desktopRoot || !IsWindow(desktopRoot)) return;
    HIGHCONTRASTW contrast{ sizeof(contrast) };
    const bool highContrast =
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    animate = animate && !highContrast;
    const RECT desktop{ GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;
    WNDCLASSEXW actionClass{ sizeof(actionClass) };
    actionClass.hInstance = instance;
    actionClass.lpfnWndProc = ActionWindowProc;
    actionClass.lpszClassName = kActionWindowClass;
    actionClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&actionClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;
    WindowOwner window{ CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
        kWindowClass, L"SnowDesktop", WS_POPUP,
        desktop.left, desktop.top, desktop.right - desktop.left, desktop.bottom - desktop.top,
        nullptr, nullptr, instance, nullptr) };
    if (!window.value) return;
    const BOOL noWindowTransition = TRUE;
    (void)DwmSetWindowAttribute(window.value, DWMWA_TRANSITIONS_FORCEDISABLED,
        &noWindowTransition, sizeof(noWindowTransition));
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
        reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) monitors.push_back({ desktop, 1.0f });
    std::vector<Layout> layouts;
    for (const auto& monitor : monitors)
    {
        layouts.push_back({ static_cast<float>(
            (monitor.bounds.left + monitor.bounds.right) / 2 - desktop.left) - 160 * monitor.scale,
            static_cast<float>(
            (monitor.bounds.top + monitor.bounds.bottom) / 2 - desktop.top) - 170 * monitor.scale,
            monitor.scale });
    }
    Scene scene;
    scene.Initialize(window.value, instance, desktop, layouts, startingText,
        animate, durationScale, highContrast);
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
        WaitForSingleObject(finish, 0) == WAIT_OBJECT_0) return;
    std::vector<std::unique_ptr<CancelButton>> buttons;
    if (cancellation.IsStarting())
    {
        for (const auto& layout : layouts)
            buttons.push_back(CreateCancelButton(instance, window.value,
                desktop, layout, cancelText, cancellation));
    }

    // Place just above the desktop root, below ordinary applications/taskbar.
    // A top-level presentation avoids cross-process child input-queue coupling.
    HWND insertAfter = GetWindow(desktopRoot, GW_HWNDPREV);
    while (insertAfter && (insertAfter == window.value ||
        GetWindow(insertAfter, GW_OWNER) == window.value))
        insertAfter = GetWindow(insertAfter, GW_HWNDPREV);
    if (!IsWindow(desktopRoot)) return;
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
        WaitForSingleObject(finish, 0) == WAIT_OBJECT_0) return;
    SetWindowPos(window.value, insertAfter ? insertAfter : HWND_TOP,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (cancellation.IsStarting())
    {
        for (const auto& button : buttons)
            SetWindowPos(button->host, insertAfter ? insertAfter : HWND_TOP,
                0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    WriteDiagnosticLogEntry(L"Startup animation shown on independent UI thread");

    HANDLE events[]{ stop, finish, handoff };
    ULONGLONG fadeEnd = 0;
    bool fading = false;
    while (IsWindow(window.value) && IsWindow(desktopRoot))
    {
        const auto now = GetTickCount64();
        if (fading && now >= fadeEnd) break;
        const DWORD timeout = fading ? static_cast<DWORD>(fadeEnd - now) : 1000;
        const DWORD ready = MsgWaitForMultipleObjectsEx(fading ? 1 : 3,
            events, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ready == WAIT_FAILED || ready == WAIT_OBJECT_0) break;
        if (!cancellation.IsStarting()) buttons.clear();
        if (!fading && ready == WAIT_OBJECT_0 + 2) ResetEvent(handoff);
        if (!fading && ready == WAIT_OBJECT_0 + 1)
        {
            if (!animate) break;
            const double duration = 0.22 * durationScale;
            scene.Fade(1, 0, duration);
            fadeEnd = GetTickCount64() + static_cast<ULONGLONG>(std::ceil(duration * 1000));
            fading = true;
        }
        MSG message{};
        unsigned processed = 0;
        while (processed++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    ShowWindow(window.value, SW_HIDE);
    WriteDiagnosticLogEntry(L"Startup animation retired");
}
}

StartupAnimation::~StartupAnimation()
{
    (void)cancellation_.BeginDesktopHandoff();
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (finishEvent_) CloseHandle(finishEvent_);
    if (handoffEvent_) CloseHandle(handoffEvent_);
    if (stopEvent_) CloseHandle(stopEvent_);
}

bool StartupAnimation::Start(HINSTANCE instance, HWND desktopHost,
    bool animate, double durationScale,
    std::wstring startingText, std::wstring cancelText)
{
    if (thread_.joinable() || stopEvent_ || finishEvent_ || handoffEvent_) return false;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    finishEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    handoffEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_ || !finishEvent_ || !handoffEvent_) return false;
    durationScale = std::isfinite(durationScale) ? std::clamp(durationScale, 0.5, 2.0) : 1.0;
    try
    {
        thread_ = std::thread([=, this] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(apartment)) return;
            // Locale owns mutable caches on the main thread. Only frozen,
            // translated strings cross into the presentation worker.
            try { RunAnimation(instance, desktopHost, animate, durationScale,
                startingText, cancelText, cancellation_, stopEvent_, finishEvent_, handoffEvent_); }
            catch (...)
            {
                WriteDiagnosticLogEntry(L"Startup animation unavailable; continuing without overlay",
                    DiagnosticLogLevel::Warning);
            }
            CoUninitialize();
        });
    }
    catch (...) { return false; }
    return true;
}

bool StartupAnimation::BeginDesktopHandoff() noexcept
{
    if (!cancellation_.BeginDesktopHandoff()) return false;
    if (handoffEvent_) SetEvent(handoffEvent_);
    return true;
}

void StartupAnimation::Finish() noexcept
{
    (void)BeginDesktopHandoff();
    if (finishEvent_) SetEvent(finishEvent_);
}
}
