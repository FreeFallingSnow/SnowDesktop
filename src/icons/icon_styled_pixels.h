#pragma once

#include "icon_hbitmap_pixels.h"
#include "icon_beautify.h"
#include <memory>
#include <unordered_map>
#include <utility>

namespace snowdesktop::icon_styled_pixels
{
// CPU-only snapshot. The source handle is borrowed solely to validate delivery;
// the owning Shell result retains it until the UI accepts or rejects the image.
// An accepted model invalidates the snapshot before releasing its source.
struct Prepared
{
    HBITMAP source = nullptr;
    IconBeautifySettings style;
    bool managedAlpha = false;
    bool nativeControlSource = false;
    bool needsGlassBackdrop = false;
    icon_bitmap_pixels::Buffer buffer;
};

// Shared by worker prewarming and the synchronous cold-miss fallback. Keep the
// read direction, alpha policy and processing order identical in both paths.
inline bool Prepare(HBITMAP source, const IconBeautifySettings& style,
    bool managedAlpha, bool nativeControlSource, Prepared& out)
{
    out = {};
    auto& buffer = out.buffer;
    if (!icon_bitmap_pixels::ReadHBitmap(source, buffer, !managedAlpha))
        return false;
    if (style.enabled)
    {
        if (!icon_beautify::PrepareCanvas(buffer.pixels, buffer.width, buffer.height, true))
            return false;
        const auto edgeFill = style.mode == 0
            ? icon_beautify::DetectPlateFill(buffer.pixels, buffer.width, buffer.height)
            : std::nullopt;
        out.needsGlassBackdrop = style.glassEnabled && !edgeFill;
        buffer.pixels = icon_beautify::Render(buffer.pixels, buffer.width, buffer.height,
            style, edgeFill, nativeControlSource ? icon_beautify::SourceKind::NativeControl :
                icon_beautify::SourceKind::Image);
    }
    out.source = source;
    out.style = style;
    out.managedAlpha = managedAlpha;
    out.nativeControlSource = nativeControlSource;
    return true;
}

// UI-owned, optional ready pixels. The pixel budget bounds unshown images;
// eviction is just a cold miss and never changes what the first frame draws.
class Cache
{
public:
    void Put(std::shared_ptr<const Prepared> prepared)
    {
        if (!prepared || !prepared->source || prepared->buffer.pixels.empty()) return;
        const auto count = prepared->buffer.pixels.size();
        Erase(prepared->source);
        if (count > kMaximumPixels) return;
        if (pixels_ > kMaximumPixels - count) Clear();
        const auto source = prepared->source;
        entries_.emplace(source, std::move(prepared));
        pixels_ += count;
    }
    std::shared_ptr<const Prepared> Take(HBITMAP source)
    {
        const auto found = entries_.find(source);
        if (found == entries_.end()) return {};
        auto result = std::move(found->second);
        pixels_ -= result->buffer.pixels.size();
        entries_.erase(found);
        return result;
    }
    void Erase(HBITMAP source) { (void)Take(source); }
    void Clear() { entries_.clear(); pixels_ = 0; }
    size_t Size() const { return entries_.size(); }
private:
    static constexpr size_t kMaximumPixels = 4u * 1024u * 1024u; // 16 MiB of pixel payload.
    size_t pixels_ = 0;
    std::unordered_map<HBITMAP, std::shared_ptr<const Prepared>> entries_;
};
}
