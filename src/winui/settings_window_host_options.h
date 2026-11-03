#pragma once

#include "windows_compat.h"
#include "shell/shell_extension_service.h"
#include "icons/large_icon_settings.h"
#include "icons/icon_library.h"
#include "icons/icon_appearance.h"
#include "settings/settings_search_index.h"
#include "backup_data_page_backend.h"
#include "general_page_types.h"
#include "general_advanced_feature_status.h"
#include "platform/window_support/window_capabilities.h"
#include "ui/render/app_font_types.h"
#include "home_about_page_model.h"
#include "runtime_modules_query.h"
#include "page_layout_page_actions.h"
#include "calendar_page_actions.h"
#include "widgets_page_backend.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::winui
{

/** Runtime dependencies supplied by DesktopApp to the WinUI settings host. */
struct SettingsWindowHostOptions
{
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;
    using LanguageCatalogProvider = std::function<
        std::vector<std::pair<std::string, std::wstring>>() >;
    using SearchInputProvider =
        std::function<SettingsSearchIndexInput()>;
    using HomeAboutStatusProvider = std::function<HomeAboutStatusPatch(
        std::uint64_t generation,
        std::uint64_t revision)>;

    std::function<shell_extensions::CatalogueView(const shell_extensions::Request&, bool)> contextMenu;
    std::function<void()> contextMenuClosed;
    LocalizeCallback localize;
    LanguageCatalogProvider languageCatalog;
    SearchInputProvider searchInput;
    HomeAboutStatusProvider homeAboutStatus;
    RuntimeModulesQuery runtimeModulesStatus;
    /** Runtime-only ownership warnings for the Windows auto-start setting. */
    std::function<GeneralStartupConflict()> startupConflict;
    std::function<window_support::SupportStatus()> windowSupportStatus;
    std::function<void(bool, HWND)> configureWindowSupport;
    /** Font actually active in the desktop host, independent of a settings child restart. */
    std::function<app_fonts::Selection()> appliedFont;
    std::function<GeneralAdvancedFeatureStatus()> advancedFeatureStatus;
    std::function<void()> registerAdvancedFeatures;
    std::function<bool()> resetAdvancedFeatures;

    /** Ensure a persisted component instance is loaded before its declarative
     * settings session is created. The application owns the instance-to-
     * package lookup needed by WidgetEngine::EnsureWidgetLoaded. */
    std::function<bool(std::wstring_view instanceId)>
        ensureWidgetSettingsInstance;

    /** Application-owned seams used by the page backends. UI ownership,
     * dispatch and snapshot publication are supplied by SettingsWindowHost. */
    WidgetsPageBackendOptions widgetsPage;
    BackupDataPageBackendOptions backupDataPage;
    PageLayoutPageActions pageLayoutPage;
    CalendarPageActions calendarPage;
    LargeIconSettingsAction largeIconSettings;
    IconLibraryAction iconLibrary;
    IconAppearanceAction iconAppearance;

    // The settings child supplies IPC adapters; the application supplies the
    // concrete backends. UI callbacks in the configured options stay local.
    std::function<std::unique_ptr<IWidgetsPageBackend>(WidgetsPageBackendOptions)>
        createWidgetsBackend;
    std::function<std::unique_ptr<IBackupDataPageBackend>(BackupDataPageBackendOptions)>
        createBackupBackend;
    std::function<void()> sessionClosed;

    /** Reconcile host-owned system state after a persisted-state reload. */
    std::function<void()> refreshExternalState;
    /** Parent IPC can defer the acknowledgement while system queries run on
     * a worker. The child still waits before showing authoritative state. */
    std::function<void(std::function<void()>)> refreshExternalStateAsync;
    std::function<bool()> developerToolsVisible;
    std::function<bool()> debugVisible;

    std::wstring windowTitle = L"SnowDesktop Settings";
};

} // namespace snowdesktop::winui
