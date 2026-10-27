#include "settings/settings_ipc_channel.h"
#include "settings/settings_ipc_services.h"
#include "settings/settings_ipc_values.h"
#include "settings/icon_library_ipc_paging.h"
#include "system/status_bar/status_bar_appearance.h"
#include "../src/winui/home_about_ipc_values.h"
#include "winui/general_advanced_feature_ipc_values.h"
#include "settings/category_rule_creation.h"
#include "icons/large_icon_edit_rules.h"
#include "icons/large_icon_preset_rules.h"
#include "settings/settings_process.h"
#include "shell/shell_extension_service.h"
#include "app/lifecycle/settings_external_state_refresh.h"
#include "settings/bar_settings_rules.h"
#include "winui/window_support_ipc_values.h"

#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>

namespace
{
using namespace snowdesktop::settings_ipc;
int failures = 0;
void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}
template<class F> void Reject(F operation, const char* message)
{
    try { operation(); Check(false, message); }
    catch (const ProtocolError&) {}
}
HANDLE CurrentProcessHandle()
{
    HANDLE result = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
        GetCurrentProcess(), &result, SYNCHRONIZE, FALSE, 0))
        throw std::runtime_error("test process handle failed");
    return result;
}

void TestPermissionAndRelocatedRoutesCodec()
{
    using snowdesktop::SettingsPage;
    using snowdesktop::SettingsRoute;
    using snowdesktop::CanonicalizeSettingsRoute;
    auto permissions = SettingsRoute::ForPage(SettingsPage::Widgets, "widgets.permissions");
    permissions.itemKey = L"org.snowdesktop.天气包";
    permissions.guideTopic = "widgets";
    const auto restored = Unpack<SettingsRoute>(Pack(permissions));
    Check(restored == permissions && restored.IsValid() &&
            CanonicalizeSettingsRoute(restored) == restored,
        "permission IPC preserves the exact Unicode package key and its focused destination");
    auto invalid = permissions;
    invalid.widgetInstanceId = L"weather-instance";
    Check(!Unpack<SettingsRoute>(Pack(invalid)).IsValid(),
        "IPC does not erase an invalid package and instance combination before route validation");
    invalid = permissions;
    invalid.focusId = "widgets.installed";
    Check(!Unpack<SettingsRoute>(Pack(invalid)).IsValid(),
        "an unrelated widget section cannot gain permission scope through IPC");
    invalid = permissions;
    invalid.page = SettingsPage::General;
    Check(!Unpack<SettingsRoute>(Pack(invalid)).IsValid(),
        "a package key on a global page remains invalid after transport");

    for (const auto focus : {"desktop.spacing", "personalization.font", "personalization.cornerRadius",
             "personalization.widgetTransformCursors", "personalization.quickNavigationTheme"})
    {
        auto legacy = SettingsRoute::ForPage(SettingsPage::AppearanceWidgets, focus);
        legacy.guideTopic = "appearance";
        const auto decoded = Unpack<SettingsRoute>(Pack(legacy));
        const auto canonical = CanonicalizeSettingsRoute(decoded);
        Check(decoded == legacy && canonical == CanonicalizeSettingsRoute(legacy) &&
                Unpack<SettingsRoute>(Pack(canonical)) == canonical &&
                CanonicalizeSettingsRoute(canonical) == canonical,
            "old settings routes retain their IDs across IPC and resolve consistently at either end");
    }
}

