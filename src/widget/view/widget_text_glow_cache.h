#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace snowdesktop::widget_runtime
{
// UI-thread cache of immutable glow images. Recorded frames retain their own
// bitmap references while the renderer consumes them on the render thread.
class WidgetTextGlowCache
{
public:
    void Draw(ID2D1DeviceContext& context, IDWriteTextLayout& layout,
        ID2D1SolidColorBrush& brush, D2D1_POINT_2F origin, float radius);
    void Clear() noexcept;

private:
    struct Entry
    {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        D2D1_RECT_F bounds{};
        D2D1_COLOR_F color{};
        float radius = 0.0f;
        std::size_t bytes = 0;
        std::uint64_t used = 0;
    };

    bool Rasterize(IDWriteTextLayout& layout, Entry& entry);

    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> raster_;
    std::vector<Entry> entries_;
    std::size_t retainedBytes_ = 0;
    std::uint64_t sequence_ = 0;
};
}
