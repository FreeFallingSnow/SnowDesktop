#pragma once

#include <dcomp.h>
#include <wrl/client.h>

namespace snowdesktop::desktop_composition_effect
{
// SetEffect gives this subtree an intermediate composition surface. The
// identity matrix deliberately preserves its existing colors and alpha.
// This is a presentation-path experiment, not a brightness compensation.
inline HRESULT ApplyIdentity(IDCompositionDesktopDevice* device, IDCompositionVisual2* visual)
{
    if (!device || !visual) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IDCompositionDevice3> effects;
    HRESULT result = device->QueryInterface(IID_PPV_ARGS(&effects));
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<IDCompositionColorMatrixEffect> effect;
    result = effects->CreateColorMatrixEffect(&effect);
    if (FAILED(result)) return result;
    const D2D1_MATRIX_5X4_F identity{
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
        0.f, 0.f, 0.f, 0.f};
    if (FAILED(result = effect->SetMatrix(identity)) ||
        FAILED(result = effect->SetAlphaMode(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED)) ||
        FAILED(result = effect->SetClampOutput(FALSE)) ||
        FAILED(result = effect->SetInput(0, nullptr, 0))) return result;
    return visual->SetEffect(effect.Get());
}
}
