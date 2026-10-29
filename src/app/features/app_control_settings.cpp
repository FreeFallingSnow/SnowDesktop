#include "app/app.h"
#include "app_control_runtime.h"
#include "settings/control_global_settings.h"
#include "settings/control_widget_settings.h"
#include "settings/control_native_widget_settings.h"
#include "settings/control_environment_settings.h"
#include "settings/control_icon_settings.h"
#include "settings/control_calendar_settings.h"
#include "settings/control_font_settings.h"
#include "settings/control_theme_settings.h"
#include "settings/control_runtime_settings.h"
#include "settings/control_package_settings.h"
#include "data/data_paths.h"
#include "theme/theme_library_settings.h"
#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_theme_selection.h"

snowdesktop::control::Value DesktopApp::HandleControlSettings(
    std::string_view method, const snowdesktop::control::Value& params)
{
    using namespace snowdesktop;
    using namespace snowdesktop::control;
    using namespace snowdesktop::control_settings;
    if (!settingsController_ || !settingsWindow_ || !applicationControlRuntime_)
        throw Error("not_ready", "Application settings services are unavailable");
    const auto& options = settingsWindow_->HostOptions();
    const auto current = settingsController_->Snapshot();
    if (!current || !current->initialized) throw Error("not_ready", "Settings controller is not initialized");
    if (method == "settings.catalog" && !params.Find("target"))
    {
        Fields(params, {});
        auto targets = Array();
        for (const auto* kind : {"global", "personalization", "dock", "navigation", "general", "category", "desktop",
                "pages", "page", "debug-environments", "debug-environment", "widget", "native-widget", "icon-appearance", "large-icon",
                "icon-library", "themes", "theme", "calendar", "calendar-event", "calendar-series", "fonts", "runtime-services",
                "packages", "package", "agent-skills"})
            targets.array.push_back(Object({{"kind", String(kind)}}));
        auto instances = Array();
        for (const auto& widget : widgets_)
            if (widget.type == DesktopWidgetType::LuaScript || widget.type == DesktopWidgetType::Collection ||
                widget.type == DesktopWidgetType::CollectionGroup || widget.type == DesktopWidgetType::FileGroup ||
                widget.type == DesktopWidgetType::FileCategories || widget.type == DesktopWidgetType::FolderMapping)
                instances.array.push_back(Object({{"target", TargetJson({widget.type == DesktopWidgetType::LuaScript ? "widget" : "native-widget", WideToUtf8(widget.id)})}, {"name", String(WideToUtf8(widget.title))}}));
        return Object({{"schemaVersion", Number(1)}, {"targets", std::move(targets)}, {"widgetInstances", std::move(instances)},
            {"revision", String(std::to_string(current->generation) + ":" + std::to_string(current->revision))},
            {"transport", String("same-application-control-endpoint")}});
    }
    const auto target = ParseTarget(params);
    const bool mutation = method == "settings.update" || method == "settings.reset" || method == "settings.action";
    if (mutation)
    {
        if (applicationControlRuntime_->privatePointer.Current().enabled || mouseDown_ || dragSession_.HasContext() ||
            widgetAction_ != WidgetAction::None || HasActiveContextMenuSession() || dragDropController_.IsTransportActive())
            throw Error("busy", "Finish native pointer, drag and menu interactions before changing settings");
        if (current->dirtyDomains != SettingsDomain::None || current->pendingCommitDomains != SettingsDomain::None || current->retryRequired)
            throw Error("busy", "Existing settings edits must finish before a direct operation");
        if (current->externalReplacementPending || exitRequested_ || reloading_ || layoutReload_.Pending())
            throw Error("not_ready", "Settings are being replaced or the application is restarting");
    }
    const auto fonts = [] {
        return app_fonts::List(std::filesystem::path(GetExecutableDirectoryPath()) / L"Assets", GetDataDirectoryPath());
    };
    if (IsGlobalSettingsTarget(target))
    {
        GlobalSettingsContext context{*settingsController_,
            [provider = options.languageCatalog](std::string_view code) {
                if (code == "system") return true;
                if (!provider) return false;
                const auto languages = provider();
                return std::any_of(languages.begin(), languages.end(), [&](const auto& language) { return language.first == code; });
            }, [fonts](const app_fonts::Selection& selection) {
                const auto choices = fonts();
                return std::any_of(choices.begin(), choices.end(), [&](const auto& choice) { return choice.selection == selection; });
            }};
        return HandleGlobalSettings(method, params, target, context);
    }
    if (target.kind == "widget")
    {
        const auto index = FindWidgetIndexById(Utf8ToWide(target.id));
        if (index >= widgets_.size() || widgets_[index].type != DesktopWidgetType::LuaScript)
            throw Error("invalid_target", "The target must be an existing declarative Lua component");
        if (!widgetSettingsService_) throw Error("settings_unavailable", "Component settings service is unavailable");
        const auto libraryIdentity = [](const themes::Library& library) {
            std::string detail;
            const auto encoded = themes::EncodeLibrary(library, detail);
            if (!detail.empty()) throw Error("theme_library_unavailable", detail);
            return Revision(String(encoded));
        };
        std::string expectedLibraryIdentity;
        WidgetThemeControl themeControl;
        themeControl.read = [&](std::wstring_view id) {
            themes::Library library; std::string detail;
            if (!themes::Load(themes::LibraryPath(), library, detail))
            {
                expectedLibraryIdentity.clear();
                return Object({{"available", Boolean(false)}, {"error", String("theme_library_unavailable")},
                    {"selectedThemeId", String("")}, {"choices", Array()}});
            }
            try { expectedLibraryIdentity = libraryIdentity(library); }
            catch (const Error&)
            {
                expectedLibraryIdentity.clear();
                return Object({{"available", Boolean(false)}, {"error", String("theme_library_unavailable")},
                    {"selectedThemeId", String("")}, {"choices", Array()}});
            }
            auto choices = Array();
            for (const auto& choice : themes::Choices(library, themes::Kind::Global, themes::Components))
                choices.array.push_back(Object({{"id", String(choice.id)}, {"name", String(choice.name)}}));
            const auto binding = library.references.find("widget/" + WideToUtf8(std::wstring(id)));
            return Object({{"available", Boolean(true)}, {"selectedThemeId", String(binding == library.references.end() ? "" : binding->second.id)},
                {"choices", std::move(choices)}, {"libraryContentIdentity", String(expectedLibraryIdentity)}});
        };
        themeControl.apply = [&](const widget_runtime::WidgetSettingMutationGuard& guard, std::string_view themeId, bool detach) {
            using namespace widget_runtime;
            if (expectedLibraryIdentity.empty())
                return WidgetSettingMutationResult{WidgetSettingMutationStatus::Unavailable, guard.generation,
                    guard.revision, "theme_library_unavailable", "Component themes are unavailable"};
            const auto snapshot = widgetSettingsService_->Snapshot(guard.widgetId);
            if (!snapshot || !guard.Matches(*snapshot))
                return WidgetSettingMutationResult{WidgetSettingMutationStatus::StaleSnapshot, guard.generation,
                    guard.revision, "stale_settings", "Component settings changed before theme selection"};
            const auto bindingTarget = "widget/" + WideToUtf8(guard.widgetId);
            const auto currentLibrary = [&](const themes::Library& library, std::string& detail) {
                if (libraryIdentity(library) == expectedLibraryIdentity) return true;
                detail = "stale_settings"; return false;
            };
            themes::Library library; std::string detail;
            if (detach)
            {
                bool changed = false;
                const auto saved = themes::Transact(themes::LibraryPath(), [&](themes::Library& value, std::string& error) {
                    if (!currentLibrary(value, error)) return false;
                    const auto latest = widgetSettingsService_->Snapshot(guard.widgetId);
                    if (!latest || !guard.Matches(*latest)) { error = "stale_settings"; return false; }
                    changed = value.references.contains(bindingTarget);
                    themes::Detach(value, bindingTarget);
                    return true;
                }, library, detail);
                return WidgetSettingMutationResult{saved ? (changed ? WidgetSettingMutationStatus::Applied : WidgetSettingMutationStatus::Unchanged)
                    : WidgetSettingMutationStatus::PersistenceFailed, guard.generation, guard.revision, saved ? "" : detail,
                    saved ? "" : "Component theme binding could not be saved"};
            }
            auto mutation = WidgetSettingMutationResult{WidgetSettingMutationStatus::Unchanged, guard.generation, guard.revision};
            auto restored = mutation;
            const auto selected = themes::detail::SelectWidgetTheme(themes::LibraryPath(), bindingTarget, std::string(themeId),
                [&](const WidgetHostAppearancePatch& patch) {
                    mutation = widgetSettingsService_->UpdateHostAppearance(guard, patch);
                    return themes::detail::WidgetThemeMutation{mutation.Succeeded(), mutation.Changed()};
                }, [&] {
                    restored = widgetSettingsService_->UpdateHostAppearance(
                        {guard.widgetId, mutation.generation, mutation.revision},
                        themes::detail::WidgetAppearanceSnapshotPatch(snapshot->hostAppearance));
                    return restored.Succeeded();
                }, library, detail, currentLibrary);
            if (selected.committed || (!selected.mutationSucceeded && detail == "widgetApplyFailed")) return mutation;
            const bool rollbackFailed = selected.rollbackAttempted && !selected.rollbackSucceeded;
            const auto& final = selected.rollbackAttempted ? restored : mutation;
            return WidgetSettingMutationResult{WidgetSettingMutationStatus::PersistenceFailed, final.generation,
                final.revision, rollbackFailed ? "themeSelectionRollbackFailed" : detail,
                rollbackFailed ? "Component appearance rollback failed after theme-library persistence failure"
                    : "Component theme selection could not be saved"};
        };
        return HandleWidgetSettings(method, params, target, *widgetSettingsService_, options.ensureWidgetSettingsInstance, themeControl);
    }
    if (target.kind == "native-widget")
        return HandleNativeWidgetSettings(method, params, target, {
            [this](std::string_view id) { return CaptureNativeWidgetSettings(Utf8ToWide(std::string(id))); },
            [this](const NativeWidgetSettingsState& expected, const NativeWidgetSettingsPatch& patch) {
                return ApplyNativeWidgetSettings(expected, patch);
            }, [this](std::string_view id, const std::string& query) {
                return NativeWidgetSearchAction(Utf8ToWide(std::string(id)), Utf8ToWide(query));
            }});
    if (IsEnvironmentSettingsTarget(target))
        return HandleEnvironmentSettings(method, params, target,
            {*settingsController_, options.pageLayoutPage, options.homeAboutStatus, options.debugVisible});
    if (IsRuntimeSettingsTarget(target))
        return HandleRuntimeSettings(method, params, target, {*settingsController_, options.windowSupportStatus,
            options.configureWindowSupport, options.advancedFeatureStatus, options.registerAdvancedFeatures,
            options.resetAdvancedFeatures, options.homeAboutStatus, [this] { return hwnd_; }, options.debugVisible});
    if (IsPackageSettingsTarget(target))
    {
        if (!widgetEngine_) throw Error("settings_unavailable", "Component package service is unavailable");
        auto& runtime = *applicationControlRuntime_;
        if (!runtime.controlPackages)
        {
            if (!runtime.settingsOwnerTasks) runtime.settingsOwnerTasks = std::make_unique<settings_ipc::Channel>();
            auto configured = options.widgetsPage;
            configured.localize = options.localize;
            configured.dispatchToOwner = runtime.settingsOwnerTasks->Poster();
            configured.snapshotChanged = {};
            configured.diagnosticsVisible = [] { return false; };
            runtime.controlPackages = options.createWidgetsBackend
                ? options.createWidgetsBackend(std::move(configured))
                : std::make_unique<winui::WidgetsPageBackend>(*widgetEngine_, std::move(configured));
        }
        if (!runtime.controlPackages || (!runtime.controlPackages->IsGenerationCurrent(current->generation) &&
            !runtime.controlPackages->Activate(current->generation, false)))
            throw Error("settings_unavailable", "Component package settings session could not start");
        if (!runtime.controlPackages->Refresh()) throw Error("settings_unavailable", "Component package state could not be refreshed");
        return HandlePackageSettings(method, params, target, {*runtime.controlPackages, current->generation});
    }
    if (IsIconSettingsTarget(target))
    {
        IconSettingsContext context{options.iconAppearance, options.iconLibrary, options.largeIconSettings,
            applicationControlRuntime_->controlIcons, [this] { return settingsWindow_ && settingsWindow_->IsVisible(); },
            [this](std::wstring_view key) {
                const auto index = FindItemIndexByKey(std::wstring(key));
                return index < items_.size() && SetLargeIconConfig(index, std::nullopt);
            }};
        return HandleIconSettings(method, params, target, context);
    }
    if (IsFontSettingsTarget(target))
        return HandleFontSettings(method, params, target, {*settingsController_, fonts,
            [](const auto& path, auto& choices, auto& error) { return app_fonts::Import(path, GetDataDirectoryPath(), choices, error); }, options.appliedFont});
    if (IsThemeSettingsTarget(target))
        return HandleThemeSettings(method, params, target, {*settingsController_, themes::LibraryPath(), [action = options.iconAppearance] {
            if (!action) return false;
            IconAppearanceRequest request; request.action = "themes";
            return action(std::move(request)).succeeded;
        }, [this, ensure = options.ensureWidgetSettingsInstance](std::string_view id, const themes::Theme& previous, const themes::Theme& next) {
            if (!widgetSettingsService_) return false;
            const auto widgetId = Utf8ToWide(std::string(id));
            const auto index = FindWidgetIndexById(widgetId);
            if (index >= widgets_.size() || widgets_[index].type != DesktopWidgetType::LuaScript || (ensure && !ensure(widgetId))) return false;
            auto snapshot = widgetSettingsService_->Snapshot(widgetId);
            if (!snapshot) { (void)widgetSettingsService_->Load(widgetId); snapshot = widgetSettingsService_->Snapshot(widgetId); }
            if (!snapshot || !themes::WidgetMatches(snapshot->hostAppearance, previous)) return false;
            return widgetSettingsService_->UpdateHostAppearance(widget_runtime::WidgetSettingMutationGuard::FromSnapshot(*snapshot), themes::WidgetPatch(next)).Succeeded();
        }});
    if (IsCalendarSettingsTarget(target))
    {
        if (!widgetEngine_) throw Error("settings_unavailable", "Calendar service is unavailable");
        winui::CalendarPageActions calendarActions;
        const auto generationCurrent = [this](std::uint64_t generation) {
            const auto snapshot = settingsController_->Snapshot();
            return snapshot && snapshot->initialized && !snapshot->externalReplacementPending && snapshot->generation == generation;
        };
        calendarActions.events = [this, generationCurrent](std::uint64_t generation) -> std::optional<std::vector<calendar::CalendarEvent>> {
            return generationCurrent(generation) ? std::optional{widgetEngine_->RuntimeCalendarSingleEvents()} : std::nullopt;
        };
        calendarActions.series = [this, generationCurrent](std::uint64_t generation) -> std::optional<std::vector<calendar::CalendarSeries>> {
            return generationCurrent(generation) ? std::optional{widgetEngine_->RuntimeCalendarSeries()} : std::nullopt;
        };
        calendarActions.occurrence = [this, generationCurrent](std::uint64_t generation, std::string id) {
            return generationCurrent(generation) ? widgetEngine_->RuntimeCalendarEventById(id) : std::optional<calendar::CalendarEvent>{};
        };
        calendarActions.mutate = [this, generationCurrent](std::uint64_t generation, calendar::CalendarEvent event, bool remove) -> calendar::MutationResult {
            if (!generationCurrent(generation)) return {false, event.id, 0, "conflict"};
            if (remove) return widgetEngine_->RuntimeCalendarRemove(event.id, event.revision);
            if (event.id.empty()) return widgetEngine_->RuntimeCalendarCreate(std::move(event));
            const auto id = event.id; const auto revision = event.revision;
            return widgetEngine_->RuntimeCalendarUpdate(id, revision, std::move(event));
        };
        calendarActions.mutateSeries = [this, generationCurrent](std::uint64_t generation, calendar::CalendarSeries series, bool remove) -> calendar::MutationResult {
            if (!generationCurrent(generation)) return {false, series.id, 0, "conflict"};
            if (remove) return widgetEngine_->RuntimeCalendarSeriesRemove(series.id, series.revision);
            if (series.id.empty()) return widgetEngine_->RuntimeCalendarSeriesCreate(std::move(series));
            const auto id = series.id; const auto revision = series.revision;
            return widgetEngine_->RuntimeCalendarSeriesUpdate(id, revision, std::move(series));
        };
        return HandleCalendarSettings(method, params, target, {*settingsController_, std::move(calendarActions)});
    }
    throw Error("invalid_target", "Settings target kind is not supported");
}
