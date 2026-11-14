#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>
#include <memory>

struct ID3D11Device;

namespace snowdesktop
{
// One desktop-window capture shared by the visible Dock hosts on the UI thread.
class DockRefractionSource
{
public:
    DockRefractionSource();
    ~DockRefractionSource();
    void Reset() noexcept;
    // True when consumers need to redraw, including when the last frame is lost.
    bool Update(ID3D11Device* device) noexcept;
    [[nodiscard]] ID2D1Bitmap1* Bitmap(ID2D1DeviceContext* context) noexcept;
    [[nodiscard]] RECT Bounds() const noexcept;

private:
    struct Capture;
    std::unique_ptr<Capture> capture_;
    ULONGLONG retryAt_ = 0;
};

class DockRefractionMaterial
{
public:
    bool Draw(ID2D1DeviceContext* context, DockRefractionSource& source,
        RECT panel, POINT screenOffset, float radius, float blur);

private:
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> displacement_;
    D2D1_SIZE_U size_{};
    float radius_ = 0;
};
}
