#pragma once

#include "settings_route.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{

/** Identifies whether a search hit describes the application or a widget. */
enum class SettingsSearchEntryKind
{
    StaticSetting,
    WidgetSetting,
};

/**
 * A localized, visible application setting that can receive keyboard focus.
 *
 * Labels, descriptions, and keywords must all belong to languageTag in the
 * SettingsSearchIndexInput used to build the index. Stable identifiers are
 * deliberately not searchable, so internal or hidden metadata cannot leak
 * through user-entered queries.
 */
struct StaticSettingSearchDescriptor
{
    SettingsPage page = SettingsPage::Home;
    std::string focusId;
    std::wstring label;
    std::wstring description;
    /** Localized owning page or section, displayed to disambiguate hits. */
    std::wstring context;
    std::vector<std::wstring> keywords;
    bool visible = true;
};

/** A localized declarative field belonging to one widget instance. */
struct WidgetSettingSearchFieldDescriptor
{
    std::string key;
    std::string focusId;
    std::wstring label;
    std::wstring description;
    std::wstring groupLabel;
    bool visible = true;
};

/** Searchable fields for one currently installed widget instance. */
struct WidgetSettingsSearchDescriptor
{
    std::wstring instanceId;
    std::wstring widgetName;
    std::vector<WidgetSettingSearchFieldDescriptor> fields;
    bool installed = true;
    bool visible = true;
};

struct SettingsSearchIndexInput
{
    std::string languageTag;
    std::vector<StaticSettingSearchDescriptor> staticSettings;
    std::vector<WidgetSettingsSearchDescriptor> widgets;
    bool developerToolsVisible = false;
    bool debugVisible = false;
};

struct SettingsSearchResult
{
    SettingsSearchEntryKind kind =
        SettingsSearchEntryKind::StaticSetting;
    SettingsRoute route;
    std::string focusId;
    std::wstring label;
    std::wstring description;
    std::wstring context;

    bool operator==(const SettingsSearchResult&) const = default;
};

/**
 * Toolkit-independent search index for the settings center.
 *
 * Rebuild replaces the complete localized index atomically. Callers rebuild
 * after a language change, widget generation change, or a change to the
 * Developer Tools / Debug visibility gates.
 */
class SettingsSearchIndex
{
public:
    SettingsSearchIndex() = default;
    explicit SettingsSearchIndex(const SettingsSearchIndexInput& input);

    void Rebuild(const SettingsSearchIndexInput& input);

    [[nodiscard]] std::vector<SettingsSearchResult> Search(
        std::wstring_view query,
        std::size_t maximumResults = 50) const;

    [[nodiscard]] const std::string& LanguageTag() const noexcept
    {
        return languageTag_;
    }

    [[nodiscard]] std::size_t EntryCount() const noexcept
    {
        return entries_.size();
    }

private:
    struct Entry
    {
        SettingsSearchResult result;
        std::u32string normalizedLabel;
        std::u32string normalizedDescription;
        std::u32string normalizedKeywords;
        std::u32string normalizedContext;
        std::size_t ordinal = 0;
    };

    std::string languageTag_;
    std::vector<Entry> entries_;
};

} // namespace snowdesktop
