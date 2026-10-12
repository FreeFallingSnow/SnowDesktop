#include "settings/settings_search_index.h"
#include "settings/settings_search_catalog.h"
#include "common/json_value.h"

#include <windows.h>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

namespace
{
using namespace snowdesktop;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

StaticSettingSearchDescriptor StaticSetting(SettingsPage page,
    std::string focusId, std::wstring label,
    std::wstring description = {})
{
    StaticSettingSearchDescriptor descriptor;
    descriptor.page = page;
    descriptor.focusId = std::move(focusId);
    descriptor.label = std::move(label);
    descriptor.description = std::move(description);
    return descriptor;
}

void TestNavigationAndFocus()
{
    SettingsSearchIndexInput input;
    input.languageTag = "zh-CN";
    input.staticSettings.push_back(StaticSetting(
        SettingsPage::Taskbar, "taskbar.theme",
        L"任务栏主题", L"设置系统任务栏外观"));
    SettingsSearchIndex index(input);

    const auto results = index.Search(L"任务栏");
    Check(results.size() == 1 &&
            results[0].route.page == SettingsPage::Taskbar &&
            results[0].route.focusId == "taskbar.theme" &&
            results[0].focusId == "taskbar.theme" &&
            results[0].route.IsValid(),
        "a static hit carries its route and stable focus target");
}

void TestConditionalPagesAndNoLeaks()
{
    SettingsSearchIndexInput input;
    input.languageTag = "en-US";
    input.staticSettings = {
        StaticSetting(SettingsPage::DeveloperTools,
            "developer.overlay", L"Developer overlay"),
        StaticSetting(SettingsPage::Debug,
            "debug.trace", L"Trace diagnostics"),
        StaticSetting(SettingsPage::General,
            "general.secret-internal", L"Hidden internal setting"),
        StaticSetting(static_cast<SettingsPage>(0xff),
            "unknown.route", L"Unknown route setting"),
    };
    input.staticSettings[2].visible = false;

    SettingsSearchIndex index(input);
    Check(index.EntryCount() == 0 &&
            index.Search(L"developer").empty() &&
            index.Search(L"trace").empty() &&
            index.Search(L"secret internal").empty() &&
            index.Search(L"unknown route").empty(),
        "disabled, hidden, and unknown static items never enter the index");

    input.developerToolsVisible = true;
    index.Rebuild(input);
    Check(index.EntryCount() == 1 &&
            index.Search(L"developer").size() == 1 &&
            index.Search(L"trace").empty(),
        "the Developer Tools gate cannot leak hidden Debug search entries");

    input.developerToolsVisible = false;
    input.debugVisible = true;
    index.Rebuild(input);
    Check(index.EntryCount() == 1 &&
            index.Search(L"developer").empty() &&
            index.Search(L"trace").size() == 1,
        "Debug search entries require their independent unlock state");

    input.developerToolsVisible = true;
    index.Rebuild(input);
    Check(index.EntryCount() == 2,
        "both conditional pages enter the index only when both gates open");
}

void TestWidgetFields()
{
    SettingsSearchIndexInput input;
    input.languageTag = "zh-CN";

    WidgetSettingsSearchDescriptor weather;
    weather.instanceId = L"weather-instance";
    weather.widgetName = L"天气组件";
    weather.fields = {
        { "apiKey", "network.api-key", L"服务密钥",
            L"用于连接天气服务", L"网络", true },
        { "internalToken", "network.hidden", L"隐藏令牌",
            L"绝不能被搜索到", L"网络", false },
    };
    input.widgets.push_back(weather);

    WidgetSettingsSearchDescriptor absent = weather;
    absent.instanceId = L"uninstalled-instance";
    absent.widgetName = L"已卸载组件";
    absent.installed = false;
    input.widgets.push_back(absent);

    WidgetSettingsSearchDescriptor unknown = weather;
    unknown.instanceId.clear();
    unknown.widgetName = L"无实例组件";
    input.widgets.push_back(unknown);

    SettingsSearchIndex index(input);
    const auto byLabel = index.Search(L"服务 密钥");
    const auto byDescription = index.Search(L"天气服务");
    const auto byWidget = index.Search(L"天气组件 网络");
    Check(byLabel.size() == 1 && byDescription.size() == 1 &&
            byWidget.size() == 1 &&
            byLabel[0].kind == SettingsSearchEntryKind::WidgetSetting &&
            byLabel[0].route.page == SettingsPage::WidgetSettings &&
            byLabel[0].route.widgetInstanceId == L"weather-instance" &&
            byLabel[0].focusId == "network.api-key",
        "installed widget labels, descriptions, and context are searchable");
    Check(index.Search(L"隐藏令牌").empty() &&
            index.Search(L"已卸载组件").empty() &&
            index.Search(L"internalToken").empty(),
        "hidden, uninstalled, and internal widget metadata do not leak");
}

void TestLanguageRebuildAndUnicodeMatching()
{
    SettingsSearchIndexInput input;
    input.languageTag = "fr-FR";
    input.staticSettings.push_back(StaticSetting(
        SettingsPage::General, "general.school-panel",
        L"École", L"Панель de configuration"));
    SettingsSearchIndex index(input);

    Check(index.Search(L"éCOLE ПАНЕЛЬ").size() == 1,
        "Unicode case folding and multi-term matching span label and description");

    input.languageTag = "en-US";
    input.staticSettings[0].label = L"School";
    input.staticSettings[0].description = L"Settings panel";
    index.Rebuild(input);
    Check(index.LanguageTag() == "en-US" &&
            index.Search(L"école").empty() &&
            index.Search(L"SCHOOL PANEL").size() == 1,
        "a language change replaces instead of appending localized terms");
}

void TestStableRankingAndLimit()
{
    SettingsSearchIndexInput input;
    input.languageTag = "en-US";
    input.staticSettings = {
        StaticSetting(SettingsPage::General,
            "general.first", L"Theme settings"),
        StaticSetting(SettingsPage::Personalization,
            "appearance.exact", L"Theme"),
        StaticSetting(SettingsPage::Desktop,
            "desktop.description", L"Desktop icons", L"Theme options"),
        StaticSetting(SettingsPage::General,
            "general.second", L"Theme colors"),
    };
    SettingsSearchIndex index(input);

    const auto results = index.Search(L"theme");
    Check(results.size() == 4 &&
            results[0].focusId == "appearance.exact" &&
            results[1].focusId == "general.first" &&
            results[2].focusId == "general.second" &&
            results[3].focusId == "desktop.description",
        "exact, title-prefix, and description matches have deterministic rank");
    const auto limited = index.Search(L"theme", 2);
    Check(limited.size() == 2 &&
            limited[0].focusId == "appearance.exact" &&
            limited[1].focusId == "general.first",
        "result limits preserve the ranked stable prefix");
}

std::wstring WideUtf8(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), length);
    return result;
}

