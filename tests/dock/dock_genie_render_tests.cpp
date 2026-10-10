#include "dock/dock_genie_rules.h"
#include "dock/dock_minimize_protocol.h"
#include "dock/dock_window_shared_thumbnail.h"
#include "dock/dock_window_source_cloak.h"
#include "dock/dock_window_transition.h"
#include "settings/animation_settings.h"

#include <d3d11.h>
#include <dxgi.h>
#include <array>
#include <cmath>
#include <iostream>
#include <string>

namespace
{
int failures = 0;
void Check(bool passed, const char* name)
{
    if (!passed) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
struct Point { double x, y; };
Point Transform(const snowdesktop::dock_genie::ProjectiveMatrix& matrix, double x, double y)
{
    // Include the float conversion used by DirectComposition in the error bound.
    const double w = static_cast<float>(matrix.m14) * x +
        static_cast<float>(matrix.m24) * y + static_cast<float>(matrix.w);
    Check(w > 0 && std::isfinite(w), "strip has no perspective pole inside its clip");
    return {(static_cast<float>(matrix.m11) * x + static_cast<float>(matrix.m21) * y +
        static_cast<float>(matrix.dx)) / w,
        (static_cast<float>(matrix.m12) * x + static_cast<float>(matrix.m22) * y +
        static_cast<float>(matrix.dy)) / w};
}
double Gap(Point first, Point second)
{
    return std::hypot(first.x - second.x, first.y - second.y);
}

void CheckContinuousOutline()
{
    using namespace snowdesktop::dock_genie;
    constexpr Rect window{-1860, -750, 1980, 1410};
    constexpr Rect dock{-1700, 1480, -1636, 1544};
    bool affineStepsObserved = false;
    for (const auto edge : {Edge::Bottom, Edge::Top, Edge::Left, Edge::Right})
        for (const double collapsed : {0.0, 0.15, 0.35, 0.55, 0.8, 0.95, 1.0})
            for (std::size_t i = 0; i + 1 < StripCount; ++i)
            {
                const double begin = static_cast<double>(i) / StripCount;
                const double boundary = static_cast<double>(i + 1) / StripCount;
                const double end = static_cast<double>(i + 2) / StripCount;
                const auto first = StripProjectiveMatrix(window, dock, edge, collapsed,
                    3840, 2160, begin, boundary, -1920, -1080);
                const auto next = StripProjectiveMatrix(window, dock, edge, collapsed,
                    3840, 2160, boundary, end, -1920, -1080);
                for (const double cross : {0.0, 0.5, 1.0})
                {
                    const double x = (Vertical(edge) ? cross : boundary) * 3840;
                    const double y = (Vertical(edge) ? boundary : cross) * 2160;
                    Check(Gap(Transform(first, x, y), Transform(next, x, y)) < 0.15,
                        "neighbouring strip edges meet to subpixel precision at every animation phase");
                    const auto oldFirst = StripMatrix(window, dock, edge, collapsed,
                        3840, 2160, begin, boundary, -1920, -1080);
                    const auto oldNext = StripMatrix(window, dock, edge, collapsed,
                        3840, 2160, boundary, end, -1920, -1080);
                    const auto affine = [x, y](const Matrix& matrix) {
                        return Point{matrix.m11 * x + matrix.m21 * y + matrix.dx,
                            matrix.m12 * x + matrix.m22 * y + matrix.dy};
                    };
                    affineStepsObserved = affineStepsObserved || Gap(affine(oldFirst), affine(oldNext)) > 0.25;
                }
            }
    Check(affineStepsObserved, "continuity regression detects the previous affine staircase");
    for (const double collapsed : {0.0, 1.0})
    {
        const auto matrix = StripProjectiveMatrix(window, dock, Edge::Bottom, collapsed,
            3840, 2160, 0, 1, 0, 0);
        const Rect target = collapsed == 0 ? window : dock;
        Check(Gap(Transform(matrix, 0, 0), {target.left, target.top}) < 0.001 &&
            Gap(Transform(matrix, 3840, 2160), {target.right, target.bottom}) < 0.001,
            "projective animation preserves both window and Dock endpoints");
    }
}

LRESULT CALLBACK FrameFixtureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        if (GetWindowLongPtrW(window, GWLP_USERDATA))
        {
            RECT marker{20, 20, 36, 60};
            HBRUSH brush = CreateSolidBrush(RGB(220, 20, 40));
            FillRect(dc, &marker, brush);
            DeleteObject(brush);
            marker = {client.right - 36, 20, client.right - 20, 60};
            brush = CreateSolidBrush(RGB(20, 220, 40));
            FillRect(dc, &marker, brush);
            DeleteObject(brush);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::array<double, 2> FrameMarkerCenters(HWND ownedDestination)
{
    RECT area{};
    if (!GetWindowRect(ownedDestination, &area)) return {-1, -1};
    const int width = area.right - area.left, height = area.bottom - area.top;
    if (width <= 0 || height <= 0 || width > 512 || height > 512) return {-1, -1};
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    std::array<double, 2> sums{}, counts{};
    if (bitmap)
    {
        const HGDIOBJ previous = SelectObject(memory, bitmap);
        // Capture only the rectangle occupied by this test's composition HWND
        // and opaque backing fixture; never inspect SnowDesktop or other apps.
        if (BitBlt(memory, 0, 0, width, height, screen, area.left, area.top, SRCCOPY | CAPTUREBLT))
        {
            const auto* pixels = static_cast<const unsigned*>(bits);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const unsigned pixel = pixels[y * width + x];
                    const unsigned red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
                    if (red > 150 && green < 70 && blue < 100) { sums[0] += x; ++counts[0]; }
                    if (green > 150 && red < 70 && blue < 100) { sums[1] += x; ++counts[1]; }
                }
        }
        SelectObject(memory, previous);
        DeleteObject(bitmap);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return {counts[0] ? sums[0] / counts[0] : -1, counts[1] ? sums[1] / counts[1] : -1};
}

void CheckFramePixelAlignment()
{
    using Microsoft::WRL::ComPtr;
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopGenieFramePixelFixture";
    type.lpfnWndProc = FrameFixtureProc;
    Check(RegisterClassW(&type) != 0, "owned pixel-alignment fixture registers");
    const HWND source = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        type.lpszClassName, L"Owned frame fixture", WS_OVERLAPPEDWINDOW, 500, 32, 320, 200,
        nullptr, nullptr, type.hInstance, nullptr);
    SetWindowLongPtrW(source, GWLP_USERDATA, 1);
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);
    DwmFlush();
    SIZE size{};
    RECT frame{}, client{};
    POINT clientOrigin{};
    HRESULT hr = snowdesktop::dock_thumbnail::SourceSize(source, size);
    if (SUCCEEDED(hr)) hr = DwmGetWindowAttribute(source, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame));
    const bool geometry = source && SUCCEEDED(hr) && GetClientRect(source, &client) && ClientToScreen(source, &clientOrigin);
    Check(geometry, "framed source supplies its visible bounds and client origin");
    HWND backing = nullptr, destination = nullptr;
    if (geometry)
    {
        const auto make = [&](DWORD extra) {
            return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | extra,
                type.lpszClassName, L"", WS_POPUP, 32, 32, size.cx, size.cy, nullptr, nullptr, type.hInstance, nullptr);
        };
        backing = make(0);
        destination = make(WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED);
        SetLayeredWindowAttributes(destination, 0, 255, LWA_ALPHA);
        ShowWindow(backing, SW_SHOWNOACTIVATE);
        UpdateWindow(backing);
        ComPtr<ID3D11Device> graphics;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDCompositionDesktopDevice> composition;
        ComPtr<IDCompositionTarget> target;
        snowdesktop::dock_thumbnail::SharedVisual image;
        if (SUCCEEDED(hr)) hr = graphics.As(&dxgi);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice3(dxgi.Get(), IID_PPV_ARGS(&composition));
        if (SUCCEEDED(hr)) hr = image.Create(destination, source, composition.Get(), size);
        if (SUCCEEDED(hr)) hr = composition->CreateTargetForHwnd(destination, TRUE, &target);
        if (SUCCEEDED(hr)) hr = target->SetRoot(image.visual.Get());
        if (SUCCEEDED(hr)) hr = composition->Commit();
        if (SUCCEEDED(hr)) hr = composition->WaitForCommitCompletion();
        Check(SUCCEEDED(hr), "cropped shared frame is composed through the production image wrapper");
        if (SUCCEEDED(hr))
        {
            ShowWindow(destination, SW_SHOWNOACTIVATE);
            const ULONGLONG deadline = GetTickCount64() + 500;
            std::array<double, 2> actual{-1, -1};
            do
            {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                { TranslateMessage(&message); DispatchMessageW(&message); }
                DwmFlush();
                actual = FrameMarkerCenters(destination);
                if (actual[0] >= 0 && actual[1] >= 0) break;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            } while (GetTickCount64() < deadline);
            const double red = clientOrigin.x - frame.left + 27.5;
            const double green = clientOrigin.x - frame.left + client.right - 28.5;
            std::cout << "Shared frame marker centers: " << actual[0] << ',' << actual[1]
                << " expected=" << red << ',' << green << '\n';
            Check(std::abs(actual[0] - red) < 1 && std::abs(actual[1] - green) < 1,
                "left and right image pixels align with the visible frame instead of its invisible resize border");
        }
        if (target) target->SetRoot(nullptr);
        if (composition) { composition->Commit(); composition->WaitForCommitCompletion(); }
        image.Reset();
    }
    if (destination) DestroyWindow(destination);
    if (backing) DestroyWindow(backing);
    if (source) DestroyWindow(source);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
}

