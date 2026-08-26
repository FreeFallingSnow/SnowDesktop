#include "pch.h"

#include "settings_window_host.h"

#include "SettingsShell.xaml.h"
#include "winui_runtime.h"
#include "../widget_settings_service.h"

#include <shobjidl.h>
#include <dwmapi.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace snowdesktop::winui
{
namespace mud = winrt::Microsoft::UI::Dispatching;
namespace muw = winrt::Microsoft::UI::Windowing;
namespace mux = winrt::Microsoft::UI::Xaml;
namespace wf = winrt::Windows::Foundation;
namespace wui = winrt::Windows::UI;
namespace shell_impl = winrt::SnowDesktop::implementation;

namespace
{
constexpr wchar_t kSettingsWindowClassName[] =
    L"SnowDesktop.WinUI3.SettingsWindow";
constexpr int kDefaultClientWidth = 1100;
constexpr int kDefaultClientHeight = 760;
constexpr int kMinimumClientWidth = 840;
constexpr int kMinimumClientHeight = 520;
constexpr UINT kDispatchOwnerTaskMessage = WM_APP + 0x347;
constexpr UINT kApplyXamlBackdropMessage = WM_APP + 0x348;
constexpr UINT kUpdateIntegratedTitleBarInsetsMessage = WM_APP + 0x349;

bool QueryHighContrastEnabled(bool& enabled) noexcept
{
    HIGHCONTRASTW state{};
    state.cbSize = sizeof(state);
    if (!SystemParametersInfoW(
            SPI_GETHIGHCONTRAST, sizeof(state), &state, 0))
    {
        enabled = false;
        return false;
    }
    enabled = (state.dwFlags & HCF_HIGHCONTRASTON) != 0;
    return true;
}

wf::IReference<wui::Color> BoxTitleBarColor(
    std::uint8_t alpha,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue)
{
    return winrt::box_value(wui::Color{alpha, red, green, blue})
        .as<wf::IReference<wui::Color>>();
}

DWORD WindowsBuildNumber() noexcept
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtlGetVersion)
        return 0;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return rtlGetVersion(&version) == 0 && version.dwMajorVersion >= 10
        ? version.dwBuildNumber
        : 0;
}

bool SupportsMicaBackdrop() noexcept
{
    return WindowsBuildNumber() >= 22000;
}

bool SupportsDwmSystemBackdrop() noexcept
{
    // DWMWA_SYSTEMBACKDROP_TYPE is formally supported starting with 22H2.
    return WindowsBuildNumber() >= 22621;
}

void ApplySettingsWindowChrome(HWND window, bool darkTheme) noexcept
{
    if (!window || !IsWindow(window))
        return;

    const BOOL dark = darkTheme ? TRUE : FALSE;
    (void)DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
        &dark, sizeof(dark));

    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    (void)DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
        &corner, sizeof(corner));

    bool highContrast = false;
    const bool allowMaterial = QueryHighContrastEnabled(highContrast) &&
        !highContrast;
    const bool useMica = SupportsDwmSystemBackdrop() && allowMaterial;
    const DWM_SYSTEMBACKDROP_TYPE backdrop = useMica
        ? DWMSBT_MAINWINDOW
        : DWMSBT_NONE;
    (void)DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE,
        &backdrop, sizeof(backdrop));

    // Windows 11 21H2 can host Island Mica but lacks the public top-level
    // system-backdrop attribute. Match its base color so the native caption
    // and Windows-owned buttons still meet the panel without a bright seam.
    COLORREF captionColor = DWMWA_COLOR_DEFAULT;
    if (allowMaterial && SupportsMicaBackdrop() &&
        !SupportsDwmSystemBackdrop())
    {
        captionColor = darkTheme ? RGB(32, 32, 32) : RGB(243, 243, 243);
    }
    (void)DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR,
        &captionColor, sizeof(captionColor));
}

struct StaticSearchDefinition
{
    SettingsPage page;
    const char* focusId;
    const char* labelKey;
    const char* descriptionKey;
};

