#include "control_theme_settings.h"

#include "control_settings_values.h"
#include "theme/theme_library_settings.h"
#include <algorithm>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
namespace theme = themes;

[[noreturn]] void Failed(std::string_view detail)
{
    throw Error(detail == "libraryBusy" ? "busy" : "theme_operation_failed", std::string(detail));
}

std::string Encode(const theme::Library& library)
{
    std::string error;
    auto encoded = theme::EncodeLibrary(library, error);
    if (encoded.empty()) Failed(error);
    return encoded;
}

std::string Identity(const theme::Library& library, const SettingsSnapshot& snapshot)
{
    return Revision(Object({{"library", String(Encode(library))},
        {"generation", String(std::to_string(snapshot.generation))}, {"settings", ToValue(snapshot.values)}}));
}

void SelectionTarget(std::string_view target)
{
    if (std::find(theme::AppearanceTargets.begin(), theme::AppearanceTargets.end(), target) == theme::AppearanceTargets.end())
        throw Error("invalid_params", "Unknown appearance selectionTarget", -32602);
}

Value Summary(const theme::Theme& item)
{
    return Object({{"id", String(item.id)}, {"name", String(item.name)},
        {"kind", String(theme::KindName(item.kind))}, {"scopes", Number(item.scopes)},
        {"builtin", Boolean(theme::Builtin(item.id).has_value())}});
}

Value Metadata(const theme::Theme& item)
{
    auto result = Summary(item);
    result.object.emplace("appearance", ToValue(item.appearance));
    result.object.emplace("icon", ToValue(item.icon));
    result.object.emplace("layout", ToValue(item.layout));
    result.object.emplace("colors", ToValue(item.colors));
    result.object.emplace("quickPanel", String(item.quickPanel));
    result.object.emplace("popup", String(item.popup));
    return result;
}

Value State(const Target& target, const theme::Library& library, const SettingsSnapshot& snapshot)
{
    if (target.kind == "theme")
    {
        const auto item = theme::Resolve(library.themes, target.id);
        if (!item) throw Error("settings_target_not_found", "Saved or built-in theme no longer exists");
        auto result = Metadata(*item);
        result.object.emplace("references", ToValue(theme::References(library, target.id)));
        bool subscribed = false;
        for (const auto& [id, origin] : library.workshop) { (void)id; subscribed = subscribed || origin.ids.contains(target.id); }
        result.object.emplace("subscribed", Boolean(subscribed));
        return result;
    }
    auto entries = Array();
    for (const auto& [id, item] : library.themes) { (void)id; entries.array.push_back(Summary(item)); }
    theme::Library empty;
    for (const auto kind : {theme::Kind::Global, theme::Kind::QuickPanel, theme::Kind::Popup, theme::Kind::Icon})
        for (const auto& item : theme::Choices(empty, kind)) entries.array.push_back(Summary(item));
    auto selections = Object();
    for (const auto key : theme::AppearanceTargets)
    {
        const auto found = library.references.find(std::string(key));
        auto current = Metadata(theme::CaptureTarget(key, snapshot.values));
        current.object["id"] = String(found == library.references.end() ? "" : found->second.id);
        selections.object.emplace(std::string(key), std::move(current));
    }
    return Object({{"entries", std::move(entries)}, {"selections", std::move(selections)}});
}

Value Envelope(const Target& target, const theme::Library& library, const SettingsSnapshot& snapshot)
{
    return Object({{"target", TargetJson(target)}, {"revision", String(Identity(library, snapshot))},
        {"values", State(target, library, snapshot)}});
}