void CheckRetainedWindowImage()
{
    using Microsoft::WRL::ComPtr;
    using snowdesktop::dock_thumbnail::SharedVisual;
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopGenieImageFixture";
    type.lpfnWndProc = DefWindowProcW;
    type.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH));
    Check(RegisterClassW(&type) != 0, "owned image fixture class registers");
    const auto makeWindow = [&] {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            type.lpszClassName, L"", WS_POPUP, -20000, -20000, 320, 200,
            nullptr, nullptr, type.hInstance, nullptr);
    };
    // The source needs a normal redirection surface. Only the destination uses
    // NOREDIRECTIONBITMAP; both windows are owned fixtures outside the desktop.
    const HWND source = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        type.lpszClassName, L"", WS_POPUP, -20000, -20000, 320, 200,
        nullptr, nullptr, type.hInstance, nullptr);
    const HWND destination = makeWindow();
    Check(source && destination, "owned source and destination fixtures are created");
    if (source && destination)
    {
        ComPtr<ID3D11Device> graphics;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
        if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        ComPtr<IDCompositionDesktopDevice> composition;
        if (SUCCEEDED(hr)) hr = graphics.As(&dxgi);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice3(dxgi.Get(), IID_PPV_ARGS(&composition));
        Check(SUCCEEDED(hr), "fixture composition device initializes");
        if (SUCCEEDED(hr))
        {
            ShowWindow(source, SW_SHOWNOACTIVATE);
            DwmFlush();
            SIZE visible{};
            hr = snowdesktop::dock_thumbnail::SourceSize(source, visible);
            std::cout << "DWM source size: " << visible.cx << 'x' << visible.cy << " hr=" << std::hex << hr << std::dec << '\n';
            Check(SUCCEEDED(hr) &&
                visible.cx == 320 && visible.cy == 200, "DWM supplies the current source image geometry");
            ShowWindow(source, SW_MINIMIZE);
            DwmFlush();
            SIZE minimized{};
            Check(IsIconic(source) && SUCCEEDED(snowdesktop::dock_thumbnail::SourceSize(source, minimized)) &&
                minimized.cx == visible.cx && minimized.cy == visible.cy,
                "DWM retains full window geometry without any SnowDesktop minimize cache");
            SharedVisual image;
            hr = image.Create(destination, source, composition.Get(), minimized);
            Check(SUCCEEDED(hr) && image.visual, "already minimized window produces a deformable shared image");
            if (SUCCEEDED(hr))
            {
                ComPtr<IDCompositionTarget> target;
                hr = composition->CreateTargetForHwnd(destination, TRUE, &target);
                if (SUCCEEDED(hr)) hr = target->SetRoot(image.visual.Get());
                const D2D_MATRIX_4X4_F identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                if (SUCCEEDED(hr)) hr = image.visual->SetTransform(identity);
                if (SUCCEEDED(hr)) hr = image.visual->SetBorderMode(DCOMPOSITION_BORDER_MODE_SOFT);
                if (SUCCEEDED(hr)) hr = composition->Commit();
                if (SUCCEEDED(hr)) hr = composition->WaitForCommitCompletion();
                std::cout << "Shared image composition: hr=" << std::hex << hr << std::dec << '\n';
                Check(SUCCEEDED(hr), "shared source accepts projective transforms and soft edge rendering");
                if (target) target->SetRoot(nullptr);
                composition->Commit();
                composition->WaitForCommitCompletion();
            }
            image.Reset();
            Check(FAILED(image.Create(destination, nullptr, composition.Get(), minimized)) && !image.visual,
                "unavailable source fails cleanly for fallback");

            ComPtr<ID2D1Factory1> factory;
            ComPtr<ID2D1Device> d2d;
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr)) hr = factory->CreateDevice(dxgi.Get(), &d2d);
            snowdesktop::UiAnimationScheduler scheduler;
            Check(SUCCEEDED(hr) && scheduler.Initialize(), "owned animation fixture initializes");
            if (SUCCEEDED(hr) && scheduler.WaitHandle())
            {
                snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                    2, 0, 60, false, false, 3);
                for (const int effect : {1, 2, 3})
                {
                    snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                        2, 0, 60, false, false, effect);
                    DockWindowTransition unavailable;
                    Check(unavailable.Initialize(type.hInstance, &scheduler, nullptr, nullptr),
                        "a missing shared-image device still leaves native window operations available");
                    std::wstring failurePresentation;
                    unavailable.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock transition:") == 0) failurePresentation = message;
                    });
                    const RECT fallbackDock{-19720, -19720, -19656, -19656};
                    ShowWindow(source, SW_SHOWNOACTIVATE);
                    Check(!unavailable.StartMinimize(source, fallbackDock) &&
                        unavailable.RequiresNativeAnimationFallback() && !unavailable.IsActive() &&
                        !unavailable.GetPresentationWindow() && !IsIconic(source) &&
                        failurePresentation.find(L"fallback=native-system-animation") != std::wstring::npos,
                        "shared-image failure requests native fallback for every selected effect");
                    DWORD cloakFlags = 0;
                    Check(SUCCEEDED(DwmGetWindowAttribute(source, DWMWA_CLOAKED, &cloakFlags, sizeof(cloakFlags))) &&
                        !(cloakFlags & DWM_CLOAKED_APP), "failed animation leaves no source cloak or overlay");
                    ShowWindow(source, SW_MINIMIZE);
                    Check(IsIconic(source) != FALSE, "the original native minimize remains operational");
                    int unexpectedRestore = 0;
                    Check(!unavailable.StartRestore(source, fallbackDock,
                        [&](HWND, DockWindowRestoreTransitionPhase) { ++unexpectedRestore; }) &&
                        unavailable.RequiresNativeAnimationFallback() && !unavailable.IsActive() &&
                        unexpectedRestore == 0, "unavailable restore uses no previous minimize image or synthetic callback");
                    ShowWindow(source, SW_SHOWNOACTIVATE);
                    Check(!IsIconic(source), "native restore remains operational after shared-image failure");
                }
                snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                    2, 0, 60, false, false, 3);
                ShowWindow(source, SW_MINIMIZE);
                DwmFlush();
                DockWindowTransition transition;
                Check(transition.Initialize(type.hInstance, &scheduler, d2d.Get(), composition.Get()),
                    "fresh transition engine initializes without window snapshots");
                std::wstring presentation;
                transition.SetDiagnosticCallback([&](const wchar_t* message) {
                    if (std::wstring(message).find(L"Dock transition:") == 0) presentation = message;
                });
                const BOOL alreadyDisabled = TRUE;
                DwmSetWindowAttribute(source, DWMWA_TRANSITIONS_FORCEDISABLED,
                    &alreadyDisabled, sizeof(alreadyDisabled));
                const RECT dock{-19720, -19720, -19656, -19656};
                Check(SetWindowPos(destination, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE,
                    "owned Dock fixture starts its independently promoted session");
                int restores = 0;
                int activations = 0;
                bool requestedWhileCloaked = false;
                const bool restoring = transition.StartRestore(source, dock,
                    [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                        if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                        {
                            ++restores;
                            DWORD flags = 0;
                            requestedWhileCloaked = SUCCEEDED(DwmGetWindowAttribute(window,
                                DWMWA_CLOAKED, &flags, sizeof(flags))) && (flags & DWM_CLOAKED_APP) != 0;
                            ShowWindow(window, SW_SHOWNOACTIVATE);
                        }
                        else if (phase == DockWindowRestoreTransitionPhase::ActivateRestored) ++activations;
                    }, destination);
                std::wcout << presentation << L'\n';
                Check(restoring && presentation.find(L"effective=3") != std::wstring::npos &&
                    presentation.find(L"image=dwm-shared-window") != std::wstring::npos,
                    "first restore uses Genie and a DWM image without a prior minimize animation");
                Check(restores == 1 && requestedWhileCloaked,
                    "restore is requested under the source cloak before the visual timeline, not after it");
                const auto drain = [&] {
                    const ULONGLONG deadline = GetTickCount64() + 3000;
                    while (transition.IsActive() && GetTickCount64() < deadline)
                    {
                        scheduler.DispatchDue();
                        MSG message{};
                        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                        {
                            TranslateMessage(&message);
                            DispatchMessageW(&message);
                        }
                        const HANDLE timer = scheduler.WaitHandle();
                        MsgWaitForMultipleObjects(1, &timer, FALSE, 10, QS_ALLINPUT);
                    }
                    Check(!transition.IsActive() && !transition.GetPresentationWindow(),
                        "animation completes and retires its entire presentation");
                    Check((GetWindowLongPtrW(destination, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                        "retiring a target animation keeps the independently promoted Dock topmost");
                };
                if (restoring) drain();
                Check(restores == 1 && !IsIconic(source), "restore handoff runs exactly once");
                Check(activations == 1, "restore handoff activates the completed source once");
                const auto appCloaked = [](HWND window) {
                    DWORD flags = 0;
                    const HRESULT query = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags));
                    Check(SUCCEEDED(query), "owned source cloak query succeeds");
                    return (flags & DWM_CLOAKED_APP) != 0;
                };
                Check(!appCloaked(source), "restore handoff releases only the animation's temporary source cloak");

                for (const int effect : {1, 2, 3})
                {
                    std::cout << "Short-bar restore effect: " << effect << '\n';
                    snowdesktop::animation::SetRuntimePreferences(snowdesktop::animation::AlwaysOn,
                        2, 0, 60, false, false, effect);
                    // Emulate the short iconic bar reported for Electron: the
                    // native flag clears before the application's full geometry.
                    RECT fullRect{};
                    GetWindowRect(source, &fullRect);
                    ShowWindow(source, SW_MINIMIZE);
                    DwmFlush();
                    struct ShortRestore
                    {
                        DockWindowTransition* transition;
                        RECT fullRect;
                        bool prepared = false;
                        bool hiddenDuringBar = false;
                        bool waitingWhileHidden = false;
                        int requests = 0;
                        int activations = 0;
                    } shortRestore{&transition, fullRect};
                    transition.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock restore prepared:") == 0) shortRestore.prepared = true;
                    });
                    SetWindowLongPtrW(source, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&shortRestore));
                    Check(transition.StartRestore(source, dock,
                        [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                            if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                            {
                                ++shortRestore.requests;
                                ShowWindow(window, SW_SHOWNOACTIVATE);
                                SetWindowPos(window, nullptr, fullRect.left, fullRect.top, 160, 24,
                                    SWP_NOZORDER | SWP_NOACTIVATE);
                                Check(SetTimer(window, 2, 120, [](HWND targetWindow, UINT, UINT_PTR timer, DWORD) {
                                    KillTimer(targetWindow, timer);
                                    auto* state = reinterpret_cast<ShortRestore*>(GetWindowLongPtrW(targetWindow, GWLP_USERDATA));
                                    DWORD flags = 0;
                                    state->hiddenDuringBar = state->transition->IsActive() && !state->prepared &&
                                        !IsIconic(targetWindow) && SUCCEEDED(DwmGetWindowAttribute(targetWindow,
                                            DWMWA_CLOAKED, &flags, sizeof(flags))) && (flags & DWM_CLOAKED_APP) != 0;
                                    SetWindowPos(targetWindow, nullptr, state->fullRect.left, state->fullRect.top,
                                        state->fullRect.right - state->fullRect.left,
                                        state->fullRect.bottom - state->fullRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
                                    // Complete geometry can precede visibility too;
                                    // neither phase may start the visual timeline.
                                    ShowWindow(targetWindow, SW_HIDE);
                                    Check(SetTimer(targetWindow, 3, 80, [](HWND visibleWindow, UINT, UINT_PTR visibleTimer, DWORD) {
                                        KillTimer(visibleWindow, visibleTimer);
                                        auto* visibleState = reinterpret_cast<ShortRestore*>(GetWindowLongPtrW(visibleWindow, GWLP_USERDATA));
                                        DWORD cloakFlags = 0;
                                        RECT hiddenRect{};
                                        GetWindowRect(visibleWindow, &hiddenRect);
                                        visibleState->waitingWhileHidden = visibleState->transition->IsActive() &&
                                            !visibleState->prepared && !IsWindowVisible(visibleWindow) &&
                                            EqualRect(&hiddenRect, &visibleState->fullRect) &&
                                            SUCCEEDED(DwmGetWindowAttribute(visibleWindow, DWMWA_CLOAKED,
                                                &cloakFlags, sizeof(cloakFlags))) && (cloakFlags & DWM_CLOAKED_APP) != 0;
                                        ShowWindow(visibleWindow, SW_SHOWNOACTIVATE);
                                    }) != 0, "owned hidden restore visibility timer is armed");
                                }) != 0, "owned short-bar restore completion timer is armed");
                            }
                            else if (phase == DockWindowRestoreTransitionPhase::ActivateRestored)
                            {
                                ++shortRestore.activations;
                                RECT actual{};
                                GetWindowRect(window, &actual);
                                Check(EqualRect(&actual, &fullRect) && IsWindowVisible(window) && !appCloaked(window),
                                    "source is exposed only at its complete geometry");
                            }
                        }), "owned short-bar restore starts");
                    Check(shortRestore.requests == 1, "short-bar fixture receives the restore immediately");
                    drain();
                    Check(shortRestore.hiddenDuringBar && shortRestore.waitingWhileHidden && shortRestore.prepared &&
                        shortRestore.requests == 1 && shortRestore.activations == 1 && !appCloaked(source),
                        "full geometry and visibility are both required before any restore effect starts or retires");
                    KillTimer(source, 2);
                    KillTimer(source, 3);
                    SetWindowLongPtrW(source, GWLP_USERDATA, 0);
                    transition.SetDiagnosticCallback([&](const wchar_t* message) {
                        if (std::wstring(message).find(L"Dock transition:") == 0) presentation = message;
                    });
                }
                const DWORD preparationStart = GetTickCount();
                const bool minimizing = transition.StartExternalMinimize(source, dock,
                    preparationStart + snowdesktop::dock_minimize::kRequestTimeoutMs, destination);
                Check(minimizing && presentation.find(L"image=dwm-shared-window") != std::wstring::npos,
                    "native minimize prepares a shared Genie image within the hook deadline");
                std::cout << "Shared Genie preparation: " << GetTickCount() - preparationStart << " ms\n";
                if (minimizing)
                {
                    Check(appCloaked(source), "native minimize keeps the real source out of the composition scene");
                    ShowWindow(source, SW_MINIMIZE);
                    drain();
                    Check(IsIconic(source) && !appCloaked(source), "minimize remains committed after scene retirement and source release");
                }
                // Model a toolkit UI thread which dispatches the native state
                // change after the visual timeline. The source must remain
                // cloaked then, otherwise a second native animation can flash.
                ShowWindow(source, SW_SHOWNOACTIVATE);
                const BOOL enabled = FALSE;
                DwmSetWindowAttribute(source, DWMWA_TRANSITIONS_FORCEDISABLED, &enabled, sizeof(enabled));
                struct LateMinimize { DockWindowTransition* transition; bool retained = false; } late{&transition};
                SetWindowLongPtrW(source, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&late));
                const bool delayed = transition.StartMinimize(source, dock);
                Check(delayed, "Dock minimize begins for a delayed toolkit state change");
                if (delayed)
                {
                    const UINT delay = static_cast<UINT>(360.0 * snowdesktop::animation::RuntimeDurationScale() + 200.0);
                    Check(SetTimer(source, 1, delay, [](HWND window, UINT, UINT_PTR timer, DWORD) {
                        KillTimer(window, timer);
                        auto* state = reinterpret_cast<LateMinimize*>(GetWindowLongPtrW(window, GWLP_USERDATA));
                        DWORD flags = 0;
                        state->retained = state->transition->IsActive() &&
                            SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags))) &&
                            (flags & DWM_CLOAKED_APP) != 0;
                        ShowWindow(window, SW_MINIMIZE);
                    }) != 0, "owned delayed state change timer is armed");
                    drain();
                    Check(late.retained && IsIconic(source),
                        "minimize timeline retains native suppression until the delayed application commits its minimized state");
                    Check(!appCloaked(source), "delayed minimize releases its temporary source cloak after handoff");
                    KillTimer(source, 1);
                }
                SetWindowLongPtrW(source, GWLP_USERDATA, 0);
                ShowWindow(source, SW_SHOWNOACTIVATE);
                Check(transition.StartMinimize(source, dock), "owned cancel fixture starts its transition");
                transition.Cancel();
                Check(!appCloaked(source) && !IsIconic(source), "cancelling an animation immediately reveals its unminimized source");
                const BOOL cloaked = TRUE;
                DwmSetWindowAttribute(source, DWMWA_CLOAK, &cloaked, sizeof(cloaked));
                Check(transition.StartMinimize(source, dock), "a precloaked source retains a DWM composition image");
                transition.Cancel();
                Check(appCloaked(source), "animation does not release an application cloak it did not acquire");
                const BOOL uncloaked = FALSE;
                DwmSetWindowAttribute(source, DWMWA_CLOAK, &uncloaked, sizeof(uncloaked));
                bool acquired = false;
                Check(FAILED(snowdesktop::dock_source_cloak::Acquire(source, nullptr, acquired)) &&
                    !acquired && !appCloaked(source), "an invalid animation owner cannot leave a hidden source behind");
                const HWND leaseOwner = makeWindow();
                Check(leaseOwner && snowdesktop::dock_source_cloak::RegisterOwner(leaseOwner),
                    "owned crash recovery fixture establishes its owner generation");
                Check(SUCCEEDED(snowdesktop::dock_source_cloak::Acquire(source, leaseOwner, acquired)) && acquired,
                    "source cloak records ownership before hiding the window");
                Check(!snowdesktop::dock_source_cloak::RecoverStale(source) && appCloaked(source),
                    "discovery preserves a live animation owner");
                Check(snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source, DWM_CLOAKED_APP) == 0 &&
                    snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source,
                        DWM_CLOAKED_APP | DWM_CLOAKED_SHELL) == DWM_CLOAKED_SHELL &&
                    snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source,
                        DWM_CLOAKED_APP | DWM_CLOAKED_INHERITED) == DWM_CLOAKED_INHERITED,
                    "task discovery retains an animating app while preserving other desktop and inherited exclusions");
                DestroyWindow(leaseOwner);
                Check(snowdesktop::dock_source_cloak::RecoverStale(source) && !appCloaked(source),
                    "discovery reveals an existing app after its animation owner disappears");
                const HWND reusedOwner = makeWindow();
                Check(reusedOwner && snowdesktop::dock_source_cloak::RegisterOwner(reusedOwner),
                    "reused owner fixture establishes a new generation");
                Check(SUCCEEDED(snowdesktop::dock_source_cloak::Acquire(source, reusedOwner, acquired)) && acquired,
                    "second recovery fixture acquires its source");
                Check(snowdesktop::dock_source_cloak::RegisterOwner(reusedOwner) &&
                    snowdesktop::dock_source_cloak::RecoverStale(source) && !appCloaked(source),
                    "a different owner generation cannot retain a stale source cloak despite a live recycled handle");
                DestroyWindow(reusedOwner);
                Check(snowdesktop::dock_source_cloak::TaskWindowCloakFlags(source, DWM_CLOAKED_APP) == DWM_CLOAKED_APP,
                    "an application's own unmarked cloak still excludes it from Dock task discovery");
            }
        }
    }
    if (source) DestroyWindow(source);
    if (destination) DestroyWindow(destination);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
}
}

int main()
{
    CheckContinuousOutline();
    CheckFramePixelAlignment();
    CheckRetainedWindowImage();
    if (!failures) std::cout << "Genie outline and cache-independent DWM image checks passed\n";
    return failures ? 1 : 0;
}