void TestCatalogUserTasks(const std::filesystem::path& repository)
{
    // Exercise the shipping metadata and translations. Queries describe user
    // tasks rather than internal IDs; UI/desktop interaction is a separate test.
    JsonValue chinese;
    for (const auto& file : std::filesystem::directory_iterator(repository / "lang"))
    {
        if (file.path().extension() != ".json") continue;
        std::ifstream stream(file.path(), std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        JsonValue language;
        Check(ParseJson(buffer.str(), language), "shipping search language is valid JSON");
        SettingsSearchIndexInput input;
        input.languageTag = file.path().stem().string();
        const auto localize = [&language](std::string_view key) {
            const auto* value = language.Find(key);
            return value && value->IsString() ? WideUtf8(value->string) : std::wstring{};
        };
        for (const auto& definition : kStaticSearchDefinitions)
            if (definition.keywordsKey)
                Check(!localize(definition.keywordsKey).empty(),
                    "each shipping language supplies actual task keywords");
        PopulateSettingsSearchCatalog(input, localize, false, false);
        const SettingsSearchIndex index(input);
        std::set<std::pair<SettingsPage, std::string>> destinations;
        for (const auto& descriptor : input.staticSettings)
            Check(destinations.emplace(descriptor.page, descriptor.focusId).second,
                "aliases share a destination instead of defining duplicate controls");
        Check(index.Search(localize("settings.search.keywords.startup")).size() == 1,
            "localized startup aliases resolve in every shipping language");
        if (input.languageTag == "zh-CN") chinese = language;
    }
    Check(chinese.IsObject(), "the Chinese user-task language was loaded");
    SettingsSearchIndexInput input;
    input.languageTag = "zh-CN";
    PopulateSettingsSearchCatalog(input, [&chinese](std::string_view key) {
        const auto* value = chinese.Find(key);
        return value && value->IsString() ? WideUtf8(value->string) : std::wstring{};
    }, false, false);
    SettingsSearchIndex index(input);
    const auto has = [&index](std::wstring_view query, SettingsPage page, std::string_view id) {
        const auto results = index.Search(query);
        return std::any_of(results.begin(), results.end(), [&](const auto& result) {
            return result.route.page == page && result.focusId == id;
        });
    };
    Check(has(L"Dock 自动隐藏", SettingsPage::Dock, "dock.showOnlyWhenSummoned"),
        "Dock hiding task resolves to the actual summon-only visibility control");
    Check(has(L"多显示器布局", SettingsPage::Dock, "dock.monitor"),
        "multi-monitor layout task discovers the Dock display selector");
    Check(has(L"集合显示方式", SettingsPage::General, "start.collection"),
        "object display tasks discover their contextual guide, not a fake global setting");
    Check(has(L"圆角半径", SettingsPage::AppearanceWidgets, "personalization.cornerRadius") &&
        has(L"标题字号", SettingsPage::AppearanceDesktopIcons, "desktop.itemFontSize"),
        "numeric search titles name the same controls that users see");
    Check(has(L"文件和文件夹", SettingsPage::ContextMenu, "contextMenu.extensions"),
        "former duplicate extension labels remain discoverable as aliases");
    Check(has(L"Ｄｏｃｋ 自动隐藏", SettingsPage::Dock, "dock.showOnlyWhenSummoned"),
        "IME full-width Latin input matches ordinary task queries");
    const auto calendar = index.Search(L"日程");
    Check(std::any_of(calendar.begin(), calendar.end(), [](const auto& result) {
        return result.route.page == SettingsPage::Calendar &&
            result.context.find(L"启动与系统栏") != std::wstring::npos;
    }), "calendar hits display their owning navigation group");

    // Causal negative control: the old metadata omitted keywords. Removing
    // just those phrases must lose the query that cannot be inferred from labels.
    for (auto& descriptor : input.staticSettings) descriptor.keywords.clear();
    index.Rebuild(input);
    Check(index.Search(L"集合显示方式").empty(),
        "user-task discovery depends on populated aliases, not incidental IDs");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) return EXIT_FAILURE;
    TestCatalogUserTasks(std::filesystem::path(argv[1]));
    SettingsSearchIndexInput catalog;
    PopulateSettingsSearchCatalog(catalog,[](std::string_view key) {return std::wstring(key.begin(),key.end());},false,false);
    const auto find = [&](std::string_view id) {return std::find_if(catalog.staticSettings.begin(),catalog.staticSettings.end(),[&](const auto& descriptor) {return descriptor.focusId == id;});};
    const auto seconds = find("statusBar.clockShowSeconds");
    Check(seconds != catalog.staticSettings.end() && seconds->page == SettingsPage::StatusBar,
        "clock seconds search opens the Status Bar page with its stable focus target");
    Check(find("personalization.savedThemes")->page == SettingsPage::ThemeManager,
        "theme management search targets the independent page");
    for (const auto focus : {"desktop.softwareDesktopHotkey", "desktop.softwareDesktopHotkey.hotkey"})
    {
        const auto entry = find(focus);
        Check(entry != catalog.staticSettings.end() && entry->page == SettingsPage::Desktop,
            "desktop switching search results open the Desktop page with a stable focus target");
    }
    for (const auto focus : {"personalization.font", "personalization.tooltipFontSize"})
    {
        const auto entry = find(focus);
        Check(entry != catalog.staticSettings.end() && entry->page == SettingsPage::AppearanceWidgets,
            "font search results open their independent cards on Interface & layout");
    }
    Check(find("general.quickNavigation")->page == SettingsPage::QuickNavigation && find("personalization.quickNavigationTheme")->page == SettingsPage::AppearanceTheme,"the shared catalog preserves the theme location and routes navigation behavior to its page");
    Check(find("quickNav.layout.iconSize") != catalog.staticSettings.end() && find("quickNav.color.searchBg") != catalog.staticSettings.end(),"advanced navigation fields are indexed individually");
    Check(find("quickNav.layout.iconSize")->page == SettingsPage::AppearanceTheme && find("quickNav.color.searchBg")->page == SettingsPage::AppearanceTheme,
        "navigation dimensions and colors belong to the original appearance theme page");
    Check(find("quickNav.prefixes")->page == SettingsPage::QuickNavigation &&
            find("quickNav.defaultCollapsed") == catalog.staticSettings.end() &&
            find("quickNav.view") == catalog.staticSettings.end(),
        "functional search settings stay on navigation and the obsolete opening preference is absent");
    Check(!find("general.advancedFeatures")->visible && find("statusBar.controlCenterPanel") == catalog.staticSettings.end(),"shared catalog honors advanced and system capability visibility");
    Check(find("dock.mergedIconScale") == catalog.staticSettings.end(),
        "search does not navigate to hidden merged bar controls when no bar is merged");
    SettingsSearchIndexInput mergedCatalog;
    PopulateSettingsSearchCatalog(mergedCatalog, [](std::string_view key) { return std::wstring(key.begin(), key.end()); }, false, false, 0, true);
    Check(std::any_of(mergedCatalog.staticSettings.begin(), mergedCatalog.staticSettings.end(),
        [](const auto& entry) { return entry.focusId == "dock.mergedIconScale" && entry.page == SettingsPage::Dock; }),
        "an active merged bar exposes its independent icon scale search target");
    for (int style = 0; style <= 6; ++style)
    {
        SettingsSearchIndexInput menus;
        PopulateSettingsSearchCatalog(menus, [](std::string_view key) {
            return std::wstring(key.begin(), key.end());
        }, false, false, style);
        const auto contains = [&](std::string_view id) {
            return std::any_of(menus.staticSettings.begin(), menus.staticSettings.end(),
                [&](const auto& descriptor) { return descriptor.focusId == id; });
        };
        Check(contains("contextMenu.expandQuickActions") == (style < 5) &&
            contains("contextMenu.hidePageManagement") && contains("contextMenu.hideMoreManagement"),
            "menu search exposes expansion only outside Win10 styles and always exposes page hiding");
    }
    TestNavigationAndFocus();
    TestConditionalPagesAndNoLeaks();
    TestWidgetFields();
    TestLanguageRebuildAndUnicodeMatching();
    TestStableRankingAndLimit();

    if (failures != 0)
    {
        std::cerr << failures << " settings search index checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "settings search index checks passed\n";
    return EXIT_SUCCESS;
}
