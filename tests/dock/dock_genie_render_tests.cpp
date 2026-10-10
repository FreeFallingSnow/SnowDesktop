#include "dock/dock_genie_rules.h"
#include "dock/dock_minimize_protocol.h"
#include "dock/dock_window_shared_thumbnail.h"
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
                int restores = 0;
                const bool restoring = transition.StartRestore(source, dock,
                    [&](HWND window, DockWindowRestoreTransitionPhase phase) {
                        if (phase == DockWindowRestoreTransitionPhase::RequestRestore)
                        {
                            ++restores;
                            ShowWindow(window, SW_SHOWNOACTIVATE);
                        }
                    });
                std::wcout << presentation << L'\n';
                Check(restoring && presentation.find(L"effective=3") != std::wstring::npos &&
                    presentation.find(L"snapshot=dwm-shared-window") != std::wstring::npos,
                    "first restore uses Genie and a DWM image without a prior minimize animation");
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
                };
                if (restoring) drain();
                Check(restores == 1 && !IsIconic(source), "restore handoff runs exactly once");
                const auto appCloaked = [](HWND window) {
                    DWORD flags = 0;
                    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &flags, sizeof(flags))) &&
                        (flags & DWM_CLOAKED_APP) != 0;
                };
                Check(!appCloaked(source), "restore handoff releases only the animation's temporary source cloak");
                const DWORD preparationStart = GetTickCount();
                const bool minimizing = transition.StartExternalMinimize(source, dock,
                    preparationStart + snowdesktop::dock_minimize::kRequestTimeoutMs);
                Check(minimizing && presentation.find(L"snapshot=dwm-shared-window") != std::wstring::npos,
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
    CheckRetainedWindowImage();
    if (!failures) std::cout << "Genie outline and cache-independent DWM image checks passed\n";
    return failures ? 1 : 0;
}
