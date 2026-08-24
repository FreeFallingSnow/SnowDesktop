#include "pch.h"

#include "settings_window_host.h"

#include "SettingsShell.xaml.h"
#include "settings_titlebar_policy.h"
#include "winui_runtime.h"
#include "../widget_settings_service.h"

#include <shobjidl.h>
#include <dwmapi.h>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
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
constexpr UINT kRefreshIntegratedTitleBarMessage = WM_APP + 0x349;

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

bool IsWindows11OrGreater() noexcept
{
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtlGetVersion)
        return false;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return rtlGetVersion(&version) == 0 && version.dwMajorVersion >= 10 &&
        version.dwBuildNumber >= 22000;
}

bool SupportsMicaBackdrop() noexcept
{
    return IsWindows11OrGreater();
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
    {SettingsPage::General, "general.softwareDesktop",
        "settings.general.softwareDesktop",
        "settings.general.softwareDesktop.description"},
    {SettingsPage::General, "general.language",
        "settings.general.language",
        "settings.general.language.description"},
    {SettingsPage::General, "general.doubleClickHide",
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
    {SettingsPage::General, "general.desktopPassthrough",
        "settings.general.desktopPassthrough",
        "settings.general.desktopPassthrough.description"},
    {SettingsPage::General, "general.desktopPassthrough.hotkey",
        "app.settings.hotkey",
        "settings.general.desktopPassthrough.description"},
    {SettingsPage::General, "general.floatingDock",
        "settings.general.floatingDock",
        "settings.general.floatingDock.description"},
    {SettingsPage::General, "general.floatingDock.hotkey",
        "app.settings.hotkey",
        "settings.general.floatingDock.description"},
    {SettingsPage::Personalization, "personalization.theme",
        "settings.personalization.theme",
        "settings.personalization.theme.description"},
    {SettingsPage::Personalization, "personalization.backgroundColor",
        "settings.personalization.colors",
        "settings.personalization.colors.description"},
    {SettingsPage::Personalization,
        "personalization.quickNavigationTheme",
        "app.settings.quick_nav_theme",
        "settings.personalization.theme.description"},
    {SettingsPage::Personalization,
        "personalization.collectionPopupTheme",
        "app.settings.collection_popup_theme",
        "settings.personalization.theme.description"},
    {SettingsPage::Personalization, "personalization.borderColor",
        "app.settings.component_border",
        "settings.personalization.colors.description"},
    {SettingsPage::Personalization, "personalization.widgetAlpha",
        "app.settings.bg_opacity",
        "settings.personalization.colors.description"},
    {SettingsPage::Personalization, "personalization.borderAlpha",
        "app.settings.border_opacity",
        "settings.personalization.colors.description"},
    {SettingsPage::Personalization, "personalization.enableGradient",
        "app.settings.enable_gradient",
        "settings.personalization.colors.description"},
    {SettingsPage::Personalization, "personalization.gradientEndAlpha",
        "app.settings.gradient_end_alpha",
        "settings.personalization.colors.description"},
    {SettingsPage::Personalization, "personalization.glass",
        "settings.personalization.glass",
        "settings.personalization.glass.description"},
    {SettingsPage::Personalization, "personalization.acrylic",
        "settings.personalization.acrylic",
        "settings.personalization.acrylic.description"},
    {SettingsPage::Personalization, "personalization.blurRadius",
        "app.settings.blur_radius",
        "settings.personalization.glass.description"},
    {SettingsPage::Personalization, "personalization.contentTheme",
        "app.settings.text_color",
        "settings.personalization.theme.description"},
    {SettingsPage::Personalization, "personalization.contextMenu",
        "settings.personalization.contextMenu",
        "settings.personalization.contextMenu.description"},
    {SettingsPage::Personalization, "personalization.cornerRadius",
        "settings.personalization.widgets",
        "settings.personalization.widgets.description"},
    {SettingsPage::Personalization, "personalization.barHeight",
        "app.settings.bar_height",
        "settings.personalization.widgets.description"},
    {SettingsPage::Personalization, "personalization.tabHeight",
        "app.settings.tab_height",
        "settings.personalization.widgets.description"},
    {SettingsPage::Personalization,
        "personalization.showCategoryTabCounts",
        "app.settings.category_show_count",
        "settings.personalization.widgets.description"},
    {SettingsPage::Desktop, "desktop.spacing", "settings.desktop.spacing",
        "settings.desktop.spacing.description"},
    {SettingsPage::Desktop, "desktop.iconSize", "settings.desktop.iconSize",
        "settings.desktop.iconSize.description"},
    {SettingsPage::Desktop, "desktop.itemFontSize",
        "settings.desktop.typography",
        "settings.desktop.typography.description"},
    {SettingsPage::Desktop, "desktop.listFontSize",
        "app.settings.list_font_size",
        "settings.desktop.typography.description"},
    {SettingsPage::Desktop, "desktop.fontWeight",
        "app.settings.title_font_weight",
        "settings.desktop.typography.description"},
    {SettingsPage::Desktop, "desktop.shortcutArrow",
        "settings.desktop.shortcutArrow",
        "settings.desktop.shortcutArrow.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify",
        "settings.desktop.iconBeautify",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.mode",
        "app.settings.beautify_mode",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.backgroundColor",
        "app.settings.default_bg",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.backgroundOpacity",
        "app.settings.bg_opacity_val",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.gradient",
        "app.settings.enable_gradient_bg",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.gradientEndColor",
        "app.settings.gradient_end_color",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.gradientDirection",
        "app.settings.beautify_gradient_dir",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.shape",
        "app.settings.beautify_shape",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.contentScale",
        "app.settings.beautify_content_scale",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.highlightStrength",
        "app.settings.beautify_texture_highlight",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.highlightSize",
        "app.settings.beautify_texture_highlight_size",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.highlightAngle",
        "app.settings.beautify_texture_highlight_angle",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.shadeStrength",
        "app.settings.beautify_texture_shade",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.edgeHighlight",
        "app.settings.beautify_texture_edge",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.filter",
        "app.settings.beautify_filter",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.filterColor",
        "app.settings.beautify_filter_color",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.filterStrength",
        "app.settings.beautify_filter_strength",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.shadowStrength",
        "app.settings.beautify_shadow_strength",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.outline",
        "app.settings.beautify_outline",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.outlineWidth",
        "app.settings.beautify_outline_width",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.outlineOpacity",
        "app.settings.beautify_outline_opacity",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.iconBeautify.outlineColor",
        "app.settings.beautify_outline_color",
        "settings.desktop.iconBeautify.description"},
    {SettingsPage::Desktop, "desktop.categoryLayout",
        "settings.desktop.categoryLayout",
        "settings.desktop.categoryLayout.description"},
    {SettingsPage::Desktop, "desktop.categories",
        "settings.desktop.categories",
        "settings.desktop.categories.description"},
    {SettingsPage::Desktop, "desktop.category.add",
        "app.settings.add_category",
        "settings.desktop.categories.description"},
    {SettingsPage::DockAndTaskbar, "dock.enable", "settings.dock.enable",
        "settings.dock.enable.description"},
    {SettingsPage::DockAndTaskbar, "dock.position",
        "settings.dock.position", "settings.dock.position.description"},
    {SettingsPage::DockAndTaskbar, "dock.layout", "settings.dock.layout",
        "settings.dock.layout.description"},
    {SettingsPage::DockAndTaskbar, "dock.monitor", "settings.dock.monitor",
        "settings.dock.monitor.description"},
    {SettingsPage::DockAndTaskbar, "dock.thickness",
        "settings.dock.thickness", "settings.dock.thickness.description"},
    {SettingsPage::DockAndTaskbar, "dock.showFrequentItems",
        "settings.dock.frequentItems",
        "settings.dock.frequentItems.description"},
    {SettingsPage::DockAndTaskbar, "dock.floatingEdgeSwipe",
        "app.dock.floating_edge_swipe",
        "settings.dock.items.description"},
    {SettingsPage::DockAndTaskbar, "dock.showWindowsButton",
        "app.dock.show_windows_button",
        "settings.dock.items.description"},
    {SettingsPage::DockAndTaskbar, "dock.frequentItemCount",
        "app.settings.show_count",
        "settings.dock.frequentItems.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.autoHide",
        "settings.taskbar.autoHide",
        "settings.taskbar.autoHide.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.alignment",
        "settings.taskbar.alignment",
        "settings.taskbar.alignment.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.theme",
        "settings.taskbar.theme", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.contentTheme",
        "app.settings.taskbar_foreground_color",
        "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.backgroundColor",
        "app.settings.bg_color", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.borderColor",
        "app.settings.border_color", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.backgroundOpacity",
        "app.settings.bg_opacity", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.borderOpacity",
        "app.settings.border_opacity", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.glass",
        "app.settings.glass_enabled", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.blurRadius",
        "app.settings.blur_radius", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.acrylic",
        "app.settings.acrylic_noise", "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.dynamic.visibleWindow",
        "app.settings.taskbar_dynamic_visible_window",
        "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.dynamic.maximizedWindow",
        "app.settings.taskbar_dynamic_maximized_window",
        "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.dynamic.shellUi",
        "app.settings.taskbar_dynamic_shell_ui",
        "settings.taskbar.theme.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.systemTheme",
        "app.settings.system_panel",
        "settings.taskbar.restartExplorer.description"},
    {SettingsPage::DockAndTaskbar, "taskbar.restartExplorer",
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
    winrt::event_token appWindowChangedToken{};
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
    bool integratedTitleBarActive = false;
    bool integratedTitleBarLayoutQueued = false;
    bool windowActive = false;
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

    [[nodiscard]] double TitleBarRasterizationScale() const noexcept
    {
        try
        {
            if (shell)
            {
                if (const mux::XamlRoot xamlRoot = shell->XamlRoot())
                {
                    const double scale = xamlRoot.RasterizationScale();
                    if (scale > 0.0)
                        return scale;
                }
            }
        }
        catch (...)
        {
        }

        const UINT dpi = window && IsWindow(window)
            ? GetDpiForWindow(window) : 96;
        return dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;
    }

    [[nodiscard]] bool ShouldUseIntegratedTitleBar() const noexcept
    {
        bool highContrast = false;
        const bool highContrastQuerySucceeded =
            QueryHighContrastEnabled(highContrast);
        bool customizationSupported = false;
        try
        {
            customizationSupported =
                muw::AppWindowTitleBar::IsCustomizationSupported();
        }
        catch (...)
        {
        }
        return ShouldUseIntegratedSettingsTitleBar({
            IsWindows11OrGreater(), highContrastQuerySucceeded,
            highContrast, customizationSupported});
    }

    void UpdateIntegratedTitleBarLayout() noexcept
    {
        integratedTitleBarLayoutQueued = false;
        if (!shell || shuttingDown)
            return;

        if (!integratedTitleBarActive || !appWindowTitleBar)
        {
            shell->SetIntegratedTitleBarLayout(false, 0, 0, 0, 1.0);
            return;
        }

        try
        {
            shell->SetIntegratedTitleBarLayout(true,
                appWindowTitleBar.Height(),
                appWindowTitleBar.LeftInset(),
                appWindowTitleBar.RightInset(),
                TitleBarRasterizationScale());
        }
        catch (...)
        {
            ResetIntegratedTitleBar();
        }
    }

    void QueueIntegratedTitleBarLayoutUpdate() noexcept
    {
        if (integratedTitleBarLayoutQueued || shuttingDown || !window ||
            !IsWindow(window) || !shell)
        {
            return;
        }
        if (PostMessageW(window, kRefreshIntegratedTitleBarMessage, 0, 0))
        {
            integratedTitleBarLayoutQueued = true;
            return;
        }
        UpdateIntegratedTitleBarLayout();
    }

    void ApplyIntegratedTitleBarTheme() noexcept
    {
        if (!integratedTitleBarActive || !appWindowTitleBar)
            return;

        try
        {
            using ColorReference = winrt::Windows::Foundation::IReference<
                winrt::Windows::UI::Color>;
            const auto transparent = winrt::box_value(
                winrt::Windows::UI::Color{0, 0, 0, 0}).as<ColorReference>();
            // Only the system caption-button background properties support
            // transparency here. Hover, pressed, and Close states remain
            // system-selected so their native affordances are preserved.
            appWindowTitleBar.ButtonBackgroundColor(transparent);
            appWindowTitleBar.ButtonInactiveBackgroundColor(transparent);
            appWindowTitleBar.PreferredTheme(darkTheme
                    ? muw::TitleBarTheme::Dark
                    : muw::TitleBarTheme::Light);
        }
        catch (...)
        {
            // A decoration failure leaves the system-selected button colors.
        }
    }

    void UpdateIntegratedTitleBarActivationVisual() noexcept
    {
        if (shell && !shuttingDown && OnOwnerThread())
            shell->SetIntegratedTitleBarWindowActive(windowActive);
    }

    void ResetIntegratedTitleBar() noexcept
    {
        const bool wasActive = integratedTitleBarActive;
        integratedTitleBarLayoutQueued = false;
        if (appWindow && appWindowChangedToken.value != 0)
        {
            try
            {
                appWindow.Changed(appWindowChangedToken);
            }
            catch (...)
            {
            }
        }
        appWindowChangedToken = {};
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
        integratedTitleBarActive = false;
        if (shell)
            shell->SetIntegratedTitleBarLayout(false, 0, 0, 0, 1.0);
        appWindowTitleBar = nullptr;
        appWindow = nullptr;
        if (wasActive)
            runtime.ResizeToClient();
    }

    void ConfigureIntegratedTitleBar() noexcept
    {
        ResetIntegratedTitleBar();
        if (!window || !IsWindow(window) || !shell ||
            !ShouldUseIntegratedTitleBar())
        {
            return;
        }

        try
        {
            const auto windowId =
                winrt::Microsoft::UI::GetWindowIdFromWindow(window);
            appWindow = muw::AppWindow::GetFromWindowId(windowId);
            if (!appWindow)
                return;
            appWindowTitleBar = appWindow.TitleBar();
            if (!appWindowTitleBar)
            {
                appWindow = nullptr;
                return;
            }

            appWindowTitleBar.ExtendsContentIntoTitleBar(true);
            if (!appWindowTitleBar.ExtendsContentIntoTitleBar())
            {
                ResetIntegratedTitleBar();
                return;
            }
            integratedTitleBarActive = true;
            runtime.ResizeToClient();
            ApplyIntegratedTitleBarTheme();
            appWindowChangedToken = appWindow.Changed(
                [this](const muw::AppWindow&,
                    const muw::AppWindowChangedEventArgs& args) {
                    if (args.DidSizeChange() || args.DidPresenterChange() ||
                        args.DidVisibilityChange())
                    {
                        QueueIntegratedTitleBarLayoutUpdate();
                    }
                });
            windowActive = GetForegroundWindow() == window;
            UpdateIntegratedTitleBarActivationVisual();
            UpdateIntegratedTitleBarLayout();
            QueueIntegratedTitleBarLayoutUpdate();
        }
        catch (...)
        {
            ResetIntegratedTitleBar();
        }
    }

    void ReconcileIntegratedTitleBar() noexcept
    {
        if (!ShouldUseIntegratedTitleBar())
        {
            ResetIntegratedTitleBar();
            return;
        }
        if (!integratedTitleBarActive || !appWindowTitleBar)
        {
            ConfigureIntegratedTitleBar();
            return;
        }
        ApplyIntegratedTitleBarTheme();
        UpdateIntegratedTitleBarActivationVisual();
        QueueIntegratedTitleBarLayoutUpdate();
    }

    void ApplyActualTheme(bool isDark) noexcept
    {
        if (shuttingDown || !OnOwnerThread())
            return;

        darkTheme = isDark;
        ApplySettingsWindowChrome(window, darkTheme);
        ApplyIntegratedTitleBarTheme();
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
        bool destructive = true)
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
        request.primaryButtonText = L("settings.dialog.confirm");
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
            input.staticSettings.reserve(std::size(kStaticSearchDefinitions));
            for (const auto& definition : kStaticSearchDefinitions)
            {
                StaticSettingSearchDescriptor descriptor;
                descriptor.page = definition.page;
                descriptor.focusId = definition.focusId;
                descriptor.label = L(definition.labelKey);
                descriptor.description = L(definition.descriptionKey);
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

        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak, expectedEpoch]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->snapshotQueued.store(false);
                        if (!state->alive.load() || !state->owner ||
                            state->owner->viewEpoch != expectedEpoch)
                        {
                            return;
                        }
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
        QueueSystemBackdropUpdate();
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
        const std::uint64_t expectedEpoch = viewEpoch;
        const std::weak_ptr<CallbackState> weak = callbacks;
        try
        {
            if (!callbacks->dispatcher.TryEnqueue(
                    [weak, expectedEpoch]() {
                        const auto state = weak.lock();
                        if (!state)
                            return;
                        state->flushQueued.store(false);
                        if (!state->alive.load() || !state->owner ||
                            state->owner->viewEpoch != expectedEpoch)
                        {
                            return;
                        }
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
        else
            RefreshLocalizedPresentation();
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
            return snapshot && snapshot->generation == generation &&
                snapshot->values.general.widgetDeveloperToolsEnabled ==
                    enabled;
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
                              WidgetsPageActions::ConfirmationCompletion done) {
            const auto state = weak.lock();
            if (!state || !state->alive.load() || !state->owner)
            {
                if (done)
                    done(false);
                return;
            }
            state->owner->ShowGenerationConfirmation(generation,
                std::move(title), std::move(message), std::move(done));
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
        RebuildSearchIndex();
        if (widgetsPageActive && widgetsPageBackend)
            (void)widgetsPageBackend->Refresh();
        if (backupDataPageActive && backupDataPageBackend)
            backupDataPageBackend->Refresh();
        std::wstring title = L("settings.shell.title");
        if (title.empty())
            title = options.windowTitle;
        if (window && IsWindow(window))
            SetWindowTextW(window, title.c_str());
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
        case kRefreshIntegratedTitleBarMessage:
            self->UpdateIntegratedTitleBarLayout();
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
        case WM_ACTIVATE:
            self->windowActive = LOWORD(wParam) != WA_INACTIVE;
            self->UpdateIntegratedTitleBarActivationVisual();
            break;
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
            self->QueueIntegratedTitleBarLayoutUpdate();
            break;
        }
        case WM_SIZE:
        case WM_DISPLAYCHANGE:
            self->QueueIntegratedTitleBarLayoutUpdate();
            break;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            ApplySettingsWindowChrome(hwnd, self->darkTheme);
            self->ReconcileIntegratedTitleBar();
            self->QueueSystemBackdropUpdate();
            break;
        case WM_NCDESTROY:
            self->systemBackdropUpdateQueued = false;
            self->integratedTitleBarLayoutQueued = false;
            self->ResetIntegratedTitleBar();
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
        !impl_->CreateHostWindow())
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
        impl_->ConfigureIntegratedTitleBar();
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
    impl_->ResetIntegratedTitleBar();
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

void SettingsWindowHost::ApplyLanguageChange()
{
    if (impl_->initialized && impl_->OnOwnerThread())
        impl_->RefreshLocalizedPresentation();
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