void TestCodec()
{
    Check(!snowdesktop::DesktopDisplaySettings{}.beautifyFileThumbnails,
        "file thumbnails remain exempt from global beautification by default");
    for (const bool enabled : {true, false})
    {
        snowdesktop::DesktopDisplaySettings desktop;
        desktop.beautifyFileThumbnails = enabled;
        Check(Unpack<snowdesktop::DesktopDisplaySettings>(Pack(desktop)).beautifyFileThumbnails == enabled,
            "enabling and disabling file thumbnail beautification crosses the settings process boundary");
    }
    snowdesktop::widget_runtime::WidgetSettingFieldSchema duration;
    duration.key = "focus"; duration.rawType = "duration"; duration.durationUnit = "minutes";
    duration.minimum = 1; duration.maximum = 120; duration.step = 1;
    const auto durationRestored = Unpack<snowdesktop::widget_runtime::WidgetSettingFieldSchema>(Pack(duration));
    Check(durationRestored == duration && durationRestored.durationUnit == "minutes",
        "duration units and numeric bounds survive the isolated settings process boundary");
    for (const auto focus : {"dock.attentionDotSize", "dock.attentionDotOffset"})
    {
        const auto requested = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Dock, focus);
        const auto routed = snowdesktop::CanonicalizeSettingsRoute(Unpack<snowdesktop::SettingsRoute>(Pack(requested)));
        Check(routed.page == snowdesktop::SettingsPage::AppearanceDesktopIcons && routed.focusId == focus,
            "Dock attention dot search and IPC focus resolve to global appearance");
    }
    for (const auto focus : {"personalization.windowPinBorder.show", "personalization.windowPinBorder.color",
             "personalization.windowPinBorder.width", "personalization.windowPinBorder.opacity"})
    {
        const auto requested = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Dock, focus);
        const auto routed = snowdesktop::CanonicalizeSettingsRoute(Unpack<snowdesktop::SettingsRoute>(Pack(requested)));
        Check(routed.page == snowdesktop::SettingsPage::AppearanceDesktopIcons && routed.focusId == focus,
            "pin-border search and IPC focus resolve to global appearance independently of the starting page");
    }
    {
        snowdesktop::StatusBarSettings statusBar;
        Check(!statusBar.showOnlyWhenSummoned && statusBar.edgeRevealGesture == 1,
            "independent bars default to persistent display and hover reveal");
        for (const bool summoned : {false, true}) for (const int gesture : {0, 1})
        {
            auto selected = statusBar;
            selected.showOnlyWhenSummoned = summoned;
            selected.edgeRevealGesture = gesture;
            Check(Unpack<snowdesktop::StatusBarSettings>(Pack(selected)) == selected,
                "both reveal choices cross the settings process IPC boundary without loss");
            JsonValue stored;
            snowdesktop::StatusBarSettings restored;
            Check(ParseJson(snowdesktop::EncodeStatusBarSettings(selected), stored) &&
                snowdesktop::DecodeStatusBarSettings(stored, restored) &&
                restored.showOnlyWhenSummoned == summoned && restored.edgeRevealGesture == gesture,
                "all independent reveal preferences survive persistence round trips");
        }
        for (const auto* json : {"{\"showOnlyWhenSummoned\":1}", "{\"edgeRevealGesture\":true}",
            "{\"edgeRevealGesture\":-1}", "{\"edgeRevealGesture\":2}", "{\"edgeRevealGesture\":0.5}"})
        {
            JsonValue invalid;
            const auto before = statusBar;
            Check(ParseJson(json, invalid) && !snowdesktop::DecodeStatusBarSettings(invalid, statusBar) && statusBar == before,
                "invalid reveal settings are rejected without partially replacing valid preferences");
        }
        for (const bool show : {false, true}) for (const bool single : {false, true})
        {
            auto selected = statusBar;
            selected.controlCenter = show; selected.controlCenterSingleIcon = single;
            selected.notifications = !show; selected.clock = !single;
            Check(Unpack<snowdesktop::StatusBarSettings>(Pack(selected)) == selected,
                "bar display choices cross the real settings serialization boundary independently");
        }
        Check(!statusBar.batteryPercentage, "new profiles keep battery percentage hidden");
        JsonValue legacy;
        Check(ParseJson("{\"enabled\":true}", legacy), "legacy status bar JSON parses");
        statusBar.batteryPercentage = true;
        statusBar.showOnlyWhenSummoned = true; statusBar.edgeRevealGesture = 0;
        Check(snowdesktop::DecodeStatusBarSettings(legacy, statusBar) && !statusBar.batteryPercentage &&
            !statusBar.showOnlyWhenSummoned && statusBar.edgeRevealGesture == 1,
            "existing profiles without the new flag retain compact control-center icons");
        for (const bool enabled : {false, true})
        {
            statusBar.batteryPercentage = enabled;
            JsonValue stored;
            snowdesktop::StatusBarSettings decoded;
            Check(ParseJson(snowdesktop::EncodeStatusBarSettings(statusBar), stored) &&
                    stored.Find("batteryPercentage") && stored.Find("batteryPercentage")->IsBoolean() &&
                    stored.Find("batteryPercentage")->boolean == enabled &&
                    snowdesktop::DecodeStatusBarSettings(stored, decoded) &&
                    decoded.batteryPercentage == enabled && decoded.enabled == statusBar.enabled &&
                    decoded.controlCenter == statusBar.controlCenter,
                "both battery percentage preferences persist without changing bar or control-center visibility");
            // Theme JSON canonicalizes a derived preset ID. IPC must preserve
            // the decoded value exactly, without treating that migration as loss.
            Check(Unpack<snowdesktop::StatusBarSettings>(Pack(decoded)) == decoded,
                "IPC preserves the decoded percentage preference and every other status-bar field");
        }
        const auto before = statusBar;
        JsonValue invalid;
        Check(ParseJson("{\"batteryPercentage\":1}", invalid) &&
                !snowdesktop::DecodeStatusBarSettings(invalid, statusBar) && statusBar == before,
            "invalid percentage preference cannot partially overwrite the current settings");
    }
    snowdesktop::PageLayoutSnapshot pages;
    pages.revision = 123;
    pages.monitorCount = 1;
    pages.editable = true;
    snowdesktop::PageLayoutEntry page;
    page.id = L"__page:2";
    page.name = L"工作 \U0001F30F & Play";
    page.guideCount = 1;
    page.widgetCount = 2;
    pages.pages.push_back(page);
    Check(Unpack<snowdesktop::PageLayoutSnapshot>(Pack(pages)) == pages,
        "settings IPC must preserve custom page names, guide counts and mutation availability");
    const snowdesktop::PageRemovalImpact removal{true, 2, 3, 1};
    const auto decodedRemoval = Unpack<snowdesktop::PageRemovalImpact>(Pack(removal));
    Check(decodedRemoval.valid && decodedRemoval.itemCount == 2 && decodedRemoval.widgetCount == 3 &&
        decodedRemoval.addedPageCount == 1 && decodedRemoval.RequiresConfirmation(),
        "the deletion confirmation must receive the host's real content and capacity impact");
    snowdesktop::SettingsHostActions::Request companion;
    companion.action = snowdesktop::SettingsHostActions::Action::ApplyDesktopStylePreset;
    companion.value = L"taskbar-dock";
    companion.desktopStyleDockPosition = DockPosition::Right;
    companion.desktopStyleDockAttached = true;
    const auto companionWire = Unpack<snowdesktop::SettingsHostActions::Request>(Pack(companion));
    Check(companionWire.action == companion.action && companionWire.value == companion.value &&
        companionWire.desktopStyleDockPosition == DockPosition::Right && companionWire.desktopStyleDockAttached,
        "preset position and form survive the settings-process action transport together");
    DockSettings dockSpace;
    for (int mode = 0; mode < 4; ++mode)
    {
        snowdesktop::SetTaskbarDisplayMode(dockSpace, mode);
        const auto restoredDock = Unpack<DockSettings>(Pack(dockSpace));
        Check(restoredDock == dockSpace && snowdesktop::TaskbarDisplayMode(restoredDock) == mode,
            "all taskbar visibility modes retain their monitor scope across settings IPC");
    }
    snowdesktop::SetTaskbarDisplayMode(dockSpace, 0);
    Check(!dockSpace.startMenuKeepTaskbarHidden, "native Start behavior remains the default");
    for (const bool enabled : {false, true})
    {
        DockSettings start = dockSpace;
        start.startMenuKeepTaskbarHidden = enabled;
        Check(Unpack<DockSettings>(Pack(start)) == start,
            "Dock Start placement preference survives settings-process IPC");
    }
    dockSpace.edgeDistanceScale = 5.f;
    dockSpace.reserveScreenSpace = true;
    dockSpace.lastMonitorUseHomeSize = false;
    dockSpace.mergedBarHeight = 64;
    dockSpace.thicknessScale = 2.f;
    dockSpace.mergedIconScale = 0.75f;
    dockSpace.followComponentCornerRadius = false;
    dockSpace.cornerRadius = 23.f;
    Check(Unpack<DockSettings>(Pack(dockSpace)) == dockSpace,
        "system Dock space reservation must cross the settings-process boundary");
    for (const bool enabled : {true, false})
    {
        dockSpace.singleClickLaunchItems = enabled;
        dockSpace.summonOnAppMinimize = enabled;
        dockSpace.followComponentCornerRadius = enabled;
        const auto restoredDock = Unpack<DockSettings>(Pack(dockSpace));
        Check(restoredDock.summonOnAppMinimize == enabled,
            "minimize summon can be enabled and disabled through the settings process");
        Check(restoredDock.followComponentCornerRadius == enabled && restoredDock.cornerRadius == 23.f,
            "Dock radius inheritance and its retained independent value reach the host through settings IPC");
        Check(Unpack<DockSettings>(Pack(dockSpace)).singleClickLaunchItems == enabled,
            "enabling and disabling single-click launch must reach the desktop host through settings IPC");
    }
    const auto styleRoute = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::DesktopStyle);
    const auto restoredRoute = Unpack<snowdesktop::SettingsRoute>(Pack(styleRoute));
    Check(restoredRoute.IsValid() && restoredRoute.page == snowdesktop::SettingsPage::DesktopStyle,
        "desktop style navigation retains its appended page identity across IPC");
    snowdesktop::runtime_modules::Snapshot moduleStatus;
    moduleStatus.policyRevision = 7;
    snowdesktop::runtime_modules::Status resident;
    resident.module = snowdesktop::runtime_modules::Module::Taskbar;
    resident.allowed = false; resident.active = 0; resident.residencyKnown = false;
    resident.targets.push_back({123, L"explorer.exe", true}); moduleStatus.modules.push_back(resident);
    const auto statusCopy = Unpack<snowdesktop::runtime_modules::Snapshot>(Pack(moduleStatus));
    Check(statusCopy.policyRevision == 7 && statusCopy.modules.size() == 1 && !statusCopy.modules[0].allowed &&
        !statusCopy.modules[0].active && !statusCopy.modules[0].residencyKnown && statusCopy.modules[0].targets[0].explorer && statusCopy.modules[0].targets[0].process == 123,
        "disabled behavior, unknown residency and an Explorer image remain distinct across IPC");
    GeneralSettings moduleSettings;
    moduleSettings.showAdvancedSettings = true;
    moduleSettings.runtimeModules.allowed = {false, true, false, false, true};
    const auto moduleCopy = Unpack<GeneralSettings>(Pack(moduleSettings));
    Check(moduleCopy.showAdvancedSettings && moduleCopy.runtimeModules == moduleSettings.runtimeModules,
        "five independent module policies and About visibility cross the private settings process boundary");
    GeneralSettings extensions;
    for (const auto& descriptor : snowdesktop::diagnostics::LogModules)
    {
        extensions.diagnosticLogModules = snowdesktop::diagnostics::ModuleBit(descriptor.module);
        Check(Unpack<GeneralSettings>(Pack(extensions)).diagnosticLogModules == extensions.diagnosticLogModules,
            "each independent logging switch crosses the settings-process IPC boundary");
    }
    for (const bool enabled : {false, true})
    for (const UINT key : {UINT{'R'}, UINT{0}})
    {
        extensions.forceRestartHotkeyEnabled = enabled;
        extensions.forceRestartHotkeyModifiers = MOD_CONTROL | MOD_SHIFT;
        extensions.forceRestartHotkeyVirtualKey = key;
        const auto copy = Unpack<GeneralSettings>(Pack(extensions));
        Check(copy.forceRestartHotkeyEnabled == enabled &&
            copy.forceRestartHotkeyModifiers == (MOD_CONTROL | MOD_SHIFT) &&
            copy.forceRestartHotkeyVirtualKey == key,
            "force restart enablement, custom chord and clearing cross settings IPC");
    }
    for (const bool enabled : {false, true})
    {
        extensions.softwareDesktopHotkeyEnabled = enabled;
        extensions.softwareDesktopHotkeyModifiers = MOD_CONTROL | MOD_SHIFT;
        extensions.softwareDesktopHotkeyVirtualKey = 'Q';
        const auto hotkey = Unpack<GeneralSettings>(Pack(extensions));
        Check(hotkey.softwareDesktopHotkeyEnabled == enabled &&
            hotkey.softwareDesktopHotkeyModifiers == (MOD_CONTROL | MOD_SHIFT) &&
            hotkey.softwareDesktopHotkeyVirtualKey == 'Q',
            "switch shortcut enablement and chosen chord cross the settings-process boundary");
    }
    // Each option must independently reach the desktop host from settings.
    for (int flags = 0; flags < 8; ++flags)
    {
        extensions.contextMenuExpandQuickActions = (flags & 1) != 0;
        extensions.contextMenuHidePageManagement = (flags & 2) != 0;
        extensions.contextMenuHideMoreManagement = (flags & 4) != 0;
        const auto restored = Unpack<GeneralSettings>(Pack(extensions));
        Check(restored.contextMenuExpandQuickActions == extensions.contextMenuExpandQuickActions &&
            restored.contextMenuHidePageManagement == extensions.contextMenuHidePageManagement &&
            restored.contextMenuHideMoreManagement == extensions.contextMenuHideMoreManagement,
            "independent context-menu options cross the settings-process boundary");
    }
    extensions.statusBar.enabled = true;
    extensions.statusBar.position = DockPosition::Left;
    extensions.statusBar.monitorScope = DockMonitorScope::All;
    extensions.statusBar.pinnedTrayItems = {"guid:test"};
    extensions.statusBar.menu = false;
    extensions.statusBar.quickSearch = false;
    extensions.statusBar.leftOrder = {"quickSearch", "menu"};
    std::reverse(extensions.statusBar.rightOrder.begin(), extensions.statusBar.rightOrder.end());
    extensions.statusBar.noWindow.enabled = true;
    extensions.statusBar.noWindow.theme.mode = snowdesktop::kStatusBarThemeTransparentDarkText;
    extensions.statusBar.legacyShellUi.enabled = true;
    extensions.statusBar.legacyShellUi.theme.mode = 5;
    extensions.statusBar.maximizedWindow.enabled = true;
    extensions.statusBar.maximizedWindow.theme.mode = -1;
    extensions.statusBar.legacyVisibleWindow.theme.mode = 4;
    extensions.statusBar.legacyVisibleWindow.theme.customized = true;
    extensions.statusBar.legacyVisibleWindow.theme.appearance.widgetAlpha = .37f;
    Check(Unpack<GeneralSettings>(Pack(extensions)).statusBar == extensions.statusBar,
        "settings process must preserve new active rules and retired scene data across its wire boundary");
    snowdesktop::VisitStatusBarAppearanceRules([&](const char*, auto member) {
        for (const bool enabled : {false, true})
        {
            auto changed = extensions;
            (changed.statusBar.*member).enabled = enabled;
            (changed.statusBar.*member).theme.mode = snowdesktop::kStatusBarThemeTransparentLightText;
            Check(Unpack<GeneralSettings>(Pack(changed)).statusBar == changed.statusBar,
                "both active scene overrides and transparent themes survive IPC without changing legacy data");
        }
    });
    snowdesktop::VisitStatusBarFlags([&](const char*, auto member) {
        auto changed = extensions; changed.statusBar.*member = !(changed.statusBar.*member);
        Check(Unpack<GeneralSettings>(Pack(changed)).statusBar == changed.statusBar,
            "each status bar flag must survive the settings process boundary in both states");
    });
    extensions.shellExtensions = {true, {{"handler:{test}", "compress", "压缩", snowdesktop::shell_extensions::Placement::Root}}};
    extensions.shellExtensions.hidden = {{"verb:sevenzip", snowdesktop::shell_extensions::Context::Folder},
        {"verb:editor", snowdesktop::shell_extensions::Context::Desktop}};
    extensions.shellExtensions.shown = {
        {"verb:sevenzip", snowdesktop::shell_extensions::Context::File},
        {"verb:editor", snowdesktop::shell_extensions::Context::FolderBackground}};
    using namespace snowdesktop::shell_extensions;
    SetCommon(extensions.shellExtensions, "clsid:test", Category::Background, true);
    SetOverride(extensions.shellExtensions, "clsid:test", Context::Desktop, Visibility::Hide);
    CatalogueView menuView;
    menuView.selection.paths = {L"C:\\测试.pdf", L"C:\\other.png"}; menuView.selection.extended = true;
      Registration registration; registration.id = "reg:test"; registration.contexts = 3; registration.linked = false;
    registration.display.label = L"Type command"; registration.sources = {L".pdf\\shell\\test"};
    registration.commandIdentity = std::string("command\0arguments", 17);
    registration.application = {"file:test-provider", L"Provider application"};
    menuView.catalogue.rows = {registration};
    const auto menuRestored = Unpack<CatalogueView>(Pack(menuView));
    Check(menuRestored.selection == menuView.selection && menuRestored.catalogue.rows.size() == 1 &&
          menuRestored.catalogue.rows[0].id == registration.id && !menuRestored.catalogue.rows[0].linked &&
          menuRestored.catalogue.rows[0].commandIdentity == registration.commandIdentity &&
          menuRestored.catalogue.rows[0].application == registration.application,
          "private menu IPC retains mixed selection, Shift and pending registration identity");
    Check(Unpack<GeneralSettings>(Pack(extensions)).shellExtensions == extensions.shellExtensions,
          "explicit visibility and legacy exclusions cross settings IPC without losing identity or context");
    snowdesktop::winui::HomeAboutStatusPatch guide;
    guide.generation = 71; guide.revision = 9;
    guide.usageGuideExpanded = false;
    guide.debugProfileEnabled = true;
    guide.debugDataDirectory = L"D:\\data.debug\\data";
    guide.debugDesktopDirectory = L"D:\\模拟桌面";
    const auto restoredGuide = Unpack<snowdesktop::winui::HomeAboutStatusPatch>(Pack(guide));
    Check(restoredGuide.debugProfileEnabled == guide.debugProfileEnabled &&
        restoredGuide.debugDataDirectory == guide.debugDataDirectory &&
        restoredGuide.debugDesktopDirectory == guide.debugDesktopDirectory,
        "debug profile status crosses the process boundary without losing paths or mode");
    Check(restoredGuide.generation == 71 && restoredGuide.revision == 9 &&
        restoredGuide.usageGuideExpanded == false,
        "host fold preference reaches the guide without inventing tutorial state");
    NavigationSettings navigation; navigation.lastCollapsed = true; navigation.layout.collapsedWidth = 700; navigation.colors["searchText"] = "#123456"; navigation.prefixes[0] = "applications";
    navigation.desktopViewMode = QuickNavigationDesktopViewMode::Frequency;
    navigation.appSuggestionsEnabled = true;
    navigation.mappingViewMode = QuickNavigationDesktopViewMode::Tile;
    navigation.widgetSortByInitial = {{"collection-a", true}, {"collection-b", false}};
    navigation.engines.push_back({"custom","Custom","custom","https://example.com/?q={query}"});
    Check(Unpack<NavigationSettings>(Pack(navigation)) == navigation,"all navigation settings survive the private process boundary");
    Check(Unpack<snowdesktop::SettingsRoute>(Pack(snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::QuickNavigation,"quickNav.layout.iconSize"))).page == snowdesktop::SettingsPage::QuickNavigation,"the appended navigation page crosses private IPC");
    Check(!CategorySettings{}.collectProgramsEnabled, "program collection is disabled by default");
    for (const bool enabled : {false, true})
    {
        CategorySettings categories;
        categories.tabFontSize = 18.5f;
        categories.collectProgramsEnabled = enabled;
        categories.rules = {{L"programs", L"程序 Programs", L"EXE LNK URL"},
            {L"documents", L"资料 Documents", L"PDF DOCX", false}};
        const auto restored = Unpack<CategorySettings>(Pack(categories));
        Check(restored.collectProgramsEnabled == enabled && restored.tabFontSize == categories.tabFontSize &&
            restored.rules.size() == categories.rules.size() &&
            restored.rules[0].id == categories.rules[0].id && restored.rules[0].customLabel == categories.rules[0].customLabel &&
            restored.rules[0].extensions == categories.rules[0].extensions &&
            restored.rules[1].id == categories.rules[1].id && restored.rules[1].customLabel == categories.rules[1].customLabel &&
            restored.rules[1].extensions == categories.rules[1].extensions,
            "category rules retain text across private IPC");
        Check(restored.rules[0].enabled && !restored.rules[1].enabled,
            "both program collection states preserve category identity, labels, extensions and font size across private IPC");
    }
    // These types exercise Unicode paths, optional values, wide counters and
    // nested metadata used by settings and component editor snapshots.
    using Value = std::tuple<std::wstring, std::uint64_t,
        std::vector<std::optional<std::string>>, std::map<std::string, double>>;
    const Value original{L"中文 / 日本語 / \U0001F3B5", UINT64_MAX,
        {"", std::nullopt, std::string("a\0b", 3)}, {{"scale", 0.75}}};
    auto encoded = Pack(original);
    Check(Unpack<Value>(encoded) == original, "IPC value round trip preserves Unicode and exact counters");
    for (std::size_t size = 0; size < encoded.size(); ++size)
        Reject([&] { (void)Unpack<Value>(std::span(encoded).first(size)); },
            "truncated snapshots must be rejected before dispatch");
    encoded.push_back(std::byte{});
    Reject([&] { (void)Unpack<Value>(encoded); }, "trailing snapshot fields reject protocol mismatch");
    Reject([] { (void)Unpack<bool>(Pack(std::uint8_t{2})); }, "invalid boolean is rejected");
    Reject([] { (void)Unpack<std::string>(Pack(UINT32_MAX)); }, "oversized string rejected before allocation");
    Reject([] { (void)Unpack<std::vector<std::string>>(Pack(UINT32_MAX)); },
        "oversized collection rejected before allocation");
    Reject([] { (void)Pack(std::numeric_limits<double>::infinity()); },
        "nonfinite slider values cannot cross IPC");
    Reject([] { (void)Unpack<std::filesystem::path>(Pack(std::wstring(L"a\0b", 3))); },
        "embedded null paths cannot change the filesystem operation target");
    Reject([] { (void)Unpack<std::map<std::string, int>>(Pack(std::uint32_t{2},
        std::string("a"), 1, std::string("a"), 2)); }, "duplicate metadata keys rejected");

    snowdesktop::SettingsSnapshot settings;
    settings.generation = 17;
    settings.route = snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Dock, "dock.position");
    settings.route.guideTopic = "dockPosition";
    const auto guideRoute = Unpack<snowdesktop::SettingsRoute>(Pack(settings.route));
    Check(guideRoute.guideTopic == "dockPosition" && guideRoute.focusId == "dock.position" &&
        guideRoute.page == snowdesktop::SettingsPage::Dock,
        "cross-process guide navigation preserves both the setting and return destination");
    settings.revision = UINT64_MAX;
    settings.externalReplacementPending = true;
    settings.values.general.animationMode = 1;
    settings.values.general.popupAnimationEffect = 1;
    settings.values.general.animationSpeed = 5;
    settings.values.general.animationFrameLimit = 120;
    settings.values.general.animationEnergySaver = false;
    settings.values.general.animationOnBattery = true;
    settings.values.general.quickNavigationAppearance.mode = 4;
    settings.values.general.quickNavigationAppearance.customized = true;
    settings.values.general.quickNavigationAppearance.appearance.panelGradient.enabled = true;
    settings.values.general.quickNavigationAppearance.appearance.panelGradient.angle = 37;
    settings.values.general.collectionPopupAppearance.mode = 3;
    settings.values.dock.followComponentAppearance = false;
    settings.values.dock.appearancePreset = kAppearancePresetGlassLight;
    settings.values.dock.customAppearance.widgetBgR = .2f;
    settings.values.dock.fullscreenPolicy = DockFullscreenPolicy::FullProtection;
    settings.values.dock.fullscreenExceptions = {{L"C:\\Apps\\播放器\\Player.exe", DockFullscreenPolicy::Allow},
        {L"D:\\Browser\\browser.exe", DockFullscreenPolicy::BlockGestures}};
    settings.values.dock.edgeRevealGesture = 1;
    settings.values.dock.edgeHoverDelayMs = 850;
    settings.values.dock.trayApplications = {{L"QQ.exe", false, true, false},
        {L"D:\\应用\\Weixin.exe", true, false, true}};
    settings.values.dock.showWindowPreviews = false;
    settings.values.dock.hoverEffect = 1;
    settings.values.dock.hoverScale = 1.75f;
    settings.values.dock.launchEffect = 2;
    settings.values.dock.windowEffect = 4;
    settings.values.dock.suppressSystemTaskbar = true;
    settings.values.dock.showWindowsButton = false;
    settings.values.dock.showQuickSearchButton = false;
    settings.values.general.language[0] = 'z';
    settings.values.general.language[1] = 'h';
    settings.values.general.language[2] = '\0';
    settings.values.dock.systemTaskbarShellUi.enabled = true;
    settings.values.dock.systemTaskbarShellUi.appearance.widgetEdgeHighlightWidth = 3.5f;
    settings.values.desktop.iconBeautify.filterTintR = 0.123f;
    settings.values.desktop.iconBeautify.preset = snowdesktop::IconBeautifyPreset::FrostedGlass;
    settings.values.desktop.iconBeautify.glassEnabled = true;
    settings.values.desktop.iconBeautify.glassBlurRadius = 28.0f;
    settings.values.desktop.iconBeautify.edgeHighlightEnabled = true;
    settings.values.desktop.iconBeautify.edgeHighlightWidth = 1.7f;
    settings.values.desktop.iconBeautify.edgeHighlightStrength = .65f;
    settings.values.desktop.iconBeautify.mode = 1;
    settings.values.desktop.iconBeautify.backgroundSource = 2;
    settings.values.desktop.iconBeautify.filterMode = 1;
    settings.values.desktop.iconBeautify.backgroundGradient = {true, 125, .2, .9,
        {{0, 0x113355, .4}, {.6, 0x7799aa, .8}, {1, 0xaaccff, 1}}};
    settings.values.category.rules.push_back({L"中文", L"文档", L"txt,md"});
    settings.values.personalization.panelGradient.enabled = true;
    settings.values.personalization.showGroupTabCounts = true;
    settings.values.personalization.scrollableTitleBarOnTop = true;
    settings.values.personalization.widgetTransformCursors = false;
    settings.values.personalization.windowPinBorderEnabled = false;
    settings.values.personalization.windowPinBorderR = 0.3f;
    settings.values.personalization.windowPinBorderG = 0.6f;
    settings.values.personalization.windowPinBorderB = 0.9f;
    settings.values.personalization.windowPinBorderWidth = 18.0f;
    settings.values.personalization.windowPinBorderOpacity = 0.7f;
    settings.values.personalization.popupHoverOpen = true;
    settings.values.personalization.popupCloseOnMouseLeave = true;
    settings.values.personalization.popupHoverDelayMs = 1700.0f;
    settings.values.personalization.groupTabHoverSwitch = true;
    settings.values.personalization.categoryTabHoverSwitch = true;
    settings.values.personalization.tabHoverDelayMs = 250.0f;
    settings.values.personalization.showCategoryTabCounts = false;
    settings.values.personalization.panelGradient.angle = 213;
    settings.values.personalization.panelGradient.stops.insert(
        settings.values.personalization.panelGradient.stops.begin() + 1, {.37, 0xaabbcc, .1});
    settings.values.general.font = {"user-package", "中文 Family"};
    settings.values.general.tooltipFontSize = 22;
    settings.values.desktop.itemFontWeight = 600;
    settings.values.desktop.fanTitleFontSizeCu = 20.5f;
    settings.values.desktop.desktopTitleLines = 1;
    settings.values.desktop.largeFolderTitleLines = 2;
    settings.values.desktop.scrollingTitleLines = 1;
    settings.values.desktop.titleEllipsis = false;
    settings.values.desktop.groupComponentBadges = false;
    settings.values.dock.attentionDotSizeScale = 1.75f;
    settings.values.dock.attentionDotOffset = -0.18f;
    settings.values.desktop.scrollingIconSpacingYScale = 5.0f;
    settings.values.desktop.listRowSpacingScale = 4.5f;
    const auto restored = Unpack<snowdesktop::SettingsSnapshot>(Pack(settings));
    Check(restored.values.dock.attentionDotSizeScale == 1.75f &&
            restored.values.dock.attentionDotOffset == -0.18f,
        "Dock attention dot size and position cross the settings-process boundary");
    Check(!restored.values.desktop.groupComponentBadges,
        "disabled group-component badges survive the settings-process boundary");
    Check(restored.values.desktop.scrollingIconSpacingYScale == 5.0f &&
            restored.values.desktop.listRowSpacingScale == 4.5f,
        "scrolling and list vertical spacing above 200 percent cross the settings-process boundary");
    Check(restored.values.dock.windowEffect == 4,
          "attraction selection survives the independent settings process boundary");
    Check(!restored.values.personalization.windowPinBorderEnabled &&
            restored.values.personalization.windowPinBorderR == 0.3f &&
            restored.values.personalization.windowPinBorderG == 0.6f &&
            restored.values.personalization.windowPinBorderB == 0.9f &&
            restored.values.personalization.windowPinBorderWidth == 18.0f &&
            restored.values.personalization.windowPinBorderOpacity == 0.7f,
        "global pin border visibility and style cross the independent settings-process boundary");
    Check(restored.values.general.font == settings.values.general.font &&
        restored.values.general.tooltipFontSize == 22 &&
        restored.values.desktop.itemFontWeight == 600.f &&
        restored.values.desktop.fanTitleFontSizeCu == 20.5f &&
        restored.values.desktop.desktopTitleLines == 1 &&
        restored.values.desktop.largeFolderTitleLines == 2 &&
        restored.values.desktop.scrollingTitleLines == 1 && !restored.values.desktop.titleEllipsis,
        "font choice, title limits and direct truncation cross settings IPC without rewriting the original weight");
    Check(restored.values.desktop.iconBeautify.preset == snowdesktop::IconBeautifyPreset::FrostedGlass &&
        restored.values.desktop.iconBeautify.glassEnabled && restored.values.desktop.iconBeautify.glassBlurRadius == 28.0f &&
        restored.values.desktop.iconBeautify.edgeHighlightEnabled && restored.values.desktop.iconBeautify.edgeHighlightWidth == 1.7f &&
        restored.values.desktop.iconBeautify.edgeHighlightStrength == .65f,
        "glass, blur and directional reflection survive the settings process boundary");
    Check(restored.values.desktop.iconBeautify.mode == 1 && restored.values.desktop.iconBeautify.backgroundSource == 2 &&
        restored.values.desktop.iconBeautify.filterMode == 1 && restored.values.desktop.iconBeautify.backgroundGradient == settings.values.desktop.iconBeautify.backgroundGradient,
        "local processing strategy, automatic color source and every gradient stop cross settings IPC");
    for (const auto preset : {snowdesktop::IconBeautifyPreset::FrostedGlassDark, snowdesktop::IconBeautifyPreset::FrostedGlassLight})
    {
        auto variant = settings;
        variant.values.desktop.iconBeautify.preset = preset;
        const auto received = Unpack<snowdesktop::SettingsSnapshot>(Pack(variant));
        Check(received.values.desktop.iconBeautify.preset == preset &&
            received.values.desktop.iconBeautify.edgeHighlightWidth == 1.7f &&
            received.values.desktop.iconBeautify.glassBlurRadius == 28.f,
            "new glass preset IDs and editable values survive settings IPC unchanged");
    }
    Check(restored.values.dock.edgeRevealGesture == 1 && restored.values.dock.edgeHoverDelayMs == 850 &&
            !restored.values.dock.showWindowPreviews,
        "hover reveal, custom dwell delay and disabled task thumbnails survive settings IPC");
    Check(restored.values.dock.trayApplications == settings.values.dock.trayApplications,
        "tray matching mode, Unicode path, opt-out and click gesture survive IPC");
    auto noTray = settings.values.dock;
    noTray.trayApplications.clear();
    Check(Unpack<DockSettings>(Pack(noTray)).trayApplications.empty(),
        "an explicitly cleared tray list stays empty across IPC");
    Check(restored.values.dock.fullscreenPolicy == settings.values.dock.fullscreenPolicy &&
        restored.values.dock.fullscreenExceptions == settings.values.dock.fullscreenExceptions,
        "fullscreen policies and Unicode executable exceptions survive IPC");
    snowdesktop::SettingsActionResult runningApplications;
    runningApplications.dockApplications = {{L"播放器", L"C:\\Apps\\播放器\\Player.exe"},
        {L"Browser", L"D:\\Browser\\browser.exe"}};
    const auto runningCopy = Unpack<snowdesktop::SettingsActionResult>(Pack(runningApplications));
    Check(runningCopy.dockApplications == runningApplications.dockApplications,
        "existing Dock running-zone names and executable paths reach the exception selector through private IPC");
    Check(restored.values.dock.suppressSystemTaskbar && !restored.values.dock.showWindowsButton &&
            !restored.values.dock.showQuickSearchButton,
        "suppression crosses private settings IPC without replacing the base Windows button preference");
    for (const int theme : {-1, 0, 1})
    {
        auto themeSettings = settings;
        themeSettings.values.dock.classicTaskbarSystemTheme = theme;
        const auto themeRestored = Unpack<snowdesktop::SettingsSnapshot>(Pack(themeSettings));
        Check(themeRestored.values.dock.classicTaskbarSystemTheme == theme,
            "Win10 automatic and manual shell choices survive the settings process boundary");
    }
    Check(restored.values.personalization.showGroupTabCounts &&
            restored.values.personalization.scrollableTitleBarOnTop &&
            restored.values.personalization.popupHoverOpen &&
            restored.values.personalization.popupCloseOnMouseLeave &&
            restored.values.personalization.popupHoverDelayMs == 1700.0f &&
            !restored.values.personalization.showCategoryTabCounts,
        "group tab counts cross the settings process boundary independently from category counts");
    Check(!restored.values.personalization.widgetTransformCursors,
        "disabling component transform cursors survives the settings process boundary despite the enabled default");
    Check(restored.values.personalization.groupTabHoverSwitch &&
            restored.values.personalization.categoryTabHoverSwitch &&
            restored.values.personalization.tabHoverDelayMs == 250.0f,
        "independent group/category hover switches and their shared delay cross the settings process boundary");
    for (const int style : {5, 6})
    {
        auto menuSettings = settings;
        menuSettings.values.personalization.contextMenuStyle = style;
        const auto styleRestored =
            Unpack<snowdesktop::SettingsSnapshot>(Pack(menuSettings));
        Check(styleRestored.values.personalization.contextMenuStyle == style,
            "Win10 menu selection reaches the independent settings process intact");
    }
    Check(restored.values.general.quickNavigationAppearance == settings.values.general.quickNavigationAppearance &&
        restored.values.general.collectionPopupAppearance == settings.values.general.collectionPopupAppearance,
        "independent surface modes and custom gradients reach the settings process intact");
    Check(restored.values.general.animationMode == 1 && restored.values.general.popupAnimationEffect == 1 &&
              restored.values.general.animationSpeed == 5 && restored.values.general.animationFrameLimit == 120 &&
              !restored.values.general.animationEnergySaver && restored.values.general.animationOnBattery,
          "animation preferences reach the independent settings process intact");
    Check(restored.externalReplacementPending && restored.revision == UINT64_MAX &&
        restored.values.dock == settings.values.dock &&
        restored.values.desktop.iconBeautify.filterTintR == 0.123f &&
        restored.values.category.rules.front().customLabel == L"文档" &&
        std::string(restored.values.general.language) == "zh",
        "controller IPC preserves replacement marker, draft fields and language array");
    Check(restored.values.personalization.panelGradient == settings.values.personalization.panelGradient,
        "full-panel gradient stops, opacities and angle reach the settings process without flattening");

    using namespace snowdesktop::widget_runtime;
    snowdesktop::LargeIconSettingsSnapshot large;
    large.key = L"图标 / 日本語"; large.session = UINT64_MAX; large.revision = 47;
    large.config = snowdesktop::EncodeLargeIconConfig({}); large.landscapePath = L"file:///C:/图片/preview.png";
    large.hasEdgeColor = true; large.edgeColor = 0x119955; large.loading = true;
    large.portraitSource = "steam-local"; large.accent = 0x445566; large.steam = true;
    {
        snowdesktop::IconAppearanceSnapshot appearance; appearance.succeeded = true; appearance.independent = true;
        appearance.current.target = L"recycle-full:BIN"; appearance.current.settings.enabled = false;
        appearance.current.settings.mode = 1; appearance.current.themeId = "saved-icon-theme";
        appearance.entries = {appearance.current}; appearance.session = 9; appearance.revision = 11;
        appearance.previewPng = std::string("png\0bytes", 9);
        const auto copy = Unpack<snowdesktop::IconAppearanceSnapshot>(Pack(appearance));
        Check(copy.entries.size() == 1 && !copy.entries[0].settings.enabled && copy.entries[0].settings.mode == 1 &&
            copy.current.target == L"recycle-full:BIN" && copy.current.themeId == "saved-icon-theme" &&
            copy.session == 9 && copy.revision == 11 && copy.previewPng == appearance.previewPng && !copy.nativeControlSource,
            "per-icon settings IPC preserves explicit None, state identity, session guard and binary preview");
        appearance.nativeControlSource = true; appearance.current.target = L"control:search";
        const auto native = Unpack<snowdesktop::IconAppearanceSnapshot>(Pack(appearance));
        Check(native.nativeControlSource && native.current.target == L"control:search",
            "native source sizing metadata crosses the settings process without shrinking replacement images");
        appearance.current.followGlobalTheme = true;
        appearance.globalSettings.preset = snowdesktop::IconBeautifyPreset::FrostedGlassLight;
        appearance.globalSettings.enabled = true;
        appearance.globalSettings.backgroundStartR = 37;
        const auto inherited = Unpack<snowdesktop::IconAppearanceSnapshot>(Pack(appearance));
        Check(inherited.current.followGlobalTheme && inherited.globalSettings.preset == appearance.globalSettings.preset &&
            inherited.globalSettings.enabled && inherited.globalSettings.backgroundStartR == 37,
            "independent processing carries live global theme metadata across settings IPC");
        snowdesktop::IconAppearanceRequest followRequest; followRequest.followGlobalTheme = true; followRequest.settings.mode = 2;
        const auto followCopy = Unpack<snowdesktop::IconAppearanceRequest>(Pack(followRequest));
        Check(followCopy.followGlobalTheme && followCopy.settings.mode == 2,
            "applying theme inheritance preserves the separate processing strategy");
    }
    large.frameWidth = 436; large.frameHeight = 213; large.frameColumns = 4; large.frameRows = 2;
    large.unitScale = 1.5; large.durationScale = 2; large.frameLimit = 30; large.animations = false; large.neutral = 0x123456;
    large.frameWidths = {100, 212, 324, 436}; large.frameHeights = {100, 213};
    auto defaults = snowdesktop::LargeIconConfig{}; defaults.radius = 27; defaults.titleSize = 15;
    large.defaultConfig = snowdesktop::EncodeLargeIconConfig(defaults); large.imageWidth = 64; large.imageHeight = 48;
    large.itemCount = 3; large.anyFill = true; large.mixedFields = {"backgroundStyle", "effect"};
    const auto largeCopy = Unpack<snowdesktop::LargeIconSettingsSnapshot>(Pack(large));
    snowdesktop::IconLibrarySnapshot library;
    library.session = UINT64_MAX; library.revision = 27; library.busy = true; library.target = L"集合-雪";
    snowdesktop::IconLibraryEntry asset; asset.id = std::string(64,'a') + ".webp"; asset.name = L"自定义图标";
    asset.previewPath = L"C:\\图片\\资源 # %.png"; asset.width = 96; asset.height = 96; asset.uses = {L"集合 · Dock"};
    snowdesktop::IconLibraryGroup group; group.id = "group-" + std::string(32, '1'); group.name = L"订阅图标包"; group.source = "android-apk"; group.packageId = std::string(64,'b');
    asset.groupIds = {group.id}; library.groups.push_back(group); library.importedGroupId = group.id;
    library.workshopAvailable=true;library.coverPath=L"C:\\预览\\cover.png";library.preparationId="123-456";
    library.publishedUrl="https://steamcommunity.com/sharedfiles/filedetails/?id=777";library.operationId=UINT64_MAX;
    library.completedAction="prepareShare";library.groups[0].license=L"许可说明";library.groups[0].workshopItem="777";
    library.groups[0].accounts={"321","654"};asset.localOwned=false;asset.groupNames[group.id]=L"包内名称";
    library.entries.push_back(asset); library.failures = {L"无效图片.svg"};
    const auto libraryCopy = Unpack<snowdesktop::IconLibrarySnapshot>(Pack(library));
    Check(libraryCopy.workshopAvailable&&libraryCopy.coverPath==library.coverPath&&libraryCopy.preparationId==library.preparationId&&
        libraryCopy.operationId==UINT64_MAX&&libraryCopy.publishedUrl==library.publishedUrl&&libraryCopy.completedAction==library.completedAction&&
        libraryCopy.groups[0].license==library.groups[0].license&&libraryCopy.groups[0].accounts==library.groups[0].accounts&&
        !libraryCopy.entries[0].localOwned&&libraryCopy.entries[0].groupNames==asset.groupNames,
        "icon package ownership, license and frozen publication state survive settings IPC without image bytes");
    Check(libraryCopy.session == library.session && libraryCopy.revision == 27 && libraryCopy.busy &&
        libraryCopy.target == library.target && libraryCopy.entries.size() == 1 && libraryCopy.entries[0].id == asset.id &&
        libraryCopy.groups.size() == 1 && libraryCopy.groups[0].name == group.name && libraryCopy.groups[0].packageId == group.packageId &&
        libraryCopy.importedGroupId == group.id && libraryCopy.entries[0].groupIds == asset.groupIds && libraryCopy.entries[0].previewPath == asset.previewPath && libraryCopy.entries[0].uses == asset.uses && libraryCopy.failures == library.failures,
        "icon library identities, usage guards, session tokens and Unicode paths survive the production IPC codec");
    Check(largeCopy.key == large.key && largeCopy.session == UINT64_MAX && largeCopy.revision == 47 &&
        largeCopy.landscapePath == large.landscapePath && largeCopy.portraitSource == large.portraitSource &&
        largeCopy.hasEdgeColor && largeCopy.edgeColor == large.edgeColor && largeCopy.loading && largeCopy.config == large.config && largeCopy.steam && largeCopy.accent == large.accent &&
        largeCopy.frameWidth == 436 && largeCopy.frameHeight == 213 && largeCopy.frameColumns == 4 && largeCopy.frameRows == 2 &&
        largeCopy.unitScale == 1.5 && largeCopy.durationScale == 2 && largeCopy.frameLimit == 30 && !largeCopy.animations && largeCopy.neutral == large.neutral &&
        largeCopy.frameWidths == large.frameWidths && largeCopy.frameHeights == large.frameHeights &&
        largeCopy.defaultConfig == large.defaultConfig && largeCopy.imageWidth == 64 && largeCopy.imageHeight == 48 &&
        largeCopy.itemCount == 3 && largeCopy.anyFill && largeCopy.mixedFields == large.mixedFields,
        "large-icon editing guards, Unicode references and thumbnail provenance survive private IPC");
    WidgetSettingsSnapshot widget;
    widget.widgetId = L"music-1";
    widget.generation = 3;
    widget.revision = 9;
    widget.hostAppearance.panelGradient = settings.values.personalization.panelGradient;
    WidgetHostAppearancePatch gradientPatch;
    gradientPatch.panelGradient = widget.hostAppearance.panelGradient;
    Check(Unpack<WidgetHostAppearancePatch>(Pack(gradientPatch)) == gradientPatch && !gradientPatch.Empty(),
        "per-widget gradient patches preserve stops and are not mistaken for empty edits");
    WidgetSettingFieldState field;
    field.schema.rawType = "password";
    field.schema.key = "secret";
    field.currentValue = MakeWidgetSettingString("must-not-leave-host");
    field.defaultValue = field.currentValue;
    field.opaque.configured = true;
    field.opaque.secretLength = 31;
    field.opaque.displayLabel = "must-not-leave-host";
    widget.fields.push_back(field);
    PrepareWidgetSettingsSnapshot(widget);
    const auto wire = Pack(widget);
    const std::string serialized(reinterpret_cast<const char*>(wire.data()), wire.size());
    Check(serialized.find("must-not-leave-host") == std::string::npos &&
        Unpack<WidgetSettingsSnapshot>(wire) == widget,
        "prepared secret fields retain only opaque status across IPC");
    Check(Unpack<WidgetSettingsSnapshot>(wire).fields.front().opaque.secretLength == 31u,
        "ordinary snapshots carry mask length without plaintext");
    WidgetSettingSecretReadResult explicitRead{
        {WidgetSettingMutationStatus::Unchanged, 3, 9, {}, {}}, "offline-readback"};
    const auto decodedSecret = Unpack<WidgetSettingSecretReadResult>(Pack(explicitRead));
    Check(decodedSecret.result.Succeeded() && decodedSecret.plaintext == "offline-readback",
        "only the explicit secret-read result carries editor plaintext");
}

