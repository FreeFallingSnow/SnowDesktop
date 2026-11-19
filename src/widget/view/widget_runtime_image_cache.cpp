#include "widget_runtime_image_cache.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
using Microsoft::WRL::ComPtr;

static std::string BindRuntimeImageToken(
    std::string_view token, std::wstring_view widgetId)
{
    if (!snowdesktop::widget_runtime::IsWidgetRuntimeImageToken(token) ||
        widgetId.empty())
        return {};
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t character : widgetId)
    {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= prime;
    }
    std::string result(token);
    result.push_back(':');
    result.append(std::to_string(hash));
    return result.size() <= 64 ? result : std::string{};
}

void WidgetRuntimeImageCache::EraseBitmap(const std::string& key)
{
    if (const auto found = bitmaps_.find(key);
        found != bitmaps_.end())
    {
        sampling_.Remove(found->second.Get());
        bitmaps_.erase(found);
    }
}

std::string WidgetRuntimeImageCache::Register(const std::wstring& widgetId,
    std::string_view token, std::shared_ptr<const WidgetRuntimeImagePixels> pixels,
    std::string source, bool preserveExisting)
{
    const std::string boundToken = BindRuntimeImageToken(token, widgetId);
    if (boundToken.empty() || !pixels ||
        !snowdesktop::widget_runtime::IsValidWidgetRuntimeImage(*pixels) ||
        source.empty())
        return {};
    constexpr std::size_t maximumBytes = 64 * 1024 * 1024;
    std::size_t bytes = pixels->bgraPremultiplied.size();
    for (const auto& [key, resource] : images_)
        if (key != boundToken && resource.pixels) bytes += resource.pixels->bgraPremultiplied.size();
    if (preserveExisting && (bytes > maximumBytes ||
        (!images_.contains(boundToken) &&
            (images_.size() >= 128 ||
             std::count_if(images_.begin(), images_.end(),
                 [&widgetId](const auto& entry) {
                     return entry.second.ownerWidgetId == widgetId;
                 }) >= 16))))
        return {};
    while (bytes > maximumBytes && !images_.empty())
    {
        auto victim = std::find_if(images_.begin(), images_.end(),
            [&boundToken](const auto& item) { return item.first != boundToken; });
        if (victim == images_.end()) break;
        if (victim->second.pixels) bytes -= victim->second.pixels->bgraPremultiplied.size();
        EraseBitmap(victim->first);
        images_.erase(victim);
    }
    const std::size_t ownerCount = static_cast<std::size_t>(std::count_if(
        images_.begin(), images_.end(),
        [&widgetId](const auto& item) {
            return item.second.ownerWidgetId == widgetId;
        }));
    if (!preserveExisting &&
        ((!images_.contains(boundToken) && ownerCount >= 16) ||
         images_.size() >= 128))
    {
        std::vector<std::string> removed;
        for (const auto& [key, resource] : images_)
        {
            if (resource.ownerWidgetId == widgetId) removed.push_back(key);
        }
        for (const auto& key : removed)
        {
            images_.erase(key);
            EraseBitmap(key);
        }
    }
    if (!images_.contains(boundToken) &&
        images_.size() >= 128)
    {
        const std::string evicted = images_.begin()->first;
        images_.erase(evicted);
        EraseBitmap(evicted);
    }
    images_.insert_or_assign(
        boundToken, RuntimeImageResource{
            std::move(pixels), widgetId, std::move(source) });
    return boundToken;
}

ID2D1Bitmap1* WidgetRuntimeImageCache::LoadBitmap(ID2D1DeviceContext* context,
    const std::wstring& widgetId, std::string_view token)
{
    if (!context || widgetId.empty() || !IsWidgetRuntimeImageToken(token))
        return nullptr;
    const std::string key(token);
    const auto source = images_.find(key);
    if (source == images_.end() || !source->second.pixels ||
        source->second.ownerWidgetId != widgetId)
        return nullptr;
    if (const auto cached = bitmaps_.find(key);
        cached != bitmaps_.end()) return cached->second.Get();
    const auto& pixels = *source->second.pixels;
    D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(context->CreateBitmap(
            D2D1::SizeU(pixels.width, pixels.height),
            pixels.bgraPremultiplied.data(), pixels.stride,
            &properties, &bitmap)) || !bitmap)
        return nullptr;
    ID2D1Bitmap1* result = bitmap.Get();
    bitmaps_.emplace(key, std::move(bitmap));
    return result;
}

void WidgetRuntimeImageCache::RemoveWidget(const std::wstring& widgetId)
{
    std::erase_if(images_, [&](const auto& entry) {
        if (entry.second.ownerWidgetId != widgetId) return false;
        EraseBitmap(entry.first);
        return true;
    });
}

void WidgetRuntimeImageCache::RemoveSource(std::string_view source)
{
    std::erase_if(images_, [&](const auto& entry) {
        if (entry.second.source != source) return false;
        EraseBitmap(entry.first);
        return true;
    });
}

void WidgetRuntimeImageCache::Retain(const std::wstring& widgetId, std::string_view source,
    std::span<const std::string_view> tokens)
{
    std::erase_if(images_, [&](const auto& entry) {
        if (entry.second.ownerWidgetId != widgetId || entry.second.source != source)
            return false;
        const bool keep = std::any_of(tokens.begin(), tokens.end(), [&](std::string_view token) {
            return !token.empty() && BindRuntimeImageToken(token, widgetId) == entry.first;
        });
        if (!keep) EraseBitmap(entry.first);
        return !keep;
    });
}

std::shared_ptr<const WidgetRuntimeImagePixels> WidgetRuntimeImageCache::GetPixels(
    const std::wstring& widgetId, const std::string& token) const
{
    const auto found = images_.find(token);
    return found != images_.end() && found->second.ownerWidgetId == widgetId
        ? found->second.pixels : nullptr;
}

bool WidgetRuntimeImageCache::ContainsBitmap(ID2D1Bitmap* bitmap) const
{
    return std::any_of(bitmaps_.begin(), bitmaps_.end(), [&](const auto& entry) {
        return static_cast<ID2D1Bitmap*>(entry.second.Get()) == bitmap;
    });
}

void WidgetRuntimeImageCache::ClearBitmaps()
{
    for (const auto& [key, bitmap] : bitmaps_)
        sampling_.Remove(bitmap.Get());
    bitmaps_.clear();
}

WidgetRuntimeImageCache::Usage WidgetRuntimeImageCache::Measure() const
{
    Usage usage;
    std::unordered_set<const WidgetRuntimeImagePixels*> unique;
    std::map<std::wstring_view, std::unordered_set<const WidgetRuntimeImagePixels*>> ownerPixels;
    for (const auto& [key, source] : images_)
    {
        auto& owner = usage.owners[source.ownerWidgetId];
        ++owner.sources;
        if (!source.pixels) continue;
        const auto* pixels = source.pixels.get();
        const auto size = pixels->bgraPremultiplied.size();
        if (ownerPixels[source.ownerWidgetId].insert(pixels).second)
            owner.pixelBytes += size;
        if (unique.insert(pixels).second)
            usage.uniquePixelBytes += size;
    }
    for (const auto& [key, bitmap] : bitmaps_)
    {
        if (!bitmap) continue;
        const auto size = bitmap->GetPixelSize();
        const auto bytes = static_cast<std::size_t>(size.width) * size.height * 4;
        usage.bitmapBytes += bytes;
        const auto source = images_.find(key);
        if (source != images_.end())
            usage.owners[source->second.ownerWidgetId].bitmapBytes += bytes;
    }
    return usage;
}
}
