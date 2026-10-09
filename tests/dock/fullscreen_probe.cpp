// Interactive DXGI diagnostic; never activates or automates SnowDesktop.
// F1 windowed, F2 borderless, F3 DXGI exclusive, F4 exclusive mode switch,
// F5 decorated maximized, Escape windowed, Alt+F4 exit. No focus-reclaim loop.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
using Microsoft::WRL::ComPtr;
enum class Mode { Windowed, Borderless, Exclusive, ExclusiveModeSwitch, Maximized };
const wchar_t* ModeName(Mode mode)
{
    switch (mode)
    {
    case Mode::Borderless: return L"Borderless";
    case Mode::Exclusive: return L"DXGI exclusive requested";
    case Mode::ExclusiveModeSwitch: return L"DXGI exclusive + resolution switch requested";
    case Mode::Maximized: return L"Decorated maximized (control)";
    default: return L"Windowed";
    }
}
std::string Utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

class Probe
{
public:
    HWND window = nullptr;
    Mode mode = Mode::Windowed;
    bool resizing = false, needsResize = false;
    HRESULT lastResult = S_OK;
    RECT saved{100, 100, 1060, 740};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    std::ofstream log;
    std::wstring logPath;
    HWND previousForeground = nullptr;
    BOOL previousExclusive = FALSE;
    QUERY_USER_NOTIFICATION_STATE previousNotification = QUNS_ACCEPTS_NOTIFICATIONS;
    ULONGLONG started = GetTickCount64();

    ~Probe()
    {
        if (swap) swap->SetFullscreenState(FALSE, nullptr);
    }

