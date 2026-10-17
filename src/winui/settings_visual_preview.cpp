#include "pch.h"
#include "settings_visual_preview.h"
#include "icon_library_page_presenter.h"
#include "settings/settings_search_catalog.h"
#include "SettingsShell.xaml.h"
#include "icons/icon_appearance_rules.h"
#include "icons/icon_control_source.h"
#include "icons/icon_image_codec.h"
#include "winui_runtime.h"
#include "common/l10n.h"
#include "data/data_paths.h"
#include "ui/preview/preview_png_writer.h"
#include <robuffer.h>
#include <shellapi.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Globalization.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace wr = widget_runtime;
namespace
{
template<class Ready> void PumpUntil(Ready ready)
{
    const auto deadline = GetTickCount64() + 15000;
    while (!ready())
    {
        if (GetTickCount64() >= deadline) throw std::runtime_error("settings render timed out");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) throw std::runtime_error("settings render closed");
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (!ready()) MsgWaitForMultipleObjectsEx(0, nullptr, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
void Settle(const x::FrameworkElement& root)
{
    const auto until = GetTickCount64() + 180;
    PumpUntil([&] { root.UpdateLayout(); return GetTickCount64() >= until; });
}
template<class Operation> auto Await(const Operation& operation)
{
    try { PumpUntil([&] { return operation.Status() != winrt::Windows::Foundation::AsyncStatus::Started; }); }
    catch (...) { operation.Cancel(); throw; }
    return operation.GetResults();
}
struct RenderHost
{
    WinUiRuntime runtime;
    HWND window = nullptr;
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* runtime = reinterpret_cast<WinUiRuntime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            runtime = static_cast<WinUiRuntime*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(runtime));
        }
        if (runtime) runtime->HandleWindowMessage(message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    ~RenderHost() { runtime.Detach(); if (window) DestroyWindow(window); runtime.Shutdown(); }
};
struct Apartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
template<class Visit> void Walk(const x::DependencyObject& node, Visit&& visit)
{
    visit(node);
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(node); ++i)
        Walk(x::Media::VisualTreeHelper::GetChild(node, i), visit);
}

bool IsPreviewElementVisible(const x::FrameworkElement& element)
{
    if (!element || element.ActualWidth() <= 0 || element.ActualHeight() <= 0) return false;
    auto node = element.as<x::DependencyObject>();
    while (node)
    {
        if (const auto current = node.try_as<x::UIElement>();
            current && current.Visibility() != x::Visibility::Visible) return false;
        node = x::Media::VisualTreeHelper::GetParent(node);
    }
    return true;
}

template<class Control> Control PreviewControl(const x::DependencyObject& root, std::wstring_view id)
{
    Control found{nullptr};
    Walk(root, [&](const x::DependencyObject& node) {
        if (const auto element = node.try_as<x::FrameworkElement>(); element &&
            x::Automation::AutomationProperties::GetAutomationId(element) == id)
            found = element.try_as<Control>();
    });
    if (!found) throw std::runtime_error("missing preview control: " + winrt::to_string(id));
    return found;
}

void InvokePreview(const x::FrameworkElement& element)
{
    const auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
    if (!peer) throw std::runtime_error("missing preview automation peer");
    peer.GetPattern(x::Automation::Peers::PatternInterface::Invoke)
        .as<x::Automation::Provider::IInvokeProvider>().Invoke();
}

unsigned ExpandGradientDetails(const x::DependencyObject& cards, const x::FrameworkElement& shell)
{
    const winrt::hstring expandText{_LW("largeIcon.showDetails")};
    const winrt::hstring detailsName{_LW("panelGradient.more")};
    unsigned invoked = 0;
    for (;;)
    {
        c::Button target{nullptr};
        Walk(cards, [&](const x::DependencyObject& node) {
            const auto button = node.try_as<c::Button>();
            if (!target && button && button.IsEnabled() && IsPreviewElementVisible(button) &&
                winrt::unbox_value_or<winrt::hstring>(button.Content(), winrt::hstring{}) == expandText &&
                x::Automation::AutomationProperties::GetName(button) == detailsName) target = button;
        });
        if (!target) return invoked;
        if (invoked == 32) throw std::runtime_error("gradient details did not finish expanding");
        // Invoke after traversal: the production click handler rebuilds its rows.
        const auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(target);
        if (!peer) throw std::runtime_error("gradient details automation peer is missing");
        peer.GetPattern(x::Automation::Peers::PatternInterface::Invoke)
            .as<x::Automation::Provider::IInvokeProvider>().Invoke();
        ++invoked;
        Settle(shell);
    }
}

// Deliberately no backend: these are example author fields, never a user's
// component data. All writes/pickers/searches remain unavailable.
class PreviewWidgetService final : public wr::IWidgetSettingsService
{
public:
    wr::WidgetSettingsSnapshot value;
    explicit PreviewWidgetService(bool english)
    {
        value.widgetId = L"settings-visual-example"; value.packageId = "offline.example";
        value.widgetName = english ? "Example component" : "示例组件"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        value.generation = value.revision = 1;
        value.groups.push_back({"content", english ? "Content" : "内容", {}, false, true}); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        value.groups.push_back({"details", english ? "More options" : "更多选项", {}, true, false}); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        const auto add = [&](const char* key, const char* type, const char* zh, const char* en, wr::InteractionValue current) {
            wr::WidgetSettingFieldState field;
            field.schema.key = key; field.schema.rawType = type; field.schema.label = english ? en : zh;
            field.schema.group = "content"; field.currentValue = current; field.defaultValue = current;
            if (std::string_view(type) == "select") field.schema.options = {{"a", english ? "Automatic" : "自动"}, {"b", english ? "Custom" : "自定义"}}; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            value.fields.push_back(std::move(field));
        };
        wr::InteractionValue title; title.type = wr::InteractionValue::Type::String; title.string = "SnowDesktop";
        wr::InteractionValue enabled; enabled.type = wr::InteractionValue::Type::Boolean; enabled.boolean = true;
        wr::InteractionValue size; size.type = wr::InteractionValue::Type::Number; size.number = 50.;
        wr::InteractionValue mode; mode.type = wr::InteractionValue::Type::String; mode.string = "a";
        wr::InteractionValue count; count.type = wr::InteractionValue::Type::Integer; count.integer = 5;
        add("title", "text", "标题", "Title", title); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("enabled", "bool", "显示详细内容", "Show detailed content", enabled); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("size", "range", "内容大小", "Content size", size); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("mode", "select", "显示方式", "Display mode", mode); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("count", "int", "显示数量", "Item count", count); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        value.fields.back().schema.group = "details";
        wr::WidgetSettingPresetSchema preset;
        preset.id = "standard";
        preset.label = english ? "Standard" : "标准"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        preset.isDefault = true;
        preset.values.emplace("title", title);
        value.presets.push_back(std::move(preset));
    }
    void SetEventCallbacks(SnapshotChangedCallback, SearchCompletedCallback) override {}
    wr::WidgetSettingsLoadResult Load(std::wstring) override { return {wr::WidgetSettingMutationStatus::Unchanged, value, {}, {}}; }
    wr::WidgetSettingsLoadResult Reload(std::wstring_view) override { return Load({}); }
    std::optional<wr::WidgetSettingsSnapshot> Snapshot(std::wstring_view) const override { return value; }
    void Close(std::wstring_view) override {}
    void CloseAll() override {}
    wr::WidgetSettingMutationResult SetOrdinary(const wr::WidgetSettingMutationGuard&, std::string_view, const wr::InteractionValue&) override { return {}; }
    wr::WidgetSettingMutationResult PreviewOrdinary(const wr::WidgetSettingMutationGuard&, std::string_view, const wr::InteractionValue&) override { return {}; }
    wr::WidgetSettingMutationResult SetSearchQuery(const wr::WidgetSettingMutationGuard&, std::string_view, std::string) override { return {}; }
    wr::WidgetSettingMutationResult SetSecret(const wr::WidgetSettingMutationGuard&, std::string_view, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult ChooseFilesystemHandle(const wr::WidgetSettingMutationGuard&, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult OpenEntityReferencePicker(const wr::WidgetSettingMutationGuard&, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult ClearOpaque(const wr::WidgetSettingMutationGuard&, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult ApplyPreset(const wr::WidgetSettingMutationGuard&, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult UpdateHostAppearance(const wr::WidgetSettingMutationGuard&, const wr::WidgetHostAppearancePatch&) override { return {}; }
    wr::WidgetSettingMutationResult PreviewHostAppearance(const wr::WidgetSettingMutationGuard&, const wr::WidgetHostAppearancePatch&) override { return {}; }
    wr::WidgetSettingMutationResult CommitPreview(const wr::WidgetSettingMutationGuard&) override { return {}; }
    wr::WidgetSettingMutationResult RevertPreview(const wr::WidgetSettingMutationGuard&) override { return {}; }
    wr::WidgetSettingMutationResult ResetField(const wr::WidgetSettingMutationGuard&, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult Reset(const wr::WidgetSettingMutationGuard&) override { return {}; }
    wr::WidgetSettingMutationResult StartSearch(const wr::WidgetSettingMutationGuard&, std::string_view, std::string, std::size_t) override { return {}; }
    wr::WidgetSettingMutationResult CancelSearch(const wr::WidgetSettingMutationGuard&, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult CommitSearchResult(const wr::WidgetSettingMutationGuard&, std::string_view, std::uint64_t, std::string_view) override { return {}; }
    std::optional<wr::WidgetSettingSearchSnapshot> SearchSnapshot(std::wstring_view, std::string_view) const override { return {}; }
};

constexpr SettingsPage pages[] = {
    SettingsPage::General, SettingsPage::Desktop, SettingsPage::QuickNavigation,
    SettingsPage::DesktopPages, SettingsPage::WidgetBehavior, SettingsPage::DesktopCategories,
    SettingsPage::DesktopStyle, SettingsPage::Dock, SettingsPage::StatusBar, SettingsPage::Taskbar,
    SettingsPage::AppearanceTheme, SettingsPage::AppearanceDesktopIcons, SettingsPage::AppearanceIconBeautification,
    SettingsPage::IconLibrary,
    SettingsPage::AnimationPerformance, SettingsPage::ContextMenu, SettingsPage::ThemeManager,
    SettingsPage::Widgets, SettingsPage::Calendar, SettingsPage::BackupAndData, SettingsPage::About,
    SettingsPage::DeveloperTools, SettingsPage::Debug, SettingsPage::WidgetSettings, SettingsPage::LargeIcon,
    // Revisit editors after the centralized theme page to verify reattachment.
    SettingsPage::Dock, SettingsPage::StatusBar, SettingsPage::Taskbar, SettingsPage::QuickNavigation};

void Export(const std::filesystem::path& output, std::string_view selected, bool dark,
    const std::string& locale, int width, int height, bool expanded, bool custom, bool disabled,
    bool scenarioRules, const std::string& focusId, std::string_view calendarScenario,
    bool searching, bool restore)
{
    Locale::Instance().Init((std::filesystem::path(GetExecutableDirectoryPath()) / L"lang").c_str());
    if (!Locale::Instance().HasLanguage(locale)) throw std::runtime_error("preview locale is not installed");
    Locale::Instance().SetLanguage(locale.c_str());
    Apartment apartment; winrt::check_hresult(apartment.result);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RenderHost host;
    if (!host.runtime.Initialize()) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    WNDCLASSW previewClass{};
    previewClass.lpfnWndProc = RenderHost::Procedure;
    previewClass.hInstance = GetModuleHandleW(nullptr);
    previewClass.lpszClassName = L"SnowDesktop.SettingsRenderHost";
    if (!RegisterClassW(&previewClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) winrt::throw_last_error();
    host.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        previewClass.lpszClassName, L"SnowDesktop offline settings render", WS_POPUP,
        -32000, -32000, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), &host.runtime);
    if (!host.window) winrt::throw_last_error();
    ShowWindow(host.window, SW_SHOWNOACTIVATE);
    const double scale = GetDpiForWindow(host.window) / 96.;
    SetWindowPos(host.window, nullptr, 0, 0, static_cast<int>(std::ceil(width * scale)),
        static_cast<int>(std::ceil(height * scale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    PreviewWidgetService widgets(locale == "en-US");
    widgets.value.customStyle = custom;
    widgets.value.hostAppearance.followPersonalization = !custom;
    if (custom) widgets.value.hostAppearance.presetId = "__custom";
    auto shell = winrt::make_self<winrt::SnowDesktop::implementation::SettingsShell>();
    shell->RequestedTheme(dark ? x::ElementTheme::Dark : x::ElementTheme::Light);
    shell->SetLocalizer([](std::string_view key) { return std::wstring(_LW(std::string(key).c_str())); });
    shell->SetConditionalPagesVisible(true, true);
    shell->SetWidgetSettingsService(&widgets);
    GeneralPageActions general;
    general.onboarding.settingsIndex = [] {
        SettingsSearchIndexInput index; index.developerToolsVisible = index.debugVisible = true;
        PopulateSettingsSearchCatalog(index, [](std::string_view key) { return std::wstring(_LW(std::string(key).c_str())); }, true, true);
        return index.staticSettings;
    };
    general.probeHotkey = [](SettingsHostActions::HotkeyTarget, HotkeyChord, std::uint64_t,
        std::uint64_t, HotkeyRecorder::AvailabilityCompletion complete) { complete(true, {}); };
    general.languageCatalog = [] { return std::vector<SettingsLanguageOption>{{"zh-CN", L"简体中文"}, {"en-US", L"English"}}; }; // l10n-allow: isolated bilingual preview fixture, not production UI text.
    general.queryAdvancedFeatureStatus = [] { GeneralAdvancedFeatureStatus status; status.state = GeneralAdvancedFeatureState::Registered;
        status.bridgeAvailable = status.registered = status.cardVisible = true; return status; };
    shell->SetGeneralPageActions(std::move(general));
    WidgetsPageActions widgetActions;
    widgetActions.invoke = [](std::uint64_t, WidgetsPageRequest) {};
    widgetActions.setDeveloperToolsEnabled = [](std::uint64_t, bool) { return true; };
    shell->SetWidgetsPageActions(std::move(widgetActions));
    PersonalizationPageActions appearance;
    const bool english = locale == "en-US";
    const auto themeLibrary = [english](std::uint64_t, const ThemeLibraryRequest& request) {
        ThemeLibraryResult result;
        if (request.command != ThemeLibraryCommand::Refresh) return result;
        result.succeeded = true;
        result.workshopAvailable = true;
        result.sharingAvailable = true;
        for (int index = 0; index < 4; ++index)
        {
            themes::Theme theme; theme.id = "preview-theme-" + std::to_string(index);
            theme.kind = index == 2 ? themes::Kind::Popup : index == 3 ? themes::Kind::QuickPanel : themes::Kind::Global;
            theme.name = english ? "Example theme for work and focus " : "工作与专注的示例主题 "; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            theme.name += std::to_string(index + 1); result.library.themes.emplace(theme.id, std::move(theme));
        }
        return result;
    };
    appearance.themeLibrary = themeLibrary;
    appearance.contextMenu = [english](const shell_extensions::Request&, bool) {
        shell_extensions::CatalogueView result; result.catalogue.revision = 1;
        for (int index = 0; index < 3; ++index)
        {
            shell_extensions::Registration item;
            item.id = "preview-menu-" + std::to_string(index);
            item.display.registration = item.id;
            item.display.label = english ? L"Example file action " : L"示例文件操作 "; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            item.display.label += std::to_wstring(index + 1);
            item.application = {"preview-tools", english ? L"Example tools" : L"示例工具"}; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            item.contexts = shell_extensions::ContextBit(shell_extensions::Context::File) |
                shell_extensions::ContextBit(shell_extensions::Context::Folder);
            item.linked = true; item.types = {L"*"};
            result.catalogue.rows.push_back(std::move(item));
        }
        return result;
    };
    appearance.listFonts = [] { return std::vector<app_fonts::Choice>{{{}, L"Segoe UI", L"Segoe UI"}}; };
    appearance.appliedFont = [] { return app_fonts::Selection{}; };
    shell->SetPersonalizationPageActions(std::move(appearance));
    DockPageActions dock; dock.themeLibrary = themeLibrary; shell->SetDockPageActions(std::move(dock));
    CalendarPageActions calendarActions;
    calendarActions.events = [english](std::uint64_t) -> std::optional<std::vector<calendar::CalendarEvent>> {
        std::vector<calendar::CalendarEvent> result;
        for (int index = 0; index < 3; ++index)
        {
            calendar::CalendarEvent event; event.id = "preview-event-" + std::to_string(index); event.revision = 1;
            event.title = english ? "Project planning and weekly review" : "项目规划与每周回顾"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            event.date = "2026-10-13"; event.startMinutes = 540 + index * 60; event.endMinutes = event.startMinutes + 30;
            result.push_back(std::move(event));
        }
        return result;
    };
    std::vector<calendar::CalendarSeries> previewSeries;
    for (const std::string kind : {"dates", "weekly", "monthly"})
    {
        calendar::CalendarSeries series;
        series.id = "preview-" + kind; series.revision = 1;
        series.event.title = kind + " / Preview";
        series.event.startMinutes = 600; series.event.endMinutes = 660; series.event.reminderMinutes = 15;
        series.rule.kind = kind; series.rule.startDate = "2026-10-06";
        if (kind == "dates")
        {
            series.rule.dates = {"2026-10-01", "2026-10-03", "2026-11-06"};
            series.rule.startDate = series.rule.dates.front(); series.rule.endDate = series.rule.dates.back();
        }
        else if (kind == "weekly")
        { series.rule.interval = 2; series.rule.weekdays = {2}; series.rule.endDate = "2026-11-16"; }
        else
        { series.rule.startDate = "2026-04-01"; series.rule.monthDay = 31; series.rule.endDate = "2027-04-30"; }
        series.event.date = series.rule.startDate;
        previewSeries.push_back(std::move(series));
    }
    calendarActions.series = [previewSeries](std::uint64_t) -> std::optional<std::vector<calendar::CalendarSeries>> {
        return previewSeries;
    };
    calendarActions.occurrence = [previewSeries](std::uint64_t, const std::string& id) -> std::optional<calendar::CalendarEvent> {
        const auto slash = id.find('/');
        if (slash == std::string::npos) return {};
        for (const auto& series : previewSeries)
        {
            const auto original = id.substr(slash + 1);
            if (series.id != id.substr(0, slash) || !calendar::CalendarService::MatchesRule(series.rule, original)) continue;
            auto event = series.event;
            event.id = id; event.revision = 1; event.seriesId = series.id; event.occurrenceDate = original;
            event.date = original == "2026-10-03" ? "2026-10-04" : original;
            event.occurrenceOverride = event.date != original;
            return event;
        }
        return {};
    };
    shell->SetCalendarPageActions(std::move(calendarActions));
    PageLayoutPageActions layout;
    layout.capture = [] { PageLayoutSnapshot snapshot; snapshot.revision = 1; snapshot.editable = true; snapshot.monitorCount = 2;
        snapshot.pages = {{L"example-1", L"Work / 工作", 12, 8, 10, 2}, {L"example-2", L"Projects / 项目", 10, 7, 5, 1}}; return snapshot; }; // l10n-allow: isolated bilingual preview fixture, not production UI text.
    shell->SetPageLayoutPageActions(std::move(layout));
    shell->SetLargeIconSettingsAction([expanded, disabled](LargeIconSettingsRequest request) {
        LargeIconSettingsSnapshot snapshot; snapshot.key = request.key; snapshot.name = L"Example / 示例"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        snapshot.available = snapshot.editable = !disabled; snapshot.succeeded = true;
        snapshot.itemCount = expanded ? 3 : 1;
        if (disabled) snapshot.error = "largeIcon.locked";
        snapshot.session = snapshot.revision = 1;
        snapshot.maxColumns = snapshot.maxRows = 4; return snapshot;
    });
    auto libraryFixture = std::make_shared<IconLibrarySnapshot>();
    libraryFixture->succeeded = true; libraryFixture->revision = 1;
    const auto iconPreview = output / L"图标 # % preview.png"; // l10n-allow: isolated fixture checks Unicode and reserved characters in local image paths.
    std::vector<std::uint32_t> iconPixels(96 * 96);
    for (int y = 0; y < 96; ++y) for (int xx = 0; xx < 96; ++xx)
        if ((xx - 48) * (xx - 48) + (y - 48) * (y - 48) < 40 * 40) iconPixels[y * 96 + xx] = 0xffe88c22;
    std::string iconError;
    if (!preview_png::Save(iconPreview, 96, 96, iconPixels, iconError)) throw std::runtime_error(iconError);
    IconLibraryGroup sampleGroup; sampleGroup.id = "group-" + std::string(32, '1'); sampleGroup.name = L"Sample pack"; // l10n-allow: isolated preview fixture.
    libraryFixture->groups.push_back(sampleGroup);
    for (int index = 0; index < 7; ++index)
    {
        IconLibraryEntry entry; entry.id = std::string(63, 'a') + std::to_string(index) + ".png";
        entry.name = L"Preview icon " + std::to_wstring(index + 1); // l10n-allow: isolated preview fixture.
        entry.width = entry.height = 96; entry.format = "png"; entry.previewPath = iconPreview.wstring();
        entry.missing = index == 6; if (index % 2 == 0) entry.groupIds.push_back(sampleGroup.id);
        libraryFixture->entries.push_back(std::move(entry));
    }
    // Optional original input exercises the production library, including the
    // WebP decoder, without touching the desktop host or the user's library.
    wchar_t previewSource[32768]{};
    const auto previewSourceLength=GetEnvironmentVariableW(L"SNOWDESKTOP_ICON_LIBRARY_PREVIEW_SOURCE",previewSource,32768);
    if(previewSourceLength>0&&previewSourceLength<32768)
    {
        IconLibrary library(output/L"fixture-library"); IconLibraryRequest request;
        request.action="import"; request.paths={previewSource};
        const auto imported=library.Invoke(request);
        if(!imported.succeeded||imported.entries.size()!=1||imported.entries[0].missing)throw std::runtime_error("original preview import failed");
        libraryFixture->entries[1]=imported.entries[0];
    }
    libraryFixture->entries[2].groupNames.emplace(sampleGroup.id, L"Frost Browser");
    libraryFixture->entries[6].groupNames.emplace(sampleGroup.id, L"Frost Browser");
    libraryFixture->targetName = L"FrostBrowser.lnk";
    const auto pickerRequests=std::make_shared<std::vector<IconLibraryRequest>>();
    shell->SetIconLibraryAction([libraryFixture,pickerRequests](IconLibraryRequest request) {
        if (request.action != "read" && request.action != "refresh" && request.action!="apply" && request.action!="cancel") throw std::runtime_error("preview library mutations are disabled");
        if(request.action=="apply"||request.action=="cancel")pickerRequests->push_back(request);
        auto result=*libraryFixture; result.target=request.target; result.session=1; return result;
    });
    struct AppearanceFixture
    {
        std::vector<IconAppearanceRecord> entries;
        IconBeautifySettings globalSettings;
        icon_appearance::EditPreview preview;
        unsigned liveUpdates = 0;
        std::uint64_t revision = 1, session = 0;
        std::wstring target;
        std::string png;
    };
    const auto appearanceFixture = std::make_shared<AppearanceFixture>();
    {
        auto style = icon_beautify::MakePreset(IconBeautifyPreset::FrostedGlassDark); style.mode = 1;
        appearanceFixture->entries.push_back({L"control:start", _LW("iconLibrary.startIcon"), style, {}});
        style.enabled = false;
        appearanceFixture->entries.push_back({L"recycle-empty:PREVIEW", _LW("iconLibrary.recycleEmpty"), style, {}});
        icon_image::Image source; std::string error;
        if (!icon_image::Decode(libraryFixture->entries[1].previewPath, 128, source, error) ||
            !icon_image::EncodePng(source, appearanceFixture->png, error)) throw std::runtime_error("appearance preview source failed");
    }
    shell->SetIconAppearanceAction([appearanceFixture, dark](IconAppearanceRequest request) {
        auto& state = *appearanceFixture; IconAppearanceSnapshot result;
        result.succeeded = true; result.available = true;
        if (request.action == "begin") { state.preview.Clear(); state.target = request.target; ++state.session; }
        if (request.action == "previewApply" || request.action == "previewInherit")
        {
            std::optional<IconAppearanceRecord> value;
            if (request.action == "previewApply") value = IconAppearanceRecord{request.target, {}, request.settings, request.themeId, true, request.followGlobalTheme};
            result.succeeded = state.preview.Update(request, state.target, state.session, state.revision, std::move(value));
            if (result.succeeded) ++state.liveUpdates;
            else result.error = "iconLibrary.stale";
            return result;
        }
        if (request.action == "close" || (request.action == "cancel" && request.target == state.target && request.session == state.session))
        { state.preview.Clear(); state.target.clear(); ++state.session; }
        if (request.action == "apply" || request.action == "inherit")
        {
            if ((request.action == "apply" || request.session) && !icon_appearance::CurrentEdit(request, state.target, state.session, state.revision))
            { result.succeeded = false; result.error = "iconLibrary.stale"; return result; }
            std::erase_if(state.entries, [&](const auto& record) { return record.target == request.target; });
            if (request.action == "apply") state.entries.push_back({request.target, _LW("iconLibrary.searchIcon"), request.settings, request.themeId, true, request.followGlobalTheme});
            ++state.revision; state.preview.Clear(); state.target.clear(); ++state.session;
        }
        result.current.target = request.target;
        result.current.name = request.target == L"control:search" ? _LW("iconLibrary.searchIcon") : _LW("iconLibrary.startIcon");
        result.globalSettings = state.globalSettings;
        result.current.settings = state.globalSettings;
        result.current.followGlobalTheme = true;
        for (const auto& entry : state.entries) if (entry.target == request.target) { result.current = entry; result.independent = true; }
        result.current.settings = icon_appearance::Resolve(state.globalSettings, result.independent ? &result.current : nullptr, false, false);
        result.entries = state.entries;
        for (auto& record : result.entries) record.settings = icon_appearance::Resolve(state.globalSettings, &record, false, false);
        result.session = state.session; result.revision = state.revision;
        result.previewPng = state.png;
        if (request.target == L"control:search")
        {
            std::string error;
            result.nativeControlSource = true;
            if (!icon_image::EncodePng(icon_control::MakeSource(request.target, !dark), result.previewPng, error))
                throw std::runtime_error("native control preview source failed");
        }
        return result;
    });
    bool loaded = false;
    const auto token = shell->Loaded([&](const auto&, const auto&) { loaded = true; });
    if (!host.runtime.Attach(host.window, shell.as<x::UIElement>())) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    host.runtime.ResizeToClient(); PumpUntil([&] { return loaded; }); shell->Loaded(token);
    std::ofstream manifest(output / L"render.tsv");
    manifest << "page\timage\twidthDip\theightDip\tscrollY\tscrollMax\n";
    std::ofstream rows(output / L"rows.tsv");
    std::ofstream choices(output / L"choices.tsv");
    choices << "page\tlabel\tcontrolRight\teditorRight\tcontrolWidth\n";
    rows << "page\tlabel\tx\ty\twidth\theight\tpaddingLeft\tpaddingTop\tpaddingRight\tpaddingBottom\teditorX\teditorY\teditorWidth\teditorHeight\n";
    std::ofstream actions(output / L"actions.tsv");
    actions << "page\tgradientDetailsInvoked\tvisibleRemoveStopButtons\n";
    std::uint64_t revision = 0;
    bool rendered = false;
    for (const auto page : pages)
    {
        const std::string key(SettingsPageKey(page));
        if (selected != "all" && selected != key) continue;
        rendered = true;
        SettingsSnapshot snapshot; snapshot.revision = ++revision; snapshot.generation = 1;
        snapshot.initialized = snapshot.sessionActive = true;
        snapshot.values.general.dockEnabled = !disabled;
        snapshot.values.general.statusBar.enabled = !disabled;
        snapshot.values.general.statusBar.batteryPercentage = expanded;
        snapshot.values.personalization.groupTabHoverSwitch = expanded;
        snapshot.values.personalization.categoryTabHoverSwitch = expanded;
        snapshot.values.personalization.popupHoverOpen = expanded;
        snapshot.values.dock.edgeRevealGesture = expanded ? 1 : 0;
        if (expanded && !custom)
        {
            snapshot.values.general.statusBar.position = snapshot.values.dock.position;
            snapshot.values.dock.edgeAttached = true;
        }
        snapshot.values.general.widgetDeveloperToolsEnabled = true;
        snapshot.values.dock.fullscreenExceptions = {
            {L"C:\\Preview\\Presentation.exe", DockFullscreenPolicy::Allow},
            {L"C:\\Preview\\Game.exe", DockFullscreenPolicy::FullProtection}};
        if (custom)
        {
            auto& icons = snapshot.values.desktop.iconBeautify;
            icons.enabled = true; icons.preset = IconBeautifyPreset::Custom;
            icons.glassEnabled = icons.edgeHighlightEnabled = icons.gradientEnabled = true;
            icons.filterEnabled = icons.outlineEnabled = true;
            snapshot.values.personalization.backgroundPreset = kAppearancePresetCustom;
            snapshot.values.personalization.panelGradient.enabled = true;
            snapshot.values.personalization.panelGradient.stops = {
                {0, 0xb6d6ef, .65}, {.5, 0xc8d0ef, .65}, {1, 0xd5c7ef, .65}};
            snapshot.values.dock.followComponentAppearance = false;
            snapshot.values.dock.appearancePreset = kAppearancePresetCustom;
            snapshot.values.dock.systemTaskbarBackdropEnabled = true;
            snapshot.values.dock.systemTaskbarFollowPersonalization = false;
            snapshot.values.dock.systemTaskbarAppearance.backgroundPreset = kAppearancePresetCustom;
            for (auto* theme : {&snapshot.values.general.quickNavigationAppearance,
                &snapshot.values.general.collectionPopupAppearance, &snapshot.values.general.statusBar.theme})
            { theme->mode = 4; theme->customized = true; theme->appearance.backgroundPreset = kAppearancePresetCustom; }
        }
        if (scenarioRules)
        {
            auto& taskbarRule = snapshot.values.dock.systemTaskbarShellUi;
            taskbarRule.enabled = true;
            taskbarRule.themeMode = SystemTaskbarThemeMode::Custom;
            taskbarRule.appearance.backgroundPreset = kAppearancePresetCustom;
            auto& statusRule = snapshot.values.general.statusBar.noWindow;
            statusRule.enabled = true;
            statusRule.theme.mode = 4;
            statusRule.theme.customized = true;
            statusRule.theme.appearance.backgroundPreset = kAppearancePresetCustom;
        }
        snapshot.domainRevisions = {revision, revision, revision, revision, revision, revision, revision};
        snapshot.route = SettingsRoute::ForPage(page);
        if (page == SettingsPage::WidgetSettings) snapshot.route = SettingsRoute::ForWidget(widgets.value.widgetId);
        if (page == SettingsPage::LargeIcon) snapshot.route.itemKey = L"settings-visual-example";
        snapshot.route.focusId = focusId;
        appearanceFixture->globalSettings = snapshot.values.desktop.iconBeautify;
        if (!shell->ApplySnapshot(snapshot)) throw std::runtime_error("preview route refused: " + key);
        HomeAboutStatusPatch status; status.generation = 1; status.revision = revision;
        status.usageGuideExpanded = expanded; status.installedWidgetCount = 3;
        status.backupState = SettingsBackupState::Ready; status.backupCount = 3;
        status.debugDataDirectory = L"C:\\Preview\\SnowDesktop";
        status.debugDesktopDirectory = L"C:\\Preview\\Desktop";
        (void)shell->ApplyHomeAboutStatusPatch(status);
        if (page == SettingsPage::Widgets || page == SettingsPage::DeveloperTools)
        {
            WidgetsPageSnapshot packages; packages.generation = 1; packages.revision = revision;
            if (searching) packages.task.kind = WidgetsPageTaskKind::Searching;
            packages.workshopAvailable = true;
            packages.developerPublisherAvailable = true;
            packages.developmentWorkspace = L"C:\\Preview\\Components";
            packages.componentCliPath = L"C:\\Preview\\SnowDesktop\\snowwidget.exe";
            WidgetAgentSkillTargetSnapshot skill; skill.id = "shared";
            skill.targetPath = L"C:\\Preview\\Skills";
            skill.state = WidgetAgentSkillInstallState::Current;
            skill.installed = skill.selected = true; skill.installedRevision = skill.bundledRevision = 1;
            packages.agentSkills.push_back(std::move(skill));
            for (int index = 0; index < 3; ++index)
            {
                InstalledWidgetPackageSnapshot package;
                package.packageId = L"preview-package-" + std::to_wstring(index);
                package.name = english ? L"Workspace overview and daily progress" : L"工作区概览与每日进度"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
                package.name += L" " + std::to_wstring(index + 1);
                package.description = english ? L"An example component with a longer description for layout review." : L"用于检查长名称、状态与操作排列的示例组件。"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
                package.version = L"1.2.0"; package.author = L"SnowDesktop"; package.builtIn = index == 0;
                package.enabled = index != 2; packages.installed.push_back(std::move(package));
                WidgetRuntimeDiagnosticSnapshot diagnostic;
                diagnostic.instanceId = L"widget-preview-" + std::to_wstring(index + 1);
                diagnostic.packageId = L"d81cee29-8023-47d8-bfaf-f39d8d7fd5ac";
                diagnostic.displayName = english ? L"Workspace overview" : L"工作区概览"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
                diagnostic.displayName += L" " + std::to_wstring(index + 1);
                diagnostic.valid = diagnostic.hasManifest = true;
                diagnostic.scriptPath = L"C:\\Preview\\Components\\overview\\main.lua";
                packages.diagnostics.push_back(std::move(diagnostic));
            }
            if (!shell->ApplyWidgetsPageSnapshot(packages)) throw std::runtime_error("preview component snapshot refused");
        }
        if (page == SettingsPage::BackupAndData)
        {
            BackupDataPageSnapshot backups; backups.generation = 1; backups.revision = revision; backups.initialized = true;
            backups.dataDirectory = L"C:\\Preview\\SnowDesktop"; backups.fullBackupDirectory = L"D:\\Preview\\Backups";
            for (int index = 0; index < 3; ++index)
            {
                const auto id = L"preview-backup-" + std::to_wstring(index);
                const auto name = (english ? L"Before reorganizing the workspace " : L"重新整理工作区之前的备份 ") + std::to_wstring(index + 1); // l10n-allow: isolated bilingual preview fixture, not production UI text.
                backups.layoutBackups.push_back({id, name, L"2026-10-13 09:00", index == 0});
                backups.fullBackups.push_back({id, name, L"2026-10-13 09:00", L"Manual", 84, 12582912, false});
            }
            if (!shell->ApplyBackupDataPageSnapshot(backups)) throw std::runtime_error("preview backup snapshot refused");
        }
        Settle(shell.as<x::FrameworkElement>());
        const auto cards = shell->FindName(L"PageCards").as<c::StackPanel>();
        if (page == SettingsPage::Dock)
        {
            Walk(cards, [&](const x::DependencyObject& node) {
                const auto button = node.try_as<c::Button>();
                if (!button || x::Automation::AutomationProperties::GetName(button) != _LW("iconAppearance.edit")) return;
                const auto parent = x::Media::VisualTreeHelper::GetParent(button).as<c::Grid>();
                const auto bounds = button.TransformToVisual(parent).TransformBounds({0,0,static_cast<float>(button.ActualWidth()),static_cast<float>(button.ActualHeight())});
                if (bounds.X < 0 || bounds.X + bounds.Width > parent.ActualWidth() + 1 || !button.Content().try_as<c::SymbolIcon>())
                    throw std::runtime_error("Dock icon appearance action exceeds its native settings column");
            });
        }
        if (page == SettingsPage::AppearanceIconBeautification)
        {
            auto list = PreviewControl<c::ListView>(cards, L"IconAppearanceList");
            if (list.Items().Size() != 2) throw std::runtime_error("following icons must not enter individual appearance list");
            auto targetRoute = SettingsRoute::ForPage(SettingsPage::AppearanceIconBeautification); targetRoute.itemKey = L"control:start";
            if (!shell->Navigate(targetRoute)) throw std::runtime_error("Start appearance route rejected");
            const auto open = [&] {
                shell->OpenRequestedIconAppearance(); c::ContentDialog opened{nullptr};
                PumpUntil([&] {
                    for (const auto& popup : x::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(shell->XamlRoot()))
                        if (popup.Child()) Walk(popup.Child(), [&](const x::DependencyObject& node) {
                            if (const auto dialog = node.try_as<c::ContentDialog>(); dialog && x::Automation::AutomationProperties::GetAutomationId(dialog) == L"IconAppearanceEditor") opened = dialog;
                        });
                    return opened && opened.ActualWidth() > 0;
                });
                Settle(opened);
                if (opened.ActualTheme() != shell->ActualTheme()) throw std::runtime_error("icon appearance dialog did not inherit its parent theme");
                return opened;
            };
            const auto close = [&](const c::ContentDialog& opened) {
                bool closed = false; const auto token = opened.Closed([&](const auto&, const auto&) { closed = true; });
                opened.Hide(); PumpUntil([&] { return closed; }); opened.Closed(token); Settle(shell.as<x::FrameworkElement>());
            };
            const auto captureAppearance = [&](const x::FrameworkElement& opened, const wchar_t* name) {
                Settle(opened);
                if (opened.ActualWidth() > shell->ActualWidth() || opened.ActualHeight() > shell->ActualHeight())
                    throw std::runtime_error("individual icon editor exceeds its settings window");
                x::Media::Imaging::RenderTargetBitmap bitmap; Await(bitmap.RenderAsync(opened));
                const auto pixels = Await(bitmap.GetPixelsAsync()); BYTE* bytes = nullptr;
                winrt::check_hresult(pixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
                std::string imageError;
                if (!preview_png::Save(output / name, bitmap.PixelWidth(), bitmap.PixelHeight(),
                    {reinterpret_cast<const std::uint32_t*>(bytes), pixels.Length()/4}, imageError)) throw std::runtime_error(imageError);
            };
            const auto applyAndClose = [&](const c::ContentDialog& opened, const c::Button& button, const char* stage) {
                std::ofstream(output / L"icon-appearance-stage.txt") << stage;
                const auto closed = std::make_shared<bool>(false);
                const auto token = opened.Closed([closed](const auto&, const auto&) { *closed = true; });
                try {
                    if (!opened.IsLoaded()) throw std::runtime_error("icon appearance dialog closed before its Apply action");
                    InvokePreview(button); PumpUntil([&] { return *closed; });
                }
                catch (...) {
                    opened.Closed(token);
                    try { captureAppearance(opened, L"icon-appearance-apply-failure.png"); } catch (...) {}
                    throw;
                }
                opened.Closed(token); Settle(shell.as<x::FrameworkElement>());
            };
            auto first = open();
            auto preview = PreviewControl<c::Image>(first, L"IconAppearancePreview");
            if (!preview.Source().try_as<x::Media::Imaging::WriteableBitmap>()) throw std::runtime_error("per-icon preview has no decoded pixels");
            c::ComboBox imageMode{nullptr};
            Walk(first, [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>(); combo &&
                    x::Automation::AutomationProperties::GetName(combo) == _LW("iconStyle.strategy")) imageMode = combo;
            });
            if (!imageMode) throw std::runtime_error("image no-enlargement selector is missing");
            imageMode.SelectedIndex(2);
            PumpUntil([&] { return appearanceFixture->liveUpdates > 0; });
            const auto liveStart = appearanceFixture->preview.Effective(L"control:start", nullptr);
            if (!liveStart || liveStart->settings.mode != 2 || appearanceFixture->entries.front().settings.mode != 1)
                throw std::runtime_error("live independent preview was not sent or changed the saved style before Apply");
            captureAppearance(first, L"icon-appearance-image-no-upscale.png");
            close(first);
            if (appearanceFixture->preview.Effective(L"control:start", nullptr))
                throw std::runtime_error("cancel left a live independent appearance active");
            auto second = open(); close(second);
            if (appearanceFixture->entries.size() != 2) throw std::runtime_error("cancel or reopening unexpectedly saved an icon override");
            targetRoute.itemKey = L"control:search";
            if (!shell->Navigate(targetRoute)) throw std::runtime_error("Search appearance route rejected");
            auto editor = open();
            const auto sourceMode = PreviewControl<c::ComboBox>(editor, L"IconAppearanceSource");
            if (sourceMode.SelectedIndex() != 0) throw std::runtime_error("unconfigured icons must default to full inheritance");
            sourceMode.SelectedIndex(1); Settle(editor);
            c::ComboBox mode{nullptr}, theme{nullptr};
            Walk(editor, [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>())
                {
                    const auto name = x::Automation::AutomationProperties::GetName(combo);
                    if (name == _LW("iconStyle.strategy")) mode = combo;
                    if (name == _LW("app.settings.icon_beautify")) theme = combo;
                }
            });
            if (!mode || !theme) throw std::runtime_error("shared processing mode/theme selectors are missing");
            if (mode.Items().Size() != 3 || theme.SelectedIndex() != 6)
                throw std::runtime_error("new per-icon editor must default to global theme with three independent strategies");
            mode.SelectedIndex(2);
            appearanceFixture->globalSettings = icon_beautify::MakePreset(IconBeautifyPreset::FrostedGlassDark);
            appearanceFixture->globalSettings.mode = 1;
            auto globalChange = snapshot; globalChange.route = targetRoute; globalChange.revision = ++revision; ++globalChange.domainRevisions.desktop;
            globalChange.values.desktop.iconBeautify = appearanceFixture->globalSettings;
            if (!shell->ApplySnapshot(globalChange)) throw std::runtime_error("live global theme update rejected");
            Settle(editor);
            const auto liveSearch = appearanceFixture->preview.Effective(L"control:search", nullptr);
            if (!liveSearch || !liveSearch->followGlobalTheme || liveSearch->settings.mode != 2 || appearanceFixture->entries.size() != 2)
                throw std::runtime_error("Start/Search live processing preview failed to retain global theme without saving");
            // Enabling the global theme first realizes its dependent strategy control.
            c::ComboBox globalMode{nullptr};
            Walk(shell->FindName(L"PageCards").as<x::DependencyObject>(), [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>(); combo &&
                    x::Automation::AutomationProperties::GetName(combo) == _LW("iconStyle.strategy")) globalMode = combo;
            });
            if (!globalMode || globalMode.Items().Size() != 2) throw std::runtime_error("global processing must expose exactly two strategies");
            if (theme.SelectedIndex() != 6 || mode.SelectedIndex() != 2)
                throw std::runtime_error("global theme update overwrote independent processing or theme inheritance");
            captureAppearance(editor, L"icon-appearance-follow-global.png");
            mode.SelectedIndex(0); theme.SelectedIndex(3);
            captureAppearance(editor, L"icon-appearance-native-smart.png");
            mode.SelectedIndex(2);
            captureAppearance(editor, L"icon-appearance-editor.png");
            c::Button apply{nullptr};
            Walk(editor, [&](const x::DependencyObject& node) { if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"PrimaryButton") apply = button; });
            if (!apply) throw std::runtime_error("individual appearance Apply button is missing");
            applyAndClose(editor, apply, "apply-search");
            if (appearanceFixture->entries.size() != 3) throw std::runtime_error("Search Apply did not save its independent record");
            if (appearanceFixture->entries.back().target != L"control:search" || appearanceFixture->entries.back().settings.mode != 2 ||
                appearanceFixture->entries.back().settings.preset != IconBeautifyPreset::FrostedGlassDark)
                throw std::runtime_error("Search mode/theme failed to reach the independent settings action");
            auto followEditor = open(); c::ComboBox followTheme{nullptr};
            Walk(followEditor, [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>(); combo &&
                    x::Automation::AutomationProperties::GetName(combo) == _LW("app.settings.icon_beautify")) followTheme = combo;
            });
            if (!followTheme) throw std::runtime_error("per-icon theme selector is missing on reopen");
            followTheme.SelectedIndex(6); Settle(followEditor);
            c::Button followApply{nullptr};
            Walk(followEditor, [&](const x::DependencyObject& node) {
                if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"PrimaryButton") followApply = button;
            });
            if (!followApply) throw std::runtime_error("theme inheritance Apply is missing");
            applyAndClose(followEditor, followApply, "apply-followed-theme");
            if (!appearanceFixture->entries.back().followGlobalTheme || appearanceFixture->entries.back().settings.mode != 2)
                throw std::runtime_error("saving followed theme lost independent processing");
            list = PreviewControl<c::ListView>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceList");
            auto filter = PreviewControl<c::TextBox>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceSearch");
            filter.Text(_LW("iconLibrary.searchIcon")); Settle(shell.as<x::FrameworkElement>());
            if (list.Items().Size() != 1) throw std::runtime_error("individual icon list search failed");
            list.SelectedIndex(0);
            InvokePreview(PreviewControl<c::Button>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceInherit"));
            if (appearanceFixture->entries.size() != 2 || list.Items().Size() != 0) throw std::runtime_error("following global settings left an individual list record");
            filter.Text(L""); Settle(shell.as<x::FrameworkElement>());
            for (int remaining = 0; remaining < 2 && list.Items().Size(); ++remaining)
            {
                list.SelectedIndex(0);
                InvokePreview(PreviewControl<c::Button>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceInherit"));
                if (list.Items().Size() == 1)
                {
                    const auto single = PreviewControl<c::Grid>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceListContent");
                    captureAppearance(single, L"icon-appearance-one-item.png");
                }
            }
            Settle(shell.as<x::FrameworkElement>());
            const auto empty = PreviewControl<c::TextBlock>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceEmpty");
            if (empty.Visibility() != x::Visibility::Visible || list.Visibility() != x::Visibility::Collapsed)
                throw std::runtime_error("individual appearance empty state failed to replace the list");
            x::Media::Imaging::RenderTargetBitmap emptyBitmap; Await(emptyBitmap.RenderAsync(shell.as<x::FrameworkElement>()));
            const auto emptyPixels = Await(emptyBitmap.GetPixelsAsync()); BYTE* emptyBytes = nullptr;
            winrt::check_hresult(emptyPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&emptyBytes));
            std::string emptyError;
            if (!preview_png::Save(output / L"icon-appearance-empty.png", emptyBitmap.PixelWidth(), emptyBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(emptyBytes), emptyPixels.Length()/4}, emptyError)) throw std::runtime_error(emptyError);
            std::ofstream(output / L"icon-appearance.tsv") << "following-hidden\tcancel\treopen\tpreview\tsearch-mode-theme\tremove-to-follow\npassed\tpassed\tpassed\tpassed\tpassed\tpassed\n";
        }
        if (page == SettingsPage::IconLibrary)
        {
            const auto grid = PreviewControl<c::GridView>(cards, L"IconLibraryGrid");
            const auto query = PreviewControl<c::TextBox>(cards, L"IconLibrarySearch");
            const auto groups = PreviewControl<c::ComboBox>(cards, L"IconLibraryGroups");
            const auto loadedImages = [&] {
                unsigned count = 0; bool complete = true;
                Walk(grid, [&](const x::DependencyObject& node) {
                    if (const auto image = node.try_as<c::Image>(); image &&
                        std::wstring_view(x::Automation::AutomationProperties::GetAutomationId(image)).starts_with(L"IconLibraryPreview-") &&
                        !winrt::unbox_value_or<bool>(image.Tag(), false))
                    {
                        ++count;
                        const auto source = image.Source().try_as<x::Media::Imaging::BitmapImage>();
                        complete = complete && source && source.PixelWidth() == 96 && source.PixelHeight() == 96;
                    }
                });
                return count == 6 && complete;
            };
            const auto waitForLibraryImages=[&](std::string_view stage) {
                try { PumpUntil(loadedImages); }
                catch (...) {
                    std::ofstream trace(output/L"icon-library-failure.tsv");
                    trace<<"stage\t"<<stage<<'\n';
                    Walk(grid,[&](const x::DependencyObject& node) {
                        trace<<winrt::to_string(winrt::get_class_name(node));
                        if(const auto element=node.try_as<x::FrameworkElement>())trace<<'\t'<<element.ActualWidth()<<'\t'<<element.ActualHeight()<<'\t'<<winrt::to_string(x::Automation::AutomationProperties::GetAutomationId(element));
                        if(const auto image=node.try_as<c::Image>())
                            if(const auto source=image.Source().try_as<x::Media::Imaging::BitmapImage>())trace<<"\timage="<<source.PixelWidth()<<'x'<<source.PixelHeight();
                        trace<<'\n';
                    });
                    try {
                        x::Media::Imaging::RenderTargetBitmap bitmap;Await(bitmap.RenderAsync(shell.as<x::UIElement>()));
                        const auto buffer=Await(bitmap.GetPixelsAsync());BYTE* bytes=nullptr;
                        winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
                        std::string error;preview_png::Save(output/L"icon-library-failure.png",bitmap.PixelWidth(),bitmap.PixelHeight(),
                            {reinterpret_cast<const std::uint32_t*>(bytes),buffer.Length()/4},error);
                    }catch(...){}
                    throw std::runtime_error("icon library images timed out: "+std::string(stage));
                }
            };
            // Library activation may choose the first pack. This scenario
            // intentionally verifies all formats before testing group filters.
            groups.SelectedIndex(0); Settle(shell.as<x::FrameworkElement>());
            waitForLibraryImages("initial page");
            const auto libraryViewer = shell->FindName(L"PageScrollViewer").as<c::ScrollViewer>();
            const auto librarySurface = shell->FindName(L"PageSurface").as<c::Border>();
            const auto gridBottom = grid.TransformToVisual(libraryViewer).TransformPoint({0,static_cast<float>(grid.ActualHeight())}).Y;
            if (std::abs(gridBottom - (libraryViewer.ViewportHeight() - librarySurface.Margin().Bottom)) > 2.)
                throw std::runtime_error("library scroll list does not fill the remaining page viewport");
            if (query.Header() || grid.Items().Size() != 7 || groups.Items().Size() != 3)
                throw std::runtime_error("library tiles/groups/header mismatch: tiles=" + std::to_string(grid.Items().Size()) +
                    " groups=" + std::to_string(groups.Items().Size()) + " header=" + std::to_string(query.Header() ? 1 : 0));
            grid.SelectedIndex(0); groups.SelectedIndex(2); Settle(shell.as<x::FrameworkElement>());
            if (grid.Items().Size() != 4 || grid.SelectedItems().Size() != 1)
                throw std::runtime_error("group filter lost a visible selected resource identity");
            const auto selectFiltered=PreviewControl<c::AppBarButton>(cards,L"IconLibrarySelectFiltered");
            InvokePreview(selectFiltered);
            if(grid.SelectedItems().Size()!=4)throw std::runtime_error("select filtered icons omitted resources in the current group");
            query.Text(L"no-matching-icon"); Settle(shell.as<x::FrameworkElement>());
            if (grid.Items().Size()) throw std::runtime_error("library search failed to filter resources");
            query.Text(L""); groups.SelectedIndex(0); Settle(shell.as<x::FrameworkElement>()); waitForLibraryImages("restore filter");
            const auto entries = libraryFixture->entries; libraryFixture->entries.clear(); ++libraryFixture->revision;
            PumpUntil([&] { return grid.Items().Size() == 0; });
            bool oldHint = false;
            Walk(cards, [&](const x::DependencyObject& node) {
                if (const auto text = node.try_as<c::TextBlock>(); text && text.Text() == _LW("iconLibrary.empty")) oldHint = true;
            });
            if (oldHint) throw std::runtime_error("removed empty-library hint is still mounted");
            libraryFixture->entries = entries; ++libraryFixture->revision;
            PumpUntil([&] { return grid.Items().Size() == 7 && loadedImages(); });
            struct PickerResult { bool completed = false; std::string id; };
            const auto pickerResult = std::make_shared<PickerResult>();
            const auto pickerReads = std::make_shared<unsigned>(0);
            struct PickerCleanup { std::function<void()> cancel; ~PickerCleanup() { if (cancel) cancel(); } } pickerCleanup;
            pickerCleanup.cancel = IconLibraryPagePresenter::Pick(shell->XamlRoot(),
                [](std::string_view name) { return std::wstring(_LW(std::string(name).c_str())); },
                [libraryFixture,pickerReads](IconLibraryRequest request) {
                    if (request.action != "read") throw std::runtime_error("picker preview mutations are disabled");
                    ++*pickerReads;
                    auto result = *libraryFixture; result.targetName.clear(); return result;
                }, entries.front().id, [pickerResult](std::string id) { pickerResult->id = std::move(id); pickerResult->completed = true; }, libraryFixture->targetName);
            c::ContentDialog pickerDialog{nullptr};
            PumpUntil([&] {
                for (const auto& popup : x::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(shell->XamlRoot()))
                    if (popup.Child()) Walk(popup.Child(), [&](const x::DependencyObject& node) {
                        if (const auto dialog = node.try_as<c::ContentDialog>()) pickerDialog = dialog;
                    });
                return pickerDialog && pickerDialog.ActualWidth() > 0;
            });
            const auto pickerGrid = PreviewControl<c::GridView>(pickerDialog, L"IconLibraryGrid");
            const auto pickerQuery = PreviewControl<c::TextBox>(pickerDialog, L"IconLibrarySearch");
            const auto pickerGroups = PreviewControl<c::ComboBox>(pickerDialog, L"IconLibraryGroups");
            Settle(pickerDialog);
            if (pickerGroups.SelectedIndex() != 0 || pickerGrid.Items().Size() != 7 ||
                winrt::unbox_value<winrt::hstring>(pickerGroups.SelectedItem().as<c::ComboBoxItem>().Tag()) != L"")
                throw std::runtime_error("fresh picker did not default to all icons");
            if (pickerDialog.ActualTheme() != shell->ActualTheme())
                throw std::runtime_error("icon picker does not inherit its owner window theme");
            PumpUntil([&] { return pickerGrid.ContainerFromIndex(0) && pickerGrid.ContainerFromIndex(1) && pickerGrid.ContainerFromIndex(2); });
            const auto firstTile = pickerGrid.ContainerFromIndex(0).as<x::FrameworkElement>().TransformToVisual(pickerGrid).TransformPoint({0,0});
            const auto secondTile = pickerGrid.ContainerFromIndex(1).as<x::FrameworkElement>().TransformToVisual(pickerGrid).TransformPoint({0,0});
            const auto thirdTile = pickerGrid.ContainerFromIndex(2).as<x::FrameworkElement>().TransformToVisual(pickerGrid).TransformPoint({0,0});
            const int firstRowColumns = 1 + (std::abs(firstTile.Y - secondTile.Y) <= 1 ? 1 : 0) +
                (std::abs(firstTile.Y - thirdTile.Y) <= 1 ? 1 : 0);
            if (pickerDialog.Content().as<x::FrameworkElement>().ActualWidth() >= 500. &&
                (std::abs(firstTile.Y - secondTile.Y) > 1 || std::abs(firstTile.Y - thirdTile.Y) > 1 || firstTile.X >= secondTile.X || secondTile.X >= thirdTile.X))
                throw std::runtime_error("default icon picker does not show three columns");
            std::ofstream(output / L"icon-library-layout.tsv") << "gridHeight\tviewportHeight\tpickerWidth\tfirstRowColumns\n" <<
                grid.ActualHeight() << '\t' << libraryViewer.ViewportHeight() << '\t' << pickerDialog.Content().as<x::FrameworkElement>().ActualWidth() << '\t' << firstRowColumns << '\n';
            x::Media::Imaging::RenderTargetBitmap columnsBitmap; Await(columnsBitmap.RenderAsync(pickerDialog));
            const auto columnsPixels = Await(columnsBitmap.GetPixelsAsync()); BYTE* columnsBytes = nullptr;
            winrt::check_hresult(columnsPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&columnsBytes));
            if (!preview_png::Save(output / L"icon-picker-columns.png", columnsBitmap.PixelWidth(), columnsBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(columnsBytes), columnsPixels.Length() / 4}, iconError)) throw std::runtime_error(iconError);
            pickerGroups.SelectedIndex(1); Settle(pickerDialog);
            if (winrt::unbox_value<winrt::hstring>(pickerGroups.SelectedItem().as<c::ComboBoxItem>().Tag()) != L"recommended" ||
                pickerGrid.Items().Size() != 1 || winrt::unbox_value<winrt::hstring>(pickerGrid.Items().GetAt(0)) != winrt::to_hstring(entries[2].id) ||
                pickerDialog.IsPrimaryButtonEnabled())
                throw std::runtime_error("name recommendations did not use pack aliases, exclude missing assets or require explicit selection");
            pickerQuery.Text(L"Frost"); Settle(pickerDialog);
            if(pickerGrid.Items().Size()!=1)throw std::runtime_error("search ignored the alias used to recommend an asset");
            pickerQuery.Text(L""); Settle(pickerDialog);
            pickerGrid.SelectedIndex(0);
            // Polling must retain the user's filter and the captured target name,
            // even though ordinary library snapshots have no target metadata.
            const auto initialReads = *pickerReads;
            PumpUntil([&] {
                bool loaded=false; Walk(pickerGrid,[&](const x::DependencyObject& node) {
                    if(const auto image=node.try_as<c::Image>();image)
                        if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==96)loaded=true;
                });
                return *pickerReads > initialReads && loaded && pickerDialog.IsPrimaryButtonEnabled();
            });
            if(pickerGroups.SelectedIndex()!=1||pickerGrid.Items().Size()!=1)throw std::runtime_error("polling reset the recommendation filter");
            x::Media::Imaging::RenderTargetBitmap recommendedBitmap; Await(recommendedBitmap.RenderAsync(pickerDialog));
            const auto recommendedPixels = Await(recommendedBitmap.GetPixelsAsync()); BYTE* recommendedBytes = nullptr;
            winrt::check_hresult(recommendedPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&recommendedBytes));
            if (!preview_png::Save(output / L"icon-picker-recommended.png", recommendedBitmap.PixelWidth(), recommendedBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(recommendedBytes), recommendedPixels.Length() / 4}, iconError)) throw std::runtime_error(iconError);
            pickerGroups.SelectedIndex(0); Settle(pickerDialog);
            pickerQuery.Text(L"Preview icon 3"); // l10n-allow: query matches the isolated preview fixture above.
            Settle(pickerDialog);
            if (pickerGrid.Items().Size() != 1) throw std::runtime_error("picker search lost its fixture resource");
            pickerGrid.SelectedIndex(0);
            PumpUntil([&] {
                bool pickerImageLoaded = false;
                Walk(pickerGrid, [&](const x::DependencyObject& node) {
                    if (const auto image = node.try_as<c::Image>(); image)
                        if (const auto bitmap = image.Source().try_as<x::Media::Imaging::BitmapImage>(); bitmap && bitmap.PixelWidth() == 96) pickerImageLoaded = true;
                });
                return pickerImageLoaded && pickerDialog.IsPrimaryButtonEnabled();
            });
            if (pickerDialog.ActualWidth() > shell->ActualWidth() || pickerDialog.ActualHeight() > shell->ActualHeight() ||
                pickerDialog.Content().as<x::FrameworkElement>().ActualWidth() > pickerDialog.ActualWidth())
                throw std::runtime_error("icon picker exceeds its dialog or window bounds");
            x::Media::Imaging::RenderTargetBitmap pickerBitmap;
            Await(pickerBitmap.RenderAsync(pickerDialog));
            const auto pickerPixels = Await(pickerBitmap.GetPixelsAsync()); BYTE* pickerBytes = nullptr;
            winrt::check_hresult(pickerPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&pickerBytes));
            if (!preview_png::Save(output / L"icon-picker.png", pickerBitmap.PixelWidth(), pickerBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(pickerBytes), pickerPixels.Length() / 4}, iconError)) throw std::runtime_error(iconError);
            c::Button useButton{nullptr};
            Walk(pickerDialog, [&](const x::DependencyObject& node) {
                if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"PrimaryButton") useButton = button;
            });
            if (!useButton) throw std::runtime_error("icon picker use button is missing");
            InvokePreview(useButton);
            PumpUntil([&] { return pickerResult->completed; });
            if (pickerResult->id != entries[2].id) throw std::runtime_error("picker returned a display index instead of the selected resource identity");
            // The page remains active after dismissal. Repeat the same target
            // through the explicit host request used when route equality skips
            // page reconstruction, then select the actual imported WebP tile.
            auto targetRoute=SettingsRoute::ForPage(SettingsPage::IconLibrary);targetRoute.itemKey=L"item:preview";
            if(!shell->Navigate(targetRoute))throw std::runtime_error("icon target route rejected");
            const auto openDialog=[&]() {
                c::ContentDialog opened{nullptr};
                PumpUntil([&] {
                    for(const auto& popup:x::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(shell->XamlRoot()))
                        if(popup.Child())Walk(popup.Child(),[&](const x::DependencyObject& node){if(const auto dialog=node.try_as<c::ContentDialog>())opened=dialog;});
                    return opened&&opened.ActualWidth()>0;
                });return opened;
            };
            const auto waitForAllPickerTiles=[&](const c::ContentDialog& opened) {
                const auto currentGrid=PreviewControl<c::GridView>(opened,L"IconLibraryGrid");
                const auto openedGroups=PreviewControl<c::ComboBox>(opened,L"IconLibraryGroups");
                if(openedGroups.SelectedIndex()!=0)throw std::runtime_error("reopened picker did not default to all icons");
                PumpUntil([&] {
                    unsigned loaded=0;
                    Walk(currentGrid,[&](const x::DependencyObject& node) {
                        if(const auto image=node.try_as<c::Image>();image&&!winrt::unbox_value_or<bool>(image.Tag(),false))
                            if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==96)++loaded;
                    });
                    return currentGrid.Items().Size()==7&&loaded==6;
                });
            };
            auto reopened=openDialog();
            waitForAllPickerTiles(reopened);
            PreviewControl<c::ComboBox>(reopened,L"IconLibraryGroups").SelectedIndex(1); Settle(reopened);
            if(PreviewControl<c::GridView>(reopened,L"IconLibraryGrid").Items().Size()!=1)throw std::runtime_error("host target name did not reach recommendations");
            reopened.Hide();PumpUntil([&]{return pickerRequests->size()==1;});
            libraryFixture->targetName=L"No matching application";
            shell->OpenRequestedIconPicker();reopened=openDialog();
            waitForAllPickerTiles(reopened);
            PreviewControl<c::ComboBox>(reopened,L"IconLibraryGroups").SelectedIndex(1); Settle(reopened);
            if(PreviewControl<c::GridView>(reopened,L"IconLibraryGrid").Items().Size()!=0||reopened.IsPrimaryButtonEnabled())throw std::runtime_error("picker reused recommendations from an earlier target");
            PreviewControl<c::ComboBox>(reopened,L"IconLibraryGroups").SelectedIndex(0); Settle(reopened);
            const auto reopenedGrid=PreviewControl<c::GridView>(reopened,L"IconLibraryGrid");reopenedGrid.SelectedIndex(1);
            PumpUntil([&]{
                bool originalLoaded=false;Walk(reopenedGrid,[&](const x::DependencyObject& node){
                    if(const auto image=node.try_as<c::Image>();image&&x::Automation::AutomationProperties::GetAutomationId(image)==L"IconLibraryPreview-"+winrt::to_hstring(entries[1].id))
                        if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==entries[1].width)originalLoaded=true;
                });return originalLoaded&&reopened.IsPrimaryButtonEnabled();
            });
            c::Button reopenedUse{nullptr};Walk(reopened,[&](const x::DependencyObject& node){if(const auto button=node.try_as<c::Button>();button&&button.Name()==L"PrimaryButton")reopenedUse=button;});
            if(!reopenedUse)throw std::runtime_error("reopened picker use button missing");
            InvokePreview(reopenedUse);PumpUntil([&]{return pickerRequests->size()==2;});
            if(pickerRequests->back().action!="apply"||pickerRequests->back().ids!=std::vector<std::string>{entries[1].id})throw std::runtime_error("reopened picker failed to apply original preview asset");
            if(!shell->Navigate(SettingsRoute::ForPage(SettingsPage::IconLibrary)))throw std::runtime_error("return to library route failed");
            const auto currentGrid=PreviewControl<c::GridView>(shell->FindName(L"PageCards").as<c::StackPanel>(),L"IconLibraryGrid");
            PumpUntil([&] {
                unsigned loaded=0;Walk(currentGrid,[&](const x::DependencyObject& node) {
                    if(const auto image=node.try_as<c::Image>();image&&!winrt::unbox_value_or<bool>(image.Tag(),false))
                        if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==96)++loaded;
                });return currentGrid.Items().Size()==7&&loaded==6;
            });
            std::ofstream(output / L"icon-library.tsv") << "stream-image-load\tgroup-filter\tstable-selection\tsearch\tempty-hint-removed\tdefault-all\trecommendations\treopen-all\ttarget-refresh\npassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\n";
        }
        if (page == SettingsPage::WidgetBehavior && expanded)
        {
            unsigned delays = 0;
            Walk(cards, [&](const x::DependencyObject& node) {
                const auto slider = node.try_as<c::Slider>();
                if (!slider) return;
                const auto label = x::Automation::AutomationProperties::GetName(slider);
                if (label != _LW("settings.widgetBehavior.tabHoverDelay") &&
                    label != _LW("app.settings.popup_hover_delay")) return;
                ++delays;
                if (slider.Value() != 250.0 || slider.StepFrequency() != 50.0)
                    throw std::runtime_error("hover default was rounded away from 250 ms");
            });
            if (delays != 2) throw std::runtime_error("hover delay editors are not mounted");
            std::ofstream(output / L"hover-delays.tsv") << "tabs\tpopup\n250\t250\n";
        }
        if (page == SettingsPage::DesktopPages)
        {
            c::ListView list{nullptr}; c::TextBox name{nullptr};
            c::NumberBox columns{nullptr}, gridRows{nullptr}; c::Button cancel{nullptr};
            Walk(cards, [&](const x::DependencyObject& node) {
                const auto element = node.try_as<x::FrameworkElement>();
                if (!element) return;
                const auto label = x::Automation::AutomationProperties::GetName(element);
                if (label == _LW("settings.pages.manage")) list = node.try_as<c::ListView>();
                if (label == _LW("settings.pages.name")) if (auto box = node.try_as<c::TextBox>()) name = box;
                if (label == _LW("settings.pages.columns")) if (auto box = node.try_as<c::NumberBox>()) columns = box;
                if (label == _LW("settings.pages.rows")) if (auto box = node.try_as<c::NumberBox>()) gridRows = box;
                if (const auto button = node.try_as<c::Button>(); button &&
                    winrt::unbox_value_or<winrt::hstring>(button.Content(), winrt::hstring{}) == _LW("app.settings.cancel")) cancel = button;
            });
            if (!list || !name || !columns || !gridRows || !cancel || list.Items().Size() < 2)
                throw std::runtime_error("page editor fixture incomplete");
            for (const int index : {1, 0, 1})
            {
                list.SelectedIndex(index); Settle(shell.as<x::FrameworkElement>());
                if (!list.IsEnabled() || !columns.IsEnabled() || !gridRows.IsEnabled())
                    throw std::runtime_error("selecting a page incorrectly entered rename state");
            }
            const auto original = name.Text();
            name.Text(original + L" edited"); Settle(shell.as<x::FrameworkElement>());
            if (list.IsEnabled() || columns.IsEnabled() || gridRows.IsEnabled())
                throw std::runtime_error("unsaved rename did not protect selected page");
            name.Text(original); Settle(shell.as<x::FrameworkElement>());
            if (!list.IsEnabled() || !columns.IsEnabled() || !gridRows.IsEnabled())
                throw std::runtime_error("restoring original name did not unlock grid");
            name.Text(original + L" draft"); Settle(shell.as<x::FrameworkElement>());
            InvokePreview(cancel); Settle(shell.as<x::FrameworkElement>());
            if (name.Text() != original || !columns.IsEnabled() || !gridRows.IsEnabled())
                throw std::runtime_error("cancel rename did not restore grid editing");
            std::ofstream(output / L"page-selection.tsv") << "selection\tedit\trevert\tcancel\npassed\tpassed\tpassed\tpassed\n";
        }
        if (page == SettingsPage::Calendar && !calendarScenario.empty())
        {
            const auto list = PreviewControl<c::ListView>(cards, L"CalendarSchedules");
            const int index = calendarScenario == "calendar-weekly" ? 4 : calendarScenario == "calendar-monthly" ? 5 : 3;
            list.SelectedIndex(index); Settle(shell.as<x::FrameworkElement>());
            const auto mode = PreviewControl<c::ComboBox>(cards, L"CalendarDateMode");
            if (mode.SelectedIndex() != index - 2) throw std::runtime_error("calendar series mode mismatch");
            if (calendarScenario == "calendar-dates" &&
                PreviewControl<c::CalendarView>(cards, L"CalendarMultipleDates").SelectedDates().Size() != 3)
                throw std::runtime_error("multiple-date selection was not restored");
            auto occurrence = PreviewControl<c::CalendarDatePicker>(cards, L"CalendarOccurrenceDate");
            if (!occurrence.Date()) throw std::runtime_error("series has no selectable occurrence");
            winrt::Windows::Globalization::Calendar original;
            original.SetDateTime(occurrence.Date().Value());
            if ((index == 4 && (original.Month() != 10 || original.Day() != 19)) ||
                (index == 5 && (original.Month() != 5 || original.Day() != 31)))
                throw std::runtime_error("series editor selected a non-occurrence date");
            if (calendarScenario == "calendar-occurrence")
            {
                original.Day(3);
                occurrence.Date(winrt::box_value(original.GetDateTime()).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
                InvokePreview(PreviewControl<c::Button>(cards, L"CalendarEditOccurrence"));
                Settle(shell.as<x::FrameworkElement>());
                const auto scope = PreviewControl<c::ComboBox>(cards, L"CalendarScope");
                const auto actual = PreviewControl<c::CalendarDatePicker>(cards, L"CalendarDate");
                original.SetDateTime(actual.Date().Value());
                if (scope.SelectedIndex() != 0 || original.Day() != 4)
                    throw std::runtime_error("moved occurrence did not keep its original identity");
            }
            std::ofstream(output / L"calendar-mode.tsv") << calendarScenario << "\tpassed\n";
        }
        else if (page == SettingsPage::Calendar && custom)
        {
            Walk(cards, [](const x::DependencyObject& node) {
                if (const auto list = node.try_as<c::ListView>(); list && list.Items().Size() > 0)
                    list.SelectedIndex(0);
            });
            Settle(shell.as<x::FrameworkElement>());
        }
        if (expanded)
        {
            for (int pass = 0; pass < 4; ++pass)
            {
                Walk(cards, [](const x::DependencyObject& node) {
                    if (const auto expander = node.try_as<c::Expander>(); expander && expander.Visibility() == x::Visibility::Visible)
                        expander.IsExpanded(true);
                });
                Settle(shell.as<x::FrameworkElement>());
            }
        }
        const unsigned gradientDetailsInvoked = expanded
            ? ExpandGradientDetails(cards, shell.as<x::FrameworkElement>()) : 0;
        unsigned removeStopButtons = 0;
        Walk(cards, [&](const x::DependencyObject& node) {
            if (const auto button = node.try_as<c::Button>(); button && IsPreviewElementVisible(button) &&
                winrt::unbox_value_or<winrt::hstring>(button.Content(), winrt::hstring{}) ==
                    winrt::hstring{_LW("panelGradient.removeStop")}) ++removeStopButtons;
        });
        actions << key << '\t' << gradientDetailsInvoked << '\t' << removeStopButtons << '\n';
        const auto scroll = shell->FindName(L"PageScrollViewer").as<c::ScrollViewer>();
        Walk(cards, [&](const x::DependencyObject& node) {
            const auto row = node.try_as<c::Grid>();
            if (!row || row.ActualHeight() <= 0 || row.Visibility() != x::Visibility::Visible ||
                !row.Resources().HasKey(winrt::box_value(L"SnowDesktopSettingRow")) || row.Children().Size() < 2) return;
            const auto text = row.Children().GetAt(0).try_as<c::StackPanel>();
            const auto editor = row.Children().GetAt(1).try_as<x::FrameworkElement>();
            if (!text || text.Children().Size() == 0 || !editor) return;
            c::TextBlock label{nullptr};
            Walk(text, [&](const x::DependencyObject& child) {
                if (!label) label = child.try_as<c::TextBlock>();
            });
            if (!label) return;
            auto title = winrt::to_string(label.Text());
            std::replace(title.begin(), title.end(), '\t', ' '); std::replace(title.begin(), title.end(), '\n', ' ');
            const auto bounds = row.TransformToVisual(cards).TransformBounds({0, 0, static_cast<float>(row.ActualWidth()), static_cast<float>(row.ActualHeight())});
            const auto controls = editor.TransformToVisual(row).TransformBounds({0, 0, static_cast<float>(editor.ActualWidth()), static_cast<float>(editor.ActualHeight())});
            if (const auto host = editor.try_as<c::ContentControl>())
                if (const auto combo = host.Content().try_as<c::ComboBox>())
                {
                    const auto choice = combo.TransformToVisual(row).TransformBounds({0, 0,
                        static_cast<float>(combo.ActualWidth()), static_cast<float>(combo.ActualHeight())});
                    choices << key << '\t' << title << '\t' << choice.X + choice.Width << '\t'
                        << controls.X + controls.Width << '\t' << choice.Width << '\n';
                }
            const auto padding = row.Padding();
            rows << key << '\t' << title << '\t' << bounds.X << '\t' << bounds.Y << '\t' << bounds.Width << '\t' << bounds.Height
                << '\t' << padding.Left << '\t' << padding.Top << '\t' << padding.Right << '\t' << padding.Bottom
                << '\t' << controls.X << '\t' << controls.Y << '\t' << controls.Width << '\t' << controls.Height << '\n';
        });
        const double maximum = scroll.ScrollableHeight();
        if (restore)
        {
            const auto originalSize = shell->ActualSize();
            for (int cycle = 0; cycle < 3; ++cycle)
            {
                ShowWindow(host.window, SW_SHOWMINNOACTIVE);
                PumpUntil([&] { return IsIconic(host.window) != FALSE; });
                ShowWindow(host.window, SW_SHOWNOACTIVATE);
                PumpUntil([&] { return IsIconic(host.window) == FALSE; });
                Settle(shell.as<x::FrameworkElement>());
                if (shell->ActualSize() != originalSize || !host.runtime.IsAttached())
                    throw std::runtime_error("restore changed the Island bounds or detached its content");
            }
            std::ofstream(output / L"restore.tsv") << "cycles\tretained-layout\n3\tpassed\n";
        }
        const double step = std::max(100., scroll.ViewportHeight() - 48.);
        for (unsigned tile = 0; tile < 64; ++tile)
        {
            const double offset = std::min(maximum, tile * step);
            (void)scroll.ChangeView(nullptr, winrt::box_value(offset).as<winrt::Windows::Foundation::IReference<double>>(), nullptr, true);
            Settle(shell.as<x::FrameworkElement>());
            x::Media::Imaging::RenderTargetBitmap bitmap;
            const double raster = shell->XamlRoot().RasterizationScale();
            Await(bitmap.RenderAsync(shell.as<x::UIElement>(), static_cast<int>(std::lround(width / raster)), static_cast<int>(std::lround(height / raster))));
            const auto buffer = Await(bitmap.GetPixelsAsync());
            const int pixelWidth = bitmap.PixelWidth(), pixelHeight = bitmap.PixelHeight();
            if (pixelWidth <= 0 || pixelHeight <= 0 || std::abs(pixelWidth - width) > 1 ||
                std::abs(pixelHeight - height) > 1 || buffer.Length() != static_cast<unsigned>(pixelWidth * pixelHeight * 4))
                throw std::runtime_error("settings bitmap dimensions or pixel buffer mismatch");
            BYTE* bytes = nullptr;
            winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
            const std::string image = key + "-" + std::to_string(tile) + ".png";
            std::string error;
            if (!preview_png::Save(output / image, bitmap.PixelWidth(), bitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(bytes), static_cast<std::size_t>(buffer.Length() / 4)}, error))
                throw std::runtime_error(error);
            manifest << key << '\t' << image << '\t' << width << '\t' << height << '\t' << scroll.VerticalOffset() << '\t' << maximum << '\n';
            manifest.flush(); rows.flush();
            if (offset >= maximum) break;
        }
        if (page == SettingsPage::DesktopCategories && focusId.empty())
        {
            c::Expander editor{nullptr};
            c::HyperlinkButton addLink{nullptr};
            const winrt::hstring addText{_LW("app.settings.add_category")};
            Walk(cards, [&](const x::DependencyObject& node) {
                if (const auto section = node.try_as<c::Expander>(); section)
                    if (const auto heading = section.Header().try_as<c::TextBlock>(); heading && heading.Text() == addText)
                        editor = section;
                if (const auto link = node.try_as<c::HyperlinkButton>(); link &&
                    winrt::unbox_value_or<winrt::hstring>(link.Content(), winrt::hstring{}) == addText)
                    addLink = link;
            });
            if (!editor || !addLink || editor.Visibility() != x::Visibility::Collapsed)
                throw std::runtime_error("new category editor must be hidden on entry");
            const auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(addLink);
            if (!peer) throw std::runtime_error("add category link automation peer is missing");
            peer.GetPattern(x::Automation::Peers::PatternInterface::Invoke)
                .as<x::Automation::Provider::IInvokeProvider>().Invoke();
            Settle(shell.as<x::FrameworkElement>());
            if (editor.Visibility() != x::Visibility::Visible || !editor.IsExpanded())
                throw std::runtime_error("add category link did not reveal the editor");
            for (const auto destination : {SettingsPage::General, SettingsPage::DesktopCategories})
            {
                snapshot.revision = ++revision;
                snapshot.domainRevisions = {revision, revision, revision, revision, revision, revision, revision};
                snapshot.route = SettingsRoute::ForPage(destination);
                if (!shell->ApplySnapshot(snapshot)) throw std::runtime_error("category revisit route refused");
                Settle(shell.as<x::FrameworkElement>());
            }
            if (editor.Visibility() != x::Visibility::Collapsed || editor.IsExpanded())
                throw std::runtime_error("new category editor must hide after leaving and returning");
            std::ofstream(output / L"category-lifecycle.tsv", std::ios::app)
                << "hidden-on-entry\tlink-revealed\thidden-on-reentry\n";
        }
    }
    shell->Close(); host.runtime.Detach(); shell = nullptr;
    if (!rendered) throw std::runtime_error("unknown settings page key");
}
}