constexpr StaticSearchDefinition kStaticSearchDefinitions[] = {
    {SettingsPage::General, "general.autoStart",
        "settings.general.startup",
        "settings.general.startup.description"},
    {SettingsPage::Desktop, "desktop.softwareDesktop",
        "settings.general.softwareDesktop",
        "settings.general.softwareDesktop.description"},
    {SettingsPage::General, "general.language",
        "settings.general.language",
        "settings.general.language.description"},
    {SettingsPage::Desktop, "desktop.doubleClickHide",
        "settings.general.doubleClickHide",
        "settings.general.doubleClickHide.description"},
    {SettingsPage::General, "general.quickNavigation",
        "settings.general.quickNavigation",
        "settings.general.quickNavigation.description"},
    {SettingsPage::General, "general.quickNavigation.hotkey",
        "app.settings.hotkey",
        "settings.general.quickNavigation.description"},
    {SettingsPage::General, "general.pageNavigation",
        "settings.general.pageNavigation",
        "settings.general.pageNavigation.description"},
    {SettingsPage::General, "general.pageNavigation.previous",
        "app.settings.page_navigation_previous",
        "settings.general.pageNavigation.description"},
    {SettingsPage::General, "general.pageNavigation.next",
        "app.settings.page_navigation_next",
        "settings.general.pageNavigation.description"},
    {SettingsPage::Desktop, "desktop.passthrough",
        "settings.general.desktopPassthrough",
        "settings.general.desktopPassthrough.description"},
    {SettingsPage::Desktop, "desktop.passthrough.hotkey",
        "app.settings.hotkey",
        "settings.general.desktopPassthrough.description"},
    {SettingsPage::Dock, "dock.floatingShortcutMode",
        "settings.general.floatingDock",
        "settings.general.floatingDock.description"},
    {SettingsPage::Dock, "dock.floatingShortcutMode.hotkey",
        "app.settings.hotkey",
        "settings.general.floatingDock.description"},
    {SettingsPage::AppearanceTheme, "personalization.theme",
        "settings.personalization.theme",
        "settings.personalization.theme.description"},
    {SettingsPage::AppearanceWidgets, "personalization.backgroundColor",
        "settings.personalization.colors",
        "settings.personalization.colors.description"},
    {SettingsPage::AppearanceTheme,
        "personalization.quickNavigationTheme",
        "app.settings.quick_nav_theme",
        "settings.personalization.theme.description"},
    {SettingsPage::AppearanceTheme,
        "personalization.collectionPopupTheme",
        "app.settings.collection_popup_theme",
        "settings.personalization.theme.description"},
    {SettingsPage::AppearanceWidgets, "personalization.borderColor",
        "app.settings.component_border",
        "settings.personalization.colors.description"},
    {SettingsPage::AppearanceWidgets, "personalization.widgetAlpha",
        "app.settings.bg_opacity",
        "settings.personalization.colors.description"},
    {SettingsPage::AppearanceWidgets, "personalization.borderAlpha",
        "app.settings.border_opacity",
        "settings.personalization.colors.description"},
    {SettingsPage::AppearanceWidgets, "personalization.enableGradient",
        "app.settings.enable_gradient",
        "settings.personalization.colors.description"},
    {SettingsPage::AppearanceWidgets,
        "personalization.gradientEndAlpha",
        "app.settings.gradient_end_alpha",
        "settings.personalization.colors.description"},
    {SettingsPage::AppearanceWidgets, "personalization.glass",
        "settings.personalization.glass",
        "settings.personalization.glass.description"},
    {SettingsPage::AppearanceWidgets, "personalization.acrylic",
        "settings.personalization.acrylic",
        "settings.personalization.acrylic.description"},
    {SettingsPage::AppearanceWidgets, "personalization.blurRadius",
        "app.settings.blur_radius",
        "settings.personalization.glass.description"},
    {SettingsPage::AppearanceWidgets, "personalization.contentTheme",
        "app.settings.text_color",
        "settings.personalization.widgets.description"},
    {SettingsPage::AppearanceTheme, "personalization.contextMenu",
        "settings.personalization.contextMenu",
        "settings.personalization.contextMenu.description"},
    {SettingsPage::AppearanceWidgets, "personalization.cornerRadius",
        "settings.personalization.widgets",
        "settings.personalization.widgets.description"},
    {SettingsPage::AppearanceWidgets, "personalization.barHeight",
        "app.settings.bar_height",
        "settings.personalization.widgets.description"},
    {SettingsPage::AppearanceWidgets, "desktop.categoryLayout",
        "app.settings.tab_height",
        "settings.personalization.widgets.description"},
    {SettingsPage::DesktopCategories,
        "desktop.categoryCounts",
        "app.settings.category_show_count",
        "settings.desktop.categoryLayout.description"},
    {SettingsPage::AppearanceDesktopIcons, "desktop.spacing",
        "settings.desktop.spacing", "settings.desktop.spacing.description"},
    {SettingsPage::AppearanceDesktopIcons, "desktop.iconSize",
        "settings.desktop.iconSize", "settings.desktop.iconSize.description"},
    {SettingsPage::AppearanceDesktopIcons, "desktop.itemFontSize",
        "settings.desktop.typography",
        "settings.desktop.typography.description"},
    {SettingsPage::AppearanceDesktopIcons, "desktop.listFontSize",
        "app.settings.list_font_size",
        "settings.desktop.typography.description"},
    {SettingsPage::AppearanceDesktopIcons, "desktop.fontWeight",
        "app.settings.title_font_weight",
        "settings.desktop.typography.description"},
    {SettingsPage::AppearanceDesktopIcons, "desktop.shortcutArrow",
        "settings.desktop.shortcutArrow",
        "settings.desktop.shortcutArrow.description"},
    {SettingsPage::AppearanceIconBeautification, "desktop.iconBeautify",
        "settings.desktop.iconBeautify",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.mode",
        "app.settings.beautify_mode",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.backgroundColor",
        "app.settings.default_bg",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.backgroundOpacity",
        "app.settings.bg_opacity_val",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.gradient",
        "app.settings.enable_gradient_bg",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.gradientEndColor",
        "app.settings.gradient_end_color",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.gradientDirection",
        "app.settings.beautify_gradient_dir",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.shape",
        "app.settings.beautify_shape",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.contentScale",
        "app.settings.beautify_content_scale",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.highlightStrength",
        "app.settings.beautify_texture_highlight",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.highlightSize",
        "app.settings.beautify_texture_highlight_size",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.highlightAngle",
        "app.settings.beautify_texture_highlight_angle",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.shadeStrength",
        "app.settings.beautify_texture_shade",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.edgeHighlight",
        "app.settings.beautify_texture_edge",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification, "desktop.iconBeautify.filter",
        "app.settings.beautify_filter",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.filterColor",
        "app.settings.beautify_filter_color",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.filterStrength",
        "app.settings.beautify_filter_strength",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.shadowStrength",
        "app.settings.beautify_shadow_strength",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.outline",
        "app.settings.beautify_outline",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.outlineWidth",
        "app.settings.beautify_outline_width",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.outlineOpacity",
        "app.settings.beautify_outline_opacity",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::AppearanceIconBeautification,
        "desktop.iconBeautify.outlineColor",
        "app.settings.beautify_outline_color",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::DesktopCategories, "desktop.categoryRules",
        "app.settings.category_rules",
        "settings.desktop.categories.description"},
    {SettingsPage::DesktopCategories, "desktop.categories",
        "settings.desktop.categories",
        "settings.desktop.categories.description"},
    {SettingsPage::DesktopCategories, "desktop.category.add",
        "app.settings.add_category",
        "settings.desktop.categories.description"},
    {SettingsPage::Dock, "dock.enable", "settings.dock.enable",
        "settings.dock.enable.description"},
    {SettingsPage::Dock, "dock.position",
        "settings.dock.position", "settings.dock.position.description"},
    {SettingsPage::Dock, "dock.layout", "settings.dock.layout",
        "settings.dock.layout.description"},
    {SettingsPage::Dock, "dock.monitor", "settings.dock.monitor",
        "settings.dock.monitor.description"},
    {SettingsPage::Dock, "dock.thickness",
        "settings.dock.thickness", "settings.dock.thickness.description"},
    {SettingsPage::Dock, "dock.showFrequentItems",
        "settings.dock.frequentItems",
        "settings.dock.frequentItems.description"},
    {SettingsPage::Dock, "dock.floatingEdgeSwipe",
        "app.dock.floating_edge_swipe",
        "settings.dock.items.description"},
    {SettingsPage::Dock, "dock.showWindowsButton",
        "app.dock.show_windows_button",
        "settings.dock.items.description"},
    {SettingsPage::Dock, "dock.frequentItemCount",
        "app.settings.show_count",
        "settings.dock.frequentItems.description"},
    {SettingsPage::Taskbar, "taskbar.autoHide",
        "settings.taskbar.autoHide",
        "settings.taskbar.autoHide.description"},
    {SettingsPage::Taskbar, "taskbar.alignment",
        "settings.taskbar.alignment",
        "settings.taskbar.alignment.description"},
    {SettingsPage::Taskbar, "taskbar.theme",
        "settings.taskbar.theme", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.contentTheme",
        "app.settings.taskbar_foreground_color",
        "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.backgroundColor",
        "app.settings.bg_color", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.borderColor",
        "app.settings.border_color", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.backgroundOpacity",
        "app.settings.bg_opacity", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.borderOpacity",
        "app.settings.border_opacity", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.glass",
        "app.settings.glass_enabled", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.blurRadius",
        "app.settings.blur_radius", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.acrylic",
        "app.settings.acrylic_noise", "settings.taskbar.theme.description"},
    {SettingsPage::Taskbar, "taskbar.dynamic.shellUi",
        "app.settings.taskbar_dynamic_shell_ui",
        "settings.taskbar.scenarioOverrides.description"},
    {SettingsPage::Taskbar, "taskbar.dynamic.maximizedWindow",
        "app.settings.taskbar_dynamic_maximized_window",
        "settings.taskbar.scenarioOverrides.description"},
    {SettingsPage::Taskbar, "taskbar.dynamic.visibleWindow",
        "app.settings.taskbar_dynamic_visible_window",
        "settings.taskbar.scenarioOverrides.description"},
    {SettingsPage::Taskbar, "taskbar.systemTheme",
        "app.settings.system_panel",
        "settings.taskbar.restartExplorer.description"},
    {SettingsPage::Taskbar, "taskbar.restartExplorer",
        "settings.taskbar.restartExplorer",
        "settings.taskbar.restartExplorer.description"},
    {SettingsPage::Widgets, "widgets.installed",
        "settings.widgets.installed",
        "settings.widgets.installed.description"},
    {SettingsPage::Widgets, "widgets.search",
        "app.settings.widgets_search",
        "settings.widgets.installed.description"},
    {SettingsPage::Widgets, "widgets.install",
        "app.settings.widgets_install_package",
        "settings.widgets.sources.description"},
    {SettingsPage::Widgets, "widgets.workshop",
        "app.settings.widgets_open_steam_workshop",
        "settings.widgets.sources.description"},
    {SettingsPage::Widgets, "widgets.developer",
        "app.settings.widgets_self_develop",
        "settings.widgets.sources.description"},
    {SettingsPage::Widgets, "widgets.included",
        "app.settings.widgets_builtin",
        "settings.widgets.installed.description"},
    {SettingsPage::Widgets, "widgets.sources", "settings.widgets.sources",
        "settings.widgets.sources.description"},
    {SettingsPage::Widgets, "widgets.permissions",
        "settings.widgets.permissions",
        "settings.widgets.permissions.description"},
    {SettingsPage::BackupAndData, "backup.layout", "settings.backup.layout",
        "settings.backup.layout.description"},
    {SettingsPage::BackupAndData, "backup.full", "settings.backup.full",
        "settings.backup.full.description"},
    {SettingsPage::BackupAndData, "backup.directory",
        "settings.backup.directory",
        "settings.backup.directory.description"},
    {SettingsPage::BackupAndData, "backup.migration",
        "app.settings.migrate_all_data",
        "settings.backup.full.description"},
    {SettingsPage::About, "about.version", "settings.about.version",
        "settings.about.version.description"},
    {SettingsPage::About, "about.profile", "app.settings.personal_homepages",
        "app.settings.about_description"},
    {SettingsPage::About, "about.project", "settings.about.project",
        "settings.about.project.description"},
    {SettingsPage::About, "about.community", "app.settings.community",
        "app.settings.join_qq"},
    {SettingsPage::About, "about.thirdparty", "settings.about.thirdparty",
        "settings.about.thirdparty.description"},
    {SettingsPage::DeveloperTools, "developer.overrides",
        "settings.developer.overrides",
        "settings.developer.overrides.description"},
    {SettingsPage::DeveloperTools, "developer.tools",
        "settings.developer.tools",
        "settings.developer.tools.description"},
    {SettingsPage::DeveloperTools, "developer.agentSkill",
        "app.settings.widgets_agent_skill",
        "app.settings.widgets_agent_skill_description"},
    {SettingsPage::DeveloperTools, "developer.workspace",
        "app.settings.widgets_authoring_workspace",
        "settings.developer.tools.description"},
    {SettingsPage::DeveloperTools, "developer.cli",
        "app.settings.widgets_component_cli",
        "settings.developer.tools.description"},
    {SettingsPage::DeveloperTools, "developer.publish",
        "app.settings.widgets_authoring_publish",
        "app.settings.widgets_authoring_publish_description"},
    {SettingsPage::DeveloperTools, "developer.reference",
        "app.settings.widgets_authoring_reference",
        "app.settings.widgets_authoring_reference_description"},
    {SettingsPage::DeveloperTools, "developer.runtime",
        "app.settings.widgets_runtime_diagnostics",
        "settings.developer.tools.description"},
    {SettingsPage::Debug, "debug.demo_mode", "app.settings.demo_mode",
        "app.settings.demo_mode_hint"},
    {SettingsPage::Debug, "debug.animation",
        "app.settings.animation_diagnostics",
        "app.settings.animation_diagnostics_desc"},
    {SettingsPage::Debug, "debug.crash", "app.settings.crash_test",
        "app.settings.crash_test_desc"},
};

std::wstring FormatWin32Error(const wchar_t* operation, DWORD error)
{
    std::wstring result = operation ? operation : L"Win32 operation";
    result += L" (";
    result += std::to_wstring(error);
    result += L")";
    return result;
}

bool IsUsableControllerSnapshot(
    const SettingsController::SnapshotPtr& snapshot) noexcept
{
    return snapshot && snapshot->initialized;
}

bool IsWidgetsBackendPage(SettingsPage page) noexcept
{
    return page == SettingsPage::Widgets ||
        page == SettingsPage::DeveloperTools;
}

std::optional<std::filesystem::path> DialogResultPath(
    IFileDialog* dialog)
{
    if (!dialog)
        return std::nullopt;
    winrt::com_ptr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.put())) || !item)
        return std::nullopt;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) ||
        !rawPath)
    {
        return std::nullopt;
    }
    std::filesystem::path result(rawPath);
    CoTaskMemFree(rawPath);
    return result;
}

std::optional<std::filesystem::path> ShowOpenPathDialog(
    HWND owner,
    std::wstring_view title,
    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
    bool chooseFolder)
{
    winrt::com_ptr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))) || !dialog)
    {
        return std::nullopt;
    }

    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        return std::nullopt;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= chooseFolder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    if (FAILED(dialog->SetOptions(options)))
        return std::nullopt;
    if (!title.empty())
        (void)dialog->SetTitle(std::wstring(title).c_str());

    std::vector<COMDLG_FILTERSPEC> specifications;
    specifications.reserve(filters.size());
    for (const auto& [name, pattern] : filters)
        specifications.push_back({name.c_str(), pattern.c_str()});
    if (!specifications.empty() && FAILED(dialog->SetFileTypes(
            static_cast<UINT>(specifications.size()),
            specifications.data())))
    {
        return std::nullopt;
    }
    if (FAILED(dialog->Show(owner)))
        return std::nullopt;
    return DialogResultPath(dialog.get());
}

std::optional<std::filesystem::path> ShowSavePathDialog(
    HWND owner,
    std::wstring_view title,
    std::wstring_view suggestedFileName,
    const std::vector<std::pair<std::wstring, std::wstring>>& filters,
    std::wstring_view defaultExtension)
{
    winrt::com_ptr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))) || !dialog)
    {
        return std::nullopt;
    }
    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        return std::nullopt;
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
        FOS_OVERWRITEPROMPT;
    if (FAILED(dialog->SetOptions(options)))
        return std::nullopt;
    if (!title.empty())
        (void)dialog->SetTitle(std::wstring(title).c_str());
    if (!suggestedFileName.empty())
        (void)dialog->SetFileName(std::wstring(suggestedFileName).c_str());
    if (!defaultExtension.empty())
    {
        (void)dialog->SetDefaultExtension(
            std::wstring(defaultExtension).c_str());
    }

    std::vector<COMDLG_FILTERSPEC> specifications;
    specifications.reserve(filters.size());
    for (const auto& [name, pattern] : filters)
        specifications.push_back({name.c_str(), pattern.c_str()});
    if (!specifications.empty() && FAILED(dialog->SetFileTypes(
            static_cast<UINT>(specifications.size()),
            specifications.data())))
    {
        return std::nullopt;
    }
    if (FAILED(dialog->Show(owner)))
        return std::nullopt;
    return DialogResultPath(dialog.get());
}
} // namespace

