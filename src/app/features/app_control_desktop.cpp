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

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace
{
using namespace snowdesktop::control;
constexpr size_t MaxEdits = 1024;
constexpr size_t NoIndex = static_cast<size_t>(-1);

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
    const bool compact = model.gridSpan.columns == 1 && model.gridSpan.rows == 1 && !model.scrollContainerMode;
    const auto viewport = model.scrollContainerMode ? view.GetContentViewportRect() : view.GetFrameRect();
    size_t full = 0, partial = 0;
    const size_t thumbnails = compact ? std::min<size_t>(model.itemKeys.size(), 4) : 0;
    if (!compact)
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
        {"presentation", String(compact ? "compact" : model.scrollContainerMode ? "scroll" : "grid")}});
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
        auto types = Array();
        for (const char* type : {"collection", "collection-group", "file-categories", "file-group", "folder-mapping", "lua"})
            types.array.push_back(String(type));
        state.object.emplace("creatableWidgetTypes", std::move(types));
        return state;
    }
    Fields(params, {"ifLayoutRevision", "addWidgets", "moveWidgets", "setCollections", "moveIcons"});
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
    const auto& iconMoves = OptionalArray(params, "moveIcons");
    if (additions.empty() && widgetMoves.empty() && collectionEdits.empty() && iconMoves.empty())
        throw Error("invalid_params", "Supply at least one desktop edit", -32602);

    struct PlannedCollection { size_t index; std::vector<std::wstring> keys; };
    struct PlannedIcon { GridCell cell; GridSpan span; };
    const size_t originalWidgetCount = widgets_.size();
    auto proposedWidgets = widgets_;
    proposedWidgets.reserve(originalWidgetCount + additions.size());
    std::vector<std::string> newRefs;
    std::unordered_set<std::string> usedRefs;
    std::unordered_map<std::wstring, size_t> widgetById, itemByKey, requestedOwner;
    std::unordered_set<std::wstring> protectedItems;
    std::unordered_set<size_t> changedWidgets;
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
    const auto setMembers = [&](size_t index, const Value& operation) {
        auto& widget = proposedWidgets[index];
        UpdateBool(operation, "scrollContainer", widget.scrollContainerMode);
        UpdateBool(operation, "showTitle", widget.showTitle);
        UpdateBool(operation, "titleOnHover", widget.bottomBarHover);
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
        Fields(operation, {"ref", "type", "packageId", "folderItemId", "pageId", "column", "row", "columns", "rows",
            "title", "itemIds", "scrollContainer", "showTitle", "titleOnHover"});
        const auto ref = Text(operation, "ref");
        if (ref.empty() || ref.size() > 128 || !usedRefs.insert(ref).second)
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
            const auto sourceId = Text(operation, "folderItemId");
            const auto& item = items_[itemIndex(sourceId)];
            if (item.isShortcut || !item.desktopIconClsid.empty() || IsItemInAnyWidget(item) ||
                !std::filesystem::path(item.parsingName).is_absolute())
                throw Error("invalid_target", "Folder mapping requires a loose filesystem folder on the desktop: " + sourceId);
            const auto attributes = GetFileAttributesW(item.parsingName.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                throw Error("invalid_target", "Folder mapping source is not an accessible directory: " + sourceId);
            if (!requestedOwner.emplace(ToUpperInvariant(item.layoutKey), NoIndex).second)
                throw Error("duplicate_item", "Folder mapping source already has a requested assignment: " + sourceId);
            widget.type = DesktopWidgetType::FolderMapping;
            widget.title = item.name;
            widget.showTitle = true;
            widget.sourceFolderPath = item.parsingName;
            widget.sourceDesktopItemKey = item.layoutKey;
            newFolders.push_back(widget.sourceFolderPath);
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
        if (type != "folder-mapping" && operation.Find("folderItemId"))
            throw Error("invalid_params", "folderItemId only applies to folder mappings", -32602);
        if (type != "collection" && (operation.Find("itemIds") || operation.Find("scrollContainer") ||
            operation.Find("showTitle") || operation.Find("titleOnHover")))
            throw Error("invalid_params", "Member and view fields apply to collections", -32602);
        widget.id = MakeNewWidgetId() + L"-batch-" + std::to_wstring(newRefs.size());
        widget.keepInitialComposition = true;
        SetTitle(widget, operation);
        ConfigureWidgetGridLimits(widget);
        setWidgetPlacement(widget, operation, ref);
        const auto index = proposedWidgets.size();
        proposedWidgets.push_back(std::move(widget));
        newRefs.push_back(ref);
        changedWidgets.insert(index);
        if (type == "collection") setMembers(index, operation);
    }
    for (const auto& operation : widgetMoves)
    {
        Fields(operation, {"id", "pageId", "column", "row", "columns", "rows"});
        const auto id = Text(operation, "id");
        const auto index = widgetIndex(id);
        const auto& original = widgets_[index];
        if (IsGroupedWidget(original) || original.type == DesktopWidgetType::Guide ||
            original.gridCell.pageId == kDockPageId)
            throw Error("invalid_target", "Only standalone desktop components can be repositioned: " + id);
        if (!changedWidgets.insert(index).second)
            throw Error("duplicate_widget", "Component has multiple requested positions: " + id);
        setWidgetPlacement(proposedWidgets[index], operation, id);
    }
    std::unordered_set<size_t> replacedCollections;
    for (const auto& operation : collectionEdits)
    {
        Fields(operation, {"id", "itemIds", "scrollContainer", "showTitle", "titleOnHover"});
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

    auto widgetResults = Array(), iconResults = Array(), collectionResults = Array();
    const bool apply = method == "desktop.applyEdits";
    for (size_t i = 0; i < proposedWidgets.size(); ++i)
    {
        if (!changedWidgets.contains(i)) continue;
        const auto& widget = proposedWidgets[i];
        const bool added = i >= originalWidgetCount;
        const auto label = added ? newRefs[i - originalWidgetCount] : WideToUtf8(widget.id);
        reserve(widget.gridCell, widget.gridSpan, label);
        auto result = Location(widget.gridCell, widget.gridSpan);
        if (added) result.object.emplace("ref", String(label));
        if (!added || apply) result.object.emplace("id", String(WideToUtf8(widget.id)));
        if (added && widget.type == DesktopWidgetType::FolderMapping)
            result.object.emplace("folderItemId", String(WideToUtf8(widget.sourceDesktopItemKey)));
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
        if (widget.type != DesktopWidgetType::Collection) continue;
        const bool added = i >= originalWidgetCount;
        if (!added && !changedWidgets.contains(i) && !replacedCollections.contains(i) &&
            widgets_[i].itemKeys == widget.itemKeys) continue;
        auto result = Object({{"title", String(WideToUtf8(widget.title))},
            {"itemCount", Number(static_cast<double>(widget.itemKeys.size()))}});
        if (added) result.object.emplace("ref", String(newRefs[i - originalWidgetCount]));
        if (!added || apply) result.object.emplace("id", String(WideToUtf8(widget.id)));
        if (added || !IsGroupedWidget(widgets_[i]))
            result.object.emplace("firstScreen", CollectionVisibility(this, gridPages_, widget));
        collectionResults.array.push_back(std::move(result));
    }

    auto result = Object({{"status", String(apply ? "applied" : "valid")},
        {"persistence", String(apply ? "queued" : "none")}, {"layoutRevision", String(revision)},
        {"widgets", std::move(widgetResults)}, {"icons", std::move(iconResults)}, {"collections", std::move(collectionResults)}});
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