    HRESULT Initialize()
    {
        wchar_t temp[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temp)) return HRESULT_FROM_WIN32(GetLastError());
        logPath = (std::filesystem::path(temp) / (L"SnowDesktop-fullscreen-probe-" + std::to_wstring(GetCurrentProcessId()) + L".log")).wstring();
        log.open(std::filesystem::path(logPath), std::ios::binary | std::ios::trunc);
        if (!log) return E_FAIL;
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 1;
        description.OutputWindow = window;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &description,
            swap.GetAddressOf(), device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (FAILED(result))
        {
            swap.Reset(); device.Reset(); context.Reset();
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &description,
                swap.GetAddressOf(), device.GetAddressOf(), nullptr, context.GetAddressOf());
        }
        lastResult = result;
        Record(L"initialize");
        return result;
    }

    void Record(const wchar_t* event)
    {
        if (!log) return;
        BOOL exclusive = FALSE;
        HRESULT stateResult = swap ? swap->GetFullscreenState(&exclusive, nullptr) : E_FAIL;
        QUERY_USER_NOTIFICATION_STATE notification = QUNS_ACCEPTS_NOTIFICATIONS;
        const HRESULT notificationResult = SHQueryUserNotificationState(&notification);
        HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &pid);
        wchar_t className[128]{};
        if (foreground) GetClassNameW(foreground, className, static_cast<int>(std::size(className)));
        std::wstring executable;
        HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (process)
        {
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &length)) executable.assign(path, length);
            CloseHandle(process);
        }
        RECT client{};
        GetClientRect(window, &client);
        std::wostringstream line;
        line << GetTickCount64() - started << L"ms event=" << event << L" requested=" << ModeName(mode)
            << L" dxgiFullscreen=" << exclusive << L" notification=" << static_cast<int>(notification)
            << L" ownForeground=" << (foreground == window) << L" minimized=" << IsIconic(window)
            << L" foreground=" << foreground << L" pid=" << pid << L" class=" << className
            << L" executable=\"" << executable << L"\" client=" << client.right << L'x' << client.bottom
            << L" dpi=" << GetDpiForWindow(window) << L" result=0x" << std::hex << static_cast<unsigned long>(lastResult)
            << L" stateResult=0x" << static_cast<unsigned long>(stateResult)
            << L" notificationResult=0x" << static_cast<unsigned long>(notificationResult) << L'\n';
        log << Utf8(line.str());
        log.flush();
        previousForeground = foreground;
        previousExclusive = exclusive;
        previousNotification = notification;
    }

    void SetMode(Mode requested)
    {
        if (!swap || resizing) return;
        resizing = true;
        if (mode == Mode::Windowed && !IsIconic(window)) GetWindowRect(window, &saved);
        lastResult = swap->SetFullscreenState(FALSE, nullptr);
        if (FAILED(lastResult)) { resizing = false; Record(L"leave-exclusive-failed"); return; }
        const HMONITOR monitorHandle = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(monitorHandle, &monitor)) { resizing = false; return; }
        mode = requested;
        const bool fullscreen = requested != Mode::Windowed && requested != Mode::Maximized;
        SetWindowLongPtrW(window, GWL_STYLE, fullscreen ? WS_POPUP | WS_VISIBLE : WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        RECT rect = fullscreen ? monitor.rcMonitor : saved;
        ShowWindow(window, SW_RESTORE);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        if (requested == Mode::Maximized) ShowWindow(window, SW_MAXIMIZE);
        if (requested == Mode::Exclusive || requested == Mode::ExclusiveModeSwitch)
        {
            lastResult = swap->SetFullscreenState(TRUE, nullptr);
            if (SUCCEEDED(lastResult) && requested == Mode::ExclusiveModeSwitch)
            {
                ComPtr<IDXGIOutput> output;
                DXGI_MODE_DESC desired{}, matched{};
                desired.Width = 1280; desired.Height = 720;
                desired.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desired.RefreshRate = {0, 0};
                lastResult = swap->GetContainingOutput(output.GetAddressOf());
                if (SUCCEEDED(lastResult)) lastResult = output->FindClosestMatchingMode(&desired, &matched, device.Get());
                if (SUCCEEDED(lastResult)) lastResult = swap->ResizeTarget(&matched);
            }
        }
        resizing = false;
        needsResize = true;
        Record(L"mode-switch");
    }

    HRESULT Render()
    {
        if (!swap) return E_UNEXPECTED;
        if (IsIconic(window) || resizing) { Sleep(50); return S_OK; }
        if (needsResize)
        {
            needsResize = false;
            lastResult = swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN,
                DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH | DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE);
            if (FAILED(lastResult)) { Record(L"resize-failed"); return lastResult; }
        }
        BOOL exclusive = FALSE;
        swap->GetFullscreenState(&exclusive, nullptr);
        QUERY_USER_NOTIFICATION_STATE notification = QUNS_ACCEPTS_NOTIFICATIONS;
        SHQueryUserNotificationState(&notification);
        if (GetForegroundWindow() != previousForeground || exclusive != previousExclusive || notification != previousNotification)
            Record(L"foreground-or-presentation-change");
        ComPtr<IDXGISurface1> surface;
        HRESULT result = swap->GetBuffer(0, IID_PPV_ARGS(surface.GetAddressOf()));
        if (FAILED(result)) return result;
        HDC dc = nullptr;
        result = surface->GetDC(TRUE, &dc);
        if (FAILED(result)) return result;
        DXGI_SURFACE_DESC description{};
        surface->GetDesc(&description);
        RECT bounds{0, 0, static_cast<LONG>(description.Width), static_cast<LONG>(description.Height)};
        HBRUSH background = CreateSolidBrush(exclusive ? RGB(40, 70, 42) : mode == Mode::Borderless ? RGB(42, 48, 85) : RGB(35, 35, 40));
        FillRect(dc, &bounds, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 245));
        std::wostringstream text;
        text << L"SnowDesktop Fullscreen Probe\n\nF1: Windowed    F2: Borderless    F3: DXGI exclusive\n"
            << L"F4: Exclusive + 1280x720 mode switch    F5: Decorated maximized\n"
            << L"Esc: Return to windowed    Alt+F4: Exit\n\nRequested: " << ModeName(mode)
            << L"\nDXGI GetFullscreenState: " << (exclusive ? L"TRUE" : L"FALSE")
            << L"\nWindows notification state: " << static_cast<int>(notification) << L" (3 = exclusive D3D hint)"
            << L"\nOwn foreground: " << (GetForegroundWindow() == window ? L"YES" : L"NO")
            << L"\nBack buffer: " << description.Width << L" x " << description.Height
            << L"\nLast operation HRESULT: 0x" << std::hex << static_cast<unsigned long>(lastResult)
            << L"\n\nWindows fullscreen optimizations may change effective presentation.\n"
            << L"This probe never reclaims foreground after losing it.\n\nLog: " << logPath;
        bounds.left += 24; bounds.top += 24; bounds.right -= 24;
        const auto content = text.str();
        DrawTextW(dc, content.c_str(), static_cast<int>(content.size()), &bounds, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        result = surface->ReleaseDC(nullptr);
        if (FAILED(result)) return result;
        result = swap->Present(1, 0);
        if (result == DXGI_STATUS_OCCLUDED) Sleep(50);
        return result;
    }
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto* probe = reinterpret_cast<Probe*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        probe = static_cast<Probe*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        probe->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(probe));
    }
    if (probe)
    {
        if (message == WM_KEYDOWN && !(lp & (1LL << 30)))
        {
            if (wp >= VK_F1 && wp <= VK_F5) probe->SetMode(static_cast<Mode>(wp - VK_F1));
            else if (wp == VK_ESCAPE) probe->SetMode(Mode::Windowed);
            return 0;
        }
        if (message == WM_SIZE && wp != SIZE_MINIMIZED && !probe->resizing) probe->needsResize = true;
        if (message == WM_ACTIVATEAPP) probe->Record(wp ? L"activate-app" : L"deactivate-app");
        if (message == WM_KILLFOCUS) probe->Record(L"kill-focus");
        if (message == WM_DISPLAYCHANGE) probe->Record(L"display-change");
        if (message == WM_CLOSE)
        {
            if (probe->swap) probe->swap->SetFullscreenState(FALSE, nullptr);
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    }
    return DefWindowProcW(window, message, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool checkRenderer = arguments && std::wstring(arguments).find(L"--check-renderer") != std::wstring::npos;
    Probe probe;
    WNDCLASSW registration{};
    registration.lpfnWndProc = WindowProc;
    registration.hInstance = instance;
    registration.lpszClassName = L"SnowDesktopFullscreenProbe";
    registration.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&registration)) return 1;
    HWND window = CreateWindowExW(0, registration.lpszClassName, L"SnowDesktop Fullscreen Probe", WS_OVERLAPPEDWINDOW,
        100, 100, 960, 640, nullptr, nullptr, instance, &probe);
    if (!window) return 1;
    HRESULT result = probe.Initialize();
    if (checkRenderer)
    {
        if (SUCCEEDED(result)) result = probe.Render();
        probe.lastResult = result;
        probe.Record(L"hidden-renderer-check");
        DestroyWindow(window);
        return SUCCEEDED(result) ? 0 : 1;
    }
    if (FAILED(result))
    {
        MessageBoxW(window, L"Could not initialize the DXGI renderer. See the probe log in the temporary directory.",
            L"Fullscreen Probe", MB_OK | MB_ICONERROR);
        DestroyWindow(window);
        return 1;
    }
    ShowWindow(window, SW_SHOW);
    MSG message{};
    while (message.message != WM_QUIT)
    {
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        else
        {
            result = probe.Render();
            if (FAILED(result)) { probe.lastResult = result; probe.Record(L"render-failed"); Sleep(100); }
        }
    }
    return 0;
}