struct SettingsWindowHost::Impl
{
    struct CallbackState
    {
        std::atomic<bool> alive{true};
        std::atomic<bool> snapshotQueued{false};
        std::atomic<bool> flushQueued{false};
        std::mutex snapshotMutex;
        SettingsController::SnapshotPtr latestSnapshot;
        mud::DispatcherQueue dispatcher{nullptr};
        Impl* owner = nullptr;
    };

    DWORD ownerThreadId = 0;
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    SettingsController* controller = nullptr;
    widget_runtime::WidgetSettingsService* widgetSettingsService = nullptr;
    WidgetEngine* widgetEngine = nullptr;
    SettingsWindowHostOptions options;
    WinUiRuntime runtime;
    winrt::com_ptr<shell_impl::SettingsShell> shell;
    muw::AppWindow appWindow{nullptr};
    muw::AppWindowTitleBar appWindowTitleBar{nullptr};
    SettingsSearchIndex searchIndex;
    std::shared_ptr<CallbackState> callbacks;
    std::unique_ptr<WidgetsPageBackend> widgetsPageBackend;
    std::unique_ptr<BackupDataPageBackend> backupDataPageBackend;
    std::uint64_t viewEpoch = 0;
    bool widgetsPageActive = false;
    SettingsPage widgetsBackendPage = SettingsPage::Home;
    bool backupDataPageActive = false;
    bool initialized = false;
    bool shuttingDown = false;
    bool interactionSuspended = true;
    bool darkTheme = false;
    /** Legacy five-click About unlock; retained for this host lifetime. */
    bool debugUnlocked = false;
    bool systemBackdropUpdateQueued = false;
    bool integratedTitleBarInsetsUpdateQueued = false;
    std::wstring lastError;

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return ownerThreadId != 0 &&
            ownerThreadId == GetCurrentThreadId();
    }

    [[nodiscard]] bool Visible() const noexcept
    {
        return window && IsWindow(window) &&
            IsWindowVisible(window) != FALSE;
    }

    [[nodiscard]] bool DebugPageVisible() const
    {
        return debugUnlocked ||
            (options.debugVisible && options.debugVisible());
    }

    std::wstring L(std::string_view key) const
    {
        if (options.localize)
        {
            std::wstring value = options.localize(key);
            if (!value.empty())
                return value;
        }
        return {};
    }

    void SetError(std::wstring message)
    {
        lastError = std::move(message);
    }

    void QueueSystemBackdropUpdate() noexcept
    {
        if (systemBackdropUpdateQueued || shuttingDown || !window ||
            !IsWindow(window) || !runtime.IsAttached())
        {
            return;
        }

        // MicaBackdrop construction can synchronously wait for the current
        // DispatcherQueue. Post back to the HWND so Attach has returned and
        // SnowDesktop's normal message pump has advanced at least once.
        if (PostMessageW(window, kApplyXamlBackdropMessage, 0, 0))
        {
            systemBackdropUpdateQueued = true;
            return;
        }

        // A failed post must leave a usable opaque surface, particularly if a
        // high-contrast transition is trying to remove an existing backdrop.
        (void)runtime.SetSystemBackdropEnabled(false);
        if (shell)
            shell->SetSystemBackdropActive(false);
    }

    void ApplyDeferredSystemBackdrop() noexcept
    {
        systemBackdropUpdateQueued = false;
        if (shuttingDown || !runtime.IsAttached() || !shell)
            return;

        bool highContrast = false;
        const bool shouldEnable = SupportsMicaBackdrop() &&
            QueryHighContrastEnabled(highContrast) && !highContrast;
        bool active = false;
        if (shouldEnable)
            active = runtime.SetSystemBackdropEnabled(true);
        else
            (void)runtime.SetSystemBackdropEnabled(false);

        // The root becomes transparent only after the Island accepted Mica.
        // Windows 10, high contrast, and platform failures retain a current
        // theme-resource solid brush instead.
        shell->SetSystemBackdropActive(active);
    }

    void ApplyIntegratedTitleBarButtonColors() noexcept
    {
        if (!appWindowTitleBar)
            return;

        try
        {
            bool highContrast = false;
            if (!QueryHighContrastEnabled(highContrast) || highContrast)
            {
                const wf::IReference<wui::Color> systemDefault{nullptr};
                appWindowTitleBar.ButtonBackgroundColor(systemDefault);
                appWindowTitleBar.ButtonInactiveBackgroundColor(
                    systemDefault);
                appWindowTitleBar.ButtonHoverBackgroundColor(systemDefault);
                appWindowTitleBar.ButtonPressedBackgroundColor(systemDefault);
                return;
            }

            // With content extended into the caption, transparent resting
            // backgrounds let the same XAML/Mica surface continue underneath
            // all three Windows-owned caption buttons. Theme-aware overlays
            // preserve pointer feedback without reintroducing a solid strip.
            const auto transparent = BoxTitleBarColor(0, 0, 0, 0);
            const auto hover = darkTheme
                ? BoxTitleBarColor(24, 255, 255, 255)
                : BoxTitleBarColor(15, 0, 0, 0);
            const auto pressed = darkTheme
                ? BoxTitleBarColor(15, 255, 255, 255)
                : BoxTitleBarColor(25, 0, 0, 0);
            appWindowTitleBar.ButtonBackgroundColor(transparent);
            appWindowTitleBar.ButtonInactiveBackgroundColor(transparent);
            appWindowTitleBar.ButtonHoverBackgroundColor(hover);
            appWindowTitleBar.ButtonPressedBackgroundColor(pressed);
        }
        catch (...)
        {
            // Keep Windows defaults if color customization is unavailable.
        }
    }

    [[nodiscard]] bool ConfigureIntegratedTitleBar()
    {
        if (!window || !IsWindow(window) ||
            !muw::AppWindowTitleBar::IsCustomizationSupported())
        {
            SetError(L"Integrated settings title bar is not supported");
            return false;
        }

        try
        {
            const auto windowId =
                winrt::Microsoft::UI::GetWindowIdFromWindow(window);
            appWindow = muw::AppWindow::GetFromWindowId(windowId);
            if (!appWindow)
            {
                SetError(L"Get AppWindow for settings window failed");
                return false;
            }
            appWindowTitleBar = appWindow.TitleBar();
            if (!appWindowTitleBar)
            {
                SetError(L"Get settings AppWindowTitleBar failed");
                return false;
            }
            // Use the WinAppSDK-owned full-customization path: XAML supplies
            // the title-bar content while Windows keeps caption buttons,
            // the default drag region, maximize/Snap behavior and their
            // accessibility providers. The title bar has no interactive
            // content, so the platform-owned default drag region must not be
            // replaced with custom caption rectangles.
            appWindowTitleBar.ExtendsContentIntoTitleBar(true);
            appWindowTitleBar.PreferredHeightOption(
                muw::TitleBarHeightOption::Standard);
            appWindowTitleBar.IconShowOptions(
                muw::IconShowOptions::HideIconAndSystemMenu);
            appWindowTitleBar.PreferredTheme(darkTheme
                    ? muw::TitleBarTheme::Dark
                    : muw::TitleBarTheme::Light);
            ApplyIntegratedTitleBarButtonColors();
            return true;
        }
        catch (const winrt::hresult_error& error)
        {
            SetError(L"Configure integrated settings title bar (" +
                std::to_wstring(
                    static_cast<unsigned int>(error.code().value)) +
                L")");
        }
        catch (...)
        {
            SetError(L"Configure integrated settings title bar failed");
        }
        appWindowTitleBar = nullptr;
        appWindow = nullptr;
        return false;
    }

    void UpdateIntegratedTitleBarInsets() noexcept
    {
        integratedTitleBarInsetsUpdateQueued = false;
        if (shuttingDown || !runtime.IsAttached() || !shell ||
            !window || !IsWindow(window) || !appWindowTitleBar ||
            !appWindowTitleBar.ExtendsContentIntoTitleBar())
        {
            return;
        }

        try
        {
            const UINT dpi = GetDpiForWindow(window);
            const double scale = dpi > 0
                ? static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI
                : 1.0;
            shell->SetIntegratedTitleBarInsets(
                appWindowTitleBar.LeftInset() / scale,
                appWindowTitleBar.RightInset() / scale);
        }
        catch (...)
        {
            // Retain the last valid padding if caption metrics are temporarily
            // unavailable during attach, DPI or restore changes.
        }
    }

    void QueueIntegratedTitleBarInsetsUpdate() noexcept
    {
        if (integratedTitleBarInsetsUpdateQueued || shuttingDown ||
            !window || !IsWindow(window) || !runtime.IsAttached() ||
            !appWindowTitleBar)
        {
            return;
        }
        if (PostMessageW(
                window, kUpdateIntegratedTitleBarInsetsMessage, 0, 0))
        {
            integratedTitleBarInsetsUpdateQueued = true;
        }
    }

    void ResetIntegratedTitleBar() noexcept
    {
        integratedTitleBarInsetsUpdateQueued = false;
        if (appWindowTitleBar)
        {
            try
            {
                appWindowTitleBar.ResetToDefault();
            }
            catch (...)
            {
            }
        }
        appWindowTitleBar = nullptr;
        appWindow = nullptr;
    }

    void ApplyActualTheme(bool isDark) noexcept
    {
        if (shuttingDown || !OnOwnerThread())
            return;

        darkTheme = isDark;
        ApplySettingsWindowChrome(window, darkTheme);
        if (appWindowTitleBar)
        {
            try
            {
                appWindowTitleBar.PreferredTheme(darkTheme
                        ? muw::TitleBarTheme::Dark
                        : muw::TitleBarTheme::Light);
                ApplyIntegratedTitleBarButtonColors();
            }
            catch (...)
            {
            }
        }
    }

    void ShowActionError(const SettingsActionResult& result)
    {
        if (!shell || !controller || result.Succeeded())
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot)
            return;
        std::wstring title = L("settings.status.error");
        if (title.empty())
            title = L"Settings";
        std::wstring message = result.message;
        if (message.empty())
            message = L("settings.status.saveFailed");
        (void)shell->ShowInfoForGeneration(snapshot->generation,
            shell_impl::SettingsShellInfoSeverity::Error,
            std::move(title), std::move(message));
    }

    [[nodiscard]] bool DispatchToOwner(std::function<void()> task)
    {
        if (!task || shuttingDown || !callbacks ||
            !callbacks->alive.load())
        {
            return false;
        }
        try
        {
            if (callbacks->dispatcher.TryEnqueue(task))
                return true;
        }
        catch (...)
        {
        }

        if (!window || !IsWindow(window))
            return false;
        auto* queued = new (std::nothrow) std::function<void()>(
            std::move(task));
        if (!queued)
            return false;
        if (PostMessageW(window, kDispatchOwnerTaskMessage, 0,
                reinterpret_cast<LPARAM>(queued)))
        {
            return true;
        }
        delete queued;
        return false;
    }

    void DiscardPostedOwnerTasks() noexcept
    {
        if (!window)
            return;
        MSG message{};
        while (PeekMessageW(&message, window, kDispatchOwnerTaskMessage,
            kDispatchOwnerTaskMessage, PM_REMOVE))
        {
            delete reinterpret_cast<std::function<void()>*>(message.lParam);
        }
    }

    void ShowGenerationConfirmation(
        std::uint64_t generation,
        std::wstring title,
        std::wstring message,
        std::function<void(bool)> completed,
        bool destructive = true,
        std::wstring primaryButtonText = {})
    {
        if (!shell || !controller ||
            !controller->IsGenerationCurrent(generation))
        {
            if (completed)
                completed(false);
            return;
        }
        shell_impl::SettingsShellDialogRequest request;
        request.generation = generation;
        request.title = std::move(title);
        request.message = std::move(message);
        request.primaryButtonText = primaryButtonText.empty()
            ? L("settings.dialog.confirm")
            : std::move(primaryButtonText);
        request.closeButtonText = L("settings.dialog.cancel");
        request.destructive = destructive;
        shell->ShowConfirmation(std::move(request), std::move(completed));
    }

    SettingsSearchIndexInput BuildSearchInput() const
    {
        SettingsSearchIndexInput input;
        if (options.searchInput)
            input = options.searchInput();

        input.developerToolsVisible = options.developerToolsVisible &&
            options.developerToolsVisible();
        input.debugVisible = DebugPageVisible();
        if (input.languageTag.empty())
            input.languageTag = "runtime";

        if (input.staticSettings.empty())
        {
            const auto pageContext = [this](SettingsPage page) {
                switch (page)
                {
                case SettingsPage::General:
                    return L("app.settings.general");
                case SettingsPage::Personalization:
                    return L("app.settings.appearance");
                case SettingsPage::AppearanceTheme:
                    return L("settings.personalization.theme");
                case SettingsPage::AppearanceWidgets:
                    return L("settings.personalization.widgets");
                case SettingsPage::AppearanceDesktopIcons:
                    return L("app.settings.desktop_icons");
                case SettingsPage::AppearanceIconBeautification:
                    return L("app.settings.icon_beautify");
                case SettingsPage::Desktop:
                    return L("settings.nav.desktop");
                case SettingsPage::DesktopCategories:
                    return L("settings.nav.categories");
                case SettingsPage::Dock:
                case SettingsPage::DockAndTaskbar:
                    return L("settings.nav.dock");
                case SettingsPage::Taskbar:
                    return L("settings.nav.taskbar");
                case SettingsPage::Widgets:
                    return L("app.settings.widgets");
                case SettingsPage::BackupAndData:
                    return L("app.settings.backup");
                case SettingsPage::About:
                    return L("app.settings.about");
                case SettingsPage::DeveloperTools:
                    return L("app.settings.widgets_developer_tools");
                case SettingsPage::Debug:
                    return L("app.settings.debug");
                default:
                    return std::wstring{};
                }
            };
            input.staticSettings.reserve(std::size(kStaticSearchDefinitions));
            for (const auto& definition : kStaticSearchDefinitions)
            {
                StaticSettingSearchDescriptor descriptor;
                descriptor.page = definition.page;
                descriptor.focusId = definition.focusId;
                descriptor.label = L(definition.labelKey);
                descriptor.description = L(definition.descriptionKey);
                descriptor.context = pageContext(definition.page);
                descriptor.visible =
                    definition.page != SettingsPage::DeveloperTools &&
                        definition.page != SettingsPage::Debug
                    ? true
                    : (definition.page == SettingsPage::DeveloperTools
                        ? input.developerToolsVisible
                        : input.debugVisible);
                if (!descriptor.label.empty())
                    input.staticSettings.push_back(std::move(descriptor));
            }
        }
        return input;
    }

    void RebuildSearchIndex()
    {
        try
        {
            SettingsSearchIndexInput input = BuildSearchInput();
            searchIndex.Rebuild(input);
            if (shell)
            {
                shell->SetConditionalPagesVisible(
                    input.developerToolsVisible, input.debugVisible);
            }
        }
        catch (...)
        {
            SetError(L"Rebuild settings search index failed");
        }
    }

    void QueueSnapshot(SettingsController::SnapshotPtr snapshot)
    {
        if (!callbacks || !snapshot)
            return;
        {
            std::lock_guard lock(callbacks->snapshotMutex);
            callbacks->latestSnapshot = std::move(snapshot);
        }
        if (callbacks->snapshotQueued.exchange(true))
            return;

        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->snapshotQueued.store(false);
                        // The immutable snapshot carries generation/revision
                        // stale-result protection. Keeping this queue bound to
                        // one rendered-view epoch could discard a newer
                        // coalesced snapshot after a visible-window Open.
                        if (!state->alive.load() || !state->owner)
                            return;
                        SettingsController::SnapshotPtr latest;
                        {
                            std::lock_guard lock(state->snapshotMutex);
                            latest = std::move(state->latestSnapshot);
                        }
                        state->owner->ApplySnapshotNow(std::move(latest));
                    }))
            {
                callbacks->snapshotQueued.store(false);
            }
        }
        catch (...)
        {
            callbacks->snapshotQueued.store(false);
        }
    }

    void ApplySnapshotNow(SettingsController::SnapshotPtr snapshot)
    {
        if (!shell || !snapshot || shuttingDown)
            return;
        if (!Visible() && !snapshot->sessionActive)
            return;
        if (!shell->ApplySnapshot(*snapshot))
            return;
        // Ordinary controller revisions must not touch the Island's window-
        // level backdrop. In particular, continuous Slider/ColorPicker
        // previews publish here while WinUI owns pointer capture or a Flyout.
        // Backdrop refresh remains tied to attach and system theme/contrast
        // messages, where a window-level material transition is intentional.
        SynchronizePageBackends(*snapshot);
        if (options.homeAboutStatus)
        {
            HomeAboutStatusPatch patch = options.homeAboutStatus(
                snapshot->generation, snapshot->revision);
            patch.generation = snapshot->generation;
            patch.revision = snapshot->revision;
            (void)shell->ApplyHomeAboutStatusPatch(patch);
        }
    }

    void QueuePendingFlush()
    {
        if (!callbacks || callbacks->flushQueued.exchange(true))
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->flushQueued.store(false);
                        // Controller work belongs to the application session,
                        // not to one rendered route. A visible-window Open can
                        // legitimately advance the rendered-view epoch first;
                        // runs; dropping the only pending notification there
                        // would strand preview/commit work until shutdown.
                        if (!state->alive.load() || !state->owner)
                            return;
                        state->owner->FlushPendingNow();
                    }))
            {
                callbacks->flushQueued.store(false);
            }
        }
        catch (...)
        {
            callbacks->flushQueued.store(false);
        }
    }

    void FlushPendingNow()
    {
        if (!controller || shuttingDown)
            return;
        const SettingsActionResult result = controller->FlushPending();
        if (!result.Succeeded())
            ShowActionError(result);
        // A successful preview/commit already publishes revisioned snapshots
        // for the affected presenters. Refreshing every localized label here
        // rewrites the active XAML tree on the DispatcherQueue turn following
        // Slider.ValueChanged/ColorChanged, which releases pointer capture and
        // dismisses flyouts after their first value. Localization refreshes are
        // reserved for initialization, reopen, and ApplyLanguageChange.
    }

    std::wstring BackupConfirmationMessage(
        BackupDataConfirmationKind kind) const
    {
        switch (kind)
        {
        case BackupDataConfirmationKind::RestoreLayoutBackup:
            return L("settings.backup.restoreLayout.confirm");
        case BackupDataConfirmationKind::DeleteLayoutBackup:
            return L("settings.backup.deleteLayout.confirm");
        case BackupDataConfirmationKind::ImportAndRestoreFullBackup:
            return L("app.settings.restore_backup_file_confirm");
        case BackupDataConfirmationKind::RestoreFullBackup:
            return L("app.settings.restore_full_backup_confirm");
        case BackupDataConfirmationKind::DeleteFullBackup:
            return L("app.settings.delete_full_backup_confirm");
        case BackupDataConfirmationKind::MigrateData:
            return L("app.settings.migrate_data_confirm");
        }
        return {};
    }

    std::wstring BackupConfirmationTitle(
        BackupDataConfirmationKind kind) const
    {
        switch (kind)
        {
        case BackupDataConfirmationKind::RestoreLayoutBackup:
        case BackupDataConfirmationKind::DeleteLayoutBackup:
            return L("app.settings.layout_backups");
        case BackupDataConfirmationKind::MigrateData:
            return L("app.settings.data_migration");
        default:
            return L("app.settings.full_data_backups");
        }
    }

    void ConfigureWidgetsPageBackend()
    {
        if (!shell || !callbacks || !widgetEngine || widgetsPageBackend)
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;
        auto configured = options.widgetsPage;
        configured.localize = [weak](std::string_view key) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->L(key)
                : std::wstring{};
        };
        configured.dispatchToOwner = [weak](std::function<void()> task) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner &&
                state->owner->DispatchToOwner(std::move(task));
        };
        configured.diagnosticsVisible = [weak]() {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
                return false;
            const auto snapshot = state->owner->controller
                ? state->owner->controller->Snapshot() : nullptr;
            if (!snapshot || !snapshot->sessionActive)
                return false;
            const auto& hostOptions = state->owner->options;
            if (snapshot->route.page == SettingsPage::DeveloperTools)
            {
                return hostOptions.developerToolsVisible &&
                    hostOptions.developerToolsVisible();
            }
            if (snapshot->route.page == SettingsPage::Debug)
            {
                return state->owner->DebugPageVisible();
            }
            return false;
        };
        configured.snapshotChanged = [weak](
            std::shared_ptr<const WidgetsPageSnapshot> snapshot) {
            const auto state = weak.lock();
            if (state && state->alive.load() && state->owner &&
                state->owner->shell && snapshot)
            {
                (void)state->owner->shell->ApplyWidgetsPageSnapshot(
                    *snapshot);
            }
        };
        configured.pickPackage = [weak](std::uint64_t generation,
                                     WidgetsPageBackendOptions::
                                         PackagePickerCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (completed)
                    completed(std::nullopt);
                return;
            }
            auto selected = ShowOpenPathDialog(state->owner->window,
                state->owner->L("app.settings.widgets_install_package"),
                {{state->owner->L("app.settings.widgets_install_package"),
                    L"*.snowwidget"}}, false);
            if (completed)
                completed(std::move(selected));
        };
        configured.confirmInstall = [weak](std::uint64_t generation,
                                        WidgetInstallConfirmationRequest request,
                                        WidgetsPageBackendOptions::
                                            ConfirmationCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell || !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (completed)
                    completed(false);
                return;
            }
            state->owner->shell->ShowWidgetInstallConfirmation(generation,
                std::move(request),
                std::move(completed));
        };

        widgetsPageBackend = std::make_unique<WidgetsPageBackend>(
            *widgetEngine, std::move(configured));
        WidgetsPageActions actions;
        actions.invoke = [weak](std::uint64_t generation,
                             WidgetsPageRequest request) {
            const auto state = weak.lock();
            if (state && state->alive.load() && state->owner &&
                state->owner->widgetsPageBackend)
            {
                (void)state->owner->widgetsPageBackend->Invoke(
                    generation, std::move(request));
            }
        };
        actions.navigate = [weak](std::uint64_t generation,
                               SettingsRoute route) {
            const auto state = weak.lock();
            if (state && state->alive.load() && state->owner &&
                state->owner->controller &&
                state->owner->controller->IsGenerationCurrent(generation))
            {
                state->owner->RequestRoute(route);
            }
        };
        actions.setDeveloperToolsEnabled = [weak](
            std::uint64_t generation, bool enabled) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return false;
            }
            state->owner->EditGeneral(
                generation, SettingsUpdateMode::PreviewAndCommit,
                [enabled](GeneralSettings& settings) {
                    settings.widgetDeveloperToolsEnabled = enabled;
                });
            const auto snapshot = state->owner->controller->Snapshot();
            const bool applied = snapshot &&
                snapshot->generation == generation &&
                snapshot->values.general.widgetDeveloperToolsEnabled ==
                    enabled;
            if (applied)
            {
                // Publish the new toggle state back to the cached Widgets
                // presenter before another click can derive its next value.
                // Without this refresh, disabling and then re-enabling from
                // the same page keeps using the stale pre-click snapshot.
                if (state->owner->widgetsPageBackend)
                    (void)state->owner->widgetsPageBackend->Refresh();
                // Keep the conditional NavigationView item and its search
                // entries in sync before the presenter optionally navigates
                // to Developer Tools on this same click.
                state->owner->RebuildSearchIndex();
            }
            return applied;
        };
        actions.reloadWidgetInstance = [weak](
                                           std::uint64_t generation,
                                           std::wstring instanceId) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation) ||
                !state->owner->options.developerToolsVisible ||
                !state->owner->options.developerToolsVisible())
            {
                return;
            }
            SettingsHostActions::Request request;
            request.action =
                SettingsHostActions::Action::ReloadWidgetInstance;
            request.widgetInstanceId = std::move(instanceId);
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
            if (result.Succeeded() && state->owner->widgetsPageBackend)
                (void)state->owner->widgetsPageBackend->Refresh();
        };
        actions.confirm = [weak](std::uint64_t generation,
                              std::wstring title,
                              std::wstring message,
                              std::wstring primaryButtonText,
                              WidgetsPageActions::ConfirmationCompletion done) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
            {
                if (done)
                    done(false);
                return;
            }
            state->owner->ShowGenerationConfirmation(generation,
                std::move(title), std::move(message), std::move(done), true,
                std::move(primaryButtonText));
        };
        actions.editPermissions = [weak](std::uint64_t generation,
                                      WidgetPermissionEditorRequest request,
                                      WidgetsPageActions::
                                          PermissionEditorCompletion done) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell || !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                if (done) done({});
                return;
            }
            state->owner->shell->ShowWidgetPermissionEditor(generation,
                std::move(request), std::move(done));
        };
        shell->SetWidgetsPageActions(std::move(actions));
    }

    void ConfigureBackupDataPageBackend()
    {
        if (!shell || !callbacks || !controller || backupDataPageBackend)
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;
        auto configured = options.backupDataPage;
        configured.ownerWindow = [weak]() -> HWND {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->window
                : nullptr;
        };
        configured.postToUi = [weak](std::function<void()> task) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner &&
                state->owner->DispatchToOwner(std::move(task));
        };
        configured.localize = [weak](std::string_view key) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->L(key)
                : std::wstring{};
        };
        configured.confirm = [weak](HWND,
                                 BackupDataConfirmationRequest request,
                                 BackupDataPageActions::
                                     ConfirmationCompletion completed) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller)
            {
                if (completed)
                    completed(false);
                return;
            }
            const auto snapshot = state->owner->controller->Snapshot();
            if (!snapshot)
            {
                if (completed)
                    completed(false);
                return;
            }
            state->owner->ShowGenerationConfirmation(snapshot->generation,
                state->owner->BackupConfirmationTitle(request.kind),
                state->owner->BackupConfirmationMessage(request.kind),
                std::move(completed));
        };
        configured.pickPath = [weak](HWND owner,
                                  BackupDataPickerRequest request,
                                  BackupDataPageActions::PickerCompletion done) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
            {
                if (done)
                    done(std::nullopt);
                return;
            }
            std::optional<std::filesystem::path> selected;
            switch (request.kind)
            {
            case BackupDataPickerKind::ImportFullBackupArchive:
                selected = ShowOpenPathDialog(owner,
                    state->owner->L(
                        "app.settings.restore_from_backup_file"),
                    {{state->owner->L(
                          "app.settings.backup_archive_file_type"),
                         L"*.snowbackup;*.zip"},
                        {state->owner->L("app.settings.snowbackup_file_type"),
                         L"*.snowbackup"},
                        {state->owner->L("app.settings.zip_file_type"),
                         L"*.zip"}},
                    false);
                break;
            case BackupDataPickerKind::ExportFullBackupArchive:
                selected = ShowSavePathDialog(owner,
                    state->owner->L("app.settings.export_backup"),
                    request.suggestedFileName,
                    {{state->owner->L("app.settings.snowbackup_file_type"),
                         L"*.snowbackup"},
                        {state->owner->L("app.settings.zip_file_type"),
                         L"*.zip"}},
                    L"snowbackup");
                break;
            case BackupDataPickerKind::MigrationSourceDirectory:
                selected = ShowOpenPathDialog(owner,
                    state->owner->L("app.settings.select_migration_data"),
                    {}, true);
                break;
            }
            if (done)
                done(std::move(selected));
        };

        backupDataPageBackend = std::make_unique<BackupDataPageBackend>(
            *controller, std::move(configured));
        backupDataPageBackend->SetSnapshotChangedCallback(
            [weak](const BackupDataPageSnapshot& snapshot) {
                const auto state = weak.lock();
                if (state && state->alive.load() && state->owner &&
                    state->owner->shell)
                {
                    (void)state->owner->shell->ApplyBackupDataPageSnapshot(
                        snapshot);
                }
            });
        shell->SetBackupDataPageActions(backupDataPageBackend->Actions());
    }

    void EnsurePageBackends()
    {
        ConfigureBackupDataPageBackend();
        ConfigureWidgetsPageBackend();
    }

    void DisposePageBackends() noexcept
    {
        widgetsPageActive = false;
        widgetsBackendPage = SettingsPage::Home;
        backupDataPageActive = false;
        if (widgetsPageBackend)
        {
            widgetsPageBackend->Close();
            widgetsPageBackend.reset();
        }
        if (backupDataPageBackend)
        {
            // Must precede SettingsController::CloseSession: a completed
            // replacement can discard dirty state or reload the layout here.
            backupDataPageBackend->Close();
            backupDataPageBackend.reset();
        }
        if (shell)
        {
            shell->SetWidgetsPageActions({});
            shell->SetBackupDataPageActions({});
        }
    }

    void SynchronizePageBackends(const SettingsSnapshot& snapshot)
    {
        if (!snapshot.sessionActive || shuttingDown)
            return;
        EnsurePageBackends();

        const bool showWidgets = IsWidgetsBackendPage(snapshot.route.page);
        if (showWidgets && widgetsPageBackend)
        {
            if (widgetsPageActive &&
                widgetsBackendPage != snapshot.route.page)
            {
                widgetsPageBackend->Deactivate();
                widgetsPageActive = false;
            }
            if (!widgetsPageActive ||
                !widgetsPageBackend->IsGenerationCurrent(
                    snapshot.generation))
            {
                widgetsPageActive = widgetsPageBackend->Activate(
                    snapshot.generation,
                    snapshot.route.page == SettingsPage::Widgets);
                if (widgetsPageActive)
                    widgetsBackendPage = snapshot.route.page;
            }
        }
        else if (widgetsPageActive && widgetsPageBackend)
        {
            widgetsPageBackend->Deactivate();
            widgetsPageActive = false;
            widgetsBackendPage = SettingsPage::Home;
        }

        const bool showBackup =
            snapshot.route.page == SettingsPage::BackupAndData;
        if (showBackup && backupDataPageBackend)
        {
            const auto current = backupDataPageBackend->CurrentSnapshot();
            if (!backupDataPageActive || !current.initialized ||
                current.generation != snapshot.generation)
            {
                backupDataPageBackend->Activate(snapshot.generation);
                backupDataPageActive = true;
            }
        }
        else if (backupDataPageActive && backupDataPageBackend)
        {
            backupDataPageBackend->Deactivate();
            backupDataPageActive = false;
        }
    }

    void ConfigurePageActions()
    {
        if (!shell || !callbacks)
            return;
        const std::weak_ptr<CallbackState> weak = callbacks;

        GeneralPageActions general;
        general.commitGeneral = [weak](std::uint64_t generation,
                                      GeneralPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.commitNavigation = [weak](
            std::uint64_t generation,
            GeneralPageActions::NavigationEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditNavigation(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.commitDock = [weak](std::uint64_t generation,
                                   GeneralPageActions::DockEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDock(
                    generation, SettingsUpdateMode::Commit, std::move(edit));
            }
        };
        general.probeHotkey = [weak](
            SettingsHostActions::HotkeyTarget target,
            HotkeyChord chord,
            std::uint64_t generation,
            std::uint64_t,
            HotkeyRecorder::AvailabilityCompletion completion) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                completion(false, L"");
                return;
            }
            SettingsHostActions::Request request;
            request.action = SettingsHostActions::Action::
                ProbeHotkeyAvailability;
            request.hotkeyTarget = target;
            request.modifiers = chord.modifiers;
            request.virtualKey = chord.virtualKey;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            completion(result.Succeeded(), result.message);
        };
        general.languageCatalog = [weak]() {
            std::vector<SettingsLanguageOption> result;
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->options.languageCatalog)
            {
                return result;
            }
            for (auto&& [code, label] :
                state->owner->options.languageCatalog())
            {
                result.push_back(
                    {std::move(code), std::move(label)});
            }
            return result;
        };
        general.setAutoStart = [weak](
                                   std::uint64_t generation,
                                   bool enabled) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            SettingsHostActions::Request request;
            request.action =
                SettingsHostActions::Action::SetAutoStartEnabled;
            request.boolValue = enabled;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        general.openStartupAppsSettings = [weak](
                                                  std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            SettingsHostActions::Request request;
            request.action =
                SettingsHostActions::Action::OpenStartupAppsSettings;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        general.queryStartupConflict = [weak]() {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->options.startupConflict)
            {
                return GeneralStartupConflict{};
            }
            return state->owner->options.startupConflict();
        };
        shell->SetGeneralPageActions(std::move(general));

        PersonalizationPageActions personalization;
        personalization.update = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            PersonalizationPageActions::Edit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditPersonalization(
                    generation, mode, std::move(edit));
            }
        };
        personalization.updateGeneral = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            PersonalizationPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, mode, std::move(edit));
            }
        };
        shell->SetPersonalizationPageActions(std::move(personalization));

        DesktopPageActions desktop;
        desktop.updateDesktop = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::DesktopEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDesktop(
                    generation, mode, std::move(edit));
            }
        };
        desktop.updateCategory = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::CategoryEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditCategory(
                    generation, mode, std::move(edit));
            }
        };
        desktop.updatePersonalization = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DesktopPageActions::PersonalizationEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditPersonalization(
                    generation, mode, std::move(edit));
            }
        };
        desktop.commitCategory = [weak](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            state->owner->controller->RequestCommit(SettingsDomain::Category);
        };
        shell->SetDesktopPageActions(std::move(desktop));

        DockPageActions dock;
        dock.updateGeneral = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DockPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, mode, std::move(edit));
            }
        };
        dock.updateDock = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            DockPageActions::DockEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditDock(
                    generation, mode, std::move(edit));
            }
        };
        dock.invokeHost = [weak](
            std::uint64_t generation,
            SettingsHostActions::Request request) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        dock.confirm = [weak](
            std::uint64_t generation,
            std::wstring title,
            std::wstring message,
            DockPageActions::ConfirmationCompletion completion) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->shell ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                completion(false);
                return;
            }
            shell_impl::SettingsShellDialogRequest request;
            request.generation = generation;
            request.title = std::move(title);
            request.message = std::move(message);
            request.primaryButtonText =
                state->owner->L("settings.dialog.confirm");
            request.closeButtonText =
                state->owner->L("settings.dialog.cancel");
            request.destructive = true;
            state->owner->shell->ShowConfirmation(
                std::move(request), std::move(completion));
        };
        shell->SetDockPageActions(std::move(dock));

        HomeAboutPageActions homeAbout;
        homeAbout.navigate = [weak](const SettingsRoute& route) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->RequestRoute(route);
            }
        };
        homeAbout.invoke = [weak](
            std::uint64_t generation,
            HomeAboutCommand command) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }

            SettingsHostActions::Request request;
            switch (command)
            {
            case HomeAboutCommand::CheckForUpdates:
                request.action = SettingsHostActions::Action::CheckForUpdates;
                break;
            case HomeAboutCommand::CancelUpdateCheck:
                request.action =
                    SettingsHostActions::Action::CancelUpdateCheck;
                break;
            case HomeAboutCommand::OpenProject:
                request.action = SettingsHostActions::Action::OpenProject;
                break;
            case HomeAboutCommand::OpenLicense:
                request.action = SettingsHostActions::Action::OpenLicense;
                break;
            case HomeAboutCommand::OpenThirdPartyNotices:
                request.action =
                    SettingsHostActions::Action::OpenThirdPartyNotices;
                break;
            }
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        homeAbout.openLink = [weak](
                                 std::uint64_t generation,
                                 HomeAboutLink link) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            const std::wstring_view uri = HomeAboutLinkUri(link);
            if (uri.empty() || reinterpret_cast<INT_PTR>(ShellExecuteW(
                    state->owner->window, L"open",
                    std::wstring(uri).c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL)) <= 32)
            {
                state->owner->ShowActionError(
                    SettingsActionResult::Failure(
                        state->owner->L(
                            "settings.about.link.openFailed")));
            }
        };
        homeAbout.updateGeneral = [weak](
            std::uint64_t generation,
            SettingsUpdateMode mode,
            HomeAboutPageActions::GeneralEdit edit) {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->EditGeneral(
                    generation, mode, std::move(edit));
            }
        };
        homeAbout.setAnimationDiagnostics = [weak](
            std::uint64_t generation, bool enabled) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return;
            }
            SettingsHostActions::Request request;
            request.action =
                SettingsHostActions::Action::SetAnimationDiagnostics;
            request.boolValue = enabled;
            const SettingsActionResult result =
                state->owner->controller->InvokeHostAction(request);
            state->owner->ShowActionError(result);
        };
        homeAbout.unlockDebug = [weak](std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->controller ||
                !state->owner->controller->IsGenerationCurrent(generation))
            {
                return false;
            }
            state->owner->debugUnlocked = true;
            state->owner->RebuildSearchIndex();
            return state->owner->DebugPageVisible();
        };
        homeAbout.requestCrashTestConfirmation = [weak](
            std::uint64_t generation) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner ||
                !state->owner->DebugPageVisible())
            {
                return;
            }
            state->owner->ShowGenerationConfirmation(
                generation,
                state->owner->L("app.settings.crash_test"),
                state->owner->L("app.settings.crash_test_desc"),
                [weak, generation](bool confirmed) {
                    if (!confirmed)
                        return;
                    const auto current = weak.lock();
                    if (!current || !current->alive.load() ||
                        !current->owner || !current->owner->controller ||
                        !current->owner->controller->IsGenerationCurrent(
                            generation) ||
                        !current->owner->DebugPageVisible())
                    {
                        return;
                    }
                    SettingsHostActions::Request request;
                    request.action =
                        SettingsHostActions::Action::TriggerCrashTest;
                    (void)current->owner->controller->InvokeHostAction(
                        request);
                },
                true);
        };
        shell->SetHomeAboutPageActions(std::move(homeAbout));
    }

    void EditGeneral(std::uint64_t generation, SettingsUpdateMode mode,
        GeneralPageActions::GeneralEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        GeneralSettings value = snapshot->values.general;
        edit(value);
        controller->UpdateGeneral(std::move(value), mode);
    }

    void EditNavigation(std::uint64_t generation, SettingsUpdateMode mode,
        GeneralPageActions::NavigationEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        NavigationSettings value = snapshot->values.navigation;
        edit(value);
        controller->UpdateNavigation(std::move(value), mode);
    }

    void EditDock(std::uint64_t generation, SettingsUpdateMode mode,
        DockPageActions::DockEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        DockSettings value = snapshot->values.dock;
        edit(value);
        controller->UpdateDock(std::move(value), mode);
    }

    void EditPersonalization(std::uint64_t generation,
        SettingsUpdateMode mode, PersonalizationPageActions::Edit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        PersonalizationSettings value = snapshot->values.personalization;
        edit(value);
        controller->UpdatePersonalization(std::move(value), mode);
    }

    void EditDesktop(std::uint64_t generation, SettingsUpdateMode mode,
        DesktopPageActions::DesktopEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        DesktopDisplaySettings value = snapshot->values.desktop;
        edit(value);
        controller->UpdateDesktop(std::move(value), mode);
    }

    void EditCategory(std::uint64_t generation, SettingsUpdateMode mode,
        DesktopPageActions::CategoryEdit edit)
    {
        if (!controller || !controller->IsGenerationCurrent(generation))
            return;
        const auto snapshot = controller->Snapshot();
        if (!snapshot || snapshot->generation != generation)
            return;
        CategorySettings value = snapshot->values.category;
        edit(value);
        controller->UpdateCategory(std::move(value), mode);
    }

    [[nodiscard]] bool CommitRoute(const SettingsRoute& route,
        SettingsActionResult* controllerResult = nullptr)
    {
        if (!controller || !route.IsValid() || shuttingDown)
            return false;
        if ((route.page == SettingsPage::DeveloperTools &&
                (!options.developerToolsVisible ||
                    !options.developerToolsVisible())) ||
            (route.page == SettingsPage::Debug &&
                !DebugPageVisible()))
        {
            SetError(L"The requested conditional settings page is hidden.");
            return false;
        }

        const auto previous = controller->Snapshot();
        if (previous && previous->sessionActive && shell &&
            previous->route.page == SettingsPage::WidgetSettings &&
            (route.page != SettingsPage::WidgetSettings ||
                route.widgetInstanceId !=
                    previous->route.widgetInstanceId))
        {
            const auto flushed = shell->FlushPendingWidgetSettings();
            if (!flushed.Succeeded())
            {
                std::wstring message;
                if (!flushed.message.empty())
                    message = winrt::to_hstring(flushed.message).c_str();
                if (message.empty())
                    message = L("settings.widget.saveFailed");
                ShowActionError(SettingsActionResult::Failure(
                    std::move(message)));
                return false;
            }
        }

        std::optional<widget_runtime::WidgetSettingsSnapshot>
            widgetSnapshot;
        if (route.page == SettingsPage::WidgetSettings)
        {
            try
            {
                if (!widgetSettingsService ||
                    (options.ensureWidgetSettingsInstance &&
                        !options.ensureWidgetSettingsInstance(
                            route.widgetInstanceId)))
                {
                    ShowActionError(SettingsActionResult::Failure(
                        L("settings.widget.loadFailed")));
                    return false;
                }

                const auto loaded = widgetSettingsService->Load(
                    route.widgetInstanceId);
                const auto current = widgetSettingsService->Snapshot(
                    route.widgetInstanceId);
                if (!loaded.Succeeded() || !loaded.snapshot || !current ||
                    loaded.snapshot->widgetId != route.widgetInstanceId ||
                    current->widgetId != loaded.snapshot->widgetId ||
                    current->generation != loaded.snapshot->generation ||
                    current->revision != loaded.snapshot->revision)
                {
                    std::wstring message;
                    if (!loaded.message.empty())
                        message = winrt::to_hstring(loaded.message).c_str();
                    if (message.empty())
                        message = L("settings.widget.loadFailed");
                    ShowActionError(SettingsActionResult::Failure(
                        std::move(message)));
                    return false;
                }
                widgetSnapshot = *loaded.snapshot;
            }
            catch (...)
            {
                ShowActionError(SettingsActionResult::Failure(
                    L("settings.widget.loadFailed")));
                return false;
            }
        }

        const SettingsActionResult opened = controller->Open(route);
        if (controllerResult)
            *controllerResult = opened;
        const auto snapshot = controller->Snapshot();
        if (!IsUsableControllerSnapshot(snapshot) ||
            !snapshot->sessionActive || snapshot->route != route)
        {
            ShowActionError(opened);
            return false;
        }

        ApplySnapshotNow(snapshot);
        if (widgetSnapshot &&
            (!shell || !shell->ApplyWidgetSettingsSnapshot(
                *widgetSnapshot)))
        {
            ShowActionError(SettingsActionResult::Failure(
                L("settings.widget.loadFailed")));
            return false;
        }
        return true;
    }

    void RequestRoute(const SettingsRoute& route)
    {
        SettingsActionResult result;
        if (CommitRoute(route, &result) && !result.Succeeded())
            ShowActionError(result);
    }

    void RequestSearch(std::wstring query, std::uint64_t generation,
        std::uint64_t requestId)
    {
        if (!callbacks)
            return;
        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            (void)callbacks->dispatcher.TryEnqueue(
                [weak, query = std::move(query), generation, requestId,
                    expectedEpoch]() mutable {
                    const auto state = weak.lock();
                    if (!state || !state->alive.load() || !state->owner ||
                        state->owner->viewEpoch != expectedEpoch ||
                        !state->owner->shell ||
                        !state->owner->controller ||
                        !state->owner->controller->IsGenerationCurrent(
                            generation))
                    {
                        return;
                    }
                    auto results = state->owner->searchIndex.Search(query);
                    (void)state->owner->shell->SetSearchResults(
                        std::move(results), generation, requestId);
                });
        }
        catch (...)
        {
        }
    }

    void RefreshLocalizedPresentation()
    {
        if (!shell)
            return;
        shell->RefreshLocalizedText();
        if (widgetsPageActive && widgetsPageBackend)
            (void)widgetsPageBackend->Refresh();
        if (backupDataPageActive && backupDataPageBackend)
            backupDataPageBackend->Refresh();
        // Component schemas are supplied by the reloaded runtime. Rebuild
        // search last so both the current editor and the management snapshot
        // have observed the new language generation first.
        RebuildSearchIndex();
        std::wstring title = L("settings.shell.title");
        if (title.empty())
            title = options.windowTitle;
        if (window && IsWindow(window))
            SetWindowTextW(window, title.c_str());
    }

    [[nodiscard]] bool PrepareLanguageChange()
    {
        if (!controller || !shell)
            return true;
        const auto current = controller->Snapshot();
        if (!current || !current->sessionActive ||
            current->route.page != SettingsPage::WidgetSettings)
        {
            return true;
        }

        const auto flushed = shell->FlushPendingWidgetSettings();
        if (flushed.Succeeded())
            return true;

        std::wstring message;
        if (!flushed.message.empty())
            message = winrt::to_hstring(flushed.message).c_str();
        if (message.empty())
            message = L("settings.widget.saveFailed");
        ShowActionError(SettingsActionResult::Failure(std::move(message)));
        return false;
    }

    void ReloadActiveWidgetSettingsForLanguageChange()
    {
        if (!controller || !shell || !widgetSettingsService)
            return;
        const auto controllerSnapshot = controller->Snapshot();
        if (!controllerSnapshot || !controllerSnapshot->sessionActive ||
            controllerSnapshot->route.page != SettingsPage::WidgetSettings)
        {
            return;
        }

        const std::wstring instanceId =
            controllerSnapshot->route.widgetInstanceId;
        const auto disableStaleEditor = [&]() {
            try
            {
                widgetSettingsService->Close(instanceId);
                const auto currentController = controller->Snapshot();
                if (currentController && currentController->sessionActive &&
                    currentController->route ==
                        controllerSnapshot->route &&
                    shell->CurrentRoute() == controllerSnapshot->route)
                {
                    // The old runtime generation must never remain editable.
                    // A normal Open resumes interaction and retries the load.
                    shell->SuspendInteraction();
                    interactionSuspended = true;
                }
            }
            catch (...)
            {
            }
        };

        try
        {
            const auto loaded = widgetSettingsService->Reload(instanceId);
            const auto current = widgetSettingsService->Snapshot(instanceId);
            if (!loaded.Succeeded() || !loaded.snapshot || !current ||
                loaded.snapshot->widgetId != instanceId ||
                current->widgetId != loaded.snapshot->widgetId ||
                current->generation != loaded.snapshot->generation ||
                current->revision != loaded.snapshot->revision ||
                !shell->ApplyWidgetSettingsSnapshot(*loaded.snapshot))
            {
                std::wstring message;
                if (!loaded.message.empty())
                    message = winrt::to_hstring(loaded.message).c_str();
                if (message.empty())
                    message = L("settings.widget.loadFailed");
                ShowActionError(
                    SettingsActionResult::Failure(std::move(message)));
                disableStaleEditor();
            }
        }
        catch (...)
        {
            ShowActionError(SettingsActionResult::Failure(
                L("settings.widget.loadFailed")));
            disableStaleEditor();
        }
    }

    void ResumeInteraction()
    {
        if (!shell || !interactionSuspended)
            return;
        shell->ResumeInteraction();
        interactionSuspended = false;
    }

    void SuspendInteraction()
    {
        if (!shell || interactionSuspended)
            return;
        shell->SuspendInteraction();
        interactionSuspended = true;
    }

    [[nodiscard]] bool FlushPendingChanges()
    {
        if (!controller || shuttingDown)
            return false;
        if (shell)
        {
            const auto widgetResult =
                shell->FlushPendingWidgetSettings();
            if (!widgetResult.Succeeded())
            {
                const auto snapshot = controller->Snapshot();
                std::wstring message;
                if (!widgetResult.message.empty())
                    message = winrt::to_hstring(widgetResult.message).c_str();
                if (message.empty())
                    message = L("settings.widget.saveFailed");
                if (snapshot)
                {
                    (void)shell->ShowInfoForGeneration(
                        snapshot->generation,
                        shell_impl::SettingsShellInfoSeverity::Error,
                        L("settings.status.error"), std::move(message));
                }
                return false;
            }
        }

        const SettingsActionResult result = controller->FlushAll();
        if (!result.Succeeded())
        {
            ShowActionError(result);
            return false;
        }
        return true;
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self)
                self->window = hwnd;
        }

        if (!self)
            return DefWindowProcW(hwnd, message, wParam, lParam);

        switch (message)
        {
        case kApplyXamlBackdropMessage:
            self->ApplyDeferredSystemBackdrop();
            return 0;
        case kUpdateIntegratedTitleBarInsetsMessage:
            self->UpdateIntegratedTitleBarInsets();
            return 0;
        case kDispatchOwnerTaskMessage:
        {
            std::unique_ptr<std::function<void()>> task(
                reinterpret_cast<std::function<void()>*>(lParam));
            if (task && *task)
            {
                try
                {
                    (*task)();
                }
                catch (...)
                {
                }
            }
            return 0;
        }
        case WM_CLOSE:
            (void)self->HideWindow();
            return 0;
        case WM_GETMINMAXINFO:
        {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT windowDpi = GetDpiForWindow(hwnd);
            const UINT dpi = windowDpi != 0 ? windowDpi : 96;
            const int minimumClientWidth = MulDiv(
                kMinimumClientWidth, static_cast<int>(dpi), 96);
            const int minimumClientHeight = MulDiv(
                kMinimumClientHeight, static_cast<int>(dpi), 96);
            RECT minimumBounds{0, 0,
                minimumClientWidth, minimumClientHeight};
            const DWORD style = static_cast<DWORD>(
                GetWindowLongPtrW(hwnd, GWL_STYLE));
            const DWORD extendedStyle = static_cast<DWORD>(
                GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            if (AdjustWindowRectExForDpi(&minimumBounds, style, FALSE,
                    extendedStyle, dpi))
            {
                info->ptMinTrackSize.x =
                    minimumBounds.right - minimumBounds.left;
                info->ptMinTrackSize.y =
                    minimumBounds.bottom - minimumBounds.top;
            }
            else
            {
                info->ptMinTrackSize.x = minimumClientWidth;
                info->ptMinTrackSize.y = minimumClientHeight;
            }
            return 0;
        }
        case WM_SIZE:
            self->QueueIntegratedTitleBarInsetsUpdate();
            break;
        case WM_DPICHANGED:
        {
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested)
            {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOACTIVATE | SWP_NOZORDER);
            }
            self->QueueIntegratedTitleBarInsetsUpdate();
            break;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplySettingsWindowChrome(hwnd, self->darkTheme);
            self->ApplyIntegratedTitleBarButtonColors();
            self->QueueSystemBackdropUpdate();
            self->QueueIntegratedTitleBarInsetsUpdate();
            break;
        case WM_NCDESTROY:
            self->systemBackdropUpdateQueued = false;
            self->integratedTitleBarInsetsUpdateQueued = false;
            self->runtime.HandleWindowMessage(message, wParam, lParam);
            self->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return DefWindowProcW(hwnd, message, wParam, lParam);
        default:
            break;
        }

        self->runtime.HandleWindowMessage(message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    [[nodiscard]] bool RegisterWindowClass()
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = instance;
        windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        if (!windowClass.hIcon)
            windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = kSettingsWindowClassName;
        if (RegisterClassExW(&windowClass) != 0)
            return true;
        const DWORD error = GetLastError();
        if (error == ERROR_CLASS_ALREADY_EXISTS)
            return true;
        SetError(FormatWin32Error(L"Register settings window class", error));
        return false;
    }

    [[nodiscard]] bool CreateHostWindow()
    {
        const UINT dpi = GetDpiForSystem();
        RECT bounds{0, 0,
            MulDiv(kDefaultClientWidth, static_cast<int>(dpi), 96),
            MulDiv(kDefaultClientHeight, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE,
            WS_EX_APPWINDOW, dpi);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
        const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);

        window = CreateWindowExW(WS_EX_APPWINDOW,
            kSettingsWindowClassName, options.windowTitle.c_str(),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, width, height, nullptr, nullptr, instance, this);
        if (!window)
        {
            SetError(FormatWin32Error(
                L"Create settings window", GetLastError()));
            return false;
        }
        ApplySettingsWindowChrome(window, darkTheme);
        return true;
    }

    [[nodiscard]] bool HideWindow()
    {
        if (!controller || !window || shuttingDown)
            return false;
        if (!FlushPendingChanges())
            return false;
        ++viewEpoch;
        SuspendInteraction();
        DisposePageBackends();
        const SettingsActionResult result = controller->CloseSession();
        if (!result.Succeeded())
        {
            --viewEpoch;
            ResumeInteraction();
            ApplySnapshotNow(controller->Snapshot());
            ShowActionError(result);
            return false;
        }
        if (widgetSettingsService)
            widgetSettingsService->CloseAll();
        ShowWindow(window, SW_HIDE);
        return true;
    }
};

