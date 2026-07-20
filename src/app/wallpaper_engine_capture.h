/**
 * @file wallpaper_engine_capture.h
 * @brief Wallpaper Engine DXGI Hook 共享纹理接收端。
 */
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct WallpaperEngineFrame {
    RECT desktopRect{};
    HWND outputWindow = nullptr;
    ID2D1Bitmap1* bitmap = nullptr;
    std::size_t slotIndex = 0;
    unsigned long long frameNumber = 0;
};

enum class WallpaperEngineFrameState {
    ready,
    pending,
    error,
};

class WallpaperEngineCaptureSession {
public:
    WallpaperEngineCaptureSession();
    ~WallpaperEngineCaptureSession();

    WallpaperEngineCaptureSession(const WallpaperEngineCaptureSession&) = delete;
    WallpaperEngineCaptureSession& operator=(const WallpaperEngineCaptureSession&) = delete;

    bool EnsureStarted(DWORD rendererPid, ID3D11Device* d3dDevice,
        ID2D1DeviceContext* d2dContext, DWORD updateIntervalMs);
    void RequestFrame();
    WallpaperEngineFrameState TryAcquireLatestFrames(
        std::vector<WallpaperEngineFrame>& frames);
    void ReleaseFrames(bool consumed);
    /** @brief 中断当前连接并按有限退避计划自动重连，保留目标渲染进程。 */
    void RequestReconnect(const std::wstring& reason);
    void Stop();

    bool IsActiveFor(DWORD rendererPid) const;
    const std::wstring& LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
