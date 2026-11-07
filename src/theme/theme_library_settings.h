#pragma once
#include "theme_library.h"
#include "settings/settings_values.h"
#include "widget/settings/widget_settings_model.h"
#include <array>

namespace snowdesktop::themes
{
std::wstring ExportFileName(std::wstring name);
std::filesystem::path LibraryPath();
widget_runtime::WidgetHostAppearancePatch WidgetPatch(const Theme& theme);
bool WidgetMatches(const widget_runtime::WidgetHostAppearanceState& state, const Theme& theme);
Kind TargetKind(std::string_view target);
unsigned TargetScope(std::string_view target);
// Private settings source identities.
inline constexpr std::array<std::string_view, 12> AppearanceTargets{
    "global", "dock", "statusBar", "taskbar", "quickPanel", "popup", "icon",
    "statusBar/noWindow", "statusBar/maximizedWindow", "taskbar/shellUi", "taskbar/maximizedWindow", "taskbar/visibleWindow"};
StatusBarAppearanceRule StatusBarSettings::* StatusRule(std::string_view target);
SystemTaskbarDynamicRule DockSettings::* TaskbarRule(std::string_view target);
Theme CaptureTarget(std::string_view target, const SettingsValues& values);
Theme CaptureGlobalBinding(Kind kind, const SettingsValues& values, const std::optional<Theme>& source);
bool PrepareGlobalCustomEdit(GeneralSettings& settings,
    const PersonalizationSettings& previous, const PersonalizationSettings& edited);
void EditSurfaceAppearance(GeneralSettings& settings, bool quick,
    const PersonalizationSettings& appearance, const PersonalizationSettings& global);
void CopyComponentAppearanceToPopup(GeneralSettings& settings,
    const PersonalizationSettings& componentAppearance);
bool AppliedAppearanceMatches(const PersonalizationSettings& saved,
    const PersonalizationSettings& current);
// Private settings draft transaction. Existing child identities are updated only
// when explicitly saving back to an editable global source.
struct BindingDraft
{
    Theme effective;
    std::string selectedId, sourceId;
};
bool SaveDraft(Library& library, Theme theme, const std::array<BindingDraft, 2>& bindings,
    bool update, std::string& savedId, std::vector<std::string>& updatedIds, std::string& error,
    const NewId& newId = CreateId);
void ReconcileReferences(Library& library, const SettingsValues& values);
bool FollowQuickBinding(const Library& library, NavigationSettings& navigation);
// Built-in selections own the complete quick-panel appearance, including the
// palette and layout stored separately from its material. Legacy conditional
// profiles and independent custom drafts retain their historical values.
bool RestoreBuiltinQuickPanel(NavigationSettings& navigation,
    const GeneralSettings& general, const PersonalizationSettings& global);
bool PrepareGlobalThemeEdit(SettingsValues& values,
    const PersonalizationSettings& previous);
void PrepareQuickPanelSelectionEdit(SettingsValues& values,
    const GeneralSettings& previous, const Library* library = nullptr);
bool ApplyTarget(const Library& library, std::string_view target, std::string_view id,
    SettingsValues& values, std::string& error);
// Change only a currently followed child and its last-successful graph. This
// graph is separate from the authored package and can select a different source.
bool ApplyFollowedGlobalBinding(Reference& reference, const Theme& nextTheme,
    std::string_view previous, SettingsValues& values, bool& changed, std::string& error);
std::vector<std::string> ApplySavedUpdate(const Library& library, std::string_view changedId,
    SettingsValues& values, std::string& error,
    std::map<std::string, Reference>* appliedReferences = nullptr);
// A saved snapshot preserves appearance after detach, not an active selection.
// Source switches require a live reference and matching current values.
bool SwitchAppliedVersion(Library& library, std::string_view previous,
    std::string_view replacement, SettingsValues& values,
    std::vector<std::string>& targets, std::string& error);
}