SettingsWindowHost::SettingsWindowHost()
    : impl_(std::make_unique<Impl>())
{
}

SettingsWindowHost::~SettingsWindowHost()
{
    Shutdown();
}

bool SettingsWindowHost::Initialize(
    HINSTANCE instance,
    SettingsController& controller,
    widget_runtime::WidgetSettingsService* widgetSettingsService,
    SettingsWindowHostOptions options)
{
    if (impl_->initialized)
        return impl_->OnOwnerThread();
    if (!instance)
    {
        impl_->SetError(L"Settings window initialization requires HINSTANCE");
        return false;
    }

    impl_->ownerThreadId = GetCurrentThreadId();
    impl_->instance = instance;
    impl_->controller = &controller;
    impl_->widgetSettingsService = widgetSettingsService;
    impl_->options = std::move(options);
    impl_->lastError.clear();

    if (!impl_->runtime.Initialize() || !impl_->RegisterWindowClass() ||
        !impl_->CreateHostWindow() || !impl_->ConfigureIntegratedTitleBar())
    {
        if (impl_->lastError.empty())
            impl_->lastError = impl_->runtime.LastError();
        Shutdown();
        return false;
    }

    try
    {
        impl_->shell = winrt::make_self<shell_impl::SettingsShell>();
        impl_->callbacks =
            std::make_shared<Impl::CallbackState>();
        impl_->callbacks->owner = impl_.get();
        impl_->callbacks->dispatcher =
            mud::DispatcherQueue::GetForCurrentThread();
        if (!impl_->callbacks->dispatcher)
            winrt::throw_hresult(E_UNEXPECTED);

        const std::weak_ptr<Impl::CallbackState> weak = impl_->callbacks;
        impl_->shell->SetLocalizer([weak](std::string_view key) {
            const auto state = weak.lock();
            return state && state->alive.load() && state->owner
                ? state->owner->L(key)
                : std::wstring{};
        });
        impl_->shell->SetRouteRequestedCallback(
            [weak](const SettingsRoute& route) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->RequestRoute(route);
                }
            });
        impl_->shell->SetSearchRequestedCallback(
            [weak](std::wstring query, std::uint64_t generation,
                   std::uint64_t requestId) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->RequestSearch(
                        std::move(query), generation, requestId);
                }
            });
        impl_->shell->SetCancelOperationCallback([](std::uint64_t) {});
        impl_->shell->SetWidgetSettingsService(widgetSettingsService);
        impl_->ConfigurePageActions();
        impl_->RebuildSearchIndex();

        if (!impl_->runtime.Attach(impl_->window,
                impl_->shell.as<mux::UIElement>()))
        {
            impl_->SetError(impl_->runtime.LastError());
            Shutdown();
            return false;
        }
        impl_->shell->SetSystemBackdropActive(false);
        impl_->shell->SetActualThemeChangedCallback(
            [weak](bool darkTheme) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->ApplyActualTheme(darkTheme);
                }
            });
        impl_->QueueIntegratedTitleBarInsetsUpdate();
        impl_->QueueSystemBackdropUpdate();

        controller.SetSnapshotChangedCallback(
            [weak](SettingsController::SnapshotPtr snapshot) {
                if (const auto state = weak.lock();
                    state && state->alive.load() && state->owner)
                {
                    state->owner->QueueSnapshot(std::move(snapshot));
                }
            });
        controller.SetPendingWorkCallback([weak]() {
            if (const auto state = weak.lock();
                state && state->alive.load() && state->owner)
            {
                state->owner->QueuePendingFlush();
            }
        });

        impl_->initialized = true;
        impl_->ApplySnapshotNow(controller.Snapshot());
        impl_->RefreshLocalizedPresentation();
        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        impl_->SetError(
            L"Initialize WinUI settings shell (" +
            std::to_wstring(static_cast<unsigned int>(error.code().value)) +
            L")");
    }
    catch (...)
    {
        impl_->SetError(L"Initialize WinUI settings shell failed");
    }

    Shutdown();
    return false;
}