std::optional<int> TryRunSettingsVisualPreview()
{
    int count = 0;
    const auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return std::nullopt;
    struct FreeArgs { LPWSTR* value; ~FreeArgs() { LocalFree(value); } } freeArgs{args};
    if (count < 2 || std::wstring_view(args[1]) != L"--settings-visual-preview") return std::nullopt;
    // Internal diagnostic only: output, page key/all, light/dark, locale,
    // supported window width/height in DIP, disclosure state, optional focus ID.
    if (count != 9 && count != 10) return ERROR_BAD_ARGUMENTS;
    std::filesystem::path output(args[2]);
    try
    {
        if (!output.is_absolute()) throw std::runtime_error("absolute output directory required");
        std::filesystem::create_directories(output);
        const int width = std::stoi(args[6]), height = std::stoi(args[7]);
        if (width < 840 || width > 2400 || height < 520 || height > 1800) throw std::runtime_error("unsupported preview size");
        const std::wstring_view theme(args[4]), state(args[8]);
        if (theme != L"light" && theme != L"dark") throw std::runtime_error("preview supports light or dark XAML surfaces");
        const bool calendarState = state == L"calendar-dates" || state == L"calendar-weekly" ||
            state == L"calendar-monthly" || state == L"calendar-occurrence";
        if (state != L"expanded" && state != L"collapsed" && state != L"custom" && state != L"disabled" && state != L"scenarios" && state != L"searching" && state != L"restore" && !calendarState) throw std::runtime_error("invalid disclosure state");
        Export(output, winrt::to_string(args[3]), theme == L"dark", winrt::to_string(args[5]), width, height,
            state == L"expanded" || state == L"custom" || state == L"scenarios",
            state == L"custom" || state == L"scenarios", state == L"disabled", state == L"scenarios",
            count == 10 ? winrt::to_string(args[9]) : std::string{}, calendarState ? winrt::to_string(state) : std::string{},
            state == L"searching", state == L"restore");
        std::ofstream(output / L"result.txt") << "passed\nIsolated production XAML, example data; no desktop material or interaction acceptance.\n";
        return 0;
    }
    catch (const winrt::hresult_error& error)
    {
        std::ofstream(output / L"result.txt") << "failed\n" << winrt::to_string(error.message()) << '\n';
    }
    catch (const std::exception& error)
    {
        std::ofstream(output / L"result.txt") << "failed\n" << error.what() << '\n';
    }
    return ERROR_INVALID_DATA;
}
}
