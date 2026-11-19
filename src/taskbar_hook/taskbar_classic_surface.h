#pragma once

#include "taskbar_hook_protocol.h"
#include <memory>

namespace snowdesktop::taskbar_hook::native
{
inline constexpr wchar_t kClassicBackdropProperty[] = L"SnowDesktop.Taskbar.ClassicBackdrop.v1";

// Independent background window immediately below Explorer's taskbar. Native
// accent supplies blur/acrylic; DComp supplies solid tint, gradients and borders.
class ClassicSurface
{
public:
    ClassicSurface();
    ClassicSurface(const ClassicSurface&) = delete;
    ClassicSurface& operator=(const ClassicSurface&) = delete;
    ~ClassicSurface();
    HRESULT Draw(HWND window, const TargetAppearance& style);
    HRESULT Synchronize(HWND window);
    void Reset();
private:
    struct Composition;
    std::unique_ptr<Composition> composition_;
    HWND window_ = nullptr;
    HWND backdrop_ = nullptr;
};
}
