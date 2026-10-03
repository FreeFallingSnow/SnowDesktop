#pragma once
#include "theme_library.h"
#include "settings_controller.h"
#include "data_paths.h"
#include "widget_settings_model.h"
#include "status_bar_appearance.h"
#include "taskbar_appearance.h"
#include <array>

namespace snowdesktop::themes
{
inline std::filesystem::path LibraryPath() { return GetDataFilePath(L"SnowDesktop.themes.json"); }
inline widget_runtime::WidgetHostAppearancePatch WidgetPatch(const Theme& theme)
{
    widget_runtime::WidgetHostAppearancePatch patch;
    const auto& appearance = theme.appearance;
    const auto rgb = [](float r, float g, float b) { return (static_cast<int>(std::lround(r * 255)) << 16) |
        (static_cast<int>(std::lround(g * 255)) << 8) | static_cast<int>(std::lround(b * 255)); };
    patch.followPersonalization = false;
    patch.presetId = "__custom";
    patch.backgroundColor = rgb(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB);
    patch.borderColor = rgb(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB);
    patch.backgroundOpacity = appearance.widgetAlpha; patch.borderOpacity = appearance.widgetBorderAlpha;
    patch.borderWidth = appearance.widgetBorderWidth; patch.edgeHighlightEnabled = appearance.widgetEdgeHighlightEnabled;
    patch.edgeHighlightWidth = appearance.widgetEdgeHighlightWidth; patch.edgeHighlightStrength = appearance.widgetEdgeHighlightStrength;
    patch.edgeLight = appearance.edgeLight; patch.gradientEndOpacity = appearance.gradientEndA;
    patch.glassEnabled = appearance.glassEnabled; patch.acrylicEnabled = appearance.acrylicEnabled;
    patch.contentTheme = appearance.contentTheme; patch.panelGradient = appearance.panelGradient;
    return patch;
}
inline bool WidgetMatches(const widget_runtime::WidgetHostAppearanceState& state, const Theme& theme)
{
    const auto patch = WidgetPatch(theme);
    return !state.followPersonalization && state.backgroundColor == patch.backgroundColor &&
        state.borderColor == patch.borderColor && state.backgroundOpacity == patch.backgroundOpacity &&
        state.borderOpacity == patch.borderOpacity && state.borderWidth == patch.borderWidth &&
        state.edgeHighlightEnabled == patch.edgeHighlightEnabled && state.edgeHighlightWidth == patch.edgeHighlightWidth &&
        state.edgeHighlightStrength == patch.edgeHighlightStrength && state.edgeLight == patch.edgeLight &&
        state.gradientEndOpacity == patch.gradientEndOpacity && state.glassEnabled == patch.glassEnabled &&
        state.acrylicEnabled == patch.acrylicEnabled && state.contentTheme == patch.contentTheme && state.panelGradient == patch.panelGradient;
}
inline Kind TargetKind(std::string_view target)
{ return target == "quickPanel" ? Kind::QuickPanel : target == "popup" ? Kind::Popup : Kind::Global; }
inline unsigned TargetScope(std::string_view target)
{ return target == "dock" ? Dock : target == "statusBar" || target.starts_with("statusBar/") ? StatusBar : target == "taskbar" || target.starts_with("taskbar/") ? Taskbar : Components; }
// Private settings source identities. Packages still contain only three kinds.
inline constexpr std::array<std::string_view, 11> AppearanceTargets{
    "global", "dock", "statusBar", "taskbar", "quickPanel", "popup",
    "statusBar/noWindow", "statusBar/maximizedWindow", "taskbar/shellUi", "taskbar/maximizedWindow", "taskbar/visibleWindow"};
inline StatusBarAppearanceRule StatusBarSettings::* StatusRule(std::string_view target)
{
    return target == "statusBar/noWindow" ? &StatusBarSettings::noWindow :
        target == "statusBar/maximizedWindow" ? &StatusBarSettings::maximizedWindow : nullptr;
}
inline SystemTaskbarDynamicRule DockSettings::* TaskbarRule(std::string_view target)
{
    return target == "taskbar/shellUi" ? &DockSettings::systemTaskbarShellUi :
        target == "taskbar/maximizedWindow" ? &DockSettings::systemTaskbarMaximizedWindow :
        target == "taskbar/visibleWindow" ? &DockSettings::systemTaskbarVisibleWindow : nullptr;
}
inline Theme CaptureTarget(std::string_view target, const SettingsValues& values)
{
    auto appearance = values.personalization;
    if (target == "dock") appearance = ResolveDockAppearance(values.dock, values.personalization);
    if (target == "taskbar") appearance = ResolveTaskbarAppearance(values.dock, values.personalization);
    if (target == "statusBar") appearance = ResolveStatusBarAppearance(values.general.statusBar.theme, values.personalization);
    if (const auto member = StatusRule(target)) appearance = ResolveStatusBarAppearance((values.general.statusBar.*member).theme, values.personalization);
    if (const auto member = TaskbarRule(target)) appearance = ResolveTaskbarRuleAppearance(values.dock.*member, values.personalization);
    if (target == "quickPanel") appearance = ResolveSurfaceTheme(values.general.quickNavigationAppearance,
        values.personalization, values.general.quickNavTheme, true, &values.general.globalQuickNavigationAppearance);
    if (target == "popup") appearance = ResolveSurfaceTheme(values.general.collectionPopupAppearance,
        values.personalization, values.general.collectionPopupTheme, false, &values.general.globalCollectionPopupAppearance);
    auto theme = Capture(TargetKind(target), appearance, values.navigation);
    theme.scopes = TargetScope(target);
    return theme;
}
inline void ReconcileReferences(Library& library, const SettingsValues& values)
{
    for (const auto target : AppearanceTargets)
    {
        auto found = library.references.find(std::string(target));
        if (found == library.references.end() || found->second.id.empty()) continue;
        const auto saved = Resolve(found->second.snapshot, found->second.id);
        auto current = CaptureTarget(target, values);
        if (!saved || EncodePanelAppearance(saved->appearance) != EncodePanelAppearance(current.appearance) ||
            saved->appearance.gradientEndA != current.appearance.gradientEndA ||
            (current.kind == Kind::QuickPanel && (saved->layout != current.layout || saved->colors != current.colors)))
            Detach(library, target);
    }
}
inline bool FollowQuickBinding(const Library& library, NavigationSettings& navigation)
{
    const auto reference = library.references.find("global");
    if (reference == library.references.end()) return false;
    for (const auto& [id, theme] : reference->second.snapshot)
    {
        (void)id;
        if (theme.kind != Kind::Global) continue;
        const auto quick = Resolve(reference->second.snapshot, theme.quickPanel);
        if (!quick || quick->kind != Kind::QuickPanel) return false;
        ApplyQuickPanel(navigation, *quick);
        return true;
    }
    return false;
}
inline bool ApplyTarget(const Library& library, std::string_view target, std::string_view id,
    SettingsValues& values, std::string& error)
{
    const auto theme = Resolve(library.themes, id);
    if (!theme || theme->kind != TargetKind(target) || (theme->kind == Kind::Global &&
        (target != "global" && !(theme->scopes & TargetScope(target))))) { error = "invalidSelection"; return false; }
    const SurfaceTheme snapshot{4, true, Capture(theme->kind, theme->appearance).appearance};
    if (target == "global")
    {
        const auto quick = Resolve(library.themes, theme->quickPanel), popup = Resolve(library.themes, theme->popup);
        if (!quick || !popup) { error = "missingDependency"; return false; }
        ApplyAppearance(values.personalization, theme->appearance);
        values.general.globalQuickNavigationAppearance = {4, true, Capture(Kind::QuickPanel, quick->appearance).appearance};
        values.general.globalCollectionPopupAppearance = {4, true, Capture(Kind::Popup, popup->appearance).appearance};
        if (values.general.quickNavigationAppearance.mode == -1) ApplyQuickPanel(values.navigation, *quick);
    }
    else if (target == "quickPanel")
    { values.general.quickNavigationAppearance = snapshot; ApplyQuickPanel(values.navigation, *theme); }
    else if (target == "popup") values.general.collectionPopupAppearance = snapshot;
    else if (target == "statusBar") values.general.statusBar.theme = snapshot;
    else if (target == "dock")
    { ApplyAppearance(values.dock.customAppearance, theme->appearance); values.dock.appearancePreset = kAppearancePresetCustom; values.dock.followComponentAppearance = false; }
    else if (target == "taskbar")
    { ApplyAppearance(values.dock.systemTaskbarAppearance, theme->appearance); values.dock.systemTaskbarFollowPersonalization = false; values.dock.systemTaskbarContentTheme = theme->appearance.contentTheme; }
    else if (const auto statusMember = StatusRule(target)) (values.general.statusBar.*statusMember).theme = snapshot;
    else if (const auto taskbarMember = TaskbarRule(target))
    {
        auto& rule = values.dock.*taskbarMember;
        ApplyAppearance(rule.appearance, theme->appearance);
        rule.themeMode = SystemTaskbarThemeMode::Custom; rule.contentTheme = theme->appearance.contentTheme;
    }
    else { error = "invalidSelection"; return false; }
    return true;
}
inline std::vector<std::string> ApplySavedUpdate(const Library& library, std::string_view changedId,
    SettingsValues& values, std::string& error)
{
    std::vector<std::string> targets;
    for (const auto target : AppearanceTargets)
    {
        const auto reference = library.references.find(std::string(target));
        if (reference == library.references.end() || reference->second.id.empty()) continue;
        const auto theme = Resolve(library.themes, reference->second.id);
        if (!theme) continue;
        const bool bindingChanged = std::string_view(target) == "global" &&
            (theme->quickPanel == changedId || theme->popup == changedId);
        if ((theme->id == changedId || bindingChanged) && ApplyTarget(library, target, theme->id, values, error))
            targets.emplace_back(target);
    }
    return targets;
}
}