void TestCustomCategoryCreation()
{
    CategorySettings persisted;
    for (unsigned index = 1; index <= 32; ++index)
        persisted.rules.push_back({L"custom-winui-1-" + std::to_wstring(index),
            L"已有分类 " + std::to_wstring(index), L"TXT", index % 2 == 0});
    const auto original = persisted.rules;
    // Reopening a settings process resets its old generation/counter, but the
    // complete previous rules arrive over the actual production codec.
    for (unsigned session = 0; session < 4; ++session)
    {
        CategorySettings edit = Unpack<CategorySettings>(Pack(persisted));
        for (unsigned index = 0; index < 8; ++index)
        {
            const std::wstring label = L"新分类 日本語 " + std::to_wstring(session * 8 + index);
            const auto acknowledged = snowdesktop::AppendCustomCategoryRule(edit, label, L"PDF DOCX");
            Check(acknowledged.has_value(), "creating a category returns its actual inserted identity");
            if (!acknowledged) continue;
            const auto snapshot = Unpack<CategorySettings>(Pack(edit));
            unsigned matches = 0;
            for (const auto& rule : snapshot.rules)
                if (rule.id == *acknowledged)
                {
                    ++matches;
                    Check(rule.customLabel == label && rule.extensions == L"PDF DOCX" && rule.enabled,
                        "creation acknowledgement identifies the newly inserted row after IPC projection");
                }
            Check(matches == 1, "reopened settings never reuse an existing category identity");
        }
        persisted = Unpack<CategorySettings>(Pack(edit));
    }
    Check(persisted.rules.size() == original.size() + 32, "every successful creation is retained across settings sessions");
    for (std::size_t index = 0; index < original.size(); ++index)
        Check(persisted.rules[index].id == original[index].id &&
            persisted.rules[index].customLabel == original[index].customLabel &&
            persisted.rules[index].extensions == original[index].extensions &&
            persisted.rules[index].enabled == original[index].enabled,
            "new category creation preserves the identity, text, order and state of old rows");
}