void SettingsWindowHost::Shutdown() noexcept
{
    if (!impl_ || impl_->shuttingDown)
        return;
    if (impl_->initialized && impl_->OnOwnerThread() && impl_->controller)
        (void)impl_->FlushPendingChanges();
    impl_->shuttingDown = true;

    if (impl_->shell)
        impl_->shell->SetActualThemeChangedCallback({});
    if (impl_->shell)
        impl_->shell->SetWidgetSettingsService(nullptr);
    impl_->DisposePageBackends();
    if (impl_->controller)
    {
        impl_->controller->SetSnapshotChangedCallback({});
        impl_->controller->SetPendingWorkCallback({});
        (void)impl_->controller->CloseSession();
    }
    if (impl_->widgetSettingsService)
        impl_->widgetSettingsService->CloseAll();

    if (impl_->callbacks)
    {
        impl_->callbacks->alive.store(false);
        impl_->callbacks->owner = nullptr;
        {
            std::lock_guard lock(impl_->callbacks->snapshotMutex);
            impl_->callbacks->latestSnapshot.reset();
        }
    }

    impl_->systemBackdropUpdateQueued = false;
    if (impl_->shell)
        impl_->shell->SetSystemBackdropActive(false);
    if (impl_->shell)
    {
        impl_->shell->Close();
        impl_->shell = nullptr;
    }
    impl_->runtime.Detach();
    impl_->ResetIntegratedTitleBar();
    impl_->DiscardPostedOwnerTasks();
    if (impl_->window && IsWindow(impl_->window))
        DestroyWindow(impl_->window);
    impl_->window = nullptr;
    impl_->runtime.Shutdown();

    impl_->callbacks.reset();
    impl_->widgetEngine = nullptr;
    impl_->widgetSettingsService = nullptr;
    impl_->controller = nullptr;
    impl_->instance = nullptr;
    impl_->initialized = false;
    impl_->interactionSuspended = true;
    impl_->ownerThreadId = 0;
    impl_->shuttingDown = false;
}

