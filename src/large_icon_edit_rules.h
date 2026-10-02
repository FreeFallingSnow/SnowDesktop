#pragma once
#include "large_icon_settings.h"
#include <utility>

namespace snowdesktop::large_icon_edit_rules
{
enum class EntryAccess { Hidden, Unlock, Edit };

inline constexpr EntryAccess ResolveEntryAccess(bool bridgeAvailable, bool unlocked)
{
    if (!bridgeAvailable) return EntryAccess::Hidden;
    return unlocked ? EntryAccess::Edit : EntryAccess::Unlock;
}

inline bool CanStore(const std::optional<LargeIconConfig>& config, bool unlocked, bool desktop)
{
    // A null configuration is the always-available return to ordinary icons.
    return !config || (unlocked && desktop && ValidateLargeIconConfig(*config));
}

inline std::string CheckRequest(LargeIconEditSession& edit, const LargeIconSettingsRequest& request, bool unlocked)
{
    if (!unlocked) { edit.preview.reset(); edit.previews.clear(); return "largeIcon.locked"; }
    if (request.action == "read") return {};
    if (edit.token == 0 || edit.key != request.key || edit.token != request.session ||
        (request.action != "status" && edit.revision != request.revision)) return "largeIcon.stale";
    if (request.action != "status" && request.action != "preview" && request.action != "commit" &&
        request.action != "cancel" && request.action != "refresh" && request.action != "import") return "largeIcon.invalid";
    return {};
}

inline bool Contains(const LargeIconEditSession& edit, std::wstring_view key)
{
    return edit.key == key || std::find(edit.keys.begin(), edit.keys.end(), key) != edit.keys.end();
}

// Field patches preserve each item's sources, cached assets and untouched settings.
inline bool EqualValue(const JsonValue& a, const JsonValue& b)
{
    if (a.type != b.type) return false;
    if (a.IsNumber()) return a.number == b.number;
    if (a.IsBoolean()) return a.boolean == b.boolean;
    if (a.IsString()) return a.string == b.string;
    if (a.IsArray())
        return a.array.size() == b.array.size() && std::equal(a.array.begin(), a.array.end(), b.array.begin(), EqualValue);
    if (a.IsObject())
    {
        if (a.object.size() != b.object.size()) return false;
        for (const auto& [key, value] : a.object)
            if (const auto other = b.Find(key); !other || !EqualValue(value, *other)) return false;
    }
    return true;
}

inline std::vector<std::string> ChangedFields(const LargeIconConfig& a, const LargeIconConfig& b)
{
    JsonValue first, second;
    std::vector<std::string> result;
    if (!ParseJson(EncodeLargeIconConfig(a), first) || !ParseJson(EncodeLargeIconConfig(b), second)) return result;
    for (const auto& [name, value] : first.object)
        if (const auto other = second.Find(name); other && !EqualValue(value, *other)) result.push_back(name);
    return result;
}

inline bool Patch(LargeIconConfig& target, const LargeIconConfig& draft, const std::vector<std::string>& fields)
{
    JsonValue merged, source;
    if (!ParseJson(EncodeLargeIconConfig(target), merged) || !ParseJson(EncodeLargeIconConfig(draft), source)) return false;
    for (const auto& name : fields)
    {
        const auto value = source.Find(name);
        if (!value || !merged.Find(name) || name == "version" || name == "cachedCover" || name == "image" || name == "foregroundImage") return false;
        merged.object[name] = *value;
    }
    LargeIconConfig candidate;
    if (!DecodeLargeIconConfig(merged, candidate)) return false;
    if (IsLargeIconFill(candidate) && candidate.effect == 2)
    {
        if (std::find(fields.begin(), fields.end(), "effect") != fields.end()) return false;
        candidate.effect = 3;
    }
    if (!ValidateLargeIconConfig(candidate)) return false;
    target = std::move(candidate);
    return true;
}

template<class Item, class Span> struct Change
{
    Item* item;
    std::optional<LargeIconConfig> config;
    Span span;
};

template<class Item, class Span, class Save>
bool StoreMany(const std::vector<Change<Item, Span>>& changes, bool unlocked, Save&& save)
{
    std::vector<Change<Item, Span>> previous;
    for (const auto& change : changes)
    {
        if (!change.item || !CanStore(change.config, unlocked, true) ||
            std::any_of(previous.begin(), previous.end(), [&](const auto& old) { return old.item == change.item; })) return false;
        previous.push_back({change.item, change.item->largeIcon, change.item->gridSpan});
    }
    if (changes.empty()) return false;
    for (const auto& change : changes) { change.item->largeIcon = change.config; change.item->gridSpan = change.span; }
    try { if (std::forward<Save>(save)()) return true; }
    catch (...) { /* Roll back the entire selection after a persistence failure. */ }
    for (const auto& old : previous) { old.item->largeIcon = old.config; old.item->gridSpan = old.span; }
    return false;
}

template<class Item, class Span, class Save>
bool Store(Item& item, std::optional<LargeIconConfig> config, Span span, bool unlocked, bool desktop, Save&& save)
{
    if (!CanStore(config, unlocked, desktop)) return false;
    const auto previous = item.largeIcon;
    const auto previousSpan = item.gridSpan;
    item.largeIcon = std::move(config); item.gridSpan = span;
    try { if (std::forward<Save>(save)()) return true; }
    catch (...) { /* Failed persistence must leave the committed display intact. */ }
    item.largeIcon = previous; item.gridSpan = previousSpan;
    return false;
}
}
