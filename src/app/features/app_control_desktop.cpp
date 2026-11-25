#include "app/app.h"

#include "app/layout/grid_geometry.h"
#include "app/navigation/quick_navigation_state.h"
#include "app_control_runtime.h"
#include "common/utils.h"
#include "core/widget.h"
#include "desktop/folder_mapping_visibility_rules.h"
#include "layout/grid_occupancy.h"
#include "platform/control/control_protocol.h"
#include "widget/permissions/widget_permission_broker.h"
#include "widget/runtime/widget_engine.h"
#include "widgets/single_cell_widget_rules.h"
#include "widgets/widget_group_drop.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace
{
using namespace snowdesktop::control;
constexpr size_t MaxEdits = 1024;
constexpr size_t NoIndex = static_cast<size_t>(-1);

struct NativeComponent
{
    DesktopWidgetType type;
    const char* id;
    const char* name;
    const char* purpose;
    const char* layoutHint;
};

constexpr NativeComponent NativeComponents[] = {
    {DesktopWidgetType::Collection, "collection", "Collection",
        "Keep a manually chosen set of desktop icons together, such as application or system shortcuts.",
        "Use a fixed preview for a small set, a scrolling panel for more items, or an icon entrance to save space."},
    {DesktopWidgetType::CollectionGroup, "collection-group", "Collection Group",
        "Switch between related collections using ordered tabs and search the active collection.",
        "Share a panel or icon entrance across categories that do not need to stay visible at the same time."},
    {DesktopWidgetType::FileCategories, "file-categories", "Desktop Files",
        "Collect actual files from the desktop and browse them by file category or search.",
        "Keep a panel for routine file browsing or use a compact entrance. Collect only within the user's requested scope."},
    {DesktopWidgetType::FileGroup, "file-group", "File Group",
        "Browse Desktop Files and folder mappings through source tabs. Search can span the group's sources.",
        "Use one panel or icon entrance for several sources. Source tabs and file-type filters serve different purposes."},
    {DesktopWidgetType::FolderMapping, "folder-mapping", "Folder Mapping",
        "Browse an existing folder in place, with optional file-type filters and search.",
        "Use a user-selected directory. A panel suits frequent browsing and an icon entrance frees space."},
};

bool HasFileCategories(DesktopWidgetType type)
{
    return type == DesktopWidgetType::FileCategories || type == DesktopWidgetType::FolderMapping;
}

const std::vector<Value>& OptionalArray(const Value& value, const char* key)
{
    static const std::vector<Value> empty;
    const auto* array = value.Find(key);
    if (!array) return empty;
    if (!array->IsArray() || array->array.size() > MaxEdits)
        throw Error("invalid_params", std::string(key) + " must be an array of at most 1024 entries", -32602);
    return array->array;
}

void UpdateBool(const Value& operation, const char* field, bool& value)
{
    const auto* argument = operation.Find(field);
    if (!argument) return;
    if (!argument->IsBoolean())
        throw Error("invalid_params", std::string(field) + " must be a boolean", -32602);
    value = argument->boolean;
}

GridCell ReadCell(const Value& value)
{
    return {Utf8ToWide(Text(value, "pageId")), Integer(value, "column", 0, 65535),
        Integer(value, "row", 0, 65535)};
}

GridSpan ReadSpan(const Value& value)
{
    return {Integer(value, "columns", 1, 65535), Integer(value, "rows", 1, 65535)};
}

Value Location(const GridCell& cell, GridSpan span)
{
    return Object({{"pageId", String(WideToUtf8(cell.pageId))},
        {"column", Number(cell.column)}, {"row", Number(cell.row)},
        {"columns", Number(span.columns)}, {"rows", Number(span.rows)}});
}

bool Fits(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span)
{
    const auto* page = FindGridPage(pages, cell.pageId);
    return page && snowdesktop::grid_occupancy::Fits(*page, cell, span);
}

void SetTitle(DesktopWidget& widget, const Value& operation)
{
    if (!operation.Find("title")) return;
    const auto title = Utf8ToWide(Text(operation, "title"));
    if (title.empty() || title.size() > 256 || std::ranges::any_of(title, [](wchar_t c) { return c < 32; }))
        throw Error("invalid_params", "title must contain 1 to 256 characters without control characters", -32602);
    widget.title = widget.customTitle = title;
    widget.userRenamed = true;
}

Value CollectionVisibility(DesktopApp* app, const std::vector<GridPage>& pages,
    const DesktopWidget& collection)
{
    const auto* page = FindGridPage(pages, collection.gridCell.pageId);
    if (!page) return {};
    DesktopWidget model = collection;
    model.bounds = GetGridRect(pages, model.gridCell, model.gridSpan);
    model.cellScale = GetGridPageCuScale(*page);
    model.scrollOffset = 0;
    Collection view(&model, app);
    const bool icon = snowdesktop::single_cell_widget::IsIconMode(model);
    const bool compact = model.gridSpan.columns == 1 && model.gridSpan.rows == 1 && !model.scrollContainerMode;
    const auto viewport = model.scrollContainerMode ? view.GetContentViewportRect() : view.GetFrameRect();
    size_t full = 0, partial = 0;
    const size_t thumbnails = compact && !icon ? std::min<size_t>(model.itemKeys.size(), 4) : 0;
    if (!compact && !icon)
        for (size_t i = 0; i < model.itemKeys.size(); ++i)
        {
            const auto member = view.GetMemberLayoutRect(i);
            RECT clipped{};
            if (!IntersectRect(&clipped, &member, &viewport)) break;
            if (EqualRect(&clipped, &member)) ++full;
            else ++partial;
        }
    return Object({{"itemCount", Number(static_cast<double>(model.itemKeys.size()))},
        {"fullyVisible", Number(static_cast<double>(full))},
        {"partiallyVisible", Number(static_cast<double>(partial))},
        {"thumbnails", Number(static_cast<double>(thumbnails))},
        {"hidden", Number(static_cast<double>(model.itemKeys.size() - full - partial - thumbnails))},
        {"presentation", String(icon ? "icon" : compact ? "compact" : model.scrollContainerMode ? "scroll" : "grid")}});
}
}

