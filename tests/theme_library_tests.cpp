#include "theme_library_settings.h"
#include "json_value.h"
#include "winui/theme_edit_state.h"
#include "app/quick_navigation_theme.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>

// Categories are not read or changed by theme application. Keep that unrelated
// SettingsValues default outside this fixture; all four appearance domains,
// codecs and file transactions below use their production implementations.
CategorySettings CategorySettings::Defaults() { return {}; }

// Exercises production codecs, transactions, selection and host appearance
// patches. Failures protect user data and dependency identity, not UI layout.
int RunThemeLibraryTests()
{
    using namespace snowdesktop;
    using namespace themes;
    int failures = 0;
    const auto fileNameCheck = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::cerr << "FAIL filename: " << message << '\n'; }
    };
    fileNameCheck(ExportFileName(L"雪夜主题") == L"雪夜主题.snowtheme", "Unicode names remain intact");
    fileNameCheck(ExportFileName(L"天气:<夜>/\\|?*\".") == L"天气__夜_______.snowtheme", "reserved filename characters are sanitized");
    fileNameCheck(ExportFileName(L" \t. ") == L"_.snowtheme", "control characters are sanitized before trailing whitespace is removed");
    fileNameCheck(ExportFileName(L" . ") == L"theme.snowtheme", "only an empty sanitized name uses the fallback");
    fileNameCheck(ExportFileName(L"NUL.txt") == L"_NUL.txt.snowtheme" && ExportFileName(L"com¹") == L"_com¹.snowtheme",
        "reserved devices remain safe even with extensions and superscript digits");
    fileNameCheck(ExportFileName(L"测试.SNOWTHEME.snowtheme. ") == L"测试.snowtheme", "the extension is not repeated");
    fileNameCheck(ExportFileName(std::wstring(150, L'雪')).size() == 130, "suggested names have a bounded Unicode length");
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::cerr << "FAIL theme library: " << message << '\n'; }
    };
    {
        using namespace snowdesktop::winui::theme_controls;
        const auto temporary = std::filesystem::temp_directory_path();
        const auto requestDirectory = TaskDirectory(temporary, CreateId());
        check(std::filesystem::equivalent(requestDirectory.parent_path(), temporary) &&
            std::filesystem::relative(requestDirectory, temporary) == requestDirectory.filename() &&
            !std::filesystem::exists(requestDirectory) && requestDirectory.filename().wstring().find(L"/") == std::wstring::npos &&
            TaskDirectory(temporary, "").empty(), "production UI request directory uses one fresh leaf despite the slash in theme IDs");
        Theme partial; partial.id = "theme/partial"; partial.name = "Two bars"; partial.scopes = Dock | Taskbar;
        check(MatchesFilter(partial, FilterTab::All) && MatchesFilter(partial, FilterTab::Dock) &&
            MatchesFilter(partial, FilterTab::Taskbar) && !MatchesFilter(partial, FilterTab::StatusBar) && !MatchesFilter(partial, FilterTab::Global),
            "single tabs match each applicable base scope rather than exact scope combinations");
        auto full = partial; full.scopes = Bars;
        check(MatchesFilter(full, FilterTab::Global) && MatchesFilter(full, FilterTab::Dock), "full themes appear in global and each applicable bar tab");
        auto child = partial; child.kind = Kind::QuickPanel;
        check(MatchesFilter(child, FilterTab::QuickPanel) && !MatchesFilter(child, FilterTab::Dock), "child kind cannot inherit bar tags from scope bits");
        Library sourceLibrary; sourceLibrary.themes.emplace(partial.id, partial);
        sourceLibrary.references["dock"] = {partial.id, Kind::Global, Dock, {{partial.id, partial}}};
        EditSource source; source.Begin(sourceLibrary, "dock");
        check(source.CanUpdate(sourceLibrary) && source.theme->id == partial.id, "edit source retains the successful applied ID");
        Detach(sourceLibrary, "dock");
        check(source.CanUpdate(sourceLibrary) && source.theme->name == "Two bars", "detaching an edit draft does not lose its source");
        sourceLibrary.workshop["123"].ids.insert(partial.id);
        check(!source.CanUpdate(sourceLibrary), "installed workshop sources only permit new copies");
        sourceLibrary.workshop.clear(); sourceLibrary.themes.clear();
        check(!source.CanUpdate(sourceLibrary) && source.theme->name == "Two bars", "source removal preserves the draft but blocks overwrite");
        source.Begin(sourceLibrary, "popup");
        check(!source.theme, "switching to a different or unsaved source clears the old update target");
    }
    std::string error, savedId;
    check(ErrorLocalizationKey("writeFailed") == "themeLibrary.error.writeFailed" &&
        ErrorLocalizationKey("unexpected") == "themeLibrary.error.invalidPackage",
        "known and unexpected failures resolve complete localized feedback keys");
    Library library;
    Theme quick = Capture(Kind::QuickPanel, MakeQuickNavigationAppearancePreset(kAppearancePresetLight));
    quick.id = "theme/quick"; quick.name = "Panel"; quick.layout.fontSize = 19; quick.colors["searchText"] = "#123456";
    Theme popup = Capture(Kind::Popup, MakeCollectionPopupAppearancePreset(kAppearancePresetAcrylicDark));
    popup.id = "theme/popup"; popup.name = "Popup";
    Theme global = Capture(Kind::Global, MakeAppearancePreset(kAppearancePresetGlassLight));
    global.name = "Same name"; global.quickPanel = quick.id; global.popup = popup.id;
    Package children{{quick.id, quick}, {popup.id, popup}};
    check(Save(library, global, children, false, savedId, error, [] { return "theme/global"; }), "parent and unsaved children save together");
    check(savedId == "theme/global" && library.themes.size() == 3, "save-as has a fresh stable ID");
    global = library.themes.at(savedId);
    check(Save(library, global, {}, false, savedId, error, [] { return "theme/global-copy"; }) && savedId != global.id,
        "equal display names never serve as identity");
    const auto beforeUpdate = EncodeLibrary(library, error);
    auto changed = global; changed.quickPanel = "follow-global";
    check(!Save(library, changed, {}, true, savedId, error) && EncodeLibrary(library, error) == beforeUpdate,
        "cyclic or missing bindings cannot partially update the library");
    changed = global; changed.name = "Renamed"; changed.appearance.widgetAlpha = .61f;
    check(Save(library, changed, {}, true, savedId, error) && savedId == global.id, "explicit update keeps ID after renaming");
    check(Select(library, "dock", global.id, Kind::Global, Scope::Dock, error), "select scoped theme by ID");
    const auto savedSnapshot = EncodePackage(library.references.at("dock").snapshot, error);
    changed.scopes = Components;
    check(!Save(library, changed, {}, true, savedId, error) && error == "themeInUse", "shrinking scopes checks object references");
    check(Save(library, changed, {}, true, savedId, error, CreateId, true) && library.references.at("dock").id.empty() &&
        EncodePackage(library.references.at("dock").snapshot, error) == savedSnapshot,
        "explicit scope reduction converts excluded object references to custom without changing their snapshots");
    check(References(library, quick.id).size() == 2, "binding references are enumerated separately from object use");
    check(Select(library, "global", "theme/global-copy", Kind::Global, Components, error), "select a global with the bound custom child");
    const auto boundGlobalSnapshot = EncodePackage(library.references.at("global").snapshot, error);
    const auto beforeDelete = EncodeLibrary(library, error);
    check(!Remove(library, quick.id, {}, true, error) && EncodeLibrary(library, error) == beforeDelete,
        "deleting a bound child is blocked atomically");
    check(Remove(library, quick.id, "builtin/quickpanel/light", true, error), "explicit replacement repairs every global binding");
    check(library.themes.at(global.id).quickPanel == "builtin/quickpanel/light", "saved global references are rewired by ID");
    check(library.references.at("global").id.empty() &&
        EncodePackage(library.references.at("global").snapshot, error) == boundGlobalSnapshot,
        "deleting a bound child preserves the active global appearance as custom with complete dependencies");
    check(Remove(library, global.id, {}, true, error) && library.references.at("dock").id.empty() &&
        EncodePackage(library.references.at("dock").snapshot, error) == savedSnapshot,
        "deleting an object-used theme retains its successful snapshot as custom");
    const auto serialized = EncodeLibrary(library, error);
    Library loaded;
    check(DecodeLibrary(serialized, loaded, error) && EncodeLibrary(loaded, error) == serialized, "complete snapshot configuration round-trip");
    Package package;
    check(Export(library, "theme/global-copy", package, error) && package.size() == 2 && !package.contains("builtin/quickpanel/light"),
        "export embeds custom dependencies and references builtins by ID");
    auto packageText = EncodePackage(package, error);
    JsonValue json;
    check(ParseJson(packageText, json) && json.Find("global")->IsArray() && json.Find("quickPanel")->IsArray() && json.Find("popup")->IsArray(),
        "all three package sections are present");
    Package decoded;
    check(DecodePackage(packageText, decoded, error) && EncodePackage(decoded, error) == packageText, "package round-trip");
    Library destination;
    std::map<std::string, std::string> mapping;
    check(Import(destination, package, mapping, error) && destination.themes.size() == 2 && destination.references.empty(),
        "installing a package does not select or apply it");
    check(Import(destination, package, mapping, error, [] { return "theme/unused"; }) && destination.themes.size() == 2,
        "identical IDs and values reuse existing themes");
    destination.themes.at(popup.id).appearance.widgetAlpha = .99f;
    int nextId = 0;
    check(Import(destination, package, mapping, error, [&] { return "theme/conflict-" + std::to_string(++nextId); }),
        "different contents under one ID create copies");
    check(mapping.at(popup.id) == "theme/conflict-1" && mapping.at("theme/global-copy") == "theme/conflict-2" &&
        destination.themes.at("theme/conflict-2").popup == "theme/conflict-1", "conflicts remap global dependency IDs too");
    Package broken = package; broken.erase(popup.id);
    const auto beforeImport = EncodeLibrary(destination, error);
    check(!Import(destination, broken, mapping, error) && EncodeLibrary(destination, error) == beforeImport,
        "missing dependency rolls back the entire install");
    check(!DecodePackage("{\"format\":\"snowdesktop.theme\",\"version\":1,\"global\":[],\"quickPanel\":[],\"popup\":[]}", decoded, error), "all-empty package is rejected");
    auto unsupported = packageText; unsupported.replace(unsupported.find("\"version\":1"), 11, "\"version\":2");
    check(!DecodePackage(unsupported, decoded, error) && error == "unsupportedVersion", "unsupported independent format version is rejected");
    check(!DecodePackage("{\"format\":\"snowdesktop.theme\",\"version\":1,\"global\":[],\"popup\":[]}", decoded, error), "missing section is rejected");
    check(!DecodePackage(std::string("\xff", 1), decoded, error), "invalid UTF-8 is rejected before parsing");
    auto personalData = packageText; personalData.insert(personalData.find("\"appearance\":"), "\"hotkey\":32,");
    check(!DecodePackage(personalData, decoded, error), "packages cannot smuggle personal settings into themes");
    auto malformedBuiltin = "{\"format\":\"snowdesktop.theme\",\"version\":1,\"global\":[],\"quickPanel\":[{\"id\":\"builtin/quickpanel/dark\",\"name\":\"Override\"}],\"popup\":[]}";
    check(!DecodePackage(malformedBuiltin, decoded, error), "builtin identities cannot carry altered contents");
    check(Classify(L"a.SNOWTHEME") == PackageType::Theme && Classify(L"a.snowwidget") == PackageType::Widget &&
        Classify(L"a.json") == PackageType::Unsupported, "suffix chooses a strict parser and unknown suffixes are refused");

    SettingsValues settings;
    settings.personalization.barHeight = 39; settings.personalization.popupHoverOpen = true;
    settings.dock.position = DockPosition::Left; settings.dock.systemTaskbarBackdropEnabled = false;
    settings.dock.systemTaskbarVisibleWindow.enabled = true;
    settings.navigation.virtualKey = 'Q'; settings.navigation.prefixes[0] = "program";
    settings.navigation.defaultEngine = "google"; settings.navigation.lastCollapsed = true;
    settings.general.collectionPopupAppearance.mode = 4;
    settings.general.collectionPopupAppearance.appearance.widgetAlpha = .41f;
    auto integration = Library{};
    global.id = "theme/main"; global.quickPanel = quick.id; global.popup = popup.id;
    integration.themes = children; integration.themes.emplace(global.id, global);
    const auto personal = settings.navigation;
    check(ApplyTarget(integration, "global", global.id, settings, error), "global production selection resolves dependency snapshots");
    check(settings.navigation.layout.fontSize == 19 && settings.navigation.colors.at("searchText") == "#123456" &&
        settings.navigation.virtualKey == personal.virtualKey && settings.navigation.prefixes == personal.prefixes &&
        settings.navigation.engines == personal.engines && settings.navigation.defaultEngine == personal.defaultEngine &&
        settings.navigation.lastCollapsed == personal.lastCollapsed, "quick-panel theme copies layout and colors while preserving personal preferences");
    check(settings.personalization.barHeight == 39 && settings.personalization.popupHoverOpen && settings.dock.position == DockPosition::Left &&
        !settings.dock.systemTaskbarBackdropEnabled && settings.dock.systemTaskbarVisibleWindow.enabled,
        "global theme excludes positions, enablement and dynamic rules");
    check(settings.general.collectionPopupAppearance.appearance.widgetAlpha == .41f &&
        ResolveSurfaceTheme(settings.general.quickNavigationAppearance, settings.personalization, 0, true,
            &settings.general.globalQuickNavigationAppearance).contentTheme == 1,
        "only follow-global surfaces resolve global bindings");
    const auto existingGlobal = settings.personalization;
    {
        for (unsigned scopes : {unsigned(Dock), unsigned(StatusBar), unsigned(Taskbar), unsigned(Dock | StatusBar),
            unsigned(Dock | Taskbar), unsigned(StatusBar | Taskbar)})
        {
            auto partial = Capture(Kind::Global, MakeAppearancePreset(kAppearancePresetAcrylicLight));
            partial.id = "theme/partial-" + std::to_string(scopes); partial.name = "雪夜主题"; partial.scopes = scopes;
            Library scoped; scoped.themes.emplace(partial.id, partial);
            auto values = settings;
            const auto before = values;
            check(!FullScope(scopes) && Validate(scoped.themes, error), "every partial bar combination saves without child bindings");
            check(ApplyTarget(scoped, "global", partial.id, values, error), "partial selection applies its explicit bar scopes");
            check(values.personalization == before.personalization && values.navigation == before.navigation &&
                values.general.quickNavigationAppearance == before.general.quickNavigationAppearance &&
                values.general.collectionPopupAppearance == before.general.collectionPopupAppearance &&
                values.general.globalQuickNavigationAppearance == before.general.globalQuickNavigationAppearance &&
                values.general.globalCollectionPopupAppearance == before.general.globalCollectionPopupAppearance,
                "partial selection preserves the global source and all unrelated surface themes");
            check((scopes & Dock) || (values.dock.followComponentAppearance == before.dock.followComponentAppearance &&
                values.dock.customAppearance == before.dock.customAppearance), "excluded Dock remains unchanged");
            check((scopes & StatusBar) || values.general.statusBar == before.general.statusBar, "excluded status bar remains unchanged");
            check((scopes & Taskbar) || (values.dock.systemTaskbarAppearance == before.dock.systemTaskbarAppearance &&
                values.dock.systemTaskbarFollowPersonalization == before.dock.systemTaskbarFollowPersonalization &&
                values.dock.systemTaskbarContentTheme == before.dock.systemTaskbarContentTheme), "excluded taskbar remains unchanged");
            check(values.dock.position == before.dock.position && values.dock.systemTaskbarBackdropEnabled == before.dock.systemTaskbarBackdropEnabled &&
                values.dock.systemTaskbarVisibleWindow == before.dock.systemTaskbarVisibleWindow,
                "partial selection preserves placement, enablement and dynamic rules");
            Package exported, reloaded; Library installed;
            check(Export(scoped, partial.id, exported, error) && exported.size() == 1 &&
                DecodePackage(EncodePackage(exported, error), reloaded, error) &&
                Import(installed, reloaded, mapping, error) && installed.themes.at(partial.id).name == partial.name &&
                installed.themes.at(partial.id).scopes == scopes && installed.themes.at(partial.id).quickPanel.empty() &&
                installed.themes.at(partial.id).popup.empty(), "partial package round-trip preserves the name, scopes and empty bindings");
            check(DecodeLibrary(EncodeLibrary(scoped, error), installed, error), "partial library reload succeeds");
            partial.scopes = Bars; scoped.themes.at(partial.id) = partial;
            check(FullScope(partial.scopes) && !Validate(scoped.themes, error), "switching back to full applicability requires both bindings");
            partial.quickPanel = "builtin/quickpanel/dark"; partial.popup = "builtin/popup/dark";
            scoped.themes.at(partial.id) = partial;
            check(Validate(scoped.themes, error), "full applicability accepts explicit bindings");
            partial.scopes = scopes; partial.quickPanel = "theme/missing"; scoped.themes.at(partial.id) = partial;
            check(!Validate(scoped.themes, error), "nonempty invalid bindings cannot be smuggled into a partial package");
        }
    }
    {
        auto bars = global;
        bars.id = "theme/three-bars"; bars.scopes = Dock | StatusBar | Taskbar;
        auto scoped = integration; scoped.themes.emplace(bars.id, bars);
        auto values = settings;
        check(ApplyTarget(scoped, "global", bars.id, values, error) &&
            Select(scoped, "global", bars.id, Kind::Global, bars.scopes, error),
            "a three-bar global is selectable as the global source without a legacy component scope");
        const auto globalChoices = Choices(scoped, Kind::Global, 0);
        const auto componentChoices = Choices(scoped, Kind::Global, Components);
        check(std::any_of(globalChoices.begin(), globalChoices.end(), [&](const auto& theme) { return theme.id == bars.id; }) &&
            std::none_of(componentChoices.begin(), componentChoices.end(),
                [&](const auto& theme) { return theme.id == bars.id; }),
            "global source choices include new bar themes while component choices remain scoped");
        for (auto target : {"dock", "statusBar", "taskbar"})
            check(ApplyTarget(scoped, target, bars.id, values, error), "each of the three saved bar scopes accepts its global theme");
        const auto bytes = EncodeLibrary(scoped, error); Library roundTrip;
        check(DecodeLibrary(bytes, roundTrip, error) && EncodeLibrary(roundTrip, error) == bytes,
            "three-bar global selection uses the unchanged nonzero reference schema");
        auto dockOnly = bars; dockOnly.scopes = Dock;
        scoped.themes.at(bars.id) = dockOnly;
        const auto original = values.dock;
        check(!ApplyTarget(scoped, "taskbar", bars.id, values, error) && values.dock == original,
            "bar entry rejects a theme outside its scope without changing personal data");
    }
    check(ApplyTarget(integration, "dock", global.id, settings, error) && settings.personalization == existingGlobal &&
        !settings.dock.followComponentAppearance, "selecting a theme for one object affects only that object");
    {
        SettingsValues scenarios = settings;
        scenarios.general.statusBar.noWindow.theme = {4, true, MakeAppearancePreset(kAppearancePresetCustom)};
        scenarios.general.statusBar.maximizedWindow.theme = scenarios.general.statusBar.noWindow.theme;
        scenarios.general.statusBar.noWindow.theme.appearance.widgetAlpha = .11f;
        scenarios.general.statusBar.maximizedWindow.theme.appearance.widgetAlpha = .22f;
        scenarios.dock.systemTaskbarShellUi.themeMode = SystemTaskbarThemeMode::Custom;
        scenarios.dock.systemTaskbarMaximizedWindow.themeMode = SystemTaskbarThemeMode::Custom;
        scenarios.dock.systemTaskbarVisibleWindow.themeMode = SystemTaskbarThemeMode::Custom;
        scenarios.dock.systemTaskbarShellUi.appearance.widgetAlpha = .33f;
        scenarios.dock.systemTaskbarMaximizedWindow.appearance.widgetAlpha = .44f;
        scenarios.dock.systemTaskbarVisibleWindow.appearance.widgetAlpha = .55f;
        constexpr std::array<const char*, 5> sources{"statusBar/noWindow", "statusBar/maximizedWindow",
            "taskbar/shellUi", "taskbar/maximizedWindow", "taskbar/visibleWindow"};
        constexpr std::array<float, 5> expected{.11f,.22f,.33f,.44f,.55f};
        for (std::size_t i = 0; i < sources.size(); ++i)
        {
            auto captured = CaptureTarget(sources[i], scenarios);
            check(captured.kind == Kind::Global && captured.scopes == (i < 2 ? StatusBar : Taskbar) &&
                std::abs(captured.appearance.widgetAlpha - expected[i]) < .0001f,
                "scenario save captures its own resolved appearance rather than the default bar");
            auto local = integration; auto theme = global; theme.id = "theme/scenario";
            theme.appearance.widgetAlpha = .83f; theme.scopes = Dock | StatusBar | Taskbar;
            local.themes.emplace(theme.id, theme);
            auto applied = scenarios;
            check(ApplyTarget(local, sources[i], theme.id, applied, error) &&
                Select(local, sources[i], theme.id, Kind::Global, TargetScope(sources[i]), error),
                "scenario selection uses the existing global kind and bar scope");
            check(CaptureTarget(sources[i], applied).appearance.widgetAlpha == .83f &&
                applied.personalization == scenarios.personalization && applied.navigation == scenarios.navigation &&
                applied.general.statusBar.noWindow.enabled == scenarios.general.statusBar.noWindow.enabled &&
                applied.dock.systemTaskbarVisibleWindow.enabled == scenarios.dock.systemTaskbarVisibleWindow.enabled,
                "scenario application preserves global appearance, personal navigation and rule enablement");
            for (std::size_t j = 0; j < sources.size(); ++j) if (i != j)
                check(CaptureTarget(sources[j], applied).appearance.widgetAlpha == expected[j], "scenario application leaves other overrides unchanged");
            local.themes.at(theme.id).appearance.widgetAlpha = .91f;
            const auto updated = ApplySavedUpdate(local, theme.id, applied, error);
            check(updated.size() == 1 && updated.front() == sources[i] && CaptureTarget(sources[i], applied).appearance.widgetAlpha == .91f,
                "explicit saved-theme update follows the selected scenario reference");
            ReconcileReferences(local, scenarios);
            check(local.references.at(sources[i]).id.empty(), "editing a scenario detaches only its saved selection");
        }
    }
    const auto patch = WidgetPatch(global);
    check(patch.presetId == "__custom" && patch.followPersonalization == false && patch.backgroundOpacity == global.appearance.widgetAlpha,
        "widget selection uses host appearance patch instead of applying author functional presets");
    check(Select(integration, "global", global.id, Kind::Global, Components, error), "global reference snapshot recorded");
    integration.themes.at(quick.id).layout.fontSize = 21;
    const auto refreshed = ApplySavedUpdate(integration, quick.id, settings, error);
    check(refreshed.size() == 1 && refreshed.front() == "global" && settings.navigation.layout.fontSize == 21 &&
        settings.general.collectionPopupAppearance.appearance.widgetAlpha == .41f,
        "explicit child update refreshes a selected global binding and only its followers");
    auto independent = settings.navigation; independent.layout.fontSize = 12;
    check(FollowQuickBinding(integration, independent) && independent.layout.fontSize == 19 &&
        independent.prefixes == personal.prefixes, "returning to follow-global restores the successful bound layout snapshot");
    settings.personalization.widgetAlpha = .2f;
    ReconcileReferences(integration, settings);
    check(integration.references.at("global").id.empty() && integration.themes.at(global.id).appearance.widgetAlpha != .2f,
        "parameter editing detaches selection without editing the saved theme");

    {
        // The reported green search bar survived a native built-in selection:
        // its material switched, but its separately persisted palette did not.
        // Exercise the same transition helpers as SettingsWindow and startup,
        // then resolve the production renderer's search color.
        auto child = Capture(Kind::QuickPanel, MakeQuickNavigationAppearancePreset(kAppearancePresetAcrylicDark));
        child.id = "theme/residue-child"; child.name = "Green search";
        child.layout.fontSize = 19; child.layout.searchHeight = 68;
        child.colors["searchBg"] = "#00B09B87";
        auto parent = Capture(Kind::Global, MakeAppearancePreset(kAppearancePresetGlassDark));
        parent.id = "theme/residue-parent"; parent.name = "Green global";
        parent.quickPanel = child.id; parent.popup = "builtin/popup/dark";
        Library transitions; transitions.themes = {{child.id, child}, {parent.id, parent}};
        SettingsValues selected;
        selected.navigation.virtualKey = 'Q'; selected.navigation.prefixes[0] = "program";
        selected.navigation.defaultEngine = "google"; selected.navigation.lastCollapsed = true;
        selected.navigation.desktopViewMode = QuickNavigationDesktopViewMode::Source;
        const auto preferences = selected.navigation;
        auto expectedBuiltin = preferences;
        expectedBuiltin.layout = {}; expectedBuiltin.colors.clear();
        check(ApplyTarget(transitions, "global", parent.id, selected, error) &&
            Select(transitions, "global", parent.id, Kind::Global, parent.scopes, error),
            "residue fixture selects a real global theme with its immutable child snapshot");
        const auto green = ResolveQuickNavTheme(false, selected.navigation, true).searchBg;
        check(green.rgb == RGB(0,176,155), "selected custom theme reaches the production search palette");
        for (const int preset : {kAppearancePresetDark, kAppearancePresetLight, kAppearancePresetGlassDark,
            kAppearancePresetGlassLight, kAppearancePresetGlassTransparent, kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight})
        {
            auto builtin = selected;
            ApplyAppearancePreset(builtin.personalization, preset);
            PrepareGlobalThemeEdit(builtin, selected.personalization);
            const auto material = ResolveSurfaceTheme(builtin.general.quickNavigationAppearance, builtin.personalization,
                builtin.general.quickNavTheme, true, &builtin.general.globalQuickNavigationAppearance);
            const auto palette = ResolveQuickNavTheme(material.contentTheme == 1, builtin.navigation, material.glassEnabled);
            check(builtin.navigation == expectedBuiltin && !builtin.general.globalQuickNavigationAppearance.customized &&
                !builtin.general.globalCollectionPopupAppearance.customized && palette.searchBg.rgb != green.rgb,
                "every native global built-in replaces follower layout/colors and retains all personal search preferences");

            // A restart must also correct configurations written before the fix.
            builtin.navigation = selected.navigation;
            check(RestoreBuiltinQuickPanel(builtin.navigation, builtin.general, builtin.personalization) &&
                builtin.navigation == expectedBuiltin &&
                !RestoreBuiltinQuickPanel(builtin.navigation, builtin.general, builtin.personalization),
                "startup clears historical built-in residues once and is then idempotent");
        }
        for (int mode : {0,1,2,3,-1})
        {
            auto next = selected;
            next.general.quickNavigationAppearance = {4,true,child.appearance};
            const auto previous = next.general;
            next.personalization = MakeAppearancePreset(kAppearancePresetGlassDark);
            next.general.globalQuickNavigationAppearance = {};
            SelectSurfaceThemeMode(next.general.quickNavigationAppearance,mode,child.appearance);
            PrepareQuickPanelSelectionEdit(next,previous,&transitions);
            check(next.navigation == expectedBuiltin,
                "independent custom to any native built-in or built-in global replaces the complete quick-panel theme");
        }
        for (int mode : {0,1,2,3,4})
        {
            auto next = selected;
            next.general.quickNavigationAppearance.mode = mode;
            next.navigation.layout.fontSize = 12; next.navigation.colors = {{"searchBg","#FF0000"}};
            const auto previous = next.general;
            next.general.quickNavigationAppearance.mode = -1;
            PrepareQuickPanelSelectionEdit(next,previous,&transitions);
            check(next.navigation == selected.navigation,
                "returning to custom global restores both bound layout and palette from the successful snapshot");
        }
        for (int mode : {0,1,2,3,4,-2})
        {
            auto next = selected; next.general.quickNavigationAppearance.mode = mode;
            const auto navigationBefore = next.navigation;
            ApplyAppearancePreset(next.personalization,kAppearancePresetLight);
            PrepareGlobalThemeEdit(next,selected.personalization);
            check(next.navigation == navigationBefore,
                "global switching leaves independent and legacy quick-panel appearance values untouched");
        }
        for (int mode : {4,-2})
        {
            auto next = selected; next.general.quickNavigationAppearance.mode = mode;
            check(!RestoreBuiltinQuickPanel(next.navigation,next.general,next.personalization) && next.navigation == selected.navigation,
                "startup retains independent custom and legacy conditional profiles");
        }
        auto custom = selected;
        check(!RestoreBuiltinQuickPanel(custom.navigation,custom.general,custom.personalization) &&
            custom.navigation == selected.navigation, "startup preserves custom global bindings");
        const auto previous = custom.general;
        custom.general.quickNavigationAppearance.mode = 4;
        PrepareQuickPanelSelectionEdit(custom,previous,&transitions);
        check(custom.navigation == selected.navigation, "entering a custom draft retains its effective layout and palette");
        const auto customPrevious = custom.general;
        custom.general.quickNavigationAppearance.mode = -1;
        PrepareQuickPanelSelectionEdit(custom,customPrevious);
        check(custom.navigation == selected.navigation,
            "missing custom-global library retains the last available quick-panel values");
        custom.navigation.colors["searchText"] = "#ABCDEF";
        const auto unrelatedBefore = custom.navigation;
        const auto unrelatedGeneral = custom.general;
        custom.general.dockEnabled = !custom.general.dockEnabled;
        PrepareQuickPanelSelectionEdit(custom,unrelatedGeneral,&transitions);
        check(custom.navigation == unrelatedBefore, "unrelated general edits do not replay or reset a theme selection");
    }

    const auto unique = CreateId();
    const auto root = std::filesystem::temp_directory_path() / ("SnowDesktopThemeTests-" + unique.substr(6));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    std::filesystem::create_directories(root);
    {
        Library bound = integration; bound.references.clear();
        auto source = global; source.appearance.widgetBorderR = 196.f / 255.f;
        bound.themes[source.id] = source;
        SettingsValues applied = settings;
        check(ApplyTarget(bound,"global",source.id,applied,error) &&
            Select(bound,"global",source.id,Kind::Global,source.scopes,error), "global binding fixture uses the real application and reference snapshot");
        const auto expectedGlobal = applied.personalization;
        const auto expectedSnapshot = EncodePackage(bound.references.at("global").snapshot,error);
        const auto personalizationPath = root / L"personalization-round-trip.json";
        PersonalizationSettings reloadedAppearance;
        check(SavePersonalization(personalizationPath.c_str(), expectedGlobal) && LoadPersonalization(personalizationPath.c_str(),reloadedAppearance) &&
            EncodePanelAppearance(reloadedAppearance,true) == EncodePanelAppearance(expectedGlobal,true),
            "settings persistence must not round away applied theme appearance precision");
        applied.personalization = reloadedAppearance;
        ReconcileReferences(bound,applied);
        check(bound.references.at("global").id == source.id, "saving and reloading the global appearance retains its named binding");
        auto legacy = applied;
        legacy.personalization.widgetBorderR = .768627f;
        check(EncodePanelAppearance(source.appearance) != EncodePanelAppearance(legacy.personalization),
            "negative control: the previous exact-text comparison rejects this real six-digit color round trip");
        ReconcileReferences(bound,legacy);
        check(bound.references.at("global").id == source.id, "existing six-significant-digit settings remain bound after child selection");
        for (const auto target : {"quickPanel", "popup"})
        {
            const auto childId = std::string_view(target) == "quickPanel" ? quick.id : popup.id;
            check(ApplyTarget(bound,target,childId,legacy,error) &&
                Select(bound,target,childId,TargetKind(target),TargetScope(target),error), "independent child applies through the production source selector");
            ReconcileReferences(bound,legacy);
            check(bound.references.at("global").id == source.id &&
                EncodePackage(bound.references.at("global").snapshot,error) == expectedSnapshot,
                "independent quick-panel or popup selection cannot detach or rewrite the global binding");
            Detach(bound,target);
            auto& surface = std::string_view(target) == "quickPanel" ? legacy.general.quickNavigationAppearance : legacy.general.collectionPopupAppearance;
            for (int mode : {0,4,-1})
            {
                SelectSurfaceThemeMode(surface,mode,source.appearance);
                ReconcileReferences(bound,legacy);
                check(bound.references.at("global").id == source.id,
                    "switching a child to built-in, custom or follow-global retains the global source");
            }
        }
        legacy.personalization.widgetBorderR = source.appearance.widgetBorderR + .000001f;
        ReconcileReferences(bound,legacy);
        check(bound.references.at("global").id.empty() && bound.themes.at(source.id).appearance.widgetBorderR == source.appearance.widgetBorderR,
            "a genuine appearance edit outside the exact legacy round trip still detaches without changing the saved theme");
    }
    {
        for (const int preset : {kAppearancePresetDark, kAppearancePresetLight,
            kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight, kAppearancePresetCustom})
        {
            auto previous = MakeAppearancePreset(preset);
            previous.widgetAlpha = .37f;
            GeneralSettings general;
            const auto quickEffective = ResolveSurfaceTheme(general.quickNavigationAppearance,
                previous, general.quickNavTheme, true);
            const auto popupEffective = ResolveSurfaceTheme(general.collectionPopupAppearance,
                previous, general.collectionPopupTheme, false);
            auto edited = previous;
            edited.backgroundPreset = kAppearancePresetCustom;
            edited.widgetAlpha = .81f;
            check(ResolveSurfaceTheme(general.quickNavigationAppearance,edited,general.quickNavTheme,true).widgetAlpha != quickEffective.widgetAlpha,
                "negative control: the former unbound follow resolver replaces the current surface with the edited global material");
            check(PrepareGlobalCustomEdit(general,previous,edited) &&
                EncodePanelAppearance(general.globalQuickNavigationAppearance.appearance,true) == EncodePanelAppearance(quickEffective,true) &&
                EncodePanelAppearance(general.globalCollectionPopupAppearance.appearance,true) == EncodePanelAppearance(popupEffective,true),
                "global custom draft copies each current effective surface before applying the global edit");
            const auto frozenQuick = general.globalQuickNavigationAppearance;
            const auto frozenPopup = general.globalCollectionPopupAppearance;
            auto later = edited; later.widgetAlpha = .11f;
            check(!PrepareGlobalCustomEdit(general,edited,later) &&
                general.globalQuickNavigationAppearance == frozenQuick && general.globalCollectionPopupAppearance == frozenPopup &&
                ResolveSurfaceTheme(general.quickNavigationAppearance,later,general.quickNavTheme,true,&frozenQuick) == frozenQuick.appearance &&
                ResolveSurfaceTheme(general.collectionPopupAppearance,later,general.collectionPopupTheme,false,&frozenPopup) == frozenPopup.appearance,
                "later global appearance changes cannot overwrite either bound surface draft");
            SelectSurfaceThemeMode(general.quickNavigationAppearance,4,frozenQuick.appearance);
            general.quickNavigationAppearance.appearance.widgetAlpha = .63f;
            check(ResolveSurfaceTheme(general.quickNavigationAppearance,later,general.quickNavTheme,true,&frozenQuick).widgetAlpha == .63f &&
                general.globalQuickNavigationAppearance == frozenQuick && general.globalCollectionPopupAppearance == frozenPopup,
                "independent quick-panel editing copies its bound source and leaves both global bindings intact");
            const auto builtin = MakeAppearancePreset(kAppearancePresetLight);
            check(PrepareGlobalCustomEdit(general,later,builtin) &&
                !general.globalQuickNavigationAppearance.customized && !general.globalCollectionPopupAppearance.customized &&
                general.quickNavigationAppearance.mode == 4 && general.quickNavigationAppearance.appearance.widgetAlpha == .63f,
                "switching global to a built-in releases its bindings without discarding an independent surface draft");
        }
        GeneralSettings bound;
        bound.globalQuickNavigationAppearance = {4,true,quick.appearance};
        bound.globalCollectionPopupAppearance = {4,true,popup.appearance};
        const auto quickBinding = bound.globalQuickNavigationAppearance;
        const auto popupBinding = bound.globalCollectionPopupAppearance;
        auto edited = global.appearance; edited.backgroundPreset = kAppearancePresetCustom;
        edited.widgetAlpha = .51f;
        check(!PrepareGlobalCustomEdit(bound,global.appearance,edited) &&
            bound.globalQuickNavigationAppearance == quickBinding && bound.globalCollectionPopupAppearance == popupBinding,
            "a saved global theme entering custom keeps its actual child-theme bindings");
    }
    {
        GeneralSettings general;
        general.globalQuickNavigationAppearance = {4,true,quick.appearance};
        general.globalCollectionPopupAppearance = {4,true,popup.appearance};
        const auto boundQuick = general.globalQuickNavigationAppearance;
        const auto boundPopup = general.globalCollectionPopupAppearance;
        const auto independentQuick = general.quickNavigationAppearance;
        general.collectionPopupAppearance.mode = 4;
        auto componentAppearance = global.appearance;
        componentAppearance.widgetAlpha = .42f;
        componentAppearance.gradientEndA = .19f;
        const auto componentBefore = componentAppearance;
        CopyComponentAppearanceToPopup(general,componentAppearance);
        const auto copied = general.collectionPopupAppearance;
        check(copied.mode == 4 && copied.customized &&
            EncodePanelAppearance(copied.appearance,true) == EncodePanelAppearance(componentAppearance,true) &&
            componentAppearance == componentBefore,
            "copying component appearance creates a complete independent popup draft including gradient opacity without modifying its source");
        componentAppearance.widgetAlpha = .91f;
        check(ResolveSurfaceTheme(copied,componentAppearance,general.collectionPopupTheme,false,&boundPopup) == copied.appearance &&
            general.globalQuickNavigationAppearance == boundQuick && general.globalCollectionPopupAppearance == boundPopup &&
            general.quickNavigationAppearance == independentQuick,
            "component copy does not bind future global edits or replace the existing global child bindings");
        general.collectionPopupAppearance.appearance.widgetAlpha = .28f;
        check(componentAppearance.widgetAlpha == .91f && general.collectionPopupAppearance.appearance.widgetAlpha == .28f,
            "copied popup remains independently editable without modifying its source");
    }
    {
        Library sourceLibrary;
        sourceLibrary.themes = children; sourceLibrary.themes.emplace(global.id,global);
        check(Select(sourceLibrary,"global",global.id,Kind::Global,All,error), "bound editing source starts from an applied global theme");
        winui::theme_controls::EditSource parent; parent.Begin(sourceLibrary,"global");
        const auto quickSource = parent.Bound(Kind::QuickPanel);
        const auto popupSource = parent.Bound(Kind::Popup);
        const auto selectedQuick = winui::theme_controls::SelectionEditSource(sourceLibrary, nullptr, quickSource, true);
        const auto selectedPopup = winui::theme_controls::SelectionEditSource(sourceLibrary, nullptr, popupSource, true);
        check(selectedQuick.theme && selectedPopup.theme && selectedQuick.theme->id == quick.id && selectedPopup.theme->id == popup.id &&
            !winui::theme_controls::SelectionEditSource(sourceLibrary, nullptr, quickSource, false).theme,
            "follow-global edit selection resolves both saved local children and cannot leak into an independent native preset");
        Detach(sourceLibrary,"global");
        check(quickSource.theme && popupSource.theme && quickSource.theme->id == quick.id && popupSource.theme->id == popup.id &&
            quickSource.CanUpdate(sourceLibrary) && popupSource.CanUpdate(sourceLibrary),
            "a global custom draft retains both actual bound source IDs after detachment for update or save-as");
        SettingsValues draft;
        draft.personalization = global.appearance;
        draft.general.globalQuickNavigationAppearance = {4,true,quick.appearance};
        draft.general.globalCollectionPopupAppearance = {4,true,popup.appearance};
        const auto stored = EncodeLibrary(sourceLibrary,error);
        const auto globalMaterial = draft.personalization;
        auto quickEdit = quick.appearance; quickEdit.widgetAlpha = .27f;
        auto popupEdit = popup.appearance; popupEdit.widgetAlpha = .73f;
        EditSurfaceAppearance(draft.general,true,quickEdit,draft.personalization);
        EditSurfaceAppearance(draft.general,false,popupEdit,draft.personalization);
        check(draft.general.quickNavigationAppearance.mode == -1 && draft.general.collectionPopupAppearance.mode == -1 &&
            CaptureTarget("quickPanel",draft).appearance.widgetAlpha == .27f && CaptureTarget("popup",draft).appearance.widgetAlpha == .73f &&
            draft.personalization == globalMaterial && EncodeLibrary(sourceLibrary,error) == stored,
            "editing either bound draft keeps follow-global selected, edits the correct binding and does not rewrite the library or global material");
        CopyComponentAppearanceToPopup(draft.general,draft.personalization);
        check(draft.general.collectionPopupAppearance.mode == -1 &&
            EncodePanelAppearance(draft.general.globalCollectionPopupAppearance.appearance,true) == EncodePanelAppearance(draft.personalization,true),
            "copying component appearance into a bound popup also preserves follow-global mode");
        auto boundAppearance = CaptureTarget("quickPanel",draft); boundAppearance.id = quickSource.theme->id; boundAppearance.name = quickSource.theme->name;
        check(Save(sourceLibrary,boundAppearance,{},true,savedId,error) && savedId == quick.id && draft.general.quickNavigationAppearance.mode == -1,
            "explicitly updating a bound local theme keeps its source ID without switching the draft selector");
        boundAppearance.id.clear(); boundAppearance.name = "Bound draft copy";
        check(Save(sourceLibrary,boundAppearance,{},false,savedId,error,[] {return "theme/bound-copy";}) && savedId == "theme/bound-copy" &&
            sourceLibrary.themes.at(quick.id).appearance == boundAppearance.appearance && draft.general.quickNavigationAppearance.mode == -1,
            "saving a bound draft as a new theme assigns a new ID and retains follow-global mode");
        EditSurfaceAppearance(draft.general,false,popupSource.theme->appearance,draft.personalization);
        check(draft.general.collectionPopupAppearance.mode == -1 && CaptureTarget("popup",draft).appearance == popupSource.theme->appearance,
            "cancelling a bound popup restores its saved material without turning it into an independent custom theme");
        sourceLibrary.workshop["321"].ids.insert(popup.id);
        check(!winui::theme_controls::SelectionEditSource(sourceLibrary, nullptr, popupSource, true).theme &&
            winui::theme_controls::SelectionEditSource(sourceLibrary, &quick, {}, false).theme &&
            !winui::theme_controls::SelectionEditSource(sourceLibrary, nullptr, parent.Bound(Kind::Popup), false).theme,
            "selection editing rejects subscribed bindings while retaining direct local theme editing");
        check(!popupSource.CanUpdate(sourceLibrary), "bound subscribed sources retain the same overwrite restriction as independent sources");
        sourceLibrary.themes.erase(quick.id);
        check(!winui::theme_controls::SelectionEditSource(sourceLibrary, nullptr, quickSource, true).theme,
            "a removed inherited local theme cannot open an editor from a retained snapshot");
        check(!quickSource.CanUpdate(sourceLibrary) && quickSource.theme->id == quick.id,
            "deleting a bound source preserves its draft identity but blocks overwrite");
        parent.Reset();
        check(!parent.Bound(Kind::Popup).theme && !parent.Bound(Kind::QuickPanel).theme,
            "switching out of a global edit clears both bound source identities");
        Library versions;
        auto localVersion = popup, subscriptionVersion = popup;
        localVersion.id = "theme/local-version"; subscriptionVersion.id = "theme/subscribed-version";
        versions.themes.emplace(localVersion.id, localVersion); versions.themes.emplace(subscriptionVersion.id, subscriptionVersion);
        versions.workshop["456"].ids.insert(subscriptionVersion.id);
        std::map<std::string, std::string> urls{{localVersion.id,"https://steamcommunity.com/sharedfiles/filedetails/?id=456"}};
        check(winui::theme_controls::VersionCounterpart(versions, urls, localVersion) == subscriptionVersion.id &&
            winui::theme_controls::VersionCounterpart(versions, urls, subscriptionVersion) == localVersion.id,
            "local and subscription versions pair in both directions by their Workshop item identity");
        urls[localVersion.id] = "https://steamcommunity.com/sharedfiles/filedetails/?id=999";
        check(winui::theme_controls::VersionCounterpart(versions, urls, localVersion).empty() &&
            winui::theme_controls::VersionCounterpart(versions, urls, subscriptionVersion).empty(),
            "same-name versions from unrelated Workshop items never pair");
        urls[localVersion.id] = "https://steamcommunity.com/sharedfiles/filedetails/?id=456";
        auto ambiguous = localVersion; ambiguous.id = "theme/ambiguous-version";
        versions.themes.emplace(ambiguous.id, ambiguous); urls[ambiguous.id] = urls[localVersion.id];
        check(winui::theme_controls::VersionCounterpart(versions, urls, subscriptionVersion).empty(),
            "multiple editable copies of one item require explicit selection rather than guessing a version");
    }
    {
        Library source; source.themes = children; source.themes.emplace(global.id,global);
        std::array<BindingDraft,2> drafts{{{quick,{},quick.id},{popup,{},popup.id}}};
        drafts[0].effective.appearance.widgetAlpha = .27f;
        drafts[0].effective.colors["resultBorder"] = "#FF000080";
        drafts[1].effective.appearance.widgetAlpha = .73f;
        auto parent = global; parent.name = "Edited parent";
        const auto original = EncodeLibrary(source,error);
        std::vector<std::string> updated;
        SettingsValues independentPanels;
        independentPanels.personalization = global.appearance;
        independentPanels.general.globalQuickNavigationAppearance = {4,true,quick.appearance};
        independentPanels.general.globalCollectionPopupAppearance = {4,true,popup.appearance};
        independentPanels.general.quickNavigationAppearance = {4,true,MakeQuickNavigationAppearancePreset(kAppearancePresetDark)};
        independentPanels.general.collectionPopupAppearance = {4,true,MakeCollectionPopupAppearancePreset(kAppearancePresetLight)};
        independentPanels.navigation.layout.fontSize = 27; independentPanels.navigation.colors["searchText"] = "#ABCDEF";
        const auto boundQuickDraft = CaptureGlobalBinding(Kind::QuickPanel,independentPanels,quick);
        const auto boundPopupDraft = CaptureGlobalBinding(Kind::Popup,independentPanels,popup);
        check(boundQuickDraft.appearance == quick.appearance && boundQuickDraft.layout == quick.layout && boundQuickDraft.colors == quick.colors &&
            boundPopupDraft.appearance == popup.appearance && independentPanels.general.quickNavigationAppearance.mode == 4 &&
            independentPanels.general.collectionPopupAppearance.mode == 4 && independentPanels.navigation.layout.fontSize == 27,
            "global save captures its own bound children without copying independently selected surface material or navigation into them");
        check(SaveDraft(source,parent,drafts,true,savedId,updated,error) && savedId == global.id && source.themes.size() == 3 &&
            source.themes.at(global.id).quickPanel == quick.id && source.themes.at(global.id).popup == popup.id &&
            source.themes.at(quick.id).appearance.widgetAlpha == .27f && source.themes.at(popup.id).appearance.widgetAlpha == .73f &&
            source.themes.at(quick.id).colors.at("resultBorder") == "#FF000080" && source.themes.at(quick.id).name == quick.name &&
            updated == std::vector<std::string>{quick.id,popup.id,global.id},
            "saving an edited global updates both bound local child IDs and values atomically without creating duplicate cards");
        Library newSource; newSource.themes = children; newSource.themes.emplace(global.id,global);
        int generated = 0;
        check(SaveDraft(newSource,parent,drafts,false,savedId,updated,error,[&]{return "theme/draft-new-" + std::to_string(++generated);}) &&
            savedId == "theme/draft-new-3" && newSource.themes.at(savedId).quickPanel == "theme/draft-new-1" &&
            newSource.themes.at(savedId).popup == "theme/draft-new-2" &&
            newSource.themes.at(quick.id).appearance == quick.appearance && newSource.themes.at(popup.id).appearance == popup.appearance && updated.empty(),
            "new global draft clones edited children with new IDs while preserving every original source");
        Library failedSource; check(DecodeLibrary(original,failedSource,error),"original draft fixture reloads");
        const auto before = EncodeLibrary(failedSource,error);
        auto invalidParent = parent; invalidParent.name.clear();
        check(!SaveDraft(failedSource,invalidParent,drafts,true,savedId,updated,error) && EncodeLibrary(failedSource,error) == before,
            "parent validation failure cannot prematurely write either edited child");
        auto missing = drafts; missing[1].sourceId = "theme/deleted-source";
        check(!SaveDraft(failedSource,parent,missing,true,savedId,updated,error) && error == "themeNotFound" &&
            EncodeLibrary(failedSource,error) == before && drafts[1].effective.appearance.widgetAlpha == .73f,
            "a deleted source stops the whole update and retains the independent editing draft");
        failedSource.workshop["321"].ids.insert(popup.id);
        const auto subscribedBefore = EncodeLibrary(failedSource,error);
        check(!SaveDraft(failedSource,parent,drafts,true,savedId,updated,error) && error == "copyRequired" &&
            EncodeLibrary(failedSource,error) == subscribedBefore,
            "global save cannot implicitly overwrite or copy an edited subscribed child");
        auto selected = drafts; selected[1].selectedId = popup.id;
        check(SaveDraft(failedSource,parent,selected,true,savedId,updated,error) &&
            failedSource.themes.at(popup.id).appearance == popup.appearance && failedSource.themes.at(global.id).popup == popup.id,
            "an explicitly selected subscribed binding is reused unchanged instead of treating unrelated draft parameters as an edit");
    }
    {
        using namespace winui::theme_controls;
        Library paired; Package authored = children; authored.emplace(global.id,global);
        paired.themes = authored;
        std::map<std::string,std::string> remoteIds;
        for (const auto& [id,theme] : authored) { (void)theme; remoteIds[id] = id + "-subscription"; }
        for (auto [id,theme] : authored)
        {
            theme.id = remoteIds.at(id); theme.name += " Updated";
            if (remoteIds.contains(theme.quickPanel)) theme.quickPanel = remoteIds.at(theme.quickPanel);
            if (remoteIds.contains(theme.popup)) theme.popup = remoteIds.at(theme.popup);
            paired.themes.emplace(theme.id,theme);
            paired.workshop["456"].ids.insert(theme.id); paired.workshop["456"].sourceIds.emplace(theme.id,id);
        }
        const auto grouped = ManagementEntries(paired,{}, {},FilterTab::All);
        check(paired.themes.size() == 6 && grouped.size() == 3 &&
            std::all_of(grouped.begin(),grouped.end(),[&](const auto& theme){return authored.contains(theme.id);}),
            "six local and subscribed nodes become three management cards using authored UUID, even when remote names differ");
        check(Select(paired,"popup",remoteIds.at(popup.id),Kind::Popup,All,error),"subscription version can be explicitly selected");
        const auto activePopup = ManagementEntries(paired,{}, {},FilterTab::Popup);
        check(activePopup.size() == 1 && activePopup.front().id == remoteIds.at(popup.id),
            "merged card initially shows the currently applied subscribed version");
        const auto popupChoices = Choices(paired,Kind::Popup);
        const auto regularChoices = CollapseVersionChoices(paired,{},popupChoices,{});
        const auto selectedChoices = CollapseVersionChoices(paired,{},popupChoices,remoteIds.at(popup.id));
        check(regularChoices.size() + 1 == popupChoices.size() && selectedChoices.size() == regularChoices.size() &&
            std::any_of(regularChoices.begin(),regularChoices.end(),[&](const auto& entry){return entry.id == popup.id;}) &&
            std::none_of(regularChoices.begin(),regularChoices.end(),[&](const auto& entry){return entry.id == remoteIds.at(popup.id);}) &&
            std::any_of(selectedChoices.begin(),selectedChoices.end(),[&](const auto& entry){return entry.id == remoteIds.at(popup.id);}),
            "ordinary choices contain one logical UUID entry and retain the currently selected version without a second indistinguishable name");
        const auto localPopup = ManagementEntries(paired,{}, {{popup.id,popup.id}},FilterTab::Popup);
        check(localPopup.size() == 1 && localPopup.front().id == popup.id && paired.references.at("popup").id == remoteIds.at(popup.id),
            "viewing a preferred local card never rewrites the active library reference");
        auto& updatedRemote = paired.themes.at(remoteIds.at(global.id)); updatedRemote.scopes = Dock;
        updatedRemote.quickPanel.clear(); updatedRemote.popup.clear();
        const auto globalCard = ManagementEntries(paired,{}, {{global.id,updatedRemote.id}},FilterTab::Global);
        check(globalCard.size() == 1 && globalCard.front().id == updatedRemote.id &&
            ManagementEntries(paired,{}, {},FilterTab::StatusBar).size() == 1,
            "scope changes do not split the UUID pair, and base-tag filtering finds either version's applicability");
        const auto statusChoices = CollapseVersionChoices(paired,{},Choices(paired,Kind::Global,StatusBar),updatedRemote.id);
        check(std::any_of(statusChoices.begin(),statusChoices.end(),[&](const auto& entry){return entry.id == global.id;}) &&
            std::none_of(statusChoices.begin(),statusChoices.end(),[&](const auto& entry){return entry.id == updatedRemote.id;}),
            "a preferred version outside the target scope cannot hide the applicable local choice");
        auto unrelated = popup; unrelated.id = "theme/same-name-unrelated";
        paired.themes.emplace(unrelated.id,unrelated);
        check(ManagementEntries(paired,{}, {},FilterTab::Popup).size() == 2,
            "same-name independent themes remain separate management cards");
        auto duplicate = paired.themes.at(remoteIds.at(popup.id)); duplicate.id = "theme/second-subscription";
        paired.themes.emplace(duplicate.id,duplicate); paired.workshop["789"].ids.insert(duplicate.id);
        paired.workshop["789"].sourceIds.emplace(duplicate.id,popup.id);
        check(VersionCounterpart(paired,{},popup).empty() && ManagementEntries(paired,{}, {},FilterTab::Popup).size() == 4,
            "ambiguous multiple subscription counterparts stay separate instead of choosing an editing target");
    }
    {
        auto rgbaPackage = children;
        rgbaPackage.emplace(global.id, global);
        rgbaPackage.at(quick.id).colors["resultBorder"] = "#FF000080";
        rgbaPackage.at(quick.id).colors["resultFill"] = "#FFFFFF00";
        Package restored;
        const auto encoded = EncodePackage(rgbaPackage,error);
        check(!encoded.empty() && DecodePackage(encoded,restored,error) &&
            restored.at(quick.id).colors.at("resultBorder") == "#FF000080" && restored.at(quick.id).colors.at("resultFill") == "#FFFFFF00",
            "theme package codecs preserve RGBA channel order, partial opacity and fully transparent fill");
        rgbaPackage.at(quick.id).colors["resultBorder"] = "#FF0000GG";
        check(!Validate(rgbaPackage,error), "invalid RGBA still rejects the whole theme package");
    }
    const auto libraryPath = root / "library.json";
    Library persisted;
    check(Transact(libraryPath, [&](auto& value, auto&) { value = destination; return true; }, persisted, error), "atomic library write succeeds");
    const auto bytes = [&](const auto& path) { std::ifstream file(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(file), {}); };
    const auto original = bytes(libraryPath);
    check(!Transact(libraryPath, [](auto& value, auto&) { value.themes.clear(); return false; }, persisted, error) && bytes(libraryPath) == original,
        "failed operation preserves the previous complete file");
    check(!Transact(libraryPath, [](auto& value, auto& detail) {
        if (!Select(value, "dock", "theme/global-copy", Kind::Global, Dock, detail)) return false;
        detail = "writeFailed"; // The settings commit failed after staging a selection.
        return false;
    }, persisted, error) && error == "writeFailed" && bytes(libraryPath) == original && persisted.references.empty(),
        "failed settings application cannot publish its staged selection snapshot");
    const auto lockPath = std::filesystem::path(libraryPath.wstring() + L".lock");
    const HANDLE lock = CreateFileW(lockPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lock != INVALID_HANDLE_VALUE && !Transact(libraryPath, [](auto&, auto&) { return true; }, persisted, error) &&
        error == "libraryBusy" && bytes(libraryPath) == original, "concurrent library writers fail without data loss");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    {
        const auto draftPath = root / "draft.json";
        Library draftLibrary; draftLibrary.themes = children; draftLibrary.themes.emplace(global.id,global);
        check(Transact(draftPath,[&](auto& value,auto&){value = draftLibrary; return true;},draftLibrary,error),
            "global edit transaction fixture persists the original three identities");
        const auto draftOriginalBytes = bytes(draftPath);
        const auto draftOriginalLibrary = EncodeLibrary(draftLibrary,error);
        std::array<BindingDraft,2> draftBindings{{{quick,{},quick.id},{popup,{},popup.id}}};
        draftBindings[0].effective.appearance.widgetAlpha = .27f;
        std::vector<std::string> draftUpdatedIds; std::string draftSavedId;
        check(!Transact(draftPath,[&](auto& value,auto& detail){
            if (!SaveDraft(value,global,draftBindings,true,draftSavedId,draftUpdatedIds,detail)) return false;
            detail = "cancelled"; return false;
        },draftLibrary,error) && error == "cancelled" && bytes(draftPath) == draftOriginalBytes &&
            EncodeLibrary(draftLibrary,error) == draftOriginalLibrary && draftBindings[0].effective.appearance.widgetAlpha == .27f,
            "cancelled global save discards all staged child updates and preserves both persisted sources and the edit draft");
        const HANDLE denyReplace = CreateFileW(draftPath.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        bool stagedDraft = false;
        check(denyReplace != INVALID_HANDLE_VALUE && !Transact(draftPath,[&](auto& value,auto& detail){
            stagedDraft = SaveDraft(value,global,draftBindings,true,draftSavedId,draftUpdatedIds,detail); return stagedDraft;
        },draftLibrary,error) && stagedDraft && error == "writeFailed" && bytes(draftPath) == draftOriginalBytes &&
            EncodeLibrary(draftLibrary,error) == draftOriginalLibrary && draftBindings[0].effective.appearance.widgetAlpha == .27f,
            "real atomic replacement failure after staging child updates preserves original IDs, file bytes and editable parameters");
        if (denyReplace != INVALID_HANDLE_VALUE) CloseHandle(denyReplace);
    }
    const auto packagePath = root / "export.snowtheme";
    check(WritePackage(packagePath, package, error) && ReadPackage(packagePath, decoded, error), "production file export and import parser round-trip");
    auto renamedPath = packagePath; renamedPath.replace_extension(L".json");
    check(!ReadPackage(renamedPath, decoded, error), "file parser refuses a renamed unsupported suffix");
    const auto directoryPath = root / "blocked.snowtheme"; std::filesystem::create_directory(directoryPath);
    check(!WritePackage(directoryPath, package, error) && std::filesystem::is_directory(directoryPath), "failed atomic replacement leaves its destination intact");
    const auto generalPath = root / "general.json";
    settings.general.globalQuickNavigationAppearance.appearance.gradientEndA = .37f;
    settings.general.globalCollectionPopupAppearance.appearance.gradientEndA = .81f;
    check(SaveGeneralSettings(generalPath.c_str(), settings.general), "persist bound surface snapshots");
    GeneralSettings general;
    check(LoadGeneralSettings(generalPath.c_str(), general) && general.globalQuickNavigationAppearance == settings.general.globalQuickNavigationAppearance &&
        general.globalCollectionPopupAppearance == settings.general.globalCollectionPopupAppearance &&
        general.globalQuickNavigationAppearance.appearance.gradientEndA == .37f &&
        general.globalCollectionPopupAppearance.appearance.gradientEndA == .81f, "bound snapshots survive settings reload");
    JsonValue legacyPanel, badPanel;
    PersonalizationSettings panel;
    check(ParseJson("{}", legacyPanel) && DecodePanelAppearance(legacyPanel, panel, true) && panel.gradientEndA == .65f,
        "legacy surface snapshots without end opacity keep their original default");
    const auto beforeBadPanel = panel;
    check(ParseJson("{\"gradientEndOpacity\":2}", badPanel) && !DecodePanelAppearance(badPanel, panel, true) && panel == beforeBadPanel,
        "invalid surface end opacity cannot partially replace a successful snapshot");
    { std::ofstream old(generalPath); old << "{}"; }
    general = {};
    check(LoadGeneralSettings(generalPath.c_str(), general) && !general.globalQuickNavigationAppearance.customized &&
        general.quickNavigationAppearance.mode == -2, "old configuration keeps its conditional legacy appearance");
    return failures;
}
