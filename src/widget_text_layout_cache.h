#pragma once

#include "widget_view_tree.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <tuple>

namespace snowdesktop::widget_runtime
{
// UI-thread cache of fully configured, read-only declarative text layouts.
// The caller owns format configuration and must replace its format identity
// when that configuration changes. Keep the format alive to prevent ABA hits.
class WidgetTextLayoutCache
{
public:
    static constexpr std::size_t MaximumEntries = 128;
    static constexpr std::size_t MaximumTextLength = 2048;
    static constexpr std::size_t MaximumInputBytes = 64 * 1024;

    struct Options
    {
        float width = 0, height = 0;
        ViewFontStyle fontStyle = ViewFontStyle::Normal;
        ViewTextDirection direction = ViewTextDirection::Auto;
        ViewTextOverflow overflow = ViewTextOverflow::Ellipsis;
        DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING;
        std::optional<float> lineHeight;
        float letterSpacing = 0;
        bool underline = false;
        bool operator==(const Options&) const = default;
    };
    struct Counts
    {
        std::uint64_t hits = 0, misses = 0, bypasses = 0;
    };

    template<class Builder>
    Microsoft::WRL::ComPtr<IDWriteTextLayout> Resolve(
        std::wstring_view owner, std::string_view surface, std::string_view key,
        IDWriteTextFormat* format, const Options& options,
        std::wstring_view text, std::string_view locale, Builder&& build)
    {
        const IdentityView identity{ owner, surface, key };
        auto found = entries_.find(identity);
        const bool eligible = format && !owner.empty() && !key.empty() &&
            owner.size() <= 256 && surface.size() <= 32 && key.size() <= 256 &&
            locale.size() <= 64 && text.size() <= MaximumTextLength;
        if (!eligible)
        {
            Remove(found);
            ++counts_.bypasses;
            return build();
        }
        if (found != entries_.end())
        {
            auto& entry = found->second;
            if (entry.format.Get() == format && entry.options == options &&
                entry.text == text && entry.locale == locale)
            {
                entry.used = ++sequence_;
                ++counts_.hits;
                return entry.layout;
            }
        }
        ++counts_.misses;
        auto layout = build();
        Remove(found);
        if (!layout) return layout; // Never keep an unsuccessful replacement.
        const std::size_t bytes = (owner.size() + text.size()) * sizeof(wchar_t) +
            surface.size() + key.size() + locale.size();
        try
        {
            while (!entries_.empty() && (entries_.size() >= MaximumEntries ||
                    retainedInputBytes_ + bytes > MaximumInputBytes))
            {
                Remove(std::min_element(entries_.begin(), entries_.end(),
                    [](const auto& a, const auto& b) {
                        return a.second.used < b.second.used;
                    }));
            }
            Entry entry;
            entry.format = format;
            entry.layout = layout;
            entry.options = options;
            entry.text = text;
            entry.locale = locale;
            entry.bytes = bytes;
            entry.used = ++sequence_;
            entries_.emplace(Identity{ std::wstring(owner), std::string(surface),
                std::string(key) }, std::move(entry));
            retainedInputBytes_ += bytes;
        }
        catch (...)
        {
            // Cache bookkeeping is optional; the completed layout still draws.
        }
        return layout;
    }

    void Erase(std::wstring_view owner, std::string_view surface = {}) noexcept
    {
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (it->first.owner == owner &&
                (surface.empty() || it->first.surface == surface))
            {
                retainedInputBytes_ -= it->second.bytes;
                it = entries_.erase(it);
            }
            else ++it;
        }
    }
    void Clear() noexcept { entries_.clear(); retainedInputBytes_ = 0; }
    std::size_t Size() const noexcept { return entries_.size(); }
    // Only owned identity/text/locale code units, not COM or allocator memory.
    std::size_t RetainedInputBytes() const noexcept { return retainedInputBytes_; }
    Counts Statistics() const noexcept { return counts_; }

private:
    struct Identity
    {
        std::wstring owner;
        std::string surface, key;
    };
    struct IdentityView
    {
        std::wstring_view owner;
        std::string_view surface, key;
    };
    struct Less
    {
        using is_transparent = void;
        template<class A, class B> bool operator()(const A& a, const B& b) const
        {
            return std::tuple(std::wstring_view(a.owner),
                       std::string_view(a.surface), std::string_view(a.key)) <
                std::tuple(std::wstring_view(b.owner),
                       std::string_view(b.surface), std::string_view(b.key));
        }
    };
    struct Entry
    {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        Options options;
        std::wstring text;
        std::string locale;
        std::size_t bytes = 0;
        std::uint64_t used = 0;
    };
    using Entries = std::map<Identity, Entry, Less>;
    void Remove(Entries::iterator entry) noexcept
    {
        if (entry == entries_.end()) return;
        retainedInputBytes_ -= entry->second.bytes;
        entries_.erase(entry);
    }
    Entries entries_;
    Counts counts_;
    std::size_t retainedInputBytes_ = 0;
    std::uint64_t sequence_ = 0;
};
}
