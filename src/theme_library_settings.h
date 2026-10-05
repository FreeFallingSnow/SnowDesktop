#pragma once
#include "theme_library.h"
#include "settings_controller.h"
#include "data_paths.h"
#include "widget_settings_model.h"
#include "status_bar_appearance.h"
#include "taskbar_appearance.h"
#include <array>
#include <cwctype>
#include <locale>
#include <sstream>

namespace snowdesktop::themes
{
inline std::wstring ExportFileName(std::wstring name)
{
    for (auto& character : name)
        if (character < 32 || std::wstring_view(L"<>:\"/\\|?*").find(character) != std::wstring_view::npos) character = L'_';
    const auto trim = [&] {
        while (!name.empty() && (name.back() == L'.' || std::iswspace(name.back()))) name.pop_back();
        while (!name.empty() && std::iswspace(name.front())) name.erase(0, 1);
    };
    trim();
    constexpr std::wstring_view extension = L".snowtheme";
    while (name.size() >= extension.size())
    {
        std::wstring suffix = name.substr(name.size() - extension.size());
        for (auto& character : suffix) character = static_cast<wchar_t>(std::towlower(character));
        if (suffix != extension) break;
        name.resize(name.size() - extension.size()); trim();
    }
    if (name.size() > 120)
    {
        name.resize(120);
        if (name.back() >= 0xd800 && name.back() <= 0xdbff) name.pop_back();
        trim();
    }
    if (name.empty()) name = L"theme";
    auto device = name.substr(0, name.find(L'.'));
    while (!device.empty() && device.back() == L' ') device.pop_back();
    for (auto& character : device) character = static_cast<wchar_t>(std::towupper(character));
    if (device == L"CON" || device == L"PRN" || device == L"AUX" || device == L"NUL" ||
        device == L"CONIN$" || device == L"CONOUT$" || (device.size() == 4 &&
        (device.starts_with(L"COM") || device.starts_with(L"LPT")) &&
        ((device[3] >= L'1' && device[3] <= L'9') || device[3] == L'\u00b9' || device[3] == L'\u00b2' || device[3] == L'\u00b3')))
        name.insert(0, L"_");
    return name + std::wstring(extension);
}
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
inline Theme CaptureGlobalBinding(Kind kind, const SettingsValues& values, const std::optional<Theme>& source)
{
    const bool quick = kind == Kind::QuickPanel;
    SurfaceTheme follow; follow.mode = -1;
    const auto appearance = ResolveSurfaceTheme(follow, values.personalization,
        quick ? values.general.quickNavTheme : values.general.collectionPopupTheme, quick,
        quick ? &values.general.globalQuickNavigationAppearance : &values.general.globalCollectionPopupAppearance);
    auto theme = Capture(kind, appearance, values.navigation);
    // An independent quick panel owns the current navigation values. Saving its
    // global parent must keep the bound child's layout/colors instead.
    if (quick && values.general.quickNavigationAppearance.mode != -1 && source && source->kind == kind)
    { theme.layout = source->layout; theme.colors = source->colors; }
    return theme;
}
inline bool PrepareGlobalCustomEdit(GeneralSettings& settings,
    const PersonalizationSettings& previous, const PersonalizationSettings& edited)
{
    const auto quickBefore = settings.globalQuickNavigationAppearance;
    const auto popupBefore = settings.globalCollectionPopupAppearance;
    if (edited.backgroundPreset != kAppearancePresetCustom)
    {
        if (edited.backgroundPreset != previous.backgroundPreset)
        {
            settings.globalQuickNavigationAppearance = {};
            settings.globalCollectionPopupAppearance = {};
        }
    }
    else
    {
        // A global draft contains separate bound surface appearances. Capture
        // each current effective appearance once; edits to the global material
        // must not continuously rewrite those bound drafts.
        if (!settings.globalQuickNavigationAppearance.customized)
        {
            const auto appearance = ResolveSurfaceTheme(settings.quickNavigationAppearance,
                previous, settings.quickNavTheme, true, &settings.globalQuickNavigationAppearance);
            settings.globalQuickNavigationAppearance = {4, true, Capture(Kind::QuickPanel, appearance).appearance};
        }
        if (!settings.globalCollectionPopupAppearance.customized)
        {
            const auto appearance = ResolveSurfaceTheme(settings.collectionPopupAppearance,
                previous, settings.collectionPopupTheme, false, &settings.globalCollectionPopupAppearance);
            settings.globalCollectionPopupAppearance = {4, true, Capture(Kind::Popup, appearance).appearance};
        }
    }
    return quickBefore != settings.globalQuickNavigationAppearance ||
        popupBefore != settings.globalCollectionPopupAppearance;
}
inline void EditSurfaceAppearance(GeneralSettings& settings, bool quick,
    const PersonalizationSettings& appearance, const PersonalizationSettings& global)
{
    auto& local = quick ? settings.quickNavigationAppearance : settings.collectionPopupAppearance;
    auto& binding = quick ? settings.globalQuickNavigationAppearance : settings.globalCollectionPopupAppearance;
    auto& draft = local.mode == -1 && global.backgroundPreset == kAppearancePresetCustom ? binding : local;
    draft = {4, true, Capture(quick ? Kind::QuickPanel : Kind::Popup, appearance).appearance};
}
inline void CopyComponentAppearanceToPopup(GeneralSettings& settings,
    const PersonalizationSettings& componentAppearance)
{
    EditSurfaceAppearance(settings, false, componentAppearance, componentAppearance);
}
inline bool AppliedAppearanceMatches(const PersonalizationSettings& saved,
    const PersonalizationSettings& current)
{
    // Older personalization files wrote six significant digits. Accept only
    // that exact serialization round trip, rather than a general tolerance
    // which could conceal a real appearance edit.
    const auto same = [](float original, float value) {
        if (original == value) return true;
        if (!std::isfinite(original) || !std::isfinite(value)) return false;
        std::stringstream legacy; legacy.imbue(std::locale::classic());
        legacy.precision(6); legacy << original;
        float persisted = 0; legacy >> persisted;
        return !legacy.fail() && persisted == value;
    };
    bool matches = saved.panelGradient == current.panelGradient &&
        saved.edgeLight == current.edgeLight && saved.contentTheme == current.contentTheme &&
        same(saved.gradientEndA, current.gradientEndA);
    VisitPanelAppearanceFields([&](auto, auto field, double, double) {
        matches = matches && same(saved.*field, current.*field);
    });
    VisitPanelAppearanceFlags([&](auto, auto field) { matches = matches && saved.*field == current.*field; });
    return matches;
}
// Private settings draft transaction. Existing child identities are updated only
// when explicitly saving back to an editable global source.
struct BindingDraft
{
    Theme effective;
    std::string selectedId, sourceId;
};
inline bool SaveDraft(Library& library, Theme theme, const std::array<BindingDraft, 2>& bindings,
    bool update, std::string& savedId, std::vector<std::string>& updatedIds, std::string& error,
    const NewId& newId = CreateId)
{
    const auto subscribed = [&](const std::string& id) {
        for (const auto& [item, origin] : library.workshop) { (void)item; if (origin.ids.contains(id)) return true; }
        return false;
    };
    if (update && subscribed(theme.id)) { error = "copyRequired"; return false; }
    Library next = library; std::vector<std::string> changed;
    if (theme.kind == Kind::Global && FullScope(theme.scopes))
    {
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            const auto kind = i == 0 ? Kind::QuickPanel : Kind::Popup;
            const auto& draft = bindings[i]; auto& binding = i == 0 ? theme.quickPanel : theme.popup;
            if (!draft.selectedId.empty())
            {
                const auto selected = Resolve(next.themes, draft.selectedId);
                if (!selected || selected->kind != kind) { error = "missingDependency"; return false; }
                binding = selected->id; continue;
            }
            if (draft.effective.kind != kind) { error = "invalidPackage"; return false; }
            const auto source = Resolve(next.themes, draft.sourceId);
            if (source && source->kind != kind) { error = "invalidSelection"; return false; }
            const bool same = source && AppliedAppearanceMatches(source->appearance, draft.effective.appearance) &&
                (kind != Kind::QuickPanel || (source->layout == draft.effective.layout && source->colors == draft.effective.colors));
            if (same) { binding = source->id; continue; }
            if (source && subscribed(source->id)) { error = "copyRequired"; return false; }
            if (update && !draft.sourceId.empty() && !source) { error = "themeNotFound"; return false; }
            auto child = draft.effective;
            const bool updateChild = update && source && !Builtin(source->id);
            child.id = updateChild ? source->id : std::string{};
            if (updateChild) child.name = source->name;
            std::string childId;
            if (!Save(next, std::move(child), {}, updateChild, childId, error, newId, true)) return false;
            binding = childId; if (updateChild) changed.push_back(childId);
        }
    }
    std::string rootId;
    if (!Save(next, std::move(theme), {}, update, rootId, error, newId, true)) return false;
    if (update) changed.push_back(rootId);
    library = std::move(next); savedId = std::move(rootId); updatedIds = std::move(changed); return true;
}
inline void ReconcileReferences(Library& library, const SettingsValues& values)
{
    for (const auto target : AppearanceTargets)
    {
        auto found = library.references.find(std::string(target));
        if (found == library.references.end() || found->second.id.empty()) continue;
        const auto saved = Resolve(found->second.snapshot, found->second.id);
        auto current = CaptureTarget(target, values);
        if (!saved || !AppliedAppearanceMatches(saved->appearance, current.appearance) ||
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
        if (theme.kind != Kind::Global || !FullScope(theme.scopes)) continue;
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
        if (!FullScope(theme->scopes))
        {
            // Partial bar themes are never a new global appearance source.
            // Apply only their explicit objects, preserving unrelated surfaces.
            for (const auto& [scope, source] : {std::pair{Dock, "dock"}, {StatusBar, "statusBar"}, {Taskbar, "taskbar"}})
                if ((theme->scopes & scope) && !ApplyTarget(library, source, id, values, error)) return false;
            return true;
        }
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
        const bool bindingChanged = std::string_view(target) == "global" && FullScope(theme->scopes) &&
            (theme->quickPanel == changedId || theme->popup == changedId);
        if ((theme->id == changedId || bindingChanged) && ApplyTarget(library, target, theme->id, values, error))
            targets.emplace_back(target);
    }
    return targets;
}
}