bool SettingsWindowHost::Open(const SettingsRoute& route)
{
    if (!impl_->initialized || !impl_->OnOwnerThread() || !route.IsValid())
        return false;

    ++impl_->viewEpoch;
    const bool reopening = !impl_->Visible();
    SettingsActionResult reloadResult = SettingsActionResult::Success();
    if (reopening)
    {
        reloadResult = impl_->controller->Reload(
            SettingsReloadPolicy::PreservePendingChanges);
        if (impl_->options.refreshExternalState)
            impl_->options.refreshExternalState();
        impl_->RefreshLocalizedPresentation();
    }

    SettingsActionResult openResult;
    if (!impl_->CommitRoute(route, &openResult))
        return false;
    const auto snapshot = impl_->controller->Snapshot();
    impl_->ApplySnapshotNow(snapshot);
    impl_->ResumeInteraction();
    if (IsIconic(impl_->window))
        ShowWindow(impl_->window, SW_RESTORE);
    else
        ShowWindow(impl_->window, SW_SHOWNORMAL);
    SetForegroundWindow(impl_->window);
    SetActiveWindow(impl_->window);

    if (!reloadResult.Succeeded())
        impl_->ShowActionError(reloadResult);
    if (!openResult.Succeeded())
        impl_->ShowActionError(openResult);
    return true;
}

