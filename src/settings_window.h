#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <memory>
#include <string>

using Microsoft::WRL::ComPtr;

class SettingsWindow
{
public:
    SettingsWindow() = default;
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    bool Init(HINSTANCE instance, ID3D11Device* device);
    void Shutdown();
    void Show();
    bool IsVisible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_); }
    void Render();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    bool CreateSwapChain();
    void CleanupSwapChain();
    void SetupFonts();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    int windowWidth_ = 700;
    int windowHeight_ = 500;
};

// Global accessor for the WndProc hook
extern SettingsWindow* g_settingsWindow;
