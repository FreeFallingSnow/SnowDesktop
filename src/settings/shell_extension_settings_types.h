#pragma once

#include <string>
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
    bool showAll = false;
    friend bool operator==(const Preferences &, const Preferences &) = default;
};

} // namespace snowdesktop::shell_extensions
