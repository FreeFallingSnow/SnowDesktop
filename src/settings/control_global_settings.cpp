#include "control_global_settings.h"
#include "control_settings_values.h"
#include "layout/layout_spacing_rules.h"
#include <cstring>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
Value* Child(Value& schema, const std::string& key)
{
    const auto properties = schema.object.find("properties");
    if (properties == schema.object.end()) return nullptr;
    const auto field = properties->second.object.find(key);
    return field == properties->second.object.end() ? nullptr : &field->second;
}
void Range(Value& schema, const char* key, double minimum, double maximum)
{
    if (auto* field = Child(schema, key))
    { field->object["minimum"] = Number(minimum); field->object["maximum"] = Number(maximum); }
}
void Choices(Value& schema, const char* key, std::initializer_list<int> values)
{
    if (auto* field = Child(schema, key))
    { auto choices = Array(); for (const int value : values) choices.array.push_back(Number(value)); field->object["enum"] = std::move(choices); }
}
void ReadOnly(Value& schema, const char* key, const char* reason)
{
    if (auto* field = Child(schema, key))
    { field->object["readOnly"] = Boolean(true); field->object["reason"] = String(reason); }
}
void Enrich(Value& schema, const std::string& path)
{
    // Ranges below are the stored units from the native controls and shared
    // production rules, rather than their percentage presentation values.
    if (Child(schema, "widgetAlpha"))
    {
        for (const auto* key : {"widgetBgR", "widgetBgG", "widgetBgB", "widgetBorderR", "widgetBorderG", "widgetBorderB",
                "widgetAlpha", "widgetBorderAlpha", "widgetEdgeHighlightStrength", "gradientEndA", "windowPinBorderR",
                "windowPinBorderG", "windowPinBorderB", "windowPinBorderOpacity"}) Range(schema, key, 0, 1);
        Range(schema, "widgetBorderWidth", .5, 4); Range(schema, "widgetEdgeHighlightWidth", .5, 4);
        Range(schema, "glassBlurRadius", 4, 48); Range(schema, "barHeight", 16, 48);
        Range(schema, "categorizedTabHeight", 24, 48); Range(schema, "luaWidgetContentRowHeight", 18, 48);
        Range(schema, "cornerRadius", 0, 100); Range(schema, "windowPinBorderWidth", 1, 20);
        Range(schema, "popupHoverDelayMs", 100, 3000); Range(schema, "tabHoverDelayMs", 100, 3000);
        Choices(schema, "contentTheme", {0, 1}); Choices(schema, "contextMenuStyle", {0, 1, 2, 3, 4, 5, 6});
        Choices(schema, "backgroundPreset", {0, 1, 6, 7, 9, 10, 11, 12, 13});
    }
    if (Child(schema, "minimumWidth") && Child(schema, "feather"))
        VisitEdgeLightFields([&](const char* key, auto, float minimum, float maximum) { Range(schema, key, minimum, maximum); });
    if (Child(schema, "stops") && Child(schema, "angle"))
    {
        Range(schema, "angle", 0, 360); Range(schema, "start", 0, 1); Range(schema, "end", 0, 1);
        auto* stops = Child(schema, "stops"); stops->object["minItems"] = Number(2); stops->object["maxItems"] = Number(5);
    }
    if (Child(schema, "position") && Child(schema, "opacity") && Child(schema, "color"))
    { Range(schema, "position", 0, 1); Range(schema, "opacity", 0, 1); Range(schema, "color", 0, 0xffffff); }
    if (Child(schema, "contentScale"))
    {
        for (const auto* key : {"backgroundOpacity", "edgeHighlightStrength", "backgroundStartR", "backgroundStartG", "backgroundStartB",
                "backgroundEndR", "backgroundEndG", "backgroundEndB", "textureHighlightStrength", "textureShadeStrength", "textureEdgeHighlight",
                "filterStrength", "filterTintR", "filterTintG", "filterTintB", "outlineOpacity", "outlineR", "outlineG", "outlineB", "shadowStrength"})
            Range(schema, key, 0, 1);
        Range(schema, "contentScale", .5, .9); Range(schema, "glassBlurRadius", 4, 48);
        Range(schema, "edgeHighlightWidth", .5, 4); Range(schema, "textureHighlightSize", .1, 1);
        Range(schema, "textureHighlightAngle", -1, 1); Range(schema, "outlineWidth", 0, 4);
        Choices(schema, "mode", path == "desktop.iconBeautify" ? std::initializer_list<int>{0, 1} : std::initializer_list<int>{0, 1, 2});
        Choices(schema, "gradientDirection", {0, 1, 2, 3}); Choices(schema, "backgroundSource", {0, 1, 2}); Choices(schema, "filterMode", {0, 1, 2});
    }
    if (Child(schema, "appearance") && Child(schema, "mode")) Range(schema, "mode", -2, path.find("statusBar") != path.npos ? 8 : 4);
    if (Child(schema, "appearance") && Child(schema, "themeMode") && Child(schema, "contentTheme"))
        Choices(schema, "contentTheme", {-1, 0, 1});
    if (path == "dock")
    {
        Range(schema, "edgeDistanceScale", kDockMinimumEdgeDistanceScale, kDockMaximumEdgeDistanceScale);
        Range(schema, "thicknessScale", .5, 2); Range(schema, "mergedIconScale", .5, 1);
        Range(schema, "hoverScale", 1, 2); Range(schema, "cornerRadius", 0, 28);
        Range(schema, "attentionDotSizeScale", .5, 2); Range(schema, "attentionDotOffset", -.25, .25);
        Range(schema, "edgeHoverDelayMs", dock_settings_rules::kMinimumEdgeHoverDelayMs, dock_settings_rules::kMaximumEdgeHoverDelayMs);
        Range(schema, "frequentItemCount", 1, 8); Range(schema, "mergedBarHeight", 32, 96);
        Choices(schema, "hoverEffect", {0, 1, 2}); Choices(schema, "launchEffect", {0, 1, 2}); Choices(schema, "windowEffect", {0, 1, 2, 3, 4});
        Choices(schema, "appearancePreset", {0, 1, 6, 7, 9, 10, 11, 13}); Choices(schema, "edgeRevealGesture", {0, 1});
        Choices(schema, "systemTaskbarAlignment", {0, 1}); Choices(schema, "systemTaskbarContentTheme", {-1, 0, 1});
        Choices(schema, "classicTaskbarSystemTheme", {-1, 0, 1});
        ReadOnly(schema, "showRunningApps", "Always enabled by the current product");
    }
    if (path == "general")
    {
        Choices(schema, "animationMode", {0, 1, 2}); Choices(schema, "popupAnimationEffect", {0, 1, 2});
        Choices(schema, "animationSpeed", {0, 1, 2, 3, 4, 5}); Choices(schema, "animationFrameLimit", {0, 30, 60, 120});
        Choices(schema, "quickNavTheme", {0, 1, 2, 3}); Choices(schema, "collectionPopupTheme", {0, 1, 2, 3});
        Range(schema, "tooltipFontSize", kMinimumTooltipFontSize, kMaximumTooltipFontSize);
        Range(schema, "desktopIdleHideDelaySeconds", 1, desktop_idle_hide::kMaxDelaySeconds);
        ReadOnly(schema, "autoStartEnabled", "Windows-owned projection; use set-auto-start action");
        if (auto* font = Child(schema, "font")) font->object["restartRequired"] = Boolean(true);
        if (auto* language = Child(schema, "language")) language->object["maxLength"] = Number(84);
    }
    if (path == "general.calendarDisplay")
    {
        ReadOnly(schema, "holidaysEnabled", "Retired preference"); ReadOnly(schema, "region", "Retired preference");
        auto choices = Array(); for (const auto* id : calendar::CalendarIds) choices.array.push_back(String(id));
        Child(schema, "calendar")->object["enum"] = std::move(choices);
    }
    if (path == "general.statusBar")
    {
        Choices(schema, "position", {0, 1}); Choices(schema, "edgeRevealGesture", {0, 1}); Range(schema, "scale", .75, 3);
        ReadOnly(schema, "legacyShellUi", "Retired scene retained for persistence"); ReadOnly(schema, "legacyVisibleWindow", "Retired scene retained for persistence");
    }
    if (path == "desktop")
    {
        ReadOnly(schema, "dockEnabled", "Shared enable switch; edit general.dockEnabled");
        Range(schema, "iconSpacingScale", layout_spacing_rules::kMinimumScale, layout_spacing_rules::kMaximumScale);
        Range(schema, "scrollingIconSpacingYScale", layout_spacing_rules::kMinimumScale, layout_spacing_rules::kMaximumRowSpacingScale);
        Range(schema, "listRowSpacingScale", layout_spacing_rules::kMinimumScale, layout_spacing_rules::kMaximumRowSpacingScale);
        Range(schema, "itemIconSizeScale", kMinimumItemIconSizeScale, kMaximumItemIconSizeScale);
        for (const auto* key : {"itemFontSizeCu", "listItemFontSizeCu", "fanTitleFontSizeCu"}) Range(schema, key, kMinimumItemFontSizeCu, kMaximumItemFontSizeCu);
        Range(schema, "itemFontWeight", font_weight_rules::kMinimumWeight, font_weight_rules::kMaximumWeight);
        Choices(schema, "desktopTitleLines", {1, 2}); Choices(schema, "largeFolderTitleLines", {1, 2}); Choices(schema, "scrollingTitleLines", {1, 2});
        Choices(schema, "shortcutArrowMode", {0, 1, 2});
    }
    if (path == "category") Range(schema, "tabFontSize", 10, 22);
    if (path == "navigation") Choices(schema, "mappingViewMode", {0, 1, 2});
    if (path == "navigation.layout")
    {
        Range(schema, "expandedWidth", 400, 1800); Range(schema, "collapsedWidth", 360, 1400); Range(schema, "maximumHeight", 220, 1400);
        Range(schema, "visibleRows", 1, 20); Range(schema, "padding", 8, 40); Range(schema, "searchHeight", 40, 80);
        Range(schema, "iconSize", 24, 96); Range(schema, "gridGap", 4, 48); Range(schema, "rowGap", 0, 48);
        Range(schema, "fontSize", 10, 24); Range(schema, "secondaryFontSize", 10, 20); Range(schema, "searchFontSize", 12, 24);
        Range(schema, "resultRowHeight", 40, 96); Range(schema, "labelLines", 1, 3);
        Range(schema, "cornerRadius", 0, 32); Range(schema, "searchRadius", 0, 24); Range(schema, "tabRadius", 0, 20); Range(schema, "itemRadius", 0, 24);
    }
    if (auto fields = schema.object.find("properties"); fields != schema.object.end())
        for (auto& [key, field] : fields->second.object)
        {
            if (key == "modifiers" || key.ends_with("Modifiers")) Range(schema, key.c_str(), 0, 15);
            if (key == "virtualKey" || key.ends_with("VirtualKey")) Range(schema, key.c_str(), 0, 255);
            Enrich(field, path.empty() ? key : path + "." + key);
        }
    if (auto items = schema.object.find("items"); items != schema.object.end()) Enrich(items->second, path + "[]");
}
void ValidatePatch(const Value& patch, const Value& schema, const std::string& path)
{
    if (const auto* readOnly = schema.Find("readOnly"); readOnly && readOnly->boolean)
        throw Error("read_only", path + " is not directly writable");
    if (const auto* choices = schema.Find("enum"); choices && !choices->array.empty() &&
        std::none_of(choices->array.begin(), choices->array.end(), [&](const auto& item) { return Serialize(item) == Serialize(patch); }))
        throw Error("invalid_params", path + " has an unsupported value", -32602);
    if (patch.IsNumber())
    {
        const auto* minimum = schema.Find("minimum"); const auto* maximum = schema.Find("maximum");
        if (!std::isfinite(patch.number) || (minimum && patch.number < minimum->number) || (maximum && patch.number > maximum->number))
            throw Error("invalid_params", path + " is outside its supported range", -32602);
    }
    if (patch.IsString())
        if (const auto* maximum = schema.Find("maxLength"); maximum && patch.string.size() > maximum->number)
            throw Error("invalid_params", path + " is too long", -32602);
    if (patch.IsArray())
    {
        const auto* minimum = schema.Find("minItems"); const auto* maximum = schema.Find("maxItems");
        if ((minimum && patch.array.size() < minimum->number) || (maximum && patch.array.size() > maximum->number))
            throw Error("invalid_params", path + " has an unsupported array length", -32602);
        if (const auto* items = schema.Find("items"))
            for (const auto& item : patch.array) ValidatePatch(item, *items, path + "[]");
    }
    if (patch.IsObject())
    {
        const auto* properties = schema.Find("properties");
        if (properties)
            for (const auto& [key, value] : patch.object)
            {
                const auto* field = properties->Find(key);
                if (!field) throw Error("invalid_params", path + "." + key + " is unknown", -32602);
                ValidatePatch(value, *field, path.empty() ? key : path + "." + key);
            }
    }
}
void StripReadOnly(Value& value, const Value& schema)
{
    const auto* properties = schema.Find("properties"); if (!properties || !value.IsObject()) return;
    for (auto it = value.object.begin(); it != value.object.end();)
    {
        const auto* field = properties->Find(it->first);
        const auto* readOnly = field ? field->Find("readOnly") : nullptr;
        if (readOnly && readOnly->boolean) it = value.object.erase(it);
        else { if (field) StripReadOnly(it->second, *field); ++it; }
    }
}
Value Values(const SettingsValues& values, const Target& target)
{
    auto result = ToValue(values);
    return target.kind == "global" ? result : result.object.at(target.kind);
}
Value Schema(const Target& target)
{
    SettingsValues defaults; defaults.category = CategorySettings::Defaults();
    auto schema = DescribeValue(defaults); Enrich(schema, {});
    return target.kind == "global" ? schema : schema.object.at("properties").object.at(target.kind);
}
std::string CurrentRevision(const SettingsSnapshot& snapshot, const Target& target)
{ return std::to_string(snapshot.generation) + ":" + std::to_string(snapshot.revision) + ":" + Revision(Values(snapshot.values, target)); }
Value Response(const Target& target, const SettingsSnapshot& snapshot, const char* status)
{
    return Object({{"target", TargetJson(target)}, {"revision", String(CurrentRevision(snapshot, target))},
        {"values", Values(snapshot.values, target)}, {"status", String(status)},
        {"persistence", String(status == std::string_view("applied") ? "completed" : "unchanged")}});
}
void CheckCorrelatedValues(const SettingsValues& values, const Value& changes, const GlobalSettingsContext& context)
{
    if (const auto* general = changes.Find("general"))
    {
        if (general->Find("language") && context.languageAvailable && !context.languageAvailable(values.general.language))
            throw Error("invalid_params", "The requested language is not installed", -32602);
        if (general->Find("font") && context.fontAvailable && !context.fontAvailable(values.general.font))
            throw Error("invalid_params", "The requested font is not installed", -32602);
        if (general->Find("diagnosticLogModules") && values.general.diagnosticLogModules != diagnostics::NormalizeLogModules(values.general.diagnosticLogModules))
            throw Error("invalid_params", "Unknown diagnostic log module flags", -32602);
    }
    const auto checkAppearance = [](const PersonalizationSettings& value) {
        if (!ValidatePanelGradient(value.panelGradient) || !ValidateEdgeLight(value.edgeLight))
            throw Error("invalid_params", "Invalid panel gradient or edge-light configuration", -32602);
    };
    if (changes.Find("personalization")) checkAppearance(values.personalization);
    if (changes.Find("general"))
    {
        const auto& general = values.general;
        for (const auto* surface : {&general.quickNavigationAppearance, &general.collectionPopupAppearance,
                &general.globalQuickNavigationAppearance, &general.globalCollectionPopupAppearance,
                &general.statusBar.theme, &general.statusBar.noWindow.theme, &general.statusBar.maximizedWindow.theme})
            checkAppearance(surface->appearance);
    }
    if (changes.Find("dock"))
    {
        checkAppearance(values.dock.customAppearance); checkAppearance(values.dock.systemTaskbarAppearance);
        for (const auto* rule : {&values.dock.systemTaskbarVisibleWindow, &values.dock.systemTaskbarMaximizedWindow, &values.dock.systemTaskbarShellUi})
            checkAppearance(rule->appearance);
    }
    if (changes.Find("desktop") && (!ValidatePanelGradient(values.desktop.iconBeautify.backgroundGradient) || !ValidateEdgeLight(values.desktop.iconBeautify.edgeLight)))
        throw Error("invalid_params", "Invalid icon gradient or edge-light configuration", -32602);
    if (changes.Find("navigation") && !ValidateNavigationSearchConfiguration(values.navigation))
        throw Error("invalid_params", "Quick Navigation search engines or prefixes are invalid", -32602);
    // Native normalizers also validate cross-field collections. Reject a
    // requested value that would be silently removed or substituted; unrelated
    // legacy values are not grounds to reject an otherwise valid field edit.
    auto normalized = values;
    if (changes.Find("dock")) NormalizeDockSettings(normalized.dock);
    if (changes.Find("navigation")) NormalizeNavigationSettings(normalized.navigation);
    if (changes.Find("general"))
    {
        NormalizeGeneralAnimationSettings(normalized.general);
        NormalizeStatusBarSettings(normalized.general.statusBar);
        shell_extensions::Normalize(normalized.general.shellExtensions);
        calendar::Normalize(normalized.general.calendarDisplay);
    }
    const auto original = ToValue(values), canonical = ToValue(normalized);
    const auto checkRequested = [&](const auto& self, const Value& requested, const Value& before, const Value& after, const std::string& path) -> void {
        if (requested.IsObject() && before.IsObject() && after.IsObject())
        {
            for (const auto& [key, child] : requested.object)
            {
                const auto* originalField = before.Find(key); const auto* canonicalField = after.Find(key);
                if (!originalField || !canonicalField)
                    throw Error("invalid_params", path + "." + key + " does not satisfy the native setting rules", -32602);
                self(self, child, *originalField, *canonicalField, path.empty() ? key : path + "." + key);
            }
        }
        else if (Serialize(before) != Serialize(after))
            throw Error("invalid_params", path + " does not satisfy the native setting rules", -32602);
    };
    checkRequested(checkRequested, changes, original, canonical, {});
}
Value ResetChanges(const Value& params, const Value& schema)
{
    auto defaults = *schema.Find("default");
    if (const auto* keys = params.Find("keys"))
    {
        if (!keys->IsArray() || keys->array.empty() || keys->array.size() > 256)
            throw Error("invalid_params", "keys must contain 1..256 setting paths", -32602);
        auto result = Object();
        for (const auto& entry : keys->array)
        {
            if (!entry.IsString() || entry.string.empty()) throw Error("invalid_params", "Reset key must be a field path", -32602);
            const Value* source = &defaults; Value* destination = &result;
            std::string_view path = entry.string;
            while (true)
            {
                const auto dot = path.find('.'); const auto key = std::string(path.substr(0, dot));
                if (key.empty() || !source->Find(key)) throw Error("invalid_params", "Unknown reset field", -32602);
                source = source->Find(key);
                if (dot == path.npos) { destination->object[key] = *source; break; }
                auto [next, inserted] = destination->object.try_emplace(key, Object()); (void)inserted;
                if (!next->second.IsObject()) throw Error("invalid_params", "Overlapping reset paths", -32602);
                destination = &next->second; path.remove_prefix(dot + 1);
            }
        }
        return result;
    }
    StripReadOnly(defaults, schema); return defaults;
}
}
bool IsGlobalSettingsTarget(const Target& target)
{
    return target.kind == "global" || target.kind == "personalization" || target.kind == "dock" || target.kind == "navigation" ||
        target.kind == "general" || target.kind == "category" || target.kind == "desktop";
}
Value HandleGlobalSettings(std::string_view method, const Value& params, const Target& target, const GlobalSettingsContext& context)
{
    if (!IsGlobalSettingsTarget(target) || !target.id.empty()) throw Error("invalid_target", "Global settings targets do not take an id");
    const auto current = context.controller.Snapshot();
    if (!current || !current->initialized || current->externalReplacementPending) throw Error("not_ready", "Settings are not ready for access");
    if (method == "settings.get") { Fields(params, {"target"}); return Response(target, *current, "current"); }
    if (method == "settings.catalog")
    {
        Fields(params, {"target"}); auto result = Response(target, *current, "current");
        result.object.emplace("fields", Schema(target));
        result.object.emplace("actions", Array());
        if (target.kind == "global" || target.kind == "general")
            result.object.at("actions").array.push_back(Object({{"id", String("set-auto-start")}, {"arguments", Object({{"enabled", Object({{"type", String("boolean")}})}})},
                {"effects", String("updates Windows logon task ownership")}}));
        return result;
    }
    if (method != "settings.update" && method != "settings.reset" && method != "settings.action")
        throw Error("method_not_found", "Unknown settings operation", -32601);
    RequireRevision(params, CurrentRevision(*current, target));
    if (current->dirtyDomains != SettingsDomain::None || current->pendingCommitDomains != SettingsDomain::None || current->retryRequired)
        throw Error("busy", "Existing settings edits must finish before a direct change");
    if (method == "settings.action")
    {
        Fields(params, {"target", "ifRevision", "action", "arguments"});
        if ((target.kind != "global" && target.kind != "general") || Text(params, "action") != "set-auto-start")
            throw Error("unsupported_action", "Action is not defined for this settings target");
        const auto* arguments = params.Find("arguments");
        if (!arguments) throw Error("invalid_params", "set-auto-start requires enabled", -32602);
        Fields(*arguments, {"enabled"}); const auto* enabled = arguments->Find("enabled");
        if (!enabled || !enabled->IsBoolean()) throw Error("invalid_params", "enabled must be boolean", -32602);
        SettingsHostActions::Request request; request.action = SettingsHostActions::Action::SetAutoStartEnabled; request.boolValue = enabled->boolean;
        const auto outcome = context.controller.InvokeHostAction(request);
        if (!outcome.Succeeded()) throw Error(outcome.status == SettingsActionStatus::Busy ? "busy" : "settings_failed", values_detail::Utf8(outcome.message));
        return Response(target, *context.controller.Snapshot(), "applied");
    }
    const auto schema = Schema(target);
    Value changes;
    if (method == "settings.update")
    {
        Fields(params, {"target", "ifRevision", "changes"}); const auto* requested = params.Find("changes");
        if (!requested || !requested->IsObject() || requested->object.empty()) throw Error("invalid_params", "changes must be a nonempty named object", -32602);
        changes = *requested;
    }
    else { Fields(params, {"target", "ifRevision", "keys"}); changes = ResetChanges(params, schema); }
    ValidatePatch(changes, schema, target.kind == "global" ? "" : target.kind);
    Value allChanges = target.kind == "global" ? changes : Object({{target.kind, changes}});
    auto candidate = current->values; PatchValue(candidate, allChanges);
    if (const auto* general = allChanges.Find("general"); general && general->Find("dockEnabled"))
        candidate.desktop.dockEnabled = candidate.general.dockEnabled;
    CheckCorrelatedValues(candidate, allChanges, context);
    // All fields have been decoded and checked before the first mutation.
    // Domain stores can still fail independently; the result reports that
    // state rather than promising an atomic multi-file transaction.
    if (allChanges.Find("personalization")) context.controller.UpdatePersonalization(std::move(candidate.personalization), SettingsUpdateMode::PreviewAndCommit);
    if (allChanges.Find("dock")) context.controller.UpdateDock(std::move(candidate.dock), SettingsUpdateMode::PreviewAndCommit);
    if (allChanges.Find("navigation")) context.controller.UpdateNavigation(std::move(candidate.navigation), SettingsUpdateMode::PreviewAndCommit);
    if (allChanges.Find("general")) context.controller.UpdateGeneral(std::move(candidate.general), SettingsUpdateMode::PreviewAndCommit);
    if (allChanges.Find("category")) context.controller.UpdateCategory(std::move(candidate.category), SettingsUpdateMode::PreviewAndCommit);
    if (allChanges.Find("desktop")) context.controller.UpdateDesktop(std::move(candidate.desktop), SettingsUpdateMode::PreviewAndCommit);
    const auto outcome = context.controller.FlushPending();
    auto result = Response(target, *context.controller.Snapshot(), outcome.Succeeded() ? "applied" : "failed");
    result.object["persistence"] = String(outcome.Succeeded() ? "completed" : "incomplete");
    result.object.emplace("message", String(values_detail::Utf8(outcome.message)));
    result.object.emplace("completedDomains", Number(static_cast<unsigned>(outcome.completedDomains)));
    result.object.emplace("failedDomains", Number(static_cast<unsigned>(outcome.failedDomains)));
    result.object.emplace("effects", Object({{"restartRequired", Boolean(allChanges.Find("general") && allChanges.Find("general")->Find("font"))},
        {"systemChangesMayBeAsynchronous", Boolean(allChanges.Find("dock") != nullptr)},
        {"desktopPresentationMayChange", Boolean(allChanges.Find("general") &&
            (allChanges.Find("general")->Find("softwareDesktopEnabled") || allChanges.Find("general")->Find("demoModeEnabled") ||
             allChanges.Find("general")->Find("dockEnabled")))},
        {"collectionMembershipMayChange", Boolean(allChanges.Find("category") != nullptr)}}));
    return result;
}
}