bool SettingsWindowHost::Hide()
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->HideWindow();
}

bool SettingsWindowHost::FlushPendingChanges()
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->FlushPendingChanges();
}

void SettingsWindowHost::SetWidgetSettingsService(
    widget_runtime::WidgetSettingsService* service) noexcept
{
    if (impl_->shell)
        impl_->shell->SetWidgetSettingsService(service);
    impl_->widgetSettingsService = service;
}

void SettingsWindowHost::SetWidgetEngine(WidgetEngine* engine)
{
    if (!impl_->OnOwnerThread() || impl_->widgetEngine == engine)
        return;
    impl_->widgetsPageActive = false;
    impl_->widgetsBackendPage = SettingsPage::Home;
    if (impl_->widgetsPageBackend)
    {
        impl_->widgetsPageBackend->Close();
        impl_->widgetsPageBackend.reset();
    }
    if (impl_->shell)
        impl_->shell->SetWidgetsPageActions({});
    impl_->widgetEngine = engine;
    if (!impl_->initialized || !impl_->controller)
        return;
    impl_->ConfigureWidgetsPageBackend();
    const auto snapshot = impl_->controller->Snapshot();
    if (snapshot && snapshot->sessionActive)
        impl_->SynchronizePageBackends(*snapshot);
    impl_->RebuildSearchIndex();
}

