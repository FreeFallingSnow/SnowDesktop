#pragma once
#include "theme/theme_library_settings.h"

namespace snowdesktop::themes::detail
{
inline widget_runtime::WidgetHostAppearancePatch WidgetAppearanceSnapshotPatch(
    const widget_runtime::WidgetHostAppearanceState& state)
{
    widget_runtime::WidgetHostAppearancePatch patch;
    patch.followPersonalization = state.followPersonalization; patch.presetId = state.presetId;
    patch.backgroundColor = state.backgroundColor; patch.borderColor = state.borderColor;
    patch.backgroundOpacity = state.backgroundOpacity; patch.borderOpacity = state.borderOpacity;
    patch.borderWidth = state.borderWidth; patch.edgeHighlightEnabled = state.edgeHighlightEnabled;
    patch.edgeHighlightWidth = state.edgeHighlightWidth; patch.edgeHighlightStrength = state.edgeHighlightStrength;
    patch.edgeLight = state.edgeLight; patch.gradientEndOpacity = state.gradientEndOpacity;
    patch.glassEnabled = state.glassEnabled; patch.acrylicEnabled = state.acrylicEnabled;
    patch.contentTheme = state.contentTheme; patch.panelGradient = state.panelGradient;
    return patch;
}

struct WidgetThemeMutation { bool succeeded = false, changed = false; };
struct WidgetThemeSelectionResult
{
    bool committed = false, mutationSucceeded = false, mutationChanged = false;
    bool rollbackAttempted = false, rollbackSucceeded = false;
};

// Hold the library writer lock across snapshot selection and the guarded host
// mutation. A failed host write leaves the previous reference file untouched;
// a failed library write restores only this mutation's host revision, never a
// stale copy of another writer's references.
template<class Apply, class Rollback>
WidgetThemeSelectionResult SelectWidgetTheme(const std::filesystem::path& path,
    const std::string& target, const std::string& id, Apply&& apply, Rollback&& rollback,
    Library& output, std::string& error)
{
    WidgetThemeSelectionResult result;
    result.committed = Transact(path, [&](Library& library, std::string& detail) {
        if (!Select(library, target, id, Kind::Global, Components, detail)) return false;
        const auto& reference = library.references.at(target);
        const auto frozen = Resolve(reference.snapshot, reference.id);
        if (!frozen) { detail = "invalidSelection"; return false; }
        const auto mutation = apply(WidgetPatch(*frozen));
        result.mutationSucceeded = mutation.succeeded; result.mutationChanged = mutation.changed;
        if (!mutation.succeeded) { detail = "widgetApplyFailed"; return false; }
        return true;
    }, output, error);
    if (!result.committed && result.mutationChanged)
    {
        result.rollbackAttempted = true;
        result.rollbackSucceeded = rollback();
    }
    return result;
}
}
