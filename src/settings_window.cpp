#include "settings_window.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

// Forward declaration - intentionally commented out in the header to avoid
// dragging Windows.h dependency
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

SettingsWindow* g_settingsWindow = nullptr;

SettingsWindow::~SettingsWindow()
{
    Shutdown();
}

bool SettingsWindow::Init(HINSTANCE instance, ID3D11Device* device)
{
    instance_ = instance;
    device_ = device;
    device_->GetImmediateContext(&context_);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SnowDesktopSettingsWindow";
    RegisterClassExW(&wc);

    // Create window
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"SnowDesktop 设置",
        WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        windowWidth_, windowHeight_,
        nullptr, nullptr, instance, this);

    if (hwnd_ == nullptr) return false;

    if (!CreateSwapChain()) return false;

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_.Get(), context_.Get());

    SetupFonts();

    g_settingsWindow = this;

    // Center on screen
    RECT rc;
    GetWindowRect(hwnd_, &rc);
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd_, nullptr,
        (screenW - (rc.right - rc.left)) / 2,
        (screenH - (rc.bottom - rc.top)) / 2,
        0, 0, SWP_NOSIZE | SWP_NOZORDER);

    return true;
}

void SettingsWindow::Shutdown()
{
    g_settingsWindow = nullptr;

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupSwapChain();

    if (hwnd_ != nullptr)
    {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void SettingsWindow::Show()
{
    if (hwnd_ != nullptr)
    {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
}

void SettingsWindow::Render()
{
    if (hwnd_ == nullptr || !IsWindowVisible(hwnd_)) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Settings panel
    {
        ImGui::Begin("SnowDesktop 设置");

        ImGui::Text("Application Settings");

        if (ImGui::Button("关闭"))
        {
            ShowWindow(hwnd_, SW_HIDE);
        }

        ImGui::End();
    }

    ImGui::Render();

    const float clearColor[4] = { 0.12f, 0.14f, 0.18f, 1.0f };
    context_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    swapChain_->Present(1, 0);
}

bool SettingsWindow::CreateSwapChain()
{
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = windowWidth_;
    desc.Height = windowHeight_;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = factory->CreateSwapChainForHwnd(
        device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) return false;

    // Create RTV for back buffer 0
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;

    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv_);
    return SUCCEEDED(hr);
}

void SettingsWindow::CleanupSwapChain()
{
    rtv_.Reset();
    swapChain_.Reset();
    context_.Reset();
}

void SettingsWindow::SetupFonts()
{
    ImGuiIO& io = ImGui::GetIO();

    // Try loading Microsoft YaHei for CJK support
    std::string fontPath = "C:\\Windows\\Fonts\\msyh.ttc";
    if (FILE* f = fopen(fontPath.c_str(), "rb"))
    {
        fclose(f);
        io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f, nullptr,
            io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    else
    {
        // Fallback: enlarge default font to include basic range
        io.Fonts->AddFontDefault();
    }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_settingsWindow != nullptr && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
