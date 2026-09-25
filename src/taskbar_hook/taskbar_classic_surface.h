#pragma once

#include "taskbar_hook_protocol.h"
#include <d2d1.h>
#include <dcomp.h>
#include <wrl/client.h>

namespace snowdesktop::taskbar_hook::native
{
inline constexpr wchar_t kClassicBackdropProperty[] = L"SnowDesktop.Taskbar.ClassicBackdrop.v1";

// Independent background window immediately below Explorer's taskbar. Native
// accent supplies blur/acrylic; DComp supplies solid tint, gradients and borders.
class ClassicSurface
{
public:
    ClassicSurface() = default;
    ClassicSurface(const ClassicSurface&) = delete;
    ClassicSurface& operator=(const ClassicSurface&) = delete;
    ~ClassicSurface() { Reset(); }
    HRESULT Draw(HWND window, const TargetAppearance& style);
    HRESULT Synchronize(HWND window);
    void Reset();
private:
    Microsoft::WRL::ComPtr<IDCompositionDevice> device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> visual_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    HWND window_ = nullptr;
    HWND backdrop_ = nullptr;
};
}