void TestChannel()
{
    DWORD handlesBefore = 0;
    // Creating the first USER message queue can lazily initialize shared
    // Windows/CRT resources. Compare equal, warmed process states.
    for (int iteration = -1; iteration < 12; ++iteration)
    {
        if (iteration == 0) GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
        HANDLE mainRead = nullptr, uiWrite = nullptr, uiRead = nullptr, mainWrite = nullptr;
        if (!CreatePipe(&mainRead, &uiWrite, nullptr, 0) ||
            !CreatePipe(&uiRead, &mainWrite, nullptr, 0))
            throw std::runtime_error("test pipe creation failed");
        Channel parent;
        parent.Open(mainRead, mainWrite, CurrentProcessHandle());
        parent.Bind<int, int>("host.preview", [](int value) { return value + 1; });
        std::promise<HWND> started;
        std::atomic<bool> childFailed = false;
        std::thread child([&] {
            try
            {
                Channel ui;
                ui.Open(uiRead, uiWrite, CurrentProcessHandle());
                ui.Bind<int, int>("ui.flush", [&ui](int value) {
                    return ui.Call<int>("host.preview", value) + 1;
                });
                ui.Bind<std::string, std::string>("ui.snapshot", [](std::string value) { return value; });
                ui.Bind<snowdesktop::winui::GeneralAdvancedFeatureStatus,
                    snowdesktop::winui::GeneralAdvancedFeatureStatus>("ui.advanced-status", [](auto value) { return value; });
                ui.Bind<void>("ui.close", [] { PostQuitMessage(0); });
                started.set_value(ui.Window());
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0)
                    DispatchMessageW(&message);
            }
            catch (...) { childFailed = true; try { started.set_value(nullptr); } catch (...) {} }
        });
        Check(started.get_future().get() != nullptr, "settings endpoint starts");
        try
        {
            Check(parent.Call<int>("ui.flush", 40) == 42,
                "nested host callback completes without deadlocking owner STAs");
            const std::string large(2 * 1024 * 1024, 'x');
            Check(parent.Call<std::string>("ui.snapshot", large) == large,
                "snapshots larger than pipe buffer arrive intact");
            for (unsigned problem = 0;
                problem <= static_cast<unsigned>(snowdesktop::steam_bridge::SteamConnectionProblem::SteamworksUnavailable);
                ++problem)
            {
                snowdesktop::winui::GeneralAdvancedFeatureStatus value;
                value.state = snowdesktop::winui::GeneralAdvancedFeatureState::RegistrationFailed;
                value.failure = snowdesktop::winui::GeneralAdvancedFeatureFailure::BridgeError;
                value.connectionProblem = static_cast<snowdesktop::steam_bridge::SteamConnectionProblem>(problem);
                value.errorDetail = iteration % 2 ? L"DLL 初始化失败 / 日本語 / 126" : L"";
                value.bridgeAvailable = true; value.registered = true;
                value.validUntil = INT64_MAX; value.cardVisible = true; value.offerSteamStore = true;
                const auto copy = parent.Call<snowdesktop::winui::GeneralAdvancedFeatureStatus>("ui.advanced-status", value);
                Check(copy.state == value.state && copy.failure == value.failure &&
                    copy.connectionProblem == value.connectionProblem && copy.errorDetail == value.errorDetail &&
                    copy.bridgeAvailable == value.bridgeAvailable && copy.registered == value.registered &&
                    copy.validUntil == value.validUntil && copy.cardVisible == value.cardVisible &&
                    copy.offerSteamStore == value.offerSteamStore,
                    "every advanced-feature status field survives the real private IPC channel");
            }
            parent.Notify("ui.close");
            child.join();
            Reject([&] { (void)parent.Request("ui.flush", Pack(1), 1000); },
                "closed child cannot acknowledge an unsaved edit");
            parent.Close();
            bool finalized = false;
            Check(parent.Post([&] { finalized = true; }),
                "host completion queue survives settings disconnect");
            MSG message{};
            while (PeekMessageW(&message, parent.Window(), 0, 0, PM_REMOVE))
                DispatchMessageW(&message);
            Check(finalized, "durable backend completion runs after UI connection closes");
        }
        catch (const std::exception& error)
        {
            std::cerr << "IPC session iteration " << iteration << ": ";
            Check(false, error.what());
            PostThreadMessageW(GetThreadId(child.native_handle()), WM_QUIT, 0, 0);
            if (child.joinable()) child.join();
        }
        Check(!childFailed, "settings peer remained healthy");
    }
    DWORD handlesAfter = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    if (handlesAfter > handlesBefore)
        std::cerr << "IPC handles: " << handlesBefore << " -> " << handlesAfter << '\n';
    Check(handlesAfter <= handlesBefore, "repeated IPC sessions release pipes, threads, events and process handles");
}

