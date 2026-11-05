#pragma once

#include <windows.h>
#include <memory>
#include <string>
#include <vector>

struct ID2D1Device;
struct ID2D1DeviceContext;
struct ID2D1CommandList;
struct IDCompositionVisual2;

namespace snowdesktop::widget_runtime
{
struct WidgetSurfaceTarget;

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

    bool Initialize(ID2D1Device* device, HWND notifyWindow);
    void Stop();
    HRESULT BeginRecording(ID2D1DeviceContext** context,
        ID2D1CommandList** commands);
    HRESULT EndRecording(ID2D1CommandList* commands);
    Target CreateTarget(const std::wstring& owner, UINT width, UINT height,
        IDCompositionVisual2** visual);
    bool Submit(const Target& target, ID2D1CommandList* commands,
        const RECT& bounds);
    void Cancel(const Target& target);
    std::vector<Result> Drain();
    static UINT ReadyMessage();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
