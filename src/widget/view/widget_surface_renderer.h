#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#include <memory>
#include <string>
#include <vector>

struct ID2D1Device;
struct ID2D1DeviceContext;
struct ID2D1CommandList;
struct IDCompositionVisual2;
struct ID3D11Device;

namespace snowdesktop::widget_runtime
{
struct WidgetSurfaceTarget;

struct WidgetImageSpin
{
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    D2D1_RECT_F destination{};
    D2D1_RECT_F source{};
    D2D1_INTERPOLATION_MODE interpolation = D2D1_INTERPOLATION_MODE_LINEAR;
    float opacity = 1.0f;
    float degreesPerSecond = 0.0f;
};

// UI-owned command recording with a separate raster/DirectComposition owner.
// Lua, hit regions and HWND state never leave the calling UI thread.
class WidgetSurfaceRenderer final
{
public:
    using Target = std::shared_ptr<WidgetSurfaceTarget>;
    struct Result
    {
        Target target;
        std::wstring owner;
        HRESULT status = S_OK;
    };

    WidgetSurfaceRenderer();
    ~WidgetSurfaceRenderer();
    WidgetSurfaceRenderer(const WidgetSurfaceRenderer&) = delete;
    WidgetSurfaceRenderer& operator=(const WidgetSurfaceRenderer&) = delete;

    bool Initialize(ID2D1Device* device, HWND notifyWindow, ID3D11Device* graphicsDevice = nullptr);
    void Stop();
    HRESULT BeginRecording(ID2D1DeviceContext** context,
        ID2D1CommandList** commands);
    HRESULT EndRecording(ID2D1CommandList* commands);
    Target CreateTarget(const std::wstring& owner, UINT width, UINT height,
        IDCompositionVisual2** visual, const std::wstring& componentOwner = {});
    bool Submit(const Target& target, ID2D1CommandList* commands,
        const RECT& bounds, WidgetImageSpin imageSpin);
    void Cancel(const Target& target);
    std::vector<Result> Drain();
    static UINT ReadyMessage();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