void TestContextMenuInspectionLifetime()
{
    HANDLE clientRead = nullptr, serverWrite = nullptr, serverRead = nullptr, clientWrite = nullptr;
    if (!CreatePipe(&clientRead, &serverWrite, nullptr, 0))
        throw std::runtime_error("menu inspection pipe creation failed");
    if (!CreatePipe(&serverRead, &clientWrite, nullptr, 0))
    {
        CloseHandle(clientRead); CloseHandle(serverWrite);
        throw std::runtime_error("menu inspection reply pipe creation failed");
    }
    Channel client;
    client.Open(clientRead, clientWrite, CurrentProcessHandle());
    std::promise<bool> ready;
    std::atomic<bool> hostFailed = false;
    std::thread host([&] {
        try
        {
            Channel server;
            server.Open(serverRead, serverWrite, CurrentProcessHandle());
            bool active = false;
            unsigned inspections = 0, closures = 0;
            ContextMenuInspectionCallbacks callbacks;
            callbacks.inspect = [&](const snowdesktop::shell_extensions::Request& request, bool refresh) {
                active = true;
                snowdesktop::shell_extensions::CatalogueView view;
                view.selection = request;
                view.menu.revision = ++inspections;
                view.menu.pending = refresh;
                view.catalogue.revision = closures;
                return view;
            };
            callbacks.closed = [&] { active = false; ++closures; };
            BindContextMenuInspection(server, std::move(callbacks));
            server.Bind<std::pair<bool, unsigned>>("test.menu.state", [&] { return std::pair{active, closures}; });
            server.Bind<void>("test.menu.exit", [] { PostQuitMessage(0); });
            server.SetDisconnected([] { PostQuitMessage(0); });
            ready.set_value(true);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        }
        catch (...)
        {
            hostFailed = true;
            try { ready.set_value(false); } catch (...) {}
        }
    });
    const bool started = ready.get_future().get();
    try
    {
        Check(started, "the real menu-page backend pipe becomes ready");
        if (started)
        {
            auto remote = CreateContextMenuInspectionProxy(client);
            snowdesktop::shell_extensions::Request request;
            request.paths = {L"C:\\Private\\菜单范围"};
            request.context = snowdesktop::shell_extensions::Context::Folder;
            const auto first = remote.inspect(request, true);
            Check(first.selection == request && first.menu.revision == 1 && first.menu.pending && first.catalogue.revision == 0,
                "the production remote menu options preserve selection and refresh intent through actual pipes");
            remote.closed();
            Check(client.Call<std::pair<bool, unsigned>>("test.menu.state") == std::pair{false, 1u},
                "leaving the menu page delivers its close notification to the actual backend before the next request");
            const auto reopened = remote.inspect(request, false);
            Check(reopened.selection == request && reopened.menu.revision == 2 && !reopened.menu.pending && reopened.catalogue.revision == 1 &&
                    client.Call<std::pair<bool, unsigned>>("test.menu.state") == std::pair{true, 1u},
                "a later menu-page inspection reopens after the prior close rather than being closed by stale IPC");
            remote.closed();
            Check(client.Call<std::pair<bool, unsigned>>("test.menu.state") == std::pair{false, 2u},
                "each explicit menu-page departure reaches the host without another menu read");
        }
    }
    catch (const std::exception& error) { Check(false, error.what()); }
    try { if (client.Connected()) client.Notify("test.menu.exit"); } catch (...) {}
    client.Close();
    PostThreadMessageW(GetThreadId(host.native_handle()), WM_QUIT, 0, 0);
    host.join();
    Check(!hostFailed, "the menu lifetime backend remains healthy through close and reopen");
}

