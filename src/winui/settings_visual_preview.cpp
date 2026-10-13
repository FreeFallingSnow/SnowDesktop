#include "pch.h"
#include "settings_visual_preview.h"
#include "SettingsShell.xaml.h"
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
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

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
    SettingsPage::AnimationPerformance, SettingsPage::ContextMenu, SettingsPage::ThemeManager,
    SettingsPage::Widgets, SettingsPage::Calendar, SettingsPage::BackupAndData, SettingsPage::About,
    SettingsPage::DeveloperTools, SettingsPage::Debug, SettingsPage::WidgetSettings, SettingsPage::LargeIcon};

void Export(const std::filesystem::path& output, std::string_view selected, bool dark,
    const std::string& locale, int width, int height, bool expanded, bool custom, bool disabled)
{
    Locale::Instance().Init((std::filesystem::path(GetExecutableDirectoryPath()) / L"lang").c_str());
    if (!Locale::Instance().HasLanguage(locale)) throw std::runtime_error("preview locale is not installed");
    Locale::Instance().SetLanguage(locale.c_str());
    Apartment apartment; winrt::check_hresult(apartment.result);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RenderHost host;
    if (!host.runtime.Initialize()) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    host.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        L"STATIC", L"SnowDesktop offline settings render", WS_POPUP,
        -32000, -32000, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!host.window) winrt::throw_last_error();
    ShowWindow(host.window, SW_SHOWNOACTIVATE);
    const double scale = GetDpiForWindow(host.window) / 96.;
    SetWindowPos(host.window, nullptr, 0, 0, static_cast<int>(std::ceil(width * scale)),
        static_cast<int>(std::ceil(height * scale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    PreviewWidgetService widgets(locale == "en-US");
    auto shell = winrt::make_self<winrt::SnowDesktop::implementation::SettingsShell>();
    shell->RequestedTheme(dark ? x::ElementTheme::Dark : x::ElementTheme::Light);
    shell->SetSystemBackdropActive(false);
    shell->SetLocalizer([](std::string_view key) { return std::wstring(_LW(std::string(key).c_str())); });
    shell->SetConditionalPagesVisible(true, true);
    shell->SetWidgetSettingsService(&widgets);
    GeneralPageActions general;
    general.languageCatalog = [] { return std::vector<SettingsLanguageOption>{{"zh-CN", L"简体中文"}, {"en-US", L"English"}}; }; // l10n-allow: isolated bilingual preview fixture, not production UI text.
    general.queryAdvancedFeatureStatus = [] { GeneralAdvancedFeatureStatus status; status.state = GeneralAdvancedFeatureState::Registered;
        status.bridgeAvailable = status.registered = status.cardVisible = true; return status; };
    shell->SetGeneralPageActions(std::move(general));
    PersonalizationPageActions appearance;
    appearance.listFonts = [] { return std::vector<app_fonts::Choice>{{{}, L"Segoe UI", L"Segoe UI"}}; };
    appearance.appliedFont = [] { return app_fonts::Selection{}; };
    shell->SetPersonalizationPageActions(std::move(appearance));
    PageLayoutPageActions layout;
    layout.capture = [] { PageLayoutSnapshot snapshot; snapshot.revision = 1; snapshot.editable = true; snapshot.monitorCount = 2;
        snapshot.pages = {{L"example-1", L"Work / 工作", 12, 8, 10, 2}, {L"example-2", L"Projects / 项目", 10, 7, 5, 1}}; return snapshot; }; // l10n-allow: isolated bilingual preview fixture, not production UI text.
    shell->SetPageLayoutPageActions(std::move(layout));
    shell->SetLargeIconSettingsAction([](LargeIconSettingsRequest request) {
        LargeIconSettingsSnapshot snapshot; snapshot.key = request.key; snapshot.name = L"Example / 示例"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        snapshot.available = snapshot.editable = snapshot.succeeded = true; snapshot.session = snapshot.revision = 1;
        snapshot.maxColumns = snapshot.maxRows = 4; return snapshot;
    });
    bool loaded = false;
    const auto token = shell->Loaded([&](const auto&, const auto&) { loaded = true; });
    if (!host.runtime.Attach(host.window, shell.as<x::UIElement>())) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    host.runtime.ResizeToClient(); PumpUntil([&] { return loaded; }); shell->Loaded(token);
    std::ofstream manifest(output / L"render.tsv");
    manifest << "page\timage\twidthDip\theightDip\tscrollY\tscrollMax\n";
    std::ofstream rows(output / L"rows.tsv");
    rows << "page\tlabel\tx\ty\twidth\theight\tpaddingLeft\tpaddingTop\tpaddingRight\tpaddingBottom\teditorX\teditorY\teditorWidth\teditorHeight\n";
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
        snapshot.values.general.widgetDeveloperToolsEnabled = true;
        if (custom)
        {
            snapshot.values.personalization.backgroundPreset = kAppearancePresetCustom;
            snapshot.values.dock.followComponentAppearance = false;
            snapshot.values.dock.appearancePreset = kAppearancePresetCustom;
            snapshot.values.dock.systemTaskbarBackdropEnabled = true;
            snapshot.values.dock.systemTaskbarFollowPersonalization = false;
            snapshot.values.dock.systemTaskbarAppearance.backgroundPreset = kAppearancePresetCustom;
            for (auto* theme : {&snapshot.values.general.quickNavigationAppearance,
                &snapshot.values.general.collectionPopupAppearance, &snapshot.values.general.statusBar.theme})
            { theme->mode = 4; theme->customized = true; theme->appearance.backgroundPreset = kAppearancePresetCustom; }
        }
        snapshot.domainRevisions = {revision, revision, revision, revision, revision, revision, revision};
        snapshot.route = SettingsRoute::ForPage(page);
        if (page == SettingsPage::WidgetSettings) snapshot.route = SettingsRoute::ForWidget(widgets.value.widgetId);
        if (page == SettingsPage::LargeIcon) snapshot.route.itemKey = L"settings-visual-example";
        if (!shell->ApplySnapshot(snapshot)) throw std::runtime_error("preview route refused: " + key);
        Settle(shell.as<x::FrameworkElement>());
        const auto cards = shell->FindName(L"PageCards").as<c::StackPanel>();
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
        const auto scroll = shell->FindName(L"PageScrollViewer").as<c::ScrollViewer>();
        Walk(cards, [&](const x::DependencyObject& node) {
            const auto row = node.try_as<c::Grid>();
            if (!row || row.ActualHeight() <= 0 || row.Visibility() != x::Visibility::Visible ||
                !row.Resources().HasKey(winrt::box_value(L"SnowDesktopSettingRow")) || row.Children().Size() < 2) return;
            const auto text = row.Children().GetAt(0).try_as<c::StackPanel>();
            const auto editor = row.Children().GetAt(1).try_as<x::FrameworkElement>();
            if (!text || text.Children().Size() == 0 || !editor) return;
            const auto label = text.Children().GetAt(0).try_as<c::TextBlock>();
            if (!label) return;
            auto title = winrt::to_string(label.Text());
            std::replace(title.begin(), title.end(), '\t', ' '); std::replace(title.begin(), title.end(), '\n', ' ');
            const auto bounds = row.TransformToVisual(cards).TransformBounds({0, 0, static_cast<float>(row.ActualWidth()), static_cast<float>(row.ActualHeight())});
            const auto controls = editor.TransformToVisual(row).TransformBounds({0, 0, static_cast<float>(editor.ActualWidth()), static_cast<float>(editor.ActualHeight())});
            const auto padding = row.Padding();
            rows << key << '\t' << title << '\t' << bounds.X << '\t' << bounds.Y << '\t' << bounds.Width << '\t' << bounds.Height
                << '\t' << padding.Left << '\t' << padding.Top << '\t' << padding.Right << '\t' << padding.Bottom
                << '\t' << controls.X << '\t' << controls.Y << '\t' << controls.Width << '\t' << controls.Height << '\n';
        });
        const double maximum = scroll.ScrollableHeight();
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
            if (offset >= maximum) break;
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
    // supported window width/height in DIP, expanded/collapsed.
    if (count != 9) return ERROR_BAD_ARGUMENTS;
    std::filesystem::path output(args[2]);
    try
    {
        if (!output.is_absolute()) throw std::runtime_error("absolute output directory required");
        std::filesystem::create_directories(output);
        const int width = std::stoi(args[6]), height = std::stoi(args[7]);
        if (width < 840 || width > 2400 || height < 520 || height > 1800) throw std::runtime_error("unsupported preview size");
        const std::wstring_view theme(args[4]), state(args[8]);
        if (theme != L"light" && theme != L"dark") throw std::runtime_error("preview supports light or dark XAML surfaces");
        if (state != L"expanded" && state != L"collapsed" && state != L"custom" && state != L"disabled") throw std::runtime_error("invalid disclosure state");
        Export(output, winrt::to_string(args[3]), theme == L"dark", winrt::to_string(args[5]), width, height,
            state == L"expanded" || state == L"custom", state == L"custom", state == L"disabled");
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
