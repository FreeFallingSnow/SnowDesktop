#pragma once

#include "../background_work.h"
#include <algorithm>
#include <cstdint>

namespace snowdesktop::initial_icon_bitmap
{
// A running window can supply its own pixels even when its executable has no
// embedded icon. Packaged identities must not preview a shared host executable.
template<class Resource, class Window>
auto ReadRunning(bool hasAppIdentity, Resource resource, Window window)
{
    if (!hasAppIdentity)
        if (auto bitmap = resource()) return bitmap;
    return window();
}

// Reuse only successful shortcut pixels as the next first image. The caller
// still queues Shell refinement, so changes to a shortcut's target/icon source
// remain observable even if the .lnk itself has not changed. No disk persistence.
class ShortcutCache final
{
public:
    explicit ShortcutCache(std::size_t capacity = 128) : capacity_(capacity) {}

    std::shared_ptr<BackgroundBitmap> Get(const std::wstring& key)
    {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(key);
        if (found == entries_.end()) return {};
        found->second.used = ++clock_;
        return Clone(found->second.image->bitmap, found->second.image->size);
    }

    void Put(const std::wstring& key, HBITMAP bitmap, SIZE size, bool refined = true)
    {
        if (key.empty() || !bitmap || !capacity_) return;
        auto image = Clone(bitmap, size);
        if (!image) return;
        std::lock_guard lock(mutex_);
        const auto previous = entries_.find(key);
        if (!refined && previous != entries_.end() && previous->second.refined) return;
        if (!entries_.contains(key) && entries_.size() >= capacity_)
        {
            const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            entries_.erase(oldest);
        }
        entries_.insert_or_assign(key, Entry{std::move(image), ++clock_, refined});
    }

private:
    static std::shared_ptr<BackgroundBitmap> Clone(HBITMAP bitmap, SIZE size)
    {
        auto image = std::make_shared<BackgroundBitmap>();
        image->bitmap = static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        if (!image->bitmap) return {};
        image->size = size;
        return image;
    }
    struct Entry { std::shared_ptr<BackgroundBitmap> image; std::uint64_t used; bool refined; };
    const std::size_t capacity_;
    std::mutex mutex_;
    std::uint64_t clock_ = 0;
    std::unordered_map<std::wstring, Entry> entries_;
};
}
