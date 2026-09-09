#include "startup_animation.h"
#include "../diagnostic_log.h"
#include "../resource.h"

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
#include <vector>

namespace snowdesktop
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr wchar_t kWindowClass[] = L"SnowDesktopStartupAnimation";

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
        bool animate, double durationScale)
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
        auto wash = Surface(1, 1, 1.0f, [](ID2D1DeviceContext* context) {
            context->Clear(D2D1::ColorF(0x15253b, 0.68f));
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

        std::vector<Monitor> monitors;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor,
            reinterpret_cast<LPARAM>(&monitors));
        if (monitors.empty()) monitors.push_back({ desktop, 1.0f });
        for (const auto& monitor : monitors)
        {
            const float scale = monitor.scale;
            const float left = static_cast<float>(
                (monitor.bounds.left + monitor.bounds.right) / 2 - desktop.left) - 128 * scale;
            const float top = static_cast<float>(
                (monitor.bounds.top + monitor.bounds.bottom) / 2 - desktop.top) - 128 * scale;
            auto emblem = Surface(256, 256, scale, [&](ID2D1DeviceContext* context) {
                ComPtr<ID2D1SolidColorBrush> brush;
                Require(context->CreateSolidColorBrush(D2D1::ColorF(0.87f, 0.94f, 1, 0.09f), &brush));
                context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(128, 104), 62, 62), brush.Get());
                ComPtr<ID2D1Bitmap> bitmap;
                Require(context->CreateBitmapFromWicBitmap(iconPixels.Get(), nullptr, &bitmap));
                context->DrawBitmap(bitmap.Get(), D2D1::RectF(84, 60, 172, 148));
                brush->SetColor(D2D1::ColorF(0.93f, 0.97f, 1));
                context->DrawText(L"SnowDesktop", 11, text.Get(),
                    D2D1::RectF(0, 190, 256, 232), brush.Get());
            });
            AddVisual(emblem.Get(), left, top);

            auto ring = Surface(168, 168, scale, [](ID2D1DeviceContext* context) {
                ComPtr<ID2D1SolidColorBrush> brush;
                Require(context->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush));
                for (int dot = 0; dot < 12; ++dot)
                {
                    const float angle = dot * 6.2831853f / 12;
                    brush->SetColor(D2D1::ColorF(0.74f, 0.88f, 1, 0.12f + dot * 0.07f));
                    const float radius = 2.0f + dot * 0.1f;
                    context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(
                        84 + std::sin(angle) * 76, 84 - std::cos(angle) * 76),
                        radius, radius), brush.Get());
                }
            });
            auto ringVisual = AddVisual(ring.Get(), left + 44 * scale, top + 20 * scale);
            if (animate)
            {
                ComPtr<IDCompositionAnimation> rotation;
                Require(device_->CreateAnimation(&rotation));
                const double period = 1.8 * durationScale;
                Require(rotation->AddCubic(0, 0, static_cast<float>(360 / period), 0, 0));
                Require(rotation->AddRepeat(period, period));
                ComPtr<IDCompositionRotateTransform> transform;
                Require(device_->CreateRotateTransform(&transform));
                Require(transform->SetCenterX(84 * scale));
                Require(transform->SetCenterY(84 * scale));
                Require(transform->SetAngle(rotation.Get()));
                Require(ringVisual->SetTransform(transform.Get()));
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
        Require(root_->AddVisual(visual.Get(), TRUE, nullptr));
        return visual;
    }

    ComPtr<IDCompositionDesktopDevice> device_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual2> root_;
    ComPtr<IDCompositionEffectGroup> opacity_;
};

void RunAnimation(HINSTANCE instance, HWND desktopHost, bool animate,
    double durationScale, HANDLE stop, HANDLE finish)
{
    // No parent/owner or AttachThreadInput: this remains independent of both
    // Explorer and the application's potentially busy initialization thread.
    const HWND desktopRoot = GetAncestor(desktopHost, GA_ROOT);
    if (!desktopRoot || !IsWindow(desktopRoot)) return;
    HIGHCONTRASTW contrast{ sizeof(contrast) };
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) return;
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
    WindowOwner window{ CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
        kWindowClass, L"SnowDesktop", WS_POPUP,
        desktop.left, desktop.top, desktop.right - desktop.left, desktop.bottom - desktop.top,
        nullptr, nullptr, instance, nullptr) };
    if (!window.value) return;
    const BOOL noWindowTransition = TRUE;
    (void)DwmSetWindowAttribute(window.value, DWMWA_TRANSITIONS_FORCEDISABLED,
        &noWindowTransition, sizeof(noWindowTransition));
    Scene scene;
    scene.Initialize(window.value, instance, desktop, animate, durationScale);
    if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
        WaitForSingleObject(finish, 0) == WAIT_OBJECT_0) return;

    // Place just above the desktop root, below ordinary applications/taskbar.
    // A top-level presentation avoids cross-process child input-queue coupling.
    HWND insertAfter = GetWindow(desktopRoot, GW_HWNDPREV);
    if (insertAfter == window.value) insertAfter = GetWindow(window.value, GW_HWNDPREV);
    if (!IsWindow(desktopRoot)) return;
    SetWindowPos(window.value, insertAfter ? insertAfter : HWND_TOP,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    WriteDiagnosticLogEntry(L"Startup animation shown on independent UI thread");

    HANDLE events[]{ stop, finish };
    ULONGLONG fadeEnd = 0;
    bool fading = false;
    while (IsWindow(window.value) && IsWindow(desktopRoot))
    {
        const auto now = GetTickCount64();
        if (fading && now >= fadeEnd) break;
        const DWORD timeout = fading ? static_cast<DWORD>(fadeEnd - now) : 1000;
        const DWORD ready = MsgWaitForMultipleObjectsEx(fading ? 1 : 2,
            events, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ready == WAIT_FAILED || ready == WAIT_OBJECT_0) break;
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
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) thread_.join();
    if (finishEvent_) CloseHandle(finishEvent_);
    if (stopEvent_) CloseHandle(stopEvent_);
}

bool StartupAnimation::Start(HINSTANCE instance, HWND desktopHost,
    bool animate, double durationScale)
{
    if (thread_.joinable() || stopEvent_ || finishEvent_) return false;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    finishEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent_ || !finishEvent_) return false;
    durationScale = std::isfinite(durationScale) ? std::clamp(durationScale, 0.5, 2.0) : 1.0;
    try
    {
        thread_ = std::thread([=, this] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(apartment)) return;
            try { RunAnimation(instance, desktopHost, animate, durationScale, stopEvent_, finishEvent_); }
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

void StartupAnimation::Finish() noexcept
{
    if (finishEvent_) SetEvent(finishEvent_);
}
}
