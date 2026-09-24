#pragma once

#include "taskbar_hook_protocol.h"
#include <d2d1.h>
#include <dcomp.h>
#include <wrl/client.h>

namespace snowdesktop::taskbar_hook::native
{
// Visual below taskbar child windows: never intercepts pointer/keyboard input.
class ClassicSurface
{
public:
    HRESULT Draw(HWND window, const TargetAppearance& style);
    void Reset();
private:
    Microsoft::WRL::ComPtr<IDCompositionDevice> device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    HWND window_ = nullptr;
};
}