void TestIconLibraryPaging()
{
    // Real icon packs contain tens of thousands of entries. A single snapshot
    // exceeded the private frame limit and made a successfully imported pack
    // unavailable to the settings process. Keep the same frame bound and use
    // the production paging handlers over actual anonymous pipes.
    snowdesktop::IconLibrarySnapshot expected;
    expected.revision = 72; expected.session = 9; expected.succeeded = true;
    expected.target = L"item:paged"; expected.targetName = L"Target";
    expected.groups.push_back({"pack", L"Large pack"});
    for(unsigned i=0;i<1200;++i)
    {
        const auto suffix=std::to_string(i);
        snowdesktop::IconLibraryGroup group;group.id="group-"+std::string(32-suffix.size(),'0')+suffix;group.name=L"Same pack title";
        group.license=std::wstring(8000,L'a');group.workshopItem=std::to_string(100000+i);group.accounts={"321","654"};
        expected.groups.push_back(std::move(group));
    }
    Reject([&] { (void)Pack(expected.groups); },"valid per-pack licenses can exceed a frame even before adding icon entries");
    expected.imported = {"last"}; expected.importedGroupId = "pack";
    expected.failures = {L"unsupported resource"};
    expected.workshopAvailable=true;expected.coverPath=L"C:\\preview\\cover.png";expected.preparationId="456-789";expected.progress=2;
    expected.operationId=123;expected.completedAction="prepareShare";expected.publishedUrl="https://steamcommunity.com/sharedfiles/filedetails/?id=777";
    for (unsigned i = 0; i < 42000; ++i)
    {
        snowdesktop::IconLibraryEntry entry;
        entry.id = std::string(64, 'a') + std::to_string(i) + ".png";
        entry.name = L"Icon " + std::to_wstring(i);
        entry.previewPath = L"C:\\library\\" + std::wstring(256, L'x') + std::to_wstring(i) + L".png";
        entry.format = "png"; entry.width = 512; entry.height = 512;
        entry.packageId = std::string(64, 'b'); entry.groupIds = {"pack"};
        entry.uses = {L"Desktop"}; expected.entries.push_back(std::move(entry));
    }
    Reject([&] { (void)Pack(expected); }, "the real-size library fixture exceeds the old single-frame transport");
    HANDLE mainRead = nullptr, uiWrite = nullptr, uiRead = nullptr, mainWrite = nullptr;
    if (!CreatePipe(&mainRead, &uiWrite, nullptr, 0) || !CreatePipe(&uiRead, &mainWrite, nullptr, 0))
        throw std::runtime_error("paging pipes failed");
    Channel client; client.Open(mainRead, mainWrite, CurrentProcessHandle());
    std::promise<bool> ready; std::atomic<unsigned> operations = 0;
    std::thread host([&] {
        try
        {
            Channel server; server.Open(uiRead, uiWrite, CurrentProcessHandle());
            BindIconLibraryPages(server, [&](auto) { ++operations; return expected; });
            server.Bind<void>("paging.exit", [] { PostQuitMessage(0); });
            ready.set_value(true);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        }
        catch (...) { try { ready.set_value(false); } catch (...) {} }
    });
    const bool started = ready.get_future().get();
    try
    {
        Check(started, "paged library endpoint starts");
        if (started)
        {
            const auto result = CallIconLibraryPages(client, {});
            bool entriesMatch = result.entries.size() == expected.entries.size();
            if (entriesMatch) for (std::size_t i = 0; i < result.entries.size(); ++i)
                if (Pack(result.entries[i]) != Pack(expected.entries[i])) { entriesMatch = false; break; }
            bool groupsMatch=result.groups.size()==expected.groups.size();
            if(groupsMatch)for(std::size_t i=0;i<result.groups.size();++i)
                if(Pack(result.groups[i])!=Pack(expected.groups[i])){groupsMatch=false;break;}
            Check(entriesMatch && result.revision == 72 && result.session == 9 && result.succeeded &&
                result.target == expected.target && result.targetName == expected.targetName &&
                groupsMatch && result.imported == expected.imported &&
                result.importedGroupId == expected.importedGroupId && result.failures == expected.failures &&
                result.workshopAvailable && result.coverPath==expected.coverPath && result.preparationId==expected.preparationId &&
                result.operationId==expected.operationId && result.completedAction==expected.completedAction && result.progress==2 && result.publishedUrl==expected.publishedUrl,
                "every filtered resource, group, usage and action field survives multi-frame transport");
            Check(operations == 1, "reading pages never repeats the import/delete operation");
            const auto first = client.Call<IconLibraryPage>("iconLibrary.action", snowdesktop::IconLibraryRequest{});
            const auto token = std::get<0>(first);
            client.Call<void>("iconLibrary.release", token);
            Reject([&] { (void)client.Call<IconLibraryPage>("iconLibrary.page", token, std::uint32_t{1024}); },
                "released snapshots cannot leak stale entries into a newer operation");
            // A rejected request deliberately disconnects Channel. The owner
            // thread is stopped below; sending cleanup on this endpoint would
            // turn the successful stale-page rejection into a false failure.
        }
    }
    catch (const std::exception& error) { Check(false, error.what()); }
    PostThreadMessageW(GetThreadId(host.native_handle()), WM_QUIT, 0, 0);
    host.join();
}

void TestRetiredUpdateProtocol()
{
    HANDLE mainRead = nullptr, uiWrite = nullptr, uiRead = nullptr, mainWrite = nullptr;
    if (!CreatePipe(&mainRead, &uiWrite, nullptr, 0) ||
        !CreatePipe(&uiRead, &mainWrite, nullptr, 0))
        throw std::runtime_error("test pipe creation failed");
    {
        Channel channel;
        channel.Open(mainRead, mainWrite, CurrentProcessHandle());
        // Version 37 omitted the advanced-feature connection diagnostics.
        // An old peer must disconnect before its payload can be interpreted.
        const auto header = Pack(std::uint32_t{0x53444950}, std::uint32_t{37},
            std::uint32_t{1}, std::uint32_t{0}, std::uint64_t{1});
        DWORD written = 0;
        Check(WriteFile(uiWrite, header.data(), static_cast<DWORD>(header.size()),
                  &written, nullptr) && written == header.size(),
            "previous settings-protocol frame reaches the channel");
        const auto deadline = GetTickCount64() + 2000;
        while (channel.Connected() && GetTickCount64() < deadline) Sleep(1);
        Check(!channel.Connected(),
            "settings peers without connection diagnostics disconnect before dispatch");
    }
    CloseHandle(uiWrite);
    CloseHandle(uiRead);
}

bool PumpUntil(const std::function<bool()>& done, DWORD timeout = 3000)
{
    const auto deadline = GetTickCount64() + timeout;
    while (!done() && GetTickCount64() < deadline)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        if (!done()) MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return done();
}

struct DesktopMessageProbe
{
    std::function<void()> apply;
    HWND window = nullptr;
    explicit DesktopMessageProbe(std::function<void()> callback) : apply(std::move(callback))
    {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"SnowDesktop.Test.SettingsDesktopProbe";
        type.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            if (message == WM_NCCREATE)
                SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            if (message == WM_APP + 1)
            {
                reinterpret_cast<DesktopMessageProbe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))->apply();
                return 0;
            }
            return DefWindowProcW(hwnd, message, wp, lp);
        };
        RegisterClassW(&type);
        window = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, type.hInstance, this);
        if (!window) throw std::runtime_error("cannot create desktop message probe");
    }
    ~DesktopMessageProbe() { DestroyWindow(window); }
    void Post() { PostMessageW(window, WM_APP + 1, 0, 0); }
};

struct AsyncPeer
{
    Channel parent;
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread worker;
    AsyncPeer()
    {
        if (!release) throw std::runtime_error("cannot create settings peer gate");
        HANDLE mainRead = nullptr, uiWrite = nullptr, uiRead = nullptr, mainWrite = nullptr;
        if (!CreatePipe(&mainRead, &uiWrite, nullptr, 0) || !CreatePipe(&uiRead, &mainWrite, nullptr, 0))
            throw std::runtime_error("cannot create settings peer pipes");
        parent.Open(mainRead, mainWrite, CurrentProcessHandle());
        auto ready = std::make_shared<std::promise<bool>>();
        auto started = ready->get_future();
        worker = std::thread([this, uiRead, uiWrite, ready] {
            try
            {
                Channel child;
                child.Open(uiRead, uiWrite, CurrentProcessHandle());
                child.SetDisconnected([] { PostQuitMessage(0); });
                child.Bind<int>("ui.initialize", [this, &child] {
                    if (WaitForSingleObject(release, 3000) != WAIT_OBJECT_0)
                        throw ProtocolError("desktop did not release settings initialization");
                    return child.Call<int>("host.state");
                });
                child.Bind<int>("ui.open", [&child] { return child.Call<int>("host.query"); });
                child.Bind<int>("ui.error", []() -> int { throw ProtocolError("startup rejected"); });
                child.Bind<void>("ui.exit", [] { PostQuitMessage(0); });
                ready->set_value(true);
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
            }
            catch (...) { try { ready->set_value(false); } catch (...) {} }
        });
        if (!started.get())
        {
            worker.join();
            CloseHandle(release);
            throw std::runtime_error("settings peer startup failed");
        }
    }
    ~AsyncPeer()
    {
        SetEvent(release);
        parent.Close();
        PostThreadMessageW(GetThreadId(worker.native_handle()), WM_QUIT, 0, 0);
        worker.join();
        CloseHandle(release);
    }
};

