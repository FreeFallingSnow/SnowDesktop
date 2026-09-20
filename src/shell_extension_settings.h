#pragma once
#include "json_value.h"
#include <algorithm>
#include <string>
#include <string_view>
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
struct Preferences
{
    // Retained only for round-trip compatibility with the first experimental
    // selector and exclusion-only settings. Only explicit scoped opt-ins are
    // active; an old configuration must not silently enable every extension.
    bool enabled = false;
    std::vector<Selection> selections;
    std::vector<HiddenItem> hidden;
    std::vector<HiddenItem> shown;
    friend bool operator==(const Preferences &, const Preferences &) = default;
};
inline void Normalize(Preferences &value)
{
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
    Normalize(result);
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
    return out + "]}";
}
inline bool IsHidden(const Preferences &prefs, const std::string &id, Context context)
{
    return std::find(prefs.shown.begin(), prefs.shown.end(), HiddenItem{id, context}) == prefs.shown.end();
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
