#pragma once

#include <string>

namespace snowdesktop::category_collection_rules
{

enum class UrlShortcutKind { None, Steam, Epic };

struct ShortcutTarget
{
    bool classified = false;
    bool directory = false;
    bool application = false;
    std::wstring extension;
    UrlShortcutKind urlKind = UrlShortcutKind::None;

    bool operator==(const ShortcutTarget&) const = default;
};

} // namespace snowdesktop::category_collection_rules
