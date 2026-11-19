#pragma once

#include "icon_hbitmap_pixels.h"
#include "icon_beautify.h"
#include <memory>
#include <condition_variable>
#include <mutex>
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
    icon_beautify::EdgeReflectionLayers edgeReflection;

    size_t PayloadBytes() const
    {
        return (buffer.pixels.size() + edgeReflection.body.size() +
            edgeReflection.material.size()) * sizeof(std::uint32_t) +
            edgeReflection.coverage.size() + edgeReflection.subjectShadow.size();
    }

    size_t CapacityBytes() const
    {
        return (buffer.pixels.capacity() + edgeReflection.body.capacity() +
            edgeReflection.material.capacity()) * sizeof(std::uint32_t) +
            edgeReflection.coverage.capacity() + edgeReflection.subjectShadow.capacity();
    }
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
                icon_beautify::SourceKind::Image, &out.edgeReflection);
    }
    out.source = source;
    out.style = style;
    out.managedAlpha = managedAlpha;
    out.nativeControlSource = nativeControlSource;
    return true;
}

// Shell providers may never return, so shutdown must not join their workers.
// Only the new bounded CPU phase is fenced: no late provider can enter it
// after Close, and existing work leaves the renderer's static caches before
// process teardown. Workers retain this state independently of DesktopApp.
class PreparationWork
{
    void Finish()
    {
        { std::lock_guard lock(mutex_); --active_; }
        ready_.notify_all();
    }
    struct Completion
    {
        PreparationWork& owner;
        ~Completion() { owner.Finish(); }
    };
public:
    template<class Callback> bool Run(Callback&& callback)
    {
        {
            std::lock_guard lock(mutex_);
            if (closing_) return false;
            ++active_;
        }
        Completion completion{*this};
        std::forward<Callback>(callback)();
        return true;
    }
    void Close()
    {
        std::unique_lock lock(mutex_);
        closing_ = true;
        ready_.wait(lock, [this] { return active_ == 0; });
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    size_t active_ = 0;
    bool closing_ = false;
};

// UI-owned, optional ready pixels. The pixel budget bounds unshown images;
// eviction is just a cold miss and never changes what the first frame draws.
class Cache
{
public:
    Cache() = default;
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;
    ~Cache() { preparation_->Close(); }
    std::shared_ptr<PreparationWork> Preparation() const { return preparation_; }
    void Put(std::shared_ptr<const Prepared> prepared)
    {
        if (!prepared || !prepared->source || prepared->buffer.pixels.empty()) return;
        const auto count = prepared->PayloadBytes();
        Erase(prepared->source);
        if (count > kMaximumBytes) return;
        if (bytes_ > kMaximumBytes - count) Clear();
        const auto source = prepared->source;
        entries_.emplace(source, std::move(prepared));
        bytes_ += count;
    }
    std::shared_ptr<const Prepared> Take(HBITMAP source)
    {
        const auto found = entries_.find(source);
        if (found == entries_.end()) return {};
        auto result = std::move(found->second);
        bytes_ -= result->PayloadBytes();
        entries_.erase(found);
        return result;
    }
    void Erase(HBITMAP source) { (void)Take(source); }
    void Clear() { entries_.clear(); bytes_ = 0; }
    size_t Size() const { return entries_.size(); }
    size_t Bytes() const { return bytes_; }
    size_t CapacityBytes() const
    {
        size_t bytes = 0;
        for (const auto& [source, prepared] : entries_)
        { (void)source; bytes += prepared->CapacityBytes(); }
        return bytes;
    }
private:
    static constexpr size_t kMaximumBytes = 16u * 1024u * 1024u;
    std::shared_ptr<PreparationWork> preparation_ = std::make_shared<PreparationWork>();
    size_t bytes_ = 0;
    std::unordered_map<HBITMAP, std::shared_ptr<const Prepared>> entries_;
};
}
