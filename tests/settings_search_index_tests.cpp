#include "settings/settings_search_index.h"
#include "settings/settings_search_catalog.h"
#include "common/json_value.h"

#include <windows.h>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <fstream>
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
        StaticSetting(SettingsPage::AgentOperations,
            "agentOperations.skill", L"SnowDesktop Control Skill"),
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
    Check(index.Search(L"SnowDesktop Control").empty(), "operation Skill search remains hidden while advanced settings are off");
    input.advancedSettingsVisible = true;
    index.Rebuild(input);
    const auto operations = index.Search(L"SnowDesktop Control");
    Check(operations.size() == 1 && operations[0].route.page == SettingsPage::AgentOperations &&
        operations[0].focusId == "agentOperations.skill", "advanced settings exposes the operation Skill with its own focus target");
    input.advancedSettingsVisible = false;
    index.Rebuild(input);
    Check(index.Search(L"SnowDesktop Control").empty(), "hiding advanced settings removes operation Skill search entries");
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

void TestLocalizedTaskAliasesAndReadOnlyQueries()
{
    // Supply translated resource values through the production catalog seam.
    // Labels remain resource IDs so these user phrases can match only aliases.
    const auto localize = [](std::string_view key) -> std::wstring {
        if (key == "settings.search.alias.dockVisibility") return L"自动隐藏 隐藏 Dock auto hide";
        if (key == "settings.search.alias.monitors") return L"多显示器 副屏 双屏";
        if (key == "settings.search.alias.grid") return L"网格间距 图标挤在一起";
        if (key == "settings.search.alias.classification") return L"自动分类 扩展名 文件类型";
        if (key == "settings.search.alias.quickAppearance") return L"搜索窗口颜色 快捷导航外观";
        if (key == "settings.search.alias.transformCursor") return L"移动组件指针 缩放组件指针";
        if (key == "settings.contextMenu.hidePageManagement") return L"隐藏页面管理";
        return std::wstring(key.begin(), key.end());
    };
    SettingsSearchIndexInput input;
    input.languageTag = "zh-CN";
    PopulateSettingsSearchCatalog(input, localize, false, false);
    SettingsSearchIndex index(input);
    const auto count = index.EntryCount();
    struct Task { const wchar_t* query; const char* focus; SettingsPage page; };
    const Task tasks[]{
        {L"自动隐藏", "dock.showOnlyWhenSummoned", SettingsPage::Dock},
        {L"AUTO HIDE", "dock.showOnlyWhenSummoned", SettingsPage::Dock},
        {L"多显示器", "pages.order", SettingsPage::DesktopPages},
        {L"副屏", "dock.monitor", SettingsPage::Dock},
        {L"图标挤在一起", "desktop.spacing", SettingsPage::AppearanceDesktopIcons},
        {L"网格间距", "pages.grid", SettingsPage::DesktopPages},
        {L"自动分类", "desktop.categoryRules", SettingsPage::DesktopCategories},
        {L"搜索窗口颜色", "personalization.quickNavigationTheme", SettingsPage::QuickNavigation},
        {L"移动组件指针", "personalization.widgetTransformCursors", SettingsPage::WidgetBehavior},
        {L"隐藏页面管理", "contextMenu.hidePageManagement", SettingsPage::ContextMenu},
    };
    for (const auto& task : tasks)
    {
        const auto results = index.Search(task.query);
        const auto hit = std::find_if(results.begin(), results.end(), [&](const auto& result) {
            return result.focusId == task.focus;
        });
        Check(hit != results.end() && hit->route.page == task.page &&
                hit->route.focusId == task.focus && hit->route.IsValid(),
            "a daily phrase or old label locates the canonical setting without changing its stable ID");
        Check(index.Search(task.query) == results && index.EntryCount() == count && input.languageTag == "zh-CN",
            "repeated queries are deterministic and do not rebuild or mutate the catalog");
    }
    Check(index.Search(L"settings.search.alias.dockVisibility").empty(),
        "alias resource keys do not become visible search terms");
    const auto descriptor = std::find_if(input.staticSettings.begin(), input.staticSettings.end(),
        [](const auto& entry) { return entry.focusId == "dock.showOnlyWhenSummoned"; });
    Check(descriptor != input.staticSettings.end() && descriptor->keywords ==
            std::vector<std::wstring>{L"自动隐藏 隐藏 Dock auto hide"},
        "search queries leave localized input metadata unchanged");

    SettingsSearchIndexInput english;
    english.languageTag = "en-US";
    PopulateSettingsSearchCatalog(english, [](std::string_view key) -> std::wstring {
        if (key == "settings.search.alias.dockVisibility") return L"automatically hide docking bar";
        return std::wstring(key.begin(), key.end());
    }, false, false);
    index.Rebuild(english);
    Check(index.Search(L"自动隐藏").empty() && index.Search(L"automatically hide").size() == 1 &&
            index.LanguageTag() == "en-US",
        "changing language replaces alias vocabulary rather than mixing translations");
    const auto missingAlias = std::find_if(english.staticSettings.begin(), english.staticSettings.end(),
        [](const auto& entry) { return entry.focusId == "desktop.spacing"; });
    Check(missingAlias != english.staticSettings.end() && missingAlias->keywords.empty(),
        "a missing alias translation is skipped instead of exposing an implementation key");
}

