#pragma once

#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
// UI-thread cache of completed decorative blur pixels. Lua still records its
// commands every draw. Only fully understood commands with immutable images
// qualify, so dynamic scripts and unsupported commands retain their behavior.
class WidgetBackgroundCache
{
public:
    static constexpr std::size_t MaximumEntries = 32;
    static constexpr std::size_t MaximumEntryBytes = 8 * 1024 * 1024;
    static constexpr std::size_t MaximumBytes = 32 * 1024 * 1024;
    enum class Outcome { Bypass, Miss, Hit };
    struct Result
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        Outcome outcome = Outcome::Bypass;
    };
    using ImmutableBitmap = std::function<bool(ID2D1Bitmap*)>;

    Result Resolve(std::wstring_view owner, ID2D1DeviceContext* context,
        ID2D1CommandList* commands, D2D1_RECT_F bounds, float blurRadius,
        const ImmutableBitmap& immutableBitmap) noexcept;
    void Erase(std::wstring_view owner) noexcept;
    void Clear() noexcept;
    std::size_t RetainedBytes() const noexcept { return retainedBytes_; }
    std::size_t Size() const noexcept { return entries_.size(); }

private:
    struct Entry
    {
        std::vector<std::byte> signature;
        // Keep image identities alive so allocator address reuse cannot hit an
        // old signature. Their estimated storage is charged to the budget too.
        std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap>> dependencies;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        std::size_t bytes = 0;
        std::uint64_t used = 0;
    };
    std::map<std::wstring, Entry, std::less<>> entries_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> rasterContext_;
    std::size_t retainedBytes_ = 0;
    std::uint64_t sequence_ = 0;
};
}
