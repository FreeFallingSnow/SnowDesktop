#include "theme_library_settings.h"
#include "json_value.h"

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
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::cerr << "FAIL theme library: " << message << '\n'; }
    };
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
    const auto beforeDelete = EncodeLibrary(library, error);
    check(!Remove(library, quick.id, {}, true, error) && EncodeLibrary(library, error) == beforeDelete,
        "deleting a bound child is blocked atomically");
    check(Remove(library, quick.id, "builtin/quickpanel/light", true, error), "explicit replacement repairs every global binding");
    check(library.themes.at(global.id).quickPanel == "builtin/quickpanel/light", "saved global references are rewired by ID");
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
    check(ApplyTarget(integration, "dock", global.id, settings, error) && settings.personalization == existingGlobal &&
        !settings.dock.followComponentAppearance, "selecting a theme for one object affects only that object");
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

    const auto unique = CreateId();
    const auto root = std::filesystem::temp_directory_path() / ("SnowDesktopThemeTests-" + unique.substr(6));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{root};
    std::filesystem::create_directories(root);
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
    check(ParseJson("{}", legacyPanel) && DecodePanelAppearance(legacyPanel, panel) && panel.gradientEndA == .65f,
        "legacy surface snapshots without end opacity keep their original default");
    const auto beforeBadPanel = panel;
    check(ParseJson("{\"gradientEndOpacity\":2}", badPanel) && !DecodePanelAppearance(badPanel, panel) && panel == beforeBadPanel,
        "invalid surface end opacity cannot partially replace a successful snapshot");
    { std::ofstream old(generalPath); old << "{}"; }
    general = {};
    check(LoadGeneralSettings(generalPath.c_str(), general) && !general.globalQuickNavigationAppearance.customized &&
        general.quickNavigationAppearance.mode == -2, "old configuration keeps its conditional legacy appearance");
    return failures;
}
