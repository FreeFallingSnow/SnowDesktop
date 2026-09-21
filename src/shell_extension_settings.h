#pragma once
#include "json_value.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <map>
#include <set>
#include <vector>

namespace snowdesktop::shell_extensions
{
enum class Context : int
{
    Automatic = -1,
    File,
    Folder,
    FolderBackground,
    Desktop
};
struct HiddenItem
{
    std::string id;
    Context context = Context::File;
    friend bool operator==(const HiddenItem &, const HiddenItem &) = default;
};
enum class Placement : int
{
    Hidden,
    Submenu,
    Root
};
struct Selection
{
    std::string provider;
    std::string command; // Empty selects the complete provider group.
    std::string label;   // Display-only; never used to resolve a command.
    Placement placement = Placement::Hidden;
    friend bool operator==(const Selection &, const Selection &) = default;
};
enum class Category : int { Objects, Background };
enum class Visibility : int { Inherit, Show, Hide };
inline Category CategoryOf(Context context)
{
    return context == Context::Desktop || context == Context::FolderBackground ? Category::Background : Category::Objects;
}
struct Rule
{
    std::string id;
    Category category = Category::Objects;
    bool shown = false;
    friend bool operator==(const Rule &, const Rule &) = default;
};
struct LocationOverride
{
    std::string id;
    Context context = Context::File;
    Visibility visibility = Visibility::Inherit;
    friend bool operator==(const LocationOverride &, const LocationOverride &) = default;
};
struct Preferences
{
    // Retained only for round-trip compatibility with the first experimental
    // selector and exclusion-only settings. Only explicit scoped opt-ins are
    // active; an old configuration must not silently enable every extension.
    bool enabled = false;
    std::vector<Selection> selections;
    std::vector<HiddenItem> hidden;
    std::vector<HiddenItem> shown;
    int rulesVersion = 2;
    std::vector<Rule> rules;
    std::vector<LocationOverride> overrides;
    friend bool operator==(const Preferences &, const Preferences &) = default;
};
inline void Normalize(Preferences &value)
{
    auto validId = [](const auto &item) { return !item.id.empty() && item.id.size() <= 4096 && item.id.find('\0') == std::string::npos; };
    std::erase_if(value.rules, [&](const Rule &r) { return !validId(r) || r.category < Category::Objects || r.category > Category::Background; });
    std::erase_if(value.overrides, [&](const LocationOverride &r) { return !validId(r) || r.context < Context::File || r.context > Context::Desktop || r.visibility < Visibility::Inherit || r.visibility > Visibility::Hide; });
    if (value.rules.size() > 8192) value.rules.resize(8192);
    if (value.overrides.size() > 16384) value.overrides.resize(16384);

    std::vector<Selection> kept;
    for (auto item : value.selections)
    {
        if (kept.size() >= 512)
            break;
        if (item.provider.empty() || item.provider.size() > 2048 || item.command.size() > 2048 ||
            item.label.size() > 4096 || item.placement < Placement::Hidden || item.placement > Placement::Root ||
            item.provider.find('\0') != std::string::npos || item.command.find('\0') != std::string::npos)
            continue;
        auto found = std::find_if(kept.begin(), kept.end(), [&](const auto &old) {
            return old.provider == item.provider && old.command == item.command;
        });
        if (found == kept.end())
            kept.push_back(std::move(item));
        else
            *found = std::move(item);
    }
    value.selections = std::move(kept);
    for (auto *items : {&value.hidden, &value.shown})
    {
        std::vector<HiddenItem> normalized;
        for (const auto &item : *items)
        {
            if (normalized.size() >= 2048)
                break;
            if (item.id.empty() || item.id.size() > 4096 || item.id.find('\0') != std::string::npos ||
                item.context < Context::File || item.context > Context::Desktop)
                continue;
            if (std::find(normalized.begin(), normalized.end(), item) == normalized.end())
                normalized.push_back(item);
        }
        *items = std::move(normalized);
    }
}
inline Preferences ReadPreferences(const JsonValue *json)
{
    Preferences result;
    if (!json || !json->IsObject())
        return result;
    if (const auto *enabled = json->Find("enabled"); enabled && enabled->IsBoolean())
        result.enabled = enabled->boolean;
    if (const auto *rows = json->Find("selections"); rows && rows->IsArray())
        for (const auto &row : rows->array)
        {
            const auto *provider = row.Find("provider");
            const auto *command = row.Find("command");
            const auto *place = row.Find("placement");
            const auto *label = row.Find("label");
            if (!provider || !provider->IsString() || !command || !command->IsString() || !place ||
                !place->IsNumber() || place->number < 0 || place->number > 2 ||
                place->number != static_cast<int>(place->number))
                continue;
            result.selections.push_back({provider->string, command->string,
                                         label && label->IsString() ? label->string : "",
                                         static_cast<Placement>(static_cast<int>(place->number))});
        }
    for (const auto key : {"hidden", "shown"})
        if (const auto *rows = json->Find(key); rows && rows->IsArray())
            for (const auto &row : rows->array)
            {
                const auto *id = row.Find("id");
                const auto *context = row.Find("context");
                if (!id || !id->IsString() || !context || !context->IsNumber() || context->number < 0 ||
                    context->number > 3 || context->number != static_cast<int>(context->number))
                    continue;
                (std::string_view(key) == "shown" ? result.shown : result.hidden)
                    .push_back({id->string, static_cast<Context>(static_cast<int>(context->number))});
            }
    const auto *version = json->Find("rulesVersion");
    result.rulesVersion = version && version->IsNumber() ? static_cast<int>(version->number) : 1;
    if (const auto *rows = json->Find("rules"); rows && rows->IsArray())
        for (const auto &row : rows->array)
        {
            const auto *id = row.Find("id"), *category = row.Find("category"), *shown = row.Find("shown");
            if (id && id->IsString() && category && category->IsNumber() && (category->number == 0 || category->number == 1) && shown && shown->IsBoolean())
                result.rules.push_back({id->string, static_cast<Category>(static_cast<int>(category->number)), shown->boolean});
        }
    if (const auto *rows = json->Find("overrides"); rows && rows->IsArray())
        for (const auto &row : rows->array)
        {
            const auto *id = row.Find("id"), *context = row.Find("context"), *visibility = row.Find("visibility");
            if (id && id->IsString() && context && context->IsNumber() && context->number >= 0 && context->number <= 3 && context->number == static_cast<int>(context->number) && visibility && visibility->IsNumber() && visibility->number >= 0 && visibility->number <= 2 && visibility->number == static_cast<int>(visibility->number))
                result.overrides.push_back({id->string, static_cast<Context>(static_cast<int>(context->number)), static_cast<Visibility>(static_cast<int>(visibility->number))});
        }
    Normalize(result);
    if (result.rulesVersion < 2)
    {
        for (const auto &item : result.shown)
        {
            const auto category = CategoryOf(item.context);
            if (std::any_of(result.rules.begin(), result.rules.end(), [&](const auto &r) { return r.id == item.id && r.category == category; })) continue;
            const auto first = category == Category::Objects ? Context::File : Context::FolderBackground;
            const auto second = category == Category::Objects ? Context::Folder : Context::Desktop;
            const bool a = std::find(result.shown.begin(), result.shown.end(), HiddenItem{item.id, first}) != result.shown.end();
            const bool b = std::find(result.shown.begin(), result.shown.end(), HiddenItem{item.id, second}) != result.shown.end();
            result.rules.push_back({item.id, category, a && b});
            if (a != b)
            {
                result.overrides.push_back({item.id, first, a ? Visibility::Show : Visibility::Hide});
                result.overrides.push_back({item.id, second, b ? Visibility::Show : Visibility::Hide});
            }
        }
        result.rulesVersion = 2;
    }
    return result;
}
inline std::string Quote(const std::string &text)
{
    std::string out = "\"";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char>(c);
        }
        else if (c < 32)
        {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        else
            out += static_cast<char>(c);
    }
    return out + '"';
}
inline std::string WritePreferences(Preferences value)
{
    Normalize(value);
    std::string out = std::string("{\"enabled\":") + (value.enabled ? "true" : "false") + ",\"selections\":[";
    bool first = true;
    for (const auto &row : value.selections)
    {
        if (!first)
            out += ',';
        first = false;
        out += "{\"provider\":" + Quote(row.provider) + ",\"command\":" + Quote(row.command) +
               ",\"label\":" + Quote(row.label) + ",\"placement\":" + std::to_string(static_cast<int>(row.placement)) +
               '}';
    }
    out += "],\"hidden\":[";
    first = true;
    for (const auto &row : value.hidden)
    {
        if (!first)
            out += ',';
        first = false;
        out += "{\"id\":" + Quote(row.id) + ",\"context\":" + std::to_string(static_cast<int>(row.context)) + '}';
    }
    out += "],\"shown\":[";
    first = true;
    for (const auto &row : value.shown)
    {
        if (!first)
            out += ',';
        first = false;
        out += "{\"id\":" + Quote(row.id) + ",\"context\":" + std::to_string(static_cast<int>(row.context)) +
               '}';
    }
    out += "],\"rulesVersion\":" + std::to_string(value.rulesVersion) + ",\"rules\":[";
    first = true;
    for (const auto &r : value.rules)
    {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":" + Quote(r.id) + ",\"category\":" + std::to_string(static_cast<int>(r.category)) + ",\"shown\":" + (r.shown ? "true" : "false") + '}';
    }
    out += "],\"overrides\":[";
    first = true;
    for (const auto &r : value.overrides)
    {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":" + Quote(r.id) + ",\"context\":" + std::to_string(static_cast<int>(r.context)) + ",\"visibility\":" + std::to_string(static_cast<int>(r.visibility)) + '}';
    }
    return out + "]}";
}
inline bool IsHidden(const Preferences &prefs, const std::string &id, Context context)
{
    for (const auto &r : prefs.overrides)
        if (r.id == id && r.context == context && r.visibility != Visibility::Inherit)
            return r.visibility == Visibility::Hide;
    for (const auto &r : prefs.rules)
        if (r.id == id && r.category == CategoryOf(context)) return !r.shown;
    return std::find(prefs.shown.begin(), prefs.shown.end(), HiddenItem{id, context}) == prefs.shown.end();
}
inline bool CommonShown(const Preferences &prefs, const std::string &id, Category category)
{
    for (const auto &r : prefs.rules) if (r.id == id && r.category == category) return r.shown;
    const auto first = category == Category::Objects ? Context::File : Context::FolderBackground;
    const auto second = category == Category::Objects ? Context::Folder : Context::Desktop;
    return !IsHidden(prefs, id, first) && !IsHidden(prefs, id, second);
}
inline Visibility OverrideOf(const Preferences &prefs, const std::string &id, Context context)
{
    for (const auto &r : prefs.overrides) if (r.id == id && r.context == context) return r.visibility;
    return Visibility::Inherit;
}
inline void SetCommon(Preferences &prefs, const std::string &id, Category category, bool shown)
{
    std::erase_if(prefs.rules, [&](const auto &r) { return r.id == id && r.category == category; });
    prefs.rules.push_back({id, category, shown});
    prefs.rulesVersion = 2;
    Normalize(prefs);
}
inline void SetOverride(Preferences &prefs, const std::string &id, Context context, Visibility value)
{
    // Persist an explicit base when restoring inheritance, so retained legacy
    // records cannot recreate a migrated exception on the next association pass.
    if (value == Visibility::Inherit && std::none_of(prefs.rules.begin(), prefs.rules.end(), [&](const auto &r) { return r.id == id && r.category == CategoryOf(context); }))
        SetCommon(prefs, id, CategoryOf(context), CommonShown(prefs, id, CategoryOf(context)));
    std::erase_if(prefs.overrides, [&](const auto &r) { return r.id == id && r.context == context; });
    if (value != Visibility::Inherit) prefs.overrides.push_back({id, context, value});
    Normalize(prefs);
}
inline std::set<std::string> EffectiveShownIds(const Preferences &prefs, Context context)
{
    std::map<std::string, bool> states;
    for (const auto &row : prefs.shown) if (row.context == context) states[row.id] = true;
    // Match IsHidden's first-record precedence, including retained legacy data.
    for (auto row = prefs.rules.rbegin(); row != prefs.rules.rend(); ++row)
        if (row->category == CategoryOf(context)) states[row->id] = row->shown;
    for (auto row = prefs.overrides.rbegin(); row != prefs.overrides.rend(); ++row)
        if (row->context == context && row->visibility != Visibility::Inherit)
            states[row->id] = row->visibility == Visibility::Show;
    std::set<std::string> result;
    for (const auto &[id, shown] : states) if (shown && !id.empty()) result.insert(id);
    return result;
}
inline bool HasOptIns(const Preferences &prefs, Context context = Context::Automatic)
{
    if (context != Context::Automatic) return !EffectiveShownIds(prefs, context).empty();
    for (int i = 0; i < 4; ++i) if (!EffectiveShownIds(prefs, static_cast<Context>(i)).empty()) return true;
    return false;
}
inline void SetHidden(Preferences &prefs, const std::string &id, Context context, bool hidden)
{
    std::erase(prefs.hidden, HiddenItem{id, context});
    std::erase(prefs.shown, HiddenItem{id, context});
    if (!hidden)
        prefs.shown.push_back({id, context});
    Normalize(prefs);
}
} // namespace snowdesktop::shell_extensions