bool ApplyValues(ISettingsController& controller, const SettingsValues& values)
{
    controller.UpdateDesktop(values.desktop, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdatePersonalization(values.personalization, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateDock(values.dock, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateNavigation(values.navigation, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateGeneral(values.general, SettingsUpdateMode::PreviewAndCommit);
    return controller.FlushAll().Succeeded();
}

std::string Id(const Value& arguments, const Target& target)
{
    const auto id = Text(arguments, "id", target.kind != "theme");
    if (target.kind == "theme" && !id.empty() && id != target.id)
        throw Error("invalid_params", "Theme id must match target.id", -32602);
    return target.kind == "theme" ? target.id : id;
}

bool Select(theme::Library& library, std::string_view target, std::string_view id,
    SettingsValues& values, std::string& error)
{
    if (!theme::ApplyTarget(library, target, id, values, error)) return false;
    const auto selected = theme::Resolve(library.themes, id);
    if (!selected) return false;
    if (target == "global" && !theme::FullScope(selected->scopes))
    {
        for (const auto& [scope, key] : {std::pair{theme::Dock, "dock"},
            {theme::StatusBar, "statusBar"}, {theme::Taskbar, "taskbar"}})
            if ((selected->scopes & scope) && !theme::Select(library, key, id, theme::Kind::Global, scope, error)) return false;
        return true;
    }
    return theme::Select(library, std::string(target), id, theme::TargetKind(target),
        target == "global" ? selected->scopes : theme::TargetScope(target), error);
}

std::filesystem::path PackagePath(const Value& arguments)
{
    const auto text = Text(arguments, "path");
    if (text.empty() || text.size() > 32768 || text.find('\0') != std::string::npos)
        throw Error("invalid_params", "Invalid package path", -32602);
    const auto path = std::filesystem::path(values_detail::Wide(text));
    const auto root = path.root_name().wstring();
    if (!path.is_absolute() || root.size() != 2 || root[1] != L':' ||
        theme::Classify(path) != theme::PackageType::Theme)
        throw Error("invalid_params", "An absolute local .snowtheme path is required", -32602);
    return path;
}

std::string PackageIdentity(const theme::Package& package)
{
    std::string error;
    const auto encoded = theme::EncodePackage(package, error);
    if (encoded.empty()) Failed(error);
    return encoded;
}

void ApplyWidgetReferences(const ThemeSettingsContext& context, theme::Library& library,
    const std::vector<std::string>& targets, std::vector<std::string>& pending)
{
    struct Applied
    {
        std::string target;
        theme::Reference previous;
        std::string sourceIdentity;
    };
    std::vector<Applied> applied;
    for (const auto& key : targets)
    {
        const auto found = library.references.find(key);
        if (found == library.references.end() || found->second.id.empty()) { pending.push_back(key); continue; }
        const auto previous = theme::Resolve(found->second.snapshot, found->second.id);
        const auto next = theme::Resolve(library.themes, found->second.id);
        theme::Package source; std::string error;
        if (!previous || !next || !context.applyWidgetTheme ||
            !theme::Export(library, found->second.id, source, error)) { pending.push_back(key); continue; }
        bool succeeded = false;
        try { succeeded = context.applyWidgetTheme(std::string_view(key).substr(7), *previous, *next); }
        catch (...) { succeeded = false; }
        if (succeeded) applied.push_back({key, found->second, PackageIdentity(source)});
        else pending.push_back(key);
    }
    if (applied.empty()) return;
    // Widget persistence may itself touch the theme library. Do not hold its
    // writer lock across the guarded callback. Record only a still-matching
    // source and last-successful snapshot in a fresh transaction afterwards.
    theme::Library recorded; std::string error;
    std::vector<std::string> changedDuringApply;
    const bool saved = theme::Transact(context.libraryPath, [&](auto& current, auto& detail) {
        for (const auto& item : applied)
        {
            const auto found = current.references.find(item.target);
            theme::Package source;
            if (found == current.references.end() || found->second.id != item.previous.id ||
                found->second.kind != item.previous.kind || found->second.scope != item.previous.scope ||
                PackageIdentity(found->second.snapshot) != PackageIdentity(item.previous.snapshot) ||
                !theme::Export(current, item.previous.id, source, detail) ||
                PackageIdentity(source) != item.sourceIdentity)
            { changedDuringApply.push_back(item.target); detail.clear(); continue; }
            if (!theme::Select(current, item.target, item.previous.id, item.previous.kind, item.previous.scope, detail)) return false;
        }
        return true;
    }, recorded, error);
    if (saved)
    {
        library = std::move(recorded);
        pending.insert(pending.end(), changedDuringApply.begin(), changedDuringApply.end());
    }
    else
    {
        for (const auto& item : applied) pending.push_back(item.target);
        theme::Library refreshed;
        if (theme::Load(context.libraryPath, refreshed, error)) library = std::move(refreshed);
    }
}
}

bool IsThemeSettingsTarget(const Target& target) noexcept
{ return target.kind == "themes" || target.kind == "theme"; }

Value HandleThemeSettings(std::string_view method, const Value& params,
    const Target& target, const ThemeSettingsContext& context)
{
    if (!IsThemeSettingsTarget(target) || (target.kind == "themes" ? !target.id.empty() : target.id.empty()))
        throw Error("invalid_params", "themes has no id; theme requires an id", -32602);
    const auto snapshot = context.controller.Snapshot();
    if (!snapshot || !snapshot->initialized || snapshot->externalReplacementPending || context.libraryPath.empty())
        throw Error("settings_unavailable", "Theme settings controller is unavailable");
    theme::Library library;
    std::string error;
    if (!theme::Load(context.libraryPath, library, error)) Failed(error);
    if (target.kind == "theme" && !theme::Resolve(library.themes, target.id))
        throw Error("settings_target_not_found", "Saved or built-in theme no longer exists");
    // Reconciliation is read-only here: catalog/get never write the library.
    auto visible = library;
    theme::ReconcileReferences(visible, snapshot->values);
    if (method == "settings.catalog" || method == "settings.get")
    {
        Fields(params, {"target"});
        auto output = Envelope(target, visible, *snapshot);
        // The optimistic token binds the file including references that were
        // detached for display, so concurrent writers cannot be overwritten.
        output.object["revision"] = String(Identity(library, *snapshot));
        if (method == "settings.catalog")
        {
            output.object.emplace("fields", Object());
            auto actions = Array();
            const auto action = [&](const char* name, Value arguments, const char* effect) {
                actions.array.push_back(Object({{"name", String(name)}, {"arguments", std::move(arguments)},
                    {"effect", String(effect)}, {"requiresRevision", Boolean(true)}}));
            };
            action("apply", Object({{"selectionTarget", String("appearance-target")}, {"id", String("theme-id")}}), "Apply a compatible saved/built-in theme through the settings controller");
            action("detach", Object({{"selectionTarget", String("appearance-target")}}), "Keep appearance and detach its live saved reference");
            action("save-as", Object({{"selectionTarget", String("appearance-target")}, {"name", String("display name")}, {"scopes", Number(theme::All)}}), "Save the current effective material and bound child snapshots under a new id");
            action("update-saved", Object({{"selectionTarget", String("appearance-target")}, {"id", String("theme-id")}, {"name", String("display name")}}), "Explicitly update the saved source and applicable active settings and guarded widget bindings; report any pending targets");
            action("remove", Object({{"id", String("theme-id")}, {"replacement", String("optional-compatible-theme-id")}}), "Remove a local theme, preserving object appearances; child dependencies require replacement");
            action("import", Object({{"path", String("absolute .snowtheme path")}}), "Validate and atomically import a local package without applying it");
            action("export", Object({{"id", String("theme-id")}, {"path", String("absolute .snowtheme path")}}), "Export the complete dependency closure to a local package");
            output.object.emplace("actions", std::move(actions));
            output.object.emplace("selectionTargets", ToValue(std::vector<std::string>(theme::AppearanceTargets.begin(), theme::AppearanceTargets.end())));
            output.object.emplace("boundaries", ToValue(std::vector<std::string>{"No Workshop/network publication", "Unavailable or concurrently changed widget bindings remain explicitly pending", "Saved definitions are managed by explicit actions, not arbitrary JSON replacement"}));
        }
        return output;
    }
    if (method != "settings.action") throw Error("unsupported_setting", "Theme selections and saved definitions use the catalog's named actions");
    Fields(params, {"target", "ifRevision", "action", "arguments"});
    RequireRevision(params, Identity(library, *snapshot));
    const auto command = Text(params, "action");
    const auto* supplied = params.Find("arguments");
    const Value empty = Object();
    const auto& arguments = supplied ? *supplied : empty;
    if (!arguments.IsObject()) throw Error("invalid_params", "arguments must be a named object", -32602);
    const auto originalIdentity = Encode(library);
    if (command == "export")
    {
        Fields(arguments, {"id", "path"});
        theme::Package package;
        if (!theme::Export(library, Id(arguments, target), package, error) || !theme::WritePackage(PackagePath(arguments), package, error)) Failed(error);
        auto output = Envelope(target, visible, *snapshot);
        output.object["revision"] = String(Identity(library, *snapshot));
        output.object.emplace("status", String("exported"));
        return output;
    }
    if (command != "apply" && command != "detach" && command != "save-as" && command != "update-saved" && command != "remove" && command != "import")
        throw Error("unsupported_setting", "Unknown local theme action");
    if (command == "apply") Fields(arguments, {"id", "selectionTarget"});
    else if (command == "detach") Fields(arguments, {"selectionTarget"});
    else if (command == "remove") Fields(arguments, {"id", "replacement"});
    else if (command == "import") Fields(arguments, {"path"});
    else Fields(arguments, {"id", "selectionTarget", "name", "scopes"});
    if (command == "apply" || command == "detach" || command == "save-as" || command == "update-saved")
        SelectionTarget(Text(arguments, "selectionTarget"));
    if (command == "apply" || command == "remove" || command == "update-saved") (void)Id(arguments, target);
    if (!context.controller.FlushAll().Succeeded()) throw Error("settings_failed", "Pending settings could not be committed before the theme action");
    if (!context.controller.IsGenerationCurrent(snapshot->generation)) throw Error("stale_settings", "Settings session changed");
    const auto flushed = context.controller.Snapshot();
    if (!flushed || Identity(library, *flushed) != Identity(library, *snapshot))
        throw Error("stale_settings", "Settings values changed while flushing; query them again");
    theme::Package imported;
    if (command == "import")
    {
        Fields(arguments, {"path"});
        if (!theme::ReadPackage(PackagePath(arguments), imported, error)) Failed(error);
    }
    std::string savedId;
    std::vector<std::string> changedIds, pending, widgetTargets;
    bool touchedSettings = false, reconcileIcon = false, staleLibrary = false;
    std::map<std::string, std::string> mapping;
    const bool succeeded = theme::Transact(context.libraryPath, [&](theme::Library& current, std::string& detail) {
        if (Encode(current) != originalIdentity) { staleLibrary = true; detail = "staleSettings"; return false; }
        theme::ReconcileReferences(current, snapshot->values);
        if (command == "import") return theme::Import(current, imported, mapping, detail);
        if (command == "detach")
        {
            Fields(arguments, {"selectionTarget"});
            const auto selection = Text(arguments, "selectionTarget"); SelectionTarget(selection);
            theme::Detach(current, selection); return true;
        }
        const auto id = command == "save-as" ? std::string{} : Id(arguments, target);
        if (command == "remove")
        {
            Fields(arguments, {"id", "replacement"});
            const auto removed = theme::Resolve(current.themes, id);
            reconcileIcon = removed && removed->kind == theme::Kind::Icon;
            return theme::Remove(current, id, Text(arguments, "replacement", false), true, detail);
        }
        const auto selection = Text(arguments, "selectionTarget"); SelectionTarget(selection);
        auto nextValues = snapshot->values;
        if (command == "apply")
        {
            Fields(arguments, {"id", "selectionTarget"});
            if (!Select(current, selection, id, nextValues, detail)) return false;
            touchedSettings = true;
            if (ApplyValues(context.controller, nextValues)) return true;
            detail = "settingsWriteFailed"; return false;
        }
        Fields(arguments, {"id", "selectionTarget", "name", "scopes"});
        auto captured = theme::CaptureTarget(selection, snapshot->values);
        captured.id = id; captured.name = Text(arguments, "name");
        captured.scopes = captured.kind == theme::Kind::Global
            ? (arguments.Find("scopes") ? static_cast<unsigned>(Integer(arguments, "scopes", 1, theme::All))
                : selection == "global" ? theme::All : theme::TargetScope(selection)) : theme::Components;
        reconcileIcon = command == "update-saved" && captured.kind == theme::Kind::Icon;
        std::array<theme::BindingDraft, 2> bindings;
        if (captured.kind == theme::Kind::Global && theme::FullScope(captured.scopes))
        {
            const auto found = current.references.find("global");
            const auto parent = found == current.references.end() ? std::optional<theme::Theme>{}
                : theme::Resolve(found->second.snapshot, found->second.id);
            for (size_t index = 0; index < bindings.size(); ++index)
            {
                const auto kind = index == 0 ? theme::Kind::QuickPanel : theme::Kind::Popup;
                auto& draft = bindings[index];
                draft.sourceId = parent ? (index == 0 ? parent->quickPanel : parent->popup) : "";
                draft.effective = theme::CaptureGlobalBinding(kind, snapshot->values, theme::Resolve(current.themes, draft.sourceId));
                draft.effective.name = captured.name + (index == 0 ? " / Quick Panel" : " / Popup");
            }
        }
        if (!theme::SaveDraft(current, std::move(captured), bindings, command == "update-saved", savedId, changedIds, detail)) return false;
        if (command != "update-saved") return true;
        auto references = current.references;
        std::vector<std::string> applied;
        for (const auto& changed : changedIds)
            for (const auto& selectionKey : theme::ApplySavedUpdate(current, changed, nextValues, detail, &references))
                if (std::find(applied.begin(), applied.end(), selectionKey) == applied.end()) applied.push_back(selectionKey);
        if (!detail.empty()) return false;
        if (!applied.empty())
        {
            touchedSettings = true;
            if (!ApplyValues(context.controller, nextValues)) { detail = "settingsWriteFailed"; return false; }
            for (const auto& key : applied)
            {
                const auto found = current.references.find(key);
                if (found == current.references.end() || found->second.id.empty()) continue;
                if (key == "global" && std::find(changedIds.begin(), changedIds.end(), found->second.id) == changedIds.end())
                { if (const auto staged = references.find(key); staged != references.end()) found->second = staged->second; }
                else if (!theme::Select(current, key, found->second.id, found->second.kind, found->second.scope, detail)) return false;
            }
        }
        for (const auto& [key, reference] : current.references)
            if (key.starts_with("widget/") && std::find(changedIds.begin(), changedIds.end(), reference.id) != changedIds.end()) widgetTargets.push_back(key);
        return true;
    }, library, error);
    if (!succeeded)
    {
        if (touchedSettings) (void)ApplyValues(context.controller, snapshot->values);
        if (staleLibrary) throw Error("stale_settings", "Theme library changed; query it again");
        Failed(error);
    }
    ApplyWidgetReferences(context, library, widgetTargets, pending);
    if (reconcileIcon)
    {
        bool reconciled = false;
        try { reconciled = context.reconcileIconThemes && context.reconcileIconThemes(); }
        catch (...) { reconciled = false; }
        if (!reconciled) pending.push_back("icon-reference-reconciliation");
    }
    const auto completed = context.controller.Snapshot();
    if (!completed) throw Error("settings_unavailable", "Controller stopped after the theme operation");
    // A removed theme cannot be queried as target:theme in the result.
    const Target resultTarget = command == "remove" ? Target{"themes", {}} : target;
    auto output = Envelope(resultTarget, library, *completed);
    output.object.emplace("status", String(pending.empty() ? "applied" : "saved-with-pending-targets"));
    if (!savedId.empty()) output.object.emplace("savedId", String(savedId));
    output.object.emplace("importMapping", ToValue(mapping));
    output.object.emplace("pendingTargets", ToValue(pending));
    return output;
}
}