void SettingsWindowHost::RefreshWidgetsPage()
{
    if (!impl_->initialized || !impl_->OnOwnerThread())
        return;
    if (impl_->widgetsPageBackend)
        (void)impl_->widgetsPageBackend->Refresh();
    impl_->RebuildSearchIndex();
}

bool SettingsWindowHost::PrepareLanguageChange()
{
    if (!impl_->initialized)
        return true;
    return impl_->OnOwnerThread() && impl_->PrepareLanguageChange();
}

void SettingsWindowHost::ApplyLanguageChange(bool widgetRuntimeReloaded)
{
    if (impl_->initialized && impl_->OnOwnerThread())
    {
        if (widgetRuntimeReloaded)
            impl_->ReloadActiveWidgetSettingsForLanguageChange();
        impl_->RefreshLocalizedPresentation();
    }
}

bool SettingsWindowHost::PublishHomeAboutStatus(
    HomeAboutStatusPatch patch)
{
    return impl_->initialized && impl_->OnOwnerThread() && impl_->shell &&
        impl_->controller &&
        impl_->controller->IsGenerationCurrent(patch.generation) &&
        impl_->shell->ApplyHomeAboutStatusPatch(patch);
}

bool SettingsWindowHost::PreTranslateMessage(MSG* message) noexcept
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->runtime.PreTranslateMessage(message);
}

bool SettingsWindowHost::ProcessTabNavigation(MSG* message) noexcept
{
    return impl_->initialized && impl_->OnOwnerThread() &&
        impl_->runtime.ProcessTabNavigation(message);
}

bool SettingsWindowHost::IsHotkeyCaptureActive() const noexcept
{
    return impl_->initialized && impl_->shell &&
        impl_->shell->IsHotkeyCaptureActive();
}

void SettingsWindowHost::CaptureRegisteredHotkey(
    UINT modifiers, UINT virtualKey)
{
    if (impl_->initialized && impl_->shell)
        impl_->shell->CaptureRegisteredHotkey(modifiers, virtualKey);
}

void SettingsWindowHost::ShowExitConfirmation(
    std::function<void(bool)> completed)
{
    if (!impl_->initialized || !impl_->shell || !impl_->controller)
    {
        if (completed)
            completed(false);
        return;
    }
    const auto snapshot = impl_->controller->Snapshot();
    if (!snapshot || !snapshot->sessionActive)
    {
        if (completed)
            completed(false);
        return;
    }
    shell_impl::SettingsShellDialogRequest request;
    request.generation = snapshot->generation;
    request.title = impl_->L("app.settings.exit_confirm");
    request.message = impl_->L("app.settings.exit_confirm_text") + L"\n\n" +
        impl_->L("app.settings.exit_restore_text");
    request.primaryButtonText = impl_->L("app.settings.exit_ok");
    request.closeButtonText = impl_->L("app.settings.cancel");
    request.destructive = true;
    impl_->shell->ShowConfirmation(std::move(request), std::move(completed));
}

bool SettingsWindowHost::IsInitialized() const noexcept
{
    return impl_->initialized;
}

bool SettingsWindowHost::IsVisible() const noexcept
{
    return impl_->Visible();
}

HWND SettingsWindowHost::Window() const noexcept
{
    return impl_->window;
}

const std::wstring& SettingsWindowHost::LastError() const noexcept
{
    return impl_->lastError;
}

} // namespace snowdesktop::winui