snowdesktop::control::Value DesktopApp::HandleControlDesktopEdits(
    std::string_view method, const snowdesktop::control::Value& params)
{
    using namespace snowdesktop::control;
    if (method == "desktop.inspect")
    {
        Fields(params, {});
        auto state = ApplicationControlLayout(true);
        auto types = Array(), catalog = Array();
        for (const auto& component : NativeComponents)
        {
            types.array.push_back(String(component.id));
            DesktopWidget limits;
            limits.type = component.type;
            ConfigureWidgetGridLimits(limits);
            auto views = Array();
            if (component.type == DesktopWidgetType::Collection)
                views.array = {String("fixed-grid"), String("scrolling-panel"), String("compact-preview")};
            else
                views.array = {String("scrolling-panel"), String("compact-preview")};
            views.array.push_back(String("icon"));
            auto controls = Object({});
            if (component.type != DesktopWidgetType::Collection)
                controls.object.emplace("search", Object({{"visibilityField", String("showSearchBox")},
                    {"action", String("set-search")}, {"requiresOpenContentView", Boolean(true)}}));
            if (HasFileCategories(component.type) || component.type == DesktopWidgetType::FileGroup)
                controls.object.emplace("fileCategories", Object({{"visibilityField", String("showFileCategories")},
                    {"selectionField", String(component.type == DesktopWidgetType::FileGroup ? "contentCategoryId" : "activeCategoryId")},
                    {"availableIdsField", String("contentCategoryIds")}}));
            if (IsGroupWidgetType(component.type))
            {
                auto children = Array();
                children.array = component.type == DesktopWidgetType::CollectionGroup
                    ? std::vector<Value>{String("collection")}
                    : std::vector<Value>{String("file-categories"), String("folder-mapping")};
                controls.object.emplace("groupTabs", Object({{"memberTypes", std::move(children)},
                    {"selectionField", String("activeCategoryId")}, {"batchOperation", String("setGroups")}}));
            }
            if (component.type == DesktopWidgetType::FileCategories)
                controls.object.emplace("collect", Object({{"action", String("collect-now")}}));
            catalog.array.push_back(Object({{"type", String(component.id)}, {"name", String(component.name)},
                {"purpose", String(component.purpose)}, {"layoutHint", String(component.layoutHint)},
                {"presentations", std::move(views)},
                {"minimumExpandedSpan", Object({{"columns", Number(limits.minGridSpan.columns)}, {"rows", Number(limits.minGridSpan.rows)}})},
                {"compactSpan", Object({{"columns", Number(1)}, {"rows", Number(1)}})},
                {"iconModeField", String("singleCellIconMode")}, {"settingsTargetKind", String("native-widget")},
                {"controls", std::move(controls)}}));
        }
        types.array.push_back(String("lua"));
        state.object.emplace("creatableWidgetTypes", std::move(types));
        state.object.emplace("componentCatalog", std::move(catalog));
        auto& instances = state.object.at("widgets").array;
        for (size_t index = 0; index < widgets_.size(); ++index)
        {
            const auto& widget = widgets_[index];
            if (!snowdesktop::single_cell_widget::Supports(widget.type)) continue;
            auto& fields = instances[index].object;
            fields.emplace("singleCellIconMode", Boolean(snowdesktop::single_cell_widget::IsIconMode(widget) &&
                !IsGroupedWidget(widget)));
            if (widget.type != DesktopWidgetType::Collection)
                fields.emplace("showSearchBox", Boolean(widget.showSearchBox));
            if (HasFileCategories(widget.type))
                fields.emplace("showFileCategories", Boolean(widget.showFileCategories));
        }
        return state;
    }
    Fields(params, {"ifLayoutRevision", "addWidgets", "moveWidgets", "setCollections", "setGroups", "moveIcons"});
    if (!generalSettings_.softwareDesktopEnabled)
        throw Error("disabled", "Software desktop is disabled");
    if (layoutReload_.Pending() || reloading_ || applicationControlRuntime_->privatePointer.Current().enabled ||
        mouseDown_ || dragSession_.HasContext() || widgetAction_ != WidgetAction::None ||
        HasActiveContextMenuSession() || dragDropController_.IsTransportActive() ||
        GetOpenPopupWidget() || quickNavigation_->open || renameInputWindow_)
        throw Error("busy", "Finish desktop interactions and layout reloads before editing the desktop");

    const auto revision = ApplicationControlLayout(true).object.at("layoutRevision").string;
    if (Text(params, "ifLayoutRevision") != revision)
        throw Error("stale_layout", "Read desktop.inspect again and rebuild the plan");
    const auto& additions = OptionalArray(params, "addWidgets");
    const auto& widgetMoves = OptionalArray(params, "moveWidgets");
    const auto& collectionEdits = OptionalArray(params, "setCollections");
    const auto& groupEdits = OptionalArray(params, "setGroups");
    const auto& iconMoves = OptionalArray(params, "moveIcons");
    if (additions.empty() && widgetMoves.empty() && collectionEdits.empty() && groupEdits.empty() && iconMoves.empty())
        throw Error("invalid_params", "Supply at least one desktop edit", -32602);

    struct PlannedCollection { size_t index; std::vector<std::wstring> keys; };
    struct PlannedIcon { GridCell cell; GridSpan span; };
    struct PlannedGroup { size_t index; std::vector<std::wstring> children; std::optional<std::wstring> active; };
    const size_t originalWidgetCount = widgets_.size();
    auto proposedWidgets = widgets_;
    proposedWidgets.reserve(originalWidgetCount + additions.size());
    std::vector<std::string> newRefs;
    std::unordered_map<std::string, size_t> widgetByRef;
    std::unordered_map<std::wstring, size_t> widgetById, itemByKey, requestedOwner;
    std::unordered_set<std::wstring> protectedItems;
    std::unordered_set<size_t> changedWidgets;
    std::unordered_set<size_t> unplacedAdditions;
    std::vector<PlannedCollection> collections;
    std::vector<std::wstring> newFolders;
    std::unordered_map<size_t, PlannedIcon> proposedIcons;

    for (size_t i = 0; i < items_.size(); ++i)
        if (!items_[i].name.empty() && !items_[i].layoutKey.empty())
            itemByKey.emplace(ToUpperInvariant(items_[i].layoutKey), i);
    for (size_t i = 0; i < originalWidgetCount; ++i)
    {
        const auto& widget = widgets_[i];
        widgetById.emplace(widget.id, i);
        if (widget.type != DesktopWidgetType::Collection && widget.type != DesktopWidgetType::FolderMapping)
            for (const auto& key : widget.itemKeys)
                protectedItems.insert(ToUpperInvariant(key));
    }
    for (const auto& item : items_)
        if (snowdesktop::folder_mapping_visibility::HasMappingForItem(item, widgets_))
            protectedItems.insert(ToUpperInvariant(item.layoutKey));
    for (const auto& entry : dockEntries_)
        if (entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop)
            protectedItems.insert(ToUpperInvariant(entry.reference));

    const auto itemIndex = [&](const std::string& id) {
        const auto key = ToUpperInvariant(Utf8ToWide(id));
        const auto found = itemByKey.find(key);
        if (found == itemByKey.end()) throw Error("item_not_found", "Unknown desktop item: " + id);
        if (protectedItems.contains(key) || items_[found->second].gridCell.pageId == kDockPageId)
            throw Error("invalid_target", "Desktop item is managed by the Dock, a folder mapping or a non-collection component: " + id);
        return found->second;
    };
    const auto widgetIndex = [&](const std::string& id) {
        const auto found = widgetById.find(Utf8ToWide(id));
        if (found == widgetById.end()) throw Error("widget_not_found", "Unknown component: " + id);
        return found->second;
    };
    const auto widgetReference = [&](const Value& value) {
        if (static_cast<bool>(value.Find("id")) == static_cast<bool>(value.Find("ref")))
            throw Error("invalid_params", "A component reference needs exactly one of id or ref", -32602);
        if (value.Find("id")) return widgetIndex(Text(value, "id"));
        const auto ref = Text(value, "ref");
        const auto found = widgetByRef.find(ref);
        if (found == widgetByRef.end()) throw Error("widget_not_found", "Unknown component ref: " + ref);
        return found->second;
    };
    const auto setMembers = [&](size_t index, const Value& operation) {
        auto& widget = proposedWidgets[index];
        UpdateBool(operation, "scrollContainer", widget.scrollContainerMode);
        const auto& members = OptionalArray(operation, "itemIds");
        std::vector<std::wstring> keys;
        keys.reserve(members.size());
        for (const auto& id : members)
        {
            if (!id.IsString() || id.string.empty())
                throw Error("invalid_params", "itemIds must contain nonempty item keys", -32602);
            const auto& canonical = items_[itemIndex(id.string)].layoutKey;
            const auto key = ToUpperInvariant(canonical);
            if (!requestedOwner.emplace(key, index).second)
                throw Error("duplicate_item", "Desktop item has multiple requested assignments: " + id.string);
            keys.push_back(key);
        }
        collections.push_back({index, std::move(keys)});
    };
    const auto setWidgetPlacement = [&](DesktopWidget& widget, const Value& operation, const std::string& label) {
        auto cell = ReadCell(operation);
        const auto span = ReadSpan(operation);
        const auto* page = FindGridPage(gridPages_, cell.pageId);
        if (!page || !snowdesktop::grid_occupancy::Fits(*page, cell, span))
            throw Error("invalid_placement", "Component does not fit the requested page: " + label);
        const auto allowed = ClampWidgetGridSpan(widget, span, page->columns - cell.column, page->rows - cell.row);
        if (allowed.columns != span.columns || allowed.rows != span.rows)
            throw Error("invalid_placement", "Requested component size is unsupported: " + label);
        widget.gridCell = std::move(cell);
        widget.gridSpan = span;
    };
    for (const auto& operation : additions)
    {
        Fields(operation, {"ref", "type", "packageId", "folderItemId", "folderPath", "pageId", "column", "row", "columns", "rows",
            "title", "itemIds", "scrollContainer", "singleCellIconMode", "showSearchBox", "showFileCategories"});
        const auto ref = Text(operation, "ref");
        const auto index = proposedWidgets.size();
        if (ref.empty() || ref.size() > 128 || !widgetByRef.emplace(ref, index).second)
            throw Error("invalid_params", "Each new component needs a unique ref of 1 to 128 bytes", -32602);
        const auto type = Text(operation, "type");
        DesktopWidget widget;
        if (type == "collection") widget = BuildNativeStorageWidget(DesktopWidgetType::Collection);
        else if (type == "collection-group") widget = BuildNativeStorageWidget(DesktopWidgetType::CollectionGroup);
        else if (type == "file-categories") widget = BuildNativeStorageWidget(DesktopWidgetType::FileCategories);
        else if (type == "file-group")
        {
            widget = BuildNativeStorageWidget(DesktopWidgetType::FileGroup);
            widget.showFileCategories = true;
        }
        else if (type == "folder-mapping")
        {
            namespace mapping = snowdesktop::folder_mapping_visibility;
            const bool byItem = operation.Find("folderItemId") != nullptr;
            if (byItem == (operation.Find("folderPath") != nullptr))
                throw Error("invalid_params", "Folder mapping needs exactly one of folderItemId or folderPath", -32602);
            std::filesystem::path path;
            if (byItem)
            {
                const auto sourceId = Text(operation, "folderItemId");
                const auto& item = items_[itemIndex(sourceId)];
                if (item.isShortcut || !item.desktopIconClsid.empty() || IsItemInAnyWidget(item))
                    throw Error("invalid_target", "Folder mapping requires a loose filesystem folder on the desktop: " + sourceId);
                path = item.parsingName;
                widget.title = item.name;
                widget.sourceDesktopItemKey = item.layoutKey;
            }
            else
            {
                const auto source = Utf8ToWide(Text(operation, "folderPath"));
                if (std::ranges::any_of(source, [](wchar_t c) { return c < 32; }))
                    throw Error("invalid_params", "folderPath cannot contain control characters", -32602);
                path = source;
            }
            if (!path.is_absolute())
                throw Error("invalid_target", "Folder mapping requires an absolute filesystem directory: " + ref);
            path = path.lexically_normal();
            auto source = path.wstring();
            const auto attributes = GetFileAttributesW(source.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                throw Error("invalid_target", "Folder mapping source is not an accessible directory: " + ref);
            if (std::ranges::any_of(newFolders, [&](const auto& existing) {
                return mapping::EqualInsensitive(mapping::SourcePathKey(existing), mapping::SourcePathKey(source));
            }))
                throw Error("duplicate_item", "Folder mapping source already has a requested assignment: " + ref);
            for (const auto& item : items_)
                if (mapping::IsSourceFolder(item, source) && !IsItemInAnyWidget(item) &&
                    item.gridCell.pageId != kDockPageId &&
                    !requestedOwner.emplace(ToUpperInvariant(item.layoutKey), NoIndex).second)
                    throw Error("duplicate_item", "Folder mapping source already has a requested assignment: " + ref);
            widget.type = DesktopWidgetType::FolderMapping;
            if (widget.title.empty()) widget.title = std::filesystem::path(mapping::SourcePathKey(source)).filename().wstring();
            if (widget.title.empty()) widget.title = source;
            widget.showTitle = true;
            widget.sourceFolderPath = source;
            newFolders.push_back(std::move(source));
        }
        else if (type == "lua")
        {
            using namespace snowdesktop::widget;
            widget.type = DesktopWidgetType::LuaScript;
            widget.packageId = Utf8ToWide(Text(operation, "packageId"));
            const auto package = WidgetEngine::GetWidgetPackage(widget.packageId);
            if (!package || !WidgetEngine::IsWidgetPackageInstalled(widget.packageId))
                throw Error("package_not_found", "Lua component package is unavailable: " + ref);
            const auto grant = WidgetPermissionBroker::Evaluate(package->permissionState,
                package->manifest.permissions, package->manifest.optionalPermissions, package->manifest.networkDomains,
                package->grantedPermissions, package->grantedNetworkDomains);
            const bool nativeConsent = !package->manifest.nativeHelpers.empty() &&
                (package->permissionState != PermissionDecisionState::Granted ||
                    (std::ranges::find(package->manifest.permissions, "native.execute") != package->manifest.permissions.end() &&
                     !WidgetPermissionBroker::AllowsPermission(package->grantedPermissions, "native.execute")));
            if (grant.runtimeBlock != PermissionRuntimeBlock::None || nativeConsent)
                throw Error("permission_required", "Grant component permissions in SnowDesktop before adding: " + ref);
            widget.title = WidgetEngine::GetWidgetDisplayName(widget.packageId);
            if (widget.title.empty()) widget.title = widget.packageId;
            CaptureWidgetPackageSource(widget);
        }
        else throw Error("invalid_target", "Unsupported component type: " + type);

        if (type != "lua" && operation.Find("packageId"))
            throw Error("invalid_params", "packageId only applies to Lua components", -32602);
        if (type != "folder-mapping" && (operation.Find("folderItemId") || operation.Find("folderPath")))
            throw Error("invalid_params", "Folder source fields only apply to folder mappings", -32602);
        if (type != "collection" && (operation.Find("itemIds") || operation.Find("scrollContainer")))
            throw Error("invalid_params", "Member and view fields apply to collections", -32602);
        widget.id = MakeNewWidgetId() + L"-batch-" + std::to_wstring(newRefs.size());
        widget.keepInitialComposition = true;
        SetTitle(widget, operation);
        ConfigureWidgetGridLimits(widget);
        if (operation.Find("pageId") || operation.Find("column") || operation.Find("row") ||
            operation.Find("columns") || operation.Find("rows"))
            setWidgetPlacement(widget, operation, ref);
        else
            unplacedAdditions.insert(index);
        if (operation.Find("singleCellIconMode") && (unplacedAdditions.contains(index) ||
            !snowdesktop::single_cell_widget::IsDesktopSingleCell(widget)))
            throw Error("invalid_params", "singleCellIconMode requires a standalone 1 by 1 native component", -32602);
        if (operation.Find("showSearchBox") && (!snowdesktop::single_cell_widget::Supports(widget.type) ||
            widget.type == DesktopWidgetType::Collection))
            throw Error("invalid_params", "showSearchBox requires a searchable native component", -32602);
        if (operation.Find("showFileCategories") && !HasFileCategories(widget.type))
            throw Error("invalid_params", "Set showFileCategories on Desktop Files or Folder Mapping, including when used as a file group source", -32602);
        UpdateBool(operation, "singleCellIconMode", widget.singleCellIconMode);
        UpdateBool(operation, "showSearchBox", widget.showSearchBox);
        UpdateBool(operation, "showFileCategories", widget.showFileCategories);
        proposedWidgets.push_back(std::move(widget));
        newRefs.push_back(ref);
        changedWidgets.insert(index);
        if (type == "collection") setMembers(index, operation);
    }
    namespace groupDrop = snowdesktop::widget_group_drop;
    const auto desktopGroupParticipant = [&](const DesktopWidget& widget) {
        return widget.gridCell.pageId != kDockPageId && !std::ranges::any_of(dockEntries_, [&](const auto& entry) {
            return IsWidgetDockEntryType(entry.type) && entry.reference == widget.id;
        });
    };
    std::unordered_map<std::wstring, size_t> originalParents;
    for (size_t i = 0; i < originalWidgetCount; ++i)
        if (IsGroupWidgetType(widgets_[i].type))
            for (const auto& child : widgets_[i].childWidgetIds)
                originalParents.emplace(child, i);
    std::unordered_set<size_t> editedGroups, assignedChildren;
    std::vector<PlannedGroup> groups;
    for (const auto& operation : groupEdits)
    {
        Fields(operation, {"id", "ref", "children", "activeChild"});
        const auto index = widgetReference(operation);
        const auto& group = proposedWidgets[index];
        if (!IsGroupWidgetType(group.type) || !desktopGroupParticipant(group))
            throw Error("invalid_target", "Only desktop collection and file groups can receive label edits");
        if (!editedGroups.insert(index).second)
            throw Error("duplicate_widget", "Group has multiple requested label lists");
        if (!operation.Find("children"))
            throw Error("invalid_params", "setGroups requires children", -32602);
        PlannedGroup edit{index, {}, std::nullopt};
        for (const auto& childReference : OptionalArray(operation, "children"))
        {
            Fields(childReference, {"id", "ref"});
            const auto childIndex = widgetReference(childReference);
            const auto& child = proposedWidgets[childIndex];
            const auto parent = originalParents.find(child.id);
            if (!groupDrop::AcceptsChild(group.type, child.type) || !desktopGroupParticipant(child) ||
                (parent != originalParents.end() && !desktopGroupParticipant(widgets_[parent->second])))
                throw Error("invalid_target", "Child is incompatible with this desktop group");
            if (childIndex >= originalWidgetCount && additions[childIndex - originalWidgetCount].Find("singleCellIconMode"))
                throw Error("invalid_params", "Set singleCellIconMode on the standalone group entrance", -32602);
            if (!assignedChildren.insert(childIndex).second)
                throw Error("duplicate_widget", "Component is assigned to more than one label");
            edit.children.push_back(child.id);
        }
        if (const auto* active = operation.Find("activeChild"))
        {
            Fields(*active, {"id", "ref"});
            edit.active = proposedWidgets[widgetReference(*active)].id;
            if (std::ranges::find(edit.children, *edit.active) == edit.children.end())
                throw Error("invalid_target", "activeChild must be one of the requested labels");
        }
        groups.push_back(std::move(edit));
    }
    for (const auto& operation : widgetMoves)
    {
        Fields(operation, {"id", "pageId", "column", "row", "columns", "rows"});
        const auto id = Text(operation, "id");
        const auto index = widgetIndex(id);
        const auto& original = widgets_[index];
        const auto parent = originalParents.find(original.id);
        if (assignedChildren.contains(index))
            throw Error("duplicate_widget", "Component cannot be positioned and assigned to a group: " + id);
        if (original.type == DesktopWidgetType::Guide || original.gridCell.pageId == kDockPageId ||
            (IsGroupedWidget(original) && (parent == originalParents.end() ||
                !desktopGroupParticipant(widgets_[parent->second]) || !desktopGroupParticipant(original))))
            throw Error("invalid_target", "Component does not have a movable desktop placement: " + id);
        if (!changedWidgets.insert(index).second)
            throw Error("duplicate_widget", "Component has multiple requested positions: " + id);
        setWidgetPlacement(proposedWidgets[index], operation, id);
        if (parent != originalParents.end())
        {
            const auto& proposed = proposedWidgets[index];
            if (!groupDrop::Release(proposedWidgets, original.id, widgets_[parent->second].type,
                [&](size_t, size_t) -> std::optional<groupDrop::ReleasePlacement> {
                    return groupDrop::ReleasePlacement{proposed.gridCell, proposed.gridSpan};
                }))
                throw Error("invalid_target", "Cannot release this component from its desktop group: " + id);
        }
    }
    std::vector<DockEntry> desktopOnly;
    for (const auto& edit : groups)
        if (!edit.children.empty() && !groupDrop::Apply(proposedWidgets, desktopOnly, edit.index, edit.children, 0))
            throw Error("invalid_target", "Cannot assign the requested group labels");
    for (const auto& edit : groups)
    {
        auto& group = proposedWidgets[edit.index];
        if (group.childWidgetIds != edit.children)
            throw Error("missing_destination", "Move every omitted group child to another group or a desktop cell");
        if (edit.active) group.activeCategoryId = *edit.active;
    }
    std::unordered_map<std::wstring, size_t> finalParents;
    for (size_t i = 0; i < proposedWidgets.size(); ++i)
        if (IsGroupWidgetType(proposedWidgets[i].type))
            for (const auto& child : proposedWidgets[i].childWidgetIds)
                finalParents.emplace(child, i);
    for (const size_t index : unplacedAdditions)
    {
        auto& child = proposedWidgets[index];
        const auto parent = finalParents.find(child.id);
        if (parent == finalParents.end())
            throw Error("invalid_placement", "A standalone component needs a desktop grid placement: " +
                newRefs[index - originalWidgetCount]);
        const auto& group = proposedWidgets[parent->second];
        child.gridCell = group.gridCell;
        const auto* page = FindGridPage(gridPages_, child.gridCell.pageId);
        if (!page)
            throw Error("invalid_placement", "The parent group needs a valid desktop grid placement");
        child.gridSpan = ClampWidgetGridSpan(child, group.gridSpan,
            page->columns - child.gridCell.column, page->rows - child.gridCell.row);
    }
    std::unordered_set<size_t> replacedCollections;
    for (const auto& operation : collectionEdits)
    {
        Fields(operation, {"id", "itemIds", "scrollContainer"});
        const auto id = Text(operation, "id");
        const auto index = widgetIndex(id);
        if (widgets_[index].type != DesktopWidgetType::Collection || widgets_[index].gridCell.pageId == kDockPageId)
            throw Error("invalid_target", "Only desktop collections can receive membership edits: " + id);
        if (!replacedCollections.insert(index).second)
            throw Error("duplicate_widget", "Collection has multiple member lists: " + id);
        if (!operation.Find("itemIds")) throw Error("invalid_params", "setCollections requires itemIds", -32602);
        setMembers(index, operation);
    }
    for (const auto& operation : iconMoves)
    {
        Fields(operation, {"key", "pageId", "column", "row"});
        const auto id = Text(operation, "key");
        const auto index = itemIndex(id);
        const auto key = ToUpperInvariant(items_[index].layoutKey);
        if (!requestedOwner.emplace(key, NoIndex).second)
            throw Error("duplicate_item", "Desktop item has multiple requested assignments: " + id);
        auto cell = ReadCell(operation);
        const auto& item = items_[index];
        auto span = item.gridSpan;
        if (const auto* page = FindGridPage(gridPages_, cell.pageId); item.largeIcon && page)
            span = {std::clamp(item.largeIcon->columns, 1, page->columns),
                std::clamp(item.largeIcon->rows, 1, page->rows)};
        if (!Fits(gridPages_, cell, span))
            throw Error("invalid_placement", "Icon does not fit the requested page: " + id);
        proposedIcons.emplace(index, PlannedIcon{std::move(cell), span});
    }
    for (size_t i = 0; i < originalWidgetCount; ++i)
    {
        auto& widget = proposedWidgets[i];
        if (widget.type != DesktopWidgetType::Collection) continue;
        if (replacedCollections.contains(i))
            for (const auto& key : widget.itemKeys)
                if (!requestedOwner.contains(ToUpperInvariant(key)))
                    throw Error("missing_destination", "Set a destination for every omitted collection member: " + WideToUtf8(key));
        if (widget.gridCell.pageId == kDockPageId &&
            std::ranges::any_of(widget.itemKeys, [&](const auto& key) { return requestedOwner.contains(ToUpperInvariant(key)); }))
            throw Error("invalid_target", "An item is owned by a Dock collection: " + WideToUtf8(widget.id));
        const size_t oldCount = widget.itemKeys.size();
        std::erase_if(widget.itemKeys, [&](const auto& key) { return requestedOwner.contains(ToUpperInvariant(key)); });
        if (widget.itemKeys.size() != oldCount && !replacedCollections.contains(i)) widget.scrollOffset = 0;
    }
    for (auto& collection : collections)
    {
        auto& widget = proposedWidgets[collection.index];
        widget.itemKeys = std::move(collection.keys);
        widget.contentSortColumn = snowdesktop::list_detail_rules::Column::None;
        widget.scrollOffset = 0;
    }

    using namespace snowdesktop::grid_occupancy;
    Slots occupied;
    for (size_t i = 0; i < originalWidgetCount; ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::Guide && !changedWidgets.contains(i) && !IsGroupedWidget(widget) &&
            !finalParents.contains(widget.id) &&
            Fits(gridPages_, widget.gridCell, widget.gridSpan))
            Mark(occupied, widget.gridCell, widget.gridSpan);
    }
    for (const auto& item : items_)
        if (!item.name.empty() && !IsItemInAnyWidget(item) && !requestedOwner.contains(ToUpperInvariant(item.layoutKey)) &&
            Fits(gridPages_, item.gridCell, item.gridSpan))
            Mark(occupied, item.gridCell, item.gridSpan);
    const auto reserve = [&](const GridCell& cell, GridSpan span, const std::string& label) {
        if (Occupied(occupied, cell, span))
            throw Error("placement_conflict", "Requested placement overlaps another item or component: " + label);
        Mark(occupied, cell, span);
    };

    const bool apply = method == "desktop.applyEdits";
    const auto identity = [&](size_t index) {
        auto result = Object({});
        if (index >= originalWidgetCount) result.object.emplace("ref", String(newRefs[index - originalWidgetCount]));
        if (index < originalWidgetCount || apply) result.object.emplace("id", String(WideToUtf8(proposedWidgets[index].id)));
        return result;
    };
    auto widgetResults = Array(), iconResults = Array(), collectionResults = Array(), groupResults = Array();
    std::unordered_map<std::wstring, size_t> resultIndexById;
    for (size_t i = 0; i < proposedWidgets.size(); ++i)
        resultIndexById.emplace(proposedWidgets[i].id, i);
    for (size_t i = 0; i < proposedWidgets.size(); ++i)
    {
        if (!changedWidgets.contains(i)) continue;
        const auto& widget = proposedWidgets[i];
        const bool added = i >= originalWidgetCount;
        const auto label = added ? newRefs[i - originalWidgetCount] : WideToUtf8(widget.id);
        const auto parent = finalParents.find(widget.id);
        auto result = identity(i);
        if (parent == finalParents.end())
        {
            reserve(widget.gridCell, widget.gridSpan, label);
            result.object.merge(Location(widget.gridCell, widget.gridSpan).object);
        }
        else
            result.object.emplace("parentGroup", identity(parent->second));
        if (added && widget.type == DesktopWidgetType::FolderMapping)
        {
            result.object.emplace("folderPath", String(WideToUtf8(widget.sourceFolderPath)));
            if (!widget.sourceDesktopItemKey.empty())
                result.object.emplace("folderItemId", String(WideToUtf8(widget.sourceDesktopItemKey)));
        }
        widgetResults.array.push_back(std::move(result));
    }
    for (const auto& operation : iconMoves)
    {
        const auto id = Text(operation, "key");
        const auto index = itemIndex(id);
        const auto& icon = proposedIcons.at(index);
        reserve(icon.cell, icon.span, id);
        auto result = Location(icon.cell, icon.span);
        result.object.emplace("key", String(WideToUtf8(items_[index].layoutKey)));
        iconResults.array.push_back(std::move(result));
    }
    for (size_t i = 0; i < proposedWidgets.size(); ++i)
    {
        const auto& widget = proposedWidgets[i];
        const bool added = i >= originalWidgetCount;
        if (IsGroupWidgetType(widget.type) && (added || editedGroups.contains(i) ||
            widgets_[i].childWidgetIds != widget.childWidgetIds))
        {
            auto result = identity(i);
            result.object.emplace("title", String(WideToUtf8(widget.title)));
            auto children = Array();
            for (const auto& child : widget.childWidgetIds)
            {
                const auto childIndex = resultIndexById.find(child);
                if (childIndex != resultIndexById.end())
                    children.array.push_back(identity(childIndex->second));
                else
                    children.array.push_back(Object({{"id", String(WideToUtf8(child))}}));
            }
            result.object.emplace("children", std::move(children));
            const auto active = resultIndexById.find(widget.activeCategoryId);
            if (active != resultIndexById.end())
                result.object.emplace("activeChild", identity(active->second));
            groupResults.array.push_back(std::move(result));
        }
        if (widget.type != DesktopWidgetType::Collection) continue;
        if (!added && !changedWidgets.contains(i) && !replacedCollections.contains(i) &&
            widgets_[i].itemKeys == widget.itemKeys) continue;
        auto result = Object({{"title", String(WideToUtf8(widget.title))},
            {"itemCount", Number(static_cast<double>(widget.itemKeys.size()))}});
        if (added) result.object.emplace("ref", String(newRefs[i - originalWidgetCount]));
        if (!added || apply) result.object.emplace("id", String(WideToUtf8(widget.id)));
        if (!finalParents.contains(widget.id))
            result.object.emplace("firstScreen", CollectionVisibility(this, gridPages_, widget));
        collectionResults.array.push_back(std::move(result));
    }

    auto result = Object({{"status", String(apply ? "applied" : "valid")},
        {"persistence", String(apply ? "queued" : "none")}, {"layoutRevision", String(revision)},
        {"widgets", std::move(widgetResults)}, {"icons", std::move(iconResults)}, {"collections", std::move(collectionResults)},
        {"groups", std::move(groupResults)}});
    if (Serialize(result).size() > MaximumFrameBytes - 2048)
        throw Error("result_too_large", "Split the desktop edits into smaller plans");
    if (apply)
    {
        // Keep the retired widget model alive until all runtime pointers have been rebuilt.
        widgets_.swap(proposedWidgets);
        for (auto& [index, icon] : proposedIcons)
        {
            items_[index].gridCell = std::move(icon.cell);
            items_[index].gridSpan = icon.span;
        }
        for (const auto& [key, owner] : requestedOwner)
            if (owner != NoIndex) items_[itemByKey.at(key)].gridCell = widgets_[owner].gridCell;
        mouseDownWidgetIndex_ = NoIndex;
        keyboardNavInsideWidget_ = false;
        keyboardNavWidgetIndex_ = NoIndex;
        EnsureNavTabOrder();
        LayoutItems();
        if (!newFolders.empty()) RequestFolderRefresh(newFolders);
        RequestLayoutSave();
        InvalidateDragStaticScene();
        InvalidateRect(hwnd_, nullptr, TRUE);
        result.object.at("layoutRevision") = ApplicationControlLayout(true).object.at("layoutRevision");
    }
    return result;
}
