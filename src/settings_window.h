#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct LayoutBackup
{
    std::wstring filename;
    std::wstring displayName;
    FILETIME timestamp;
};

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

    void SetReloadCallback(std::function<void()> callback) { reloadCallback_ = std::move(callback); }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    bool CreateSwapChain();
    void CleanupSwapChain();
    void SetupFonts();
    void DrawTitleBar();
    void DrawSidebar();
    void DrawBackupPage();
    void DrawGeneralPage();
    void DrawAboutPage();

    // Backup helpers
    std::wstring GetBackupDir() const;
    std::vector<LayoutBackup> ListBackups() const;
    bool SaveBackup(const std::wstring& name);
    bool RestoreBackup(const std::wstring& filename);
    bool DeleteBackup(const std::wstring& filename);

    bool IsAutoStartEnabled() const;
    void SetAutoStart(bool enable) const;

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    int windowWidth_ = 800;
    int windowHeight_ = 560;
    float dpiScale_ = 1.0f;
    int activePage_ = 0;
    char backupNameBuf_[128] = {};
    bool titleBarHovered_ = false;

    std::function<void()> reloadCallback_;
};

extern SettingsWindow* g_settingsWindow;
