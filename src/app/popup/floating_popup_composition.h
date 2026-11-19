#pragma once

#include <dcomp.h>
#include <wrl/client.h>

namespace snowdesktop
{
// Device-bound surfaces for the live popup and its separate stack indicator.
// Window geometry and placement in the visual tree belong to the presenter.
class FloatingPopupCompositionSurfaces
{
public:
    IDCompositionSurface* Content() const { return content_.surface.Get(); }
    IDCompositionSurface* Indicator() const { return indicator_.surface.Get(); }

    HRESULT PrepareContent(IDCompositionDevice2* device, const RECT& bounds);
    HRESULT PrepareIndicator(IDCompositionDevice2* device, SIZE size);
    void Release();
    bool BeginLocalRecovery();
    void EndLocalRecovery() { recovering_ = false; }

private:
    struct Layer
    {
        Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
        UINT width = 0;
        UINT height = 0;

        HRESULT Prepare(IDCompositionDevice2* device, UINT newWidth, UINT newHeight);
    };

    Layer content_;
    Layer indicator_;
    bool recovering_ = false;
};
}
