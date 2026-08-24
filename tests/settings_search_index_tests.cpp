#include "settings_search_index.h"

#include <cstdlib>
#include <iostream>
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
        SettingsPage::DockAndTaskbar, "taskbar.theme",
        L"任务栏主题", L"设置系统任务栏外观"));
    SettingsSearchIndex index(input);

    const auto results = index.Search(L"任务栏");
    Check(results.size() == 1 &&
            results[0].route.page == SettingsPage::DockAndTaskbar &&
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

} // namespace

int main()
{
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