void TestAsyncOpening()
{
    // Negative control for the reported production failure: even a separate
    // settings process starves an unrelated desktop window during Request.
    {
        AsyncPeer peer;
        bool desktopDispatched = false;
        DesktopMessageProbe desktop([&] { desktopDispatched = true; SetEvent(peer.release); });
        peer.parent.Bind<int>("host.state", [] { return 41; });
        desktop.Post();
        Reject([&] { (void)peer.parent.Request("ui.initialize", {}, 100); },
            "synchronous startup stalls until its deadline while desktop work is queued");
        Check(!desktopDispatched, "the old synchronous wait leaves desktop input/render messages undispatched");
        PumpUntil([&] { return desktopDispatched; });
    }
    {
        AsyncPeer peer;
        bool desktopDispatched = false, initialized = false, opened = false;
        bool failed = false;
        const auto owner = GetCurrentThreadId();
        Channel::Completion queryReply;
        DesktopMessageProbe desktop([&] { desktopDispatched = true; SetEvent(peer.release); });
        peer.parent.Bind<int>("host.state", [] { return 41; });
        peer.parent.BindDeferredRaw("host.query", [&](auto arguments, auto reply) {
            Reader(arguments).Finish();
            queryReply = std::move(reply);
        });
        desktop.Post();
        peer.parent.RequestAsync("ui.initialize", {}, [&](Bytes reply, std::exception_ptr error) {
            initialized = true;
            failed = error != nullptr;
            Check(GetCurrentThreadId() == owner && !error && Unpack<int>(reply) == 41,
                "delayed initialization and nested host requests complete on the desktop owner STA");
            if (error) return;
            peer.parent.RequestAsync("ui.open", {}, [&](Bytes result, std::exception_ptr failure) {
                opened = true;
                failed = failure != nullptr;
                Check(!failure && Unpack<int>(result) == 42,
                    "navigation waits for a deferred system-state reply without blocking the desktop");
            });
        });
        Check(!initialized && !desktopDispatched,
            "queuing startup returns before either the child or desktop messages execute");
        Check(PumpUntil([&] { return static_cast<bool>(queryReply); }) && desktopDispatched && initialized && !opened,
            "desktop messages run during delayed initialization and deferred navigation");
        if (queryReply)
        {
            // The responder is also usable by a real background worker.
            std::thread systemQuery([reply = std::move(queryReply)] { reply(Pack(42), {}); });
            systemQuery.join();
        }
        Check(PumpUntil([&] { return opened; }) && !failed, "deferred system query resumes the pending open once");
        int errors = 0;
        peer.parent.RequestAsync("ui.error", {}, [&](Bytes, std::exception_ptr error) {
            Check(error != nullptr, "child startup rejection reaches the asynchronous caller");
            ++errors;
        });
        Check(PumpUntil([&] { return errors == 1; }), "remote startup failure completes without a synchronous wait");
        Channel::Completion staleReply;
        peer.parent.BindDeferredRaw("host.query", [&](auto, auto reply) { staleReply = std::move(reply); });
        int cancelled = 0;
        peer.parent.RequestAsync("ui.open", {}, [&](Bytes, std::exception_ptr) { ++cancelled; });
        Check(PumpUntil([&] { return static_cast<bool>(staleReply); }), "a deferred query is active before connection cancellation");
        peer.parent.Close();
        if (staleReply) staleReply(Pack(99), {});
        bool staleDrained = false;
        peer.parent.Post([&] { staleDrained = true; });
        Check(PumpUntil([&] { return staleDrained; }) && cancelled == 0,
            "a late background reply cannot acknowledge an explicitly cancelled open");
    }
    {
        AsyncPeer peer;
        int disconnected = 0;
        peer.parent.RequestAsync("ui.exit", {}, [&](Bytes, std::exception_ptr) { ++disconnected; });
        Check(PumpUntil([&] { return disconnected == 1; }), "peer exit retires each pending open exactly once");
    }
    Channel::Completion orphanedReply;
    {
        AsyncPeer peer;
        peer.parent.BindDeferredRaw("host.query", [&](auto, auto reply) { orphanedReply = std::move(reply); });
        peer.parent.RequestAsync("ui.open", {}, [](Bytes, std::exception_ptr) {});
        Check(PumpUntil([&] { return static_cast<bool>(orphanedReply); }),
            "a pending system query retains a safe responder before endpoint destruction");
    }
    if (orphanedReply) orphanedReply(Pack(42), {});
    for (const bool explicitClose : {false, true})
    {
        AsyncPeer peer;
        int completed = 0;
        peer.parent.RequestAsync("ui.initialize", {}, [&](Bytes, std::exception_ptr error) {
            Check(error != nullptr, "stalled asynchronous startup reports a timeout");
            ++completed;
        }, 100);
        if (explicitClose)
        {
            peer.parent.Close();
            // Includes stale dispatch messages already queued before Close.
            bool drained = false;
            peer.parent.Post([&] { drained = true; });
            Check(PumpUntil([&] { return drained; }) && completed == 0,
                "explicit shutdown cancels open delivery instead of calling a destroyed facade");
        }
        else
        {
            Check(PumpUntil([&] { return completed == 1; }) && !peer.parent.Connected(),
                "startup timeout disconnects the session while the desktop message loop keeps running");
        }
    }
}

void TestOverlappingSettingsRefresh()
{
    // Real child nested RPCs reproduce activation refresh A, then navigation
    // refresh B while A waits. Dropping A's responder used to strand the outer
    // wait, eventually closing an otherwise successfully opened settings UI.
    AsyncPeer peer;
    auto refresh = std::make_shared<snowdesktop::SettingsExternalStateRefresh>();
    int requests = 0, queries = 0, replies = 0;
    bool desktopDispatched = false;
    DesktopMessageProbe desktop([&] { desktopDispatched = true; });
    peer.parent.BindDeferredRaw("host.query", [&](auto, auto reply) {
        ++requests;
        if (refresh->Request([reply = std::move(reply)] { reply(Pack(42), {}); })) ++queries;
    });
    const auto completed = [&](Bytes reply, std::exception_ptr error) {
        ++replies;
        Check(!error && Unpack<int>(reply) == 42, "each overlapping settings request receives the authoritative query result");
    };
    peer.parent.RequestAsync("ui.open", {}, completed, 1000);
    peer.parent.RequestAsync("ui.open", {}, completed, 1000);
    desktop.Post();
    Check(PumpUntil([&] { return requests == 2; }) && queries == 1 && replies == 0 && desktopDispatched,
        "activation and navigation share one query while the desktop continues dispatching messages");
    refresh->Complete();
    Check(PumpUntil([&] { return replies == 2; }) && peer.parent.Connected(),
        "both nested refresh waits finish without a lost acknowledgement or retired connection");
    peer.parent.RequestAsync("ui.open", {}, completed, 1000);
    Check(PumpUntil([&] { return requests == 3; }) && queries == 2,
        "a later settings session queries Windows again instead of reusing a completed query");
    refresh->Complete();
    Check(PumpUntil([&] { return replies == 3; }), "the later refresh also completes exactly once");

    int afterDestruction = 0;
    {
        snowdesktop::SettingsExternalStateRefresh abandoned;
        abandoned.Request([&] { ++afterDestruction; });
    }
    Check(afterDestruction == 0, "destroying settings infrastructure cancels unfulfilled query acknowledgements");
}

void TestStalledPeer()
{
    HANDLE mainRead = nullptr, uiWrite = nullptr, uiRead = nullptr, mainWrite = nullptr;
    if (!CreatePipe(&mainRead, &uiWrite, nullptr, 0) ||
        !CreatePipe(&uiRead, &mainWrite, nullptr, 0))
        throw std::runtime_error("test pipe creation failed");
    const auto started = GetTickCount64();
    {
        Channel channel;
        channel.Open(mainRead, mainWrite, CurrentProcessHandle());
        // No peer reader: this exceeds pipe capacity and stalls its writer.
        Reject([&] { (void)channel.Request("ui.flush", Pack(std::string(1024 * 1024, 'x')), 100); },
            "unresponsive peer fails instead of acknowledging an edit");
    }
    Check(GetTickCount64() - started < 3000,
        "a stalled pipe writer cannot block timeout or endpoint destruction");
    CloseHandle(uiWrite);
    CloseHandle(uiRead);
}

void TestProcessLifecycle()
{
    for (int iteration = 0; iteration < 3; ++iteration)
    {
        Channel channel;
        SettingsProcess process;
        process.Start(channel);
        Check(process.Running() && process.ProcessId() != GetCurrentProcessId(),
            "settings UI runs in a distinct supervised process");
        Check(channel.Call<bool>("test.identity", ExecutableIdentity()),
            "child validates inherited channel and exact executable identity");
        snowdesktop::SettingsRoute route = snowdesktop::SettingsRoute::ForWidget(L"组件-42");
        Check(channel.Call<snowdesktop::SettingsRoute>("test.route", route) == route,
            "real child receives component route without sharing pointers");
        channel.Notify("test.exit");
        const auto deadline = GetTickCount64() + 5000;
        while (process.Running() && GetTickCount64() < deadline) Sleep(10);
        Check(!process.Running(), "settings child exits after close, without a resident UI process");
        channel.Close();
        process.Stop();
    }
    Channel channel;
    SettingsProcess process;
    process.Start(channel);
    Check(channel.Call<bool>("test.identity", ExecutableIdentity()),
        "supervised child starts before application shutdown simulation");
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, process.ProcessId());
    process.Stop();
    Check(child && WaitForSingleObject(child, 5000) == WAIT_OBJECT_0,
        "closing the application-owned job terminates the settings UI without an orphan process");
    if (child) CloseHandle(child);
    channel.Close();
}
}

void TestMenuHelperStartupFeedback()
{
    using namespace snowdesktop::settings_ipc;
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY parentPolicy{};
    const bool parentPolicyKnown = GetProcessMitigationPolicy(GetCurrentProcess(), ProcessExtensionPointDisablePolicy,
        &parentPolicy, sizeof(parentPolicy)) != FALSE;
    Check(parentPolicyKnown, "read the original process extension-point policy for role isolation checks");
    for (const auto command : {L"--settings-ui", L"--shell-menu-helper", L"--start-menu-helper"})
    {
        Channel channel;
        SettingsProcess process;
        process.Start(channel, command);
        const auto flags = channel.Call<DWORD>("test.startup-flags");
        const bool menuHelper = std::wstring_view(command) != L"--settings-ui";
        Check(((flags & STARTF_FORCEOFFFEEDBACK) != 0) == menuHelper,
            "the real launcher suppresses startup cursor feedback for both private background menu modes");
        HANDLE childPolicyProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process.ProcessId());
        PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY childPolicy{};
        const bool childPolicyKnown = childPolicyProcess && GetProcessMitigationPolicy(childPolicyProcess,
            ProcessExtensionPointDisablePolicy, &childPolicy, sizeof(childPolicy)) != FALSE;
        if (childPolicyProcess) CloseHandle(childPolicyProcess);
        Check(childPolicyKnown, "read extension-point mitigation from the actual supervised child process");
        if (parentPolicyKnown && childPolicyKnown)
        {
            const bool shellMenu = std::wstring_view(command) == L"--shell-menu-helper";
            Check((childPolicy.DisableExtensionPoints != 0) == (shellMenu || parentPolicy.DisableExtensionPoints != 0),
                "only Shell menu helpers add legacy extension-point isolation; settings and Start retain their original baseline");
        }
        if (std::wstring_view(command) == L"--start-menu-helper")
        {
            Check(channel.CallWithTimeout<std::pair<LONG, bool>>(500, "start.state") == std::pair<LONG, bool>{S_OK, true},
                "the Start helper exchanges launcher state through the real inherited process channel");
            Check(channel.CallWithTimeout<LONG>(500, "start.dismiss") == S_OK &&
                    channel.CallWithTimeout<std::pair<LONG, bool>>(500, "start.state") == std::pair<LONG, bool>{S_OK, false},
                "the Start helper can acknowledge one-way dismissal and report closed state");
            HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, process.ProcessId());
            Reject([&] { (void)channel.CallWithTimeout<LONG>(100, "start.stall"); },
                "a stuck Start Shell operation has a bounded host IPC deadline");
            process.Stop();
            Check(child && WaitForSingleObject(child, 3000) == WAIT_OBJECT_0,
                "retiring a stalled Start helper terminates only the supervised child");
            if (child) CloseHandle(child);
        }
        channel.Close();
        process.Stop();
    }
}

