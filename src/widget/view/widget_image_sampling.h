#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace snowdesktop::widget_runtime
{
// Only pass immutable decoded images. Remove their variants before evicting a
// source from its owning cache; entries retain source identity to avoid reuse
// of a released COM address. All methods run on the host's render thread.
class WidgetImageSamplingCache
{
public:
    enum class Outcome { Bypass, Miss, Hit };
    struct Result
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        D2D1_RECT_F source{};
        D2D1_INTERPOLATION_MODE interpolation = D2D1_INTERPOLATION_MODE_LINEAR;
        Outcome outcome = Outcome::Bypass;
    };

    Result Resolve(ID2D1DeviceContext* context, ID2D1Bitmap1* bitmap,
        const D2D1_RECT_F& destination, const D2D1_RECT_F& source,
        bool nearest = false) noexcept;
    void Remove(ID2D1Bitmap* source) noexcept;
    void Clear() noexcept;
    bool Contains(ID2D1Bitmap* bitmap) const noexcept;
    std::size_t Size() const noexcept { return entries_.size(); }
    // Derived textures only; original textures are reported by their owners.
    std::size_t RetainedBytes() const noexcept { return bytes_; }

private:
    struct Entry
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> source, bitmap;
        std::uint32_t width = 0, height = 0;
        std::uint64_t used = 0;
        std::size_t bytes = 0;
    };
    std::vector<Entry> entries_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> raster_;
    std::size_t bytes_ = 0;
    std::uint64_t clock_ = 0;
};
}