void TestShippedTaskVocabulary(const std::filesystem::path& projectRoot)
{
    struct Task { const char* language; const wchar_t* query; const char* focus; SettingsPage page; };
    const Task tasks[]{
        {"zh-CN", L"Dock自动隐藏", "dock.showOnlyWhenSummoned", SettingsPage::Dock},
        {"zh-CN", L"状态栏 仅在唤起时显示", "statusBar.showOnlyWhenSummoned", SettingsPage::StatusBar},
        {"zh-CN", L"状态栏 屏幕边缘唤起方式", "statusBar.edgeRevealGesture", SettingsPage::StatusBar},
        {"zh-CN", L"状态栏 悬停", "statusBar.edgeRevealGesture", SettingsPage::StatusBar},
        {"zh-CN", L"状态栏 扫动", "statusBar.edgeRevealGesture", SettingsPage::StatusBar},
        {"en-US", L"Status bar screen edge reveal", "statusBar.edgeRevealGesture", SettingsPage::StatusBar},
        {"zh-CN", L"屏幕边缘悬停延时", "dock.edgeHoverDelayMs", SettingsPage::Dock},
        {"en-US", L"Screen edge hover delay", "dock.edgeHoverDelayMs", SettingsPage::Dock},
        {"zh-CN", L"多显示器布局", "pages.order", SettingsPage::DesktopPages},
        {"zh-CN", L"搜索框颜色", "personalization.quickNavigationTheme", SettingsPage::QuickNavigation},
        {"zh-CN", L"组件缩放指针", "personalization.widgetTransformCursors", SettingsPage::WidgetBehavior},
        {"zh-CN", L"电量百分比", "statusBar.batteryPercentage", SettingsPage::StatusBar},
        {"zh-CN", L"控制中心显示", "statusBar.controlCenterDisplay", SettingsPage::StatusBar},
        {"zh-CN", L"通知中心", "statusBar.notifications", SettingsPage::StatusBar},
        {"zh-CN", L"悬停切换组标签", "personalization.groupTabHoverSwitch", SettingsPage::WidgetBehavior},
        {"zh-CN", L"悬停切换分类标签", "personalization.categoryTabHoverSwitch", SettingsPage::WidgetBehavior},
        {"zh-CN", L"分类与规则", "desktop.categoryRules", SettingsPage::DesktopCategories},
        {"zh-CN", L"类型标签圆角", "quickNav.layout.searchRadius", SettingsPage::QuickNavigation},
        {"zh-CN", L"任务缩略图", "dock.showWindowPreviews", SettingsPage::Dock},
        {"zh-CN", L"默认保护", "dock.fullscreenPolicy", SettingsPage::Dock},
        {"en-US", L"automatically hide Dock", "dock.showOnlyWhenSummoned", SettingsPage::Dock},
        {"en-US", L"multi monitor layout", "pages.order", SettingsPage::DesktopPages},
        {"en-US", L"battery percentage", "statusBar.batteryPercentage", SettingsPage::StatusBar},
        {"en-US", L"Control center display", "statusBar.controlCenterDisplay", SettingsPage::StatusBar},
        {"en-US", L"Notification center", "statusBar.notifications", SettingsPage::StatusBar},
        {"en-US", L"Tab hover delay", "personalization.tabHoverDelayMs", SettingsPage::WidgetBehavior},
        {"en-US", L"search panel color", "personalization.quickNavigationTheme", SettingsPage::QuickNavigation},
        {"en-US", L"Type chip corner radius", "quickNav.layout.searchRadius", SettingsPage::QuickNavigation},
        {"en-US", L"Task thumbnails", "dock.showWindowPreviews", SettingsPage::Dock},
    };
    for (const char* language : {"zh-CN", "en-US"})
    {
        std::ifstream file(projectRoot / "lang" / (std::string(language) + ".json"), std::ios::binary);
        Check(file.good(), "the shipped language catalog must be readable");
        std::ostringstream contents; contents << file.rdbuf();
        JsonValue catalog;
        Check(ParseJson(contents.str(), catalog) && catalog.IsObject(), "the shipped language catalog must parse");
        SettingsSearchIndexInput input;
        input.languageTag = language;
        PopulateSettingsSearchCatalog(input, [&catalog](std::string_view key) -> std::wstring {
            const auto* entry = catalog.Find(key);
            if (!entry || !entry->IsString()) return {};
            const auto& text = entry->string;
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (length <= 0) return {};
            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                text.data(), static_cast<int>(text.size()), wide.data(), length);
            return wide;
        }, false, false);
        SettingsSearchIndex index(input);
        for (const auto& task : tasks)
        {
            if (std::string_view(task.language) != language) continue;
            const auto hits = index.Search(task.query);
            Check(std::any_of(hits.begin(), hits.end(), [&task](const auto& hit) {
                return hit.route.page == task.page && hit.route.focusId == task.focus;
            }), "a real user task resolves with the actual shipped resource values");
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) { std::cerr << "usage: SnowDesktopSettingsSearchIndexTests <project-root>\n"; return 2; }
    SettingsSearchIndexInput catalog;
    PopulateSettingsSearchCatalog(catalog,[](std::string_view key) {return std::wstring(key.begin(),key.end());},false,false);
    const auto find = [&](std::string_view id) {return std::find_if(catalog.staticSettings.begin(),catalog.staticSettings.end(),[&](const auto& descriptor) {return descriptor.focusId == id;});};
    const auto seconds = find("statusBar.clockShowSeconds");
    Check(seconds != catalog.staticSettings.end() && seconds->page == SettingsPage::StatusBar,
        "clock seconds search opens the Status Bar page with its stable focus target");
    const auto showDesktop = find("statusBar.showDesktop");
    Check(showDesktop != catalog.staticSettings.end() && showDesktop->page == SettingsPage::StatusBar,
        "show desktop search opens the Status Bar page with its stable toggle target");
    Check(find("personalization.savedThemes")->page == SettingsPage::ThemeManager,
        "theme management search targets the independent page");
    for (const auto focus : {"desktop.softwareDesktopHotkey", "desktop.softwareDesktopHotkey.hotkey"})
    {
        const auto entry = find(focus);
        Check(entry != catalog.staticSettings.end() && entry->page == SettingsPage::Desktop,
            "desktop switching search results open the Desktop page with a stable focus target");
    }
    for (const auto focus : {"personalization.font", "personalization.tooltipFontSize",
            "dock.attentionDotSize", "dock.attentionDotOffset"})
    {
        const auto entry = find(focus);
        Check(entry != catalog.staticSettings.end() && entry->page == SettingsPage::AppearanceDesktopIcons,
            "font and Dock attention dot search results open global appearance");
    }
    Check(find("general.quickNavigation")->page == SettingsPage::QuickNavigation && find("personalization.quickNavigationTheme")->page == SettingsPage::QuickNavigation,"the shared catalog brings navigation behavior and appearance to one page");
    Check(find("quickNav.layout.iconSize") != catalog.staticSettings.end() && find("quickNav.color.searchBg") != catalog.staticSettings.end(),"advanced navigation fields are indexed individually");
    Check(find("quickNav.layout.iconSize")->page == SettingsPage::QuickNavigation && find("quickNav.color.searchBg")->page == SettingsPage::QuickNavigation,
        "navigation dimensions and colors belong to the functional navigation page");
    for (const auto& descriptor : catalog.staticSettings)
    {
        const auto route = SettingsRoute::ForPage(descriptor.page, descriptor.focusId);
        Check(route == CanonicalizeSettingsRoute(route) && descriptor.page != SettingsPage::AppearanceWidgets,
            "all catalog destinations are already canonical and avoid the retired appearance leaf");
    }
    for (const auto focus : {"pages.order", "pages.grid"})
    {
        const auto entry = find(focus);
        Check(entry != catalog.staticSettings.end() && entry->page == SettingsPage::DesktopPages,
            "page and grid search tasks remain on Pages and displays");
    }
    for (const auto focus : {"desktop.spacing", "desktop.categoryLayout", "personalization.luaWidgetRowHeight"})
    {
        const auto entry = find(focus);
        Check(entry != catalog.staticSettings.end() && entry->page == SettingsPage::AppearanceDesktopIcons,
            "shared layout dimensions are indexed under Global appearance");
    }
    Check(find("quickNav.prefixes")->page == SettingsPage::QuickNavigation &&
            find("quickNav.defaultCollapsed") == catalog.staticSettings.end() &&
            find("quickNav.view") == catalog.staticSettings.end(),
        "functional search settings stay on navigation and the obsolete opening preference is absent");
    Check(!find("general.advancedFeatures")->visible && find("statusBar.controlCenterPanel") == catalog.staticSettings.end(),"shared catalog honors advanced and system capability visibility");
    Check(find("general.steamRunningStatus") != catalog.staticSettings.end() && !find("general.steamRunningStatus")->visible,
        "Steam presence is hidden from search without a compatible bridge");
    SettingsSearchIndexInput steamCatalog;
    PopulateSettingsSearchCatalog(steamCatalog, [](std::string_view key) { return std::wstring(key.begin(), key.end()); },
        false, false, 0, false, true);
    Check(std::any_of(steamCatalog.staticSettings.begin(), steamCatalog.staticSettings.end(),
        [](const auto& entry) { return entry.focusId == "general.steamRunningStatus" && entry.visible; }),
        "Steam presence search does not depend on advanced-feature unlock visibility");
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
    {
        SettingsSearchIndexInput modules;
        StaticSettingSearchDescriptor descriptor;
        descriptor.page = SettingsPage::RuntimeModules; descriptor.focusId = "runtimeModules.taskbar"; descriptor.label = L"ModuleLoadingProbe";
        modules.staticSettings.push_back(descriptor);
        SettingsSearchIndex hidden(modules);
        Check(hidden.Search(L"ModuleLoadingProbe").empty(), "hidden advanced modules cannot leak through search");
        modules.advancedSettingsVisible = true; hidden.Rebuild(modules);
        Check(hidden.Search(L"ModuleLoadingProbe").size() == 1, "module search follows the About visibility preference");
    }
    TestNavigationAndFocus();
    TestConditionalPagesAndNoLeaks();
    TestWidgetFields();
    TestLanguageRebuildAndUnicodeMatching();
    TestStableRankingAndLimit();
    TestLocalizedTaskAliasesAndReadOnlyQueries();
    TestShippedTaskVocabulary(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures << " settings search index checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "settings search index checks passed\n";
    return EXIT_SUCCESS;
}
