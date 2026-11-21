#pragma once

#include "widget_image_sampling.h"
#include "widget/runtime/widget_runtime_image.h"

#include <d2d1_1.h>
#include <wrl/client.h>

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
class WidgetRuntimeImageCache
{
public:
    explicit WidgetRuntimeImageCache(WidgetImageSamplingCache& sampling) noexcept : sampling_(sampling) {}

    std::string Register(const std::wstring& widgetId, std::string_view token,
        std::shared_ptr<const WidgetRuntimeImagePixels> pixels, std::string source,
        bool preserveExisting);
    void RemoveWidget(const std::wstring& widgetId);
    void RemoveSource(std::string_view source);
    void Retain(const std::wstring& widgetId, std::string_view source,
        std::span<const std::string_view> tokens);
    std::shared_ptr<const WidgetRuntimeImagePixels> GetPixels(
        const std::wstring& widgetId, const std::string& token) const;
    ID2D1Bitmap1* LoadBitmap(ID2D1DeviceContext* context, const std::wstring& widgetId,
        std::string_view token);
    bool ContainsBitmap(ID2D1Bitmap* bitmap) const;
    void ClearBitmaps();

    struct OwnerUsage
    {
        std::size_t sources = 0;
        std::size_t pixelBytes = 0;
        std::size_t bitmapBytes = 0;
    };

    struct Usage
    {
        std::map<std::wstring, OwnerUsage> owners;
        std::size_t bitmapBytes = 0;
        std::size_t uniquePixelBytes = 0;
    };

    Usage Measure() const;

private:
    void EraseBitmap(const std::string& key);
    void MakeRoomForClipboardImage(const std::wstring& widgetId,
        const WidgetRuntimeImagePixels& incoming);

    struct RuntimeImageResource
    {
        std::shared_ptr<const snowdesktop::widget_runtime::
            WidgetRuntimeImagePixels> pixels;
        std::wstring ownerWidgetId;
        std::string source;
        std::uint64_t clipboardRevision = 0;
    };

    WidgetImageSamplingCache& sampling_;
    std::uint64_t clipboardRevision_ = 0;
    std::unordered_map<std::string, RuntimeImageResource> images_;
    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> bitmaps_;
};
}