int RunSettingsIpcChildIfRequested()
{
    using namespace snowdesktop::settings_ipc;
    const bool menuHelper = IsSettingsProcessCommand(L"--shell-menu-helper");
    const bool startHelper = IsSettingsProcessCommand(L"--start-menu-helper");
    if (!menuHelper && !startHelper && !IsSettingsProcessCommand()) return -1;
    try
    {
        Channel channel;
        OpenInheritedSettingsChannel(channel, startHelper ? L"--start-menu-helper" :
            menuHelper ? L"--shell-menu-helper" : L"--settings-ui");
        bool startVisible = true;
        channel.Bind<std::pair<LONG, bool>>("start.state", [&] { return std::pair<LONG, bool>{S_OK, startVisible}; });
        channel.Bind<LONG>("start.dismiss", [&] { startVisible = false; return S_OK; });
        channel.Bind<LONG>("start.stall", [] { Sleep(10000); return S_OK; });
        channel.Bind<DWORD>("test.startup-flags", [] {
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            GetStartupInfoW(&startup);
            return startup.dwFlags;
        });
        channel.Bind<bool, std::string>("test.identity", [](const std::string& identity) {
            return identity == ExecutableIdentity();
        });
        channel.Bind<snowdesktop::SettingsRoute, snowdesktop::SettingsRoute>("test.route", [](auto route) { return route; });
        channel.Bind<void>("test.exit", [] { PostQuitMessage(0); });
        channel.SetDisconnected([] { PostQuitMessage(ERROR_BROKEN_PIPE); });
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        return 0;
    }
    catch (...) { return 1; }
}

void TestLargeIconEditing()
{
    using namespace snowdesktop;
    using namespace snowdesktop::large_icon_edit_rules;
    Check(ResolveEntryAccess(false, false) == EntryAccess::Hidden &&
            ResolveEntryAccess(false, true) == EntryAccess::Hidden,
        "large-icon authoring entries stay hidden without a Bridge, even with a retained unlock flag");
    Check(ResolveEntryAccess(true, false) == EntryAccess::Unlock,
        "a locked build with a Bridge directs authoring entries to unlock settings");
    Check(ResolveEntryAccess(true, true) == EntryAccess::Edit,
        "an unlocked build with a Bridge exposes normal large-icon editing");
    struct Item { std::optional<LargeIconConfig> largeIcon; std::pair<int, int> gridSpan{1, 1}; } item;
    LargeIconConfig config; config.columns = 3; config.rows = 2; config.opacity = .2;
    int writes = 0;
    auto persist = [&] { ++writes; return true; };
    Check(!Store(item, config, std::pair{3, 2}, false, true, persist) && !item.largeIcon && writes == 0,
        "host rejects creation without effective unlock before touching state or persistence");
    Check(!Store(item, config, std::pair{3, 2}, true, false, persist) && writes == 0,
        "host rejects large-icon creation inside a storage container");
    Check(Store(item, config, std::pair{3, 2}, true, true, persist) && item.largeIcon == config && writes == 1,
        "effective unlock permits a new large icon, including an offline-valid entitlement result");
    auto changed = config; changed.opacity = .8;
    Check(!Store(item, changed, std::pair{2, 2}, false, true, persist) && item.largeIcon == config && item.gridSpan == std::pair{3, 2} && writes == 1,
        "unlock loss preserves committed appearance and size while preventing edits");
    Check(!Store(item, changed, std::pair{2, 2}, true, true, [] { return false; }) && item.largeIcon == config && item.gridSpan == std::pair{3, 2},
        "failed persistence rolls back both appearance and span");
    Check(!Store(item, changed, std::pair{2, 2}, true, true, []() -> bool { throw std::runtime_error("write failed"); }) && item.largeIcon == config,
        "exceptional persistence failure also preserves the committed configuration");
    Check(Store(item, changed, std::pair{2, 2}, true, true, persist) && item.largeIcon == changed,
        "editing can retry successfully after re-unlock or a persistence failure");
    Check(Store(item, std::nullopt, std::pair{1, 1}, false, true, persist) && !item.largeIcon && item.gridSpan == std::pair{1, 1},
        "returning to an ordinary icon remains available after unlock loss");
    config.radius = -1;
    Check(!Store(item, config, std::pair{2, 2}, true, true, persist) && !item.largeIcon,
        "host transaction rejects invalid configuration even with an effective unlock");

    LargeIconEditSession edit{L"game", 11, 7, LargeIconConfig{}};
    LargeIconSettingsRequest request{L"game", 11, 7, "preview", EncodeLargeIconConfig({}), {}};
    Check(CheckRequest(edit, request, true).empty(), "current item session may preview");
    for (const auto action : {"preview", "commit", "cancel", "import", "refresh"})
    {
        request.action = action; request.revision = 6;
        Check(CheckRequest(edit, request, true) == "largeIcon.stale", "all edit operations reject an obsolete revision");
        request.revision = 7; request.session = 10;
        Check(CheckRequest(edit, request, true) == "largeIcon.stale", "all edit operations reject an obsolete editor session");
        request.session = 11; request.key = L"other";
        Check(CheckRequest(edit, request, true) == "largeIcon.stale", "all edit operations validate the original project identity");
        request.key = L"game"; edit.preview = LargeIconConfig{};
        Check(CheckRequest(edit, request, false) == "largeIcon.locked" && !edit.preview,
            "unlock loss rejects every editing operation and cancels its uncommitted preview");
    }
    request.action = "status"; request.revision = 1;
    Check(CheckRequest(edit, request, true).empty(), "status can observe a newer revision in the same editor session");
    request.action = "commit"; request.revision = 7;
    edit = {};
    Check(CheckRequest(edit, request, true) == "largeIcon.stale", "deletion, conversion or editor teardown invalidates old requests");
    request.action = "read";
    Check(CheckRequest(edit, request, true).empty(), "reopening may establish a fresh session after re-unlock");

    // The real batch editor sends a field patch, not a cloned first-item config.
    // Protect distinct Steam covers, imported foregrounds and untouched spans.
    LargeIconConfig first, second;
    first.backgroundStyle = -2; first.content = 2; first.cachedCover = "steam-first.png";
    second.backgroundStyle = 9; second.foregroundContent = 1; second.foregroundImage = "second.png";
    second.columns = 3; second.opacity = .4; second.gradient.enabled = true;
    auto draft = first; draft.backgroundStyle = -1;
    const auto preserved = second;
    Check(Patch(second, draft, {"backgroundStyle"}) && second.backgroundStyle == -1 &&
        second.foregroundImage == preserved.foregroundImage && second.columns == 3 && second.opacity == .4 && second.gradient == preserved.gradient,
        "batch background change preserves each item's resources, size and custom settings");
    Check(Patch(first, draft, {"backgroundStyle"}) && first.cachedCover == "steam-first.png" && first.content == 2,
        "switching Steam background preserves the item's own retained cover source");
    draft.opacity = .4;
    Check(Patch(second, draft, {"opacity"}) && second.opacity == .4,
        "an explicitly selected field applies even when equal to the reference item's value");
    Check(!Patch(second, draft, {"cachedCover"}) && !Patch(second, draft, {"unknown"}),
        "batch patches cannot overwrite asset ownership or introduce unknown fields");
    auto fill = first; fill.backgroundStyle = -2;
    draft.effect = 2;
    Check(!Patch(fill, draft, {"effect"}), "batch dynamic-title selection rejects any image-fill target");
    auto legacyCustom = second;
    legacyCustom.backgroundStyle = -3; legacyCustom.defaultBackground = 2;
    legacyCustom.defaultSolidColor = 0x123456; legacyCustom.defaultSolidOpacity = .7;
    snowdesktop::large_icon_preset_rules::PrepareForEditing(legacyCustom, 0, false, 0);
    draft.opacity = .25;
    Check(Patch(legacyCustom, draft, {"opacity"}) && legacyCustom.backgroundStyle == 9 &&
        legacyCustom.manualColor == 0x123456 && legacyCustom.opacity == .25 && legacyCustom.foregroundImage == "second.png",
        "batch editing a legacy custom background reaches its visible settings without losing its own color or image");

    Item a{first, {1, 1}}, b{second, {3, 1}};
    const std::vector<Change<Item, std::pair<int, int>>> changes{{&a, draft, {2, 2}}, {&b, draft, {2, 2}}};
    Check(!StoreMany(changes, false, persist) && a.largeIcon == first && b.largeIcon == second,
        "unlock loss rejects the entire batch before persistence");
    Check(!StoreMany(changes, true, [] { return false; }) && a.largeIcon == first && b.largeIcon == second &&
        a.gridSpan == std::pair{1, 1} && b.gridSpan == std::pair{3, 1},
        "failed batch persistence restores every appearance and span");
    Check(!StoreMany(changes, true, []() -> bool { throw std::runtime_error("batch write failed"); }) &&
        a.largeIcon == first && b.largeIcon == second, "exceptional batch persistence rolls back every target");
    const auto writesBeforeBatch = writes;
    Check(StoreMany(changes, true, persist) && writes == writesBeforeBatch + 1 && a.largeIcon == draft && b.largeIcon == draft,
        "one successful batch stores all targets in a single persistence operation");
    const std::vector<Change<Item, std::pair<int, int>>> restore{{&a, {}, {1, 1}}, {&b, {}, {1, 1}}};
    Check(StoreMany(restore, false, persist) && !a.largeIcon && !b.largeIcon,
        "batch return to ordinary icons remains possible after losing unlock");
    edit = {L"first", 12, 1, draft}; edit.keys = {L"first", L"second"}; edit.previews.emplace(L"second", draft);
    Check(Contains(edit, L"second") && !Contains(edit, L"other"), "session membership is the captured batch rather than live selection");
    request = {L"first", 12, 1, "preview", EncodeLargeIconConfig(draft), {}, {"backgroundStyle"}};
    const auto requestCopy = settings_ipc::Unpack<LargeIconSettingsRequest>(settings_ipc::Pack(request));
    Check(requestCopy.fields == request.fields && requestCopy.key == request.key, "batch field intent reaches the host through private IPC");
    Check(CheckRequest(edit, request, false) == "largeIcon.locked" && !edit.preview && edit.previews.empty(),
        "unlock loss clears all batch previews");
}

int RunSettingsIpcTests()
{
    {
        using namespace snowdesktop::window_support;
        SupportStatus status{{Mask(Capability::WindowPin), Mask(Capability::PointerObservation), ProviderState::Ready, 57},
            AuthorizationState::Registered, false, ERROR_NOT_SUPPORTED};
        const auto copy = Unpack<SupportStatus>(Pack(status));
        Check(copy.capabilities.inProcess == status.capabilities.inProcess && copy.capabilities.helper == status.capabilities.helper &&
            copy.capabilities.helperState == ProviderState::Ready && copy.capabilities.generation == 57 &&
            copy.authorization == AuthorizationState::Registered && !copy.hostElevated && copy.error == ERROR_NOT_SUPPORTED,
            "settings IPC preserves independent capabilities, authorization and real host elevation");
    }
    TestCodec();
    TestPermissionAndRelocatedRoutesCodec();
    TestLargeIconEditing();
    TestCustomCategoryCreation();
    TestChannel();
    TestContextMenuInspectionLifetime();
    TestIconLibraryPaging();
    TestAsyncOpening();
    TestOverlappingSettingsRefresh();
    TestRetiredUpdateProtocol();
    TestStalledPeer();
    TestProcessLifecycle();
    TestMenuHelperStartupFeedback();
    return failures;
}
