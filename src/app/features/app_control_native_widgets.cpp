#include "app/app.h"
#include "settings/control_native_widget_settings.h"
#include "platform/demo_collection_rules.h"
#include "widgets/single_cell_widget_rules.h"
#include "widgets/storage_title_bar_layout.h"

namespace
{
using namespace snowdesktop::control;
using namespace snowdesktop::control_settings;
std::vector<std::string> NativeStrings(const std::vector<std::wstring>& values)
{
    std::vector<std::string> result;
    for (const auto& value : values) result.push_back(WideToUtf8(value));
    return result;
}
std::vector<std::wstring> NativeWideStrings(const std::vector<std::string>& values)
{
    std::vector<std::wstring> result;
    for (const auto& value : values) result.push_back(Utf8ToWide(value));
    return result;
}
bool NativeKey(const NativeWidgetSettingsPatch& patch, std::string_view key)
{ return std::find(patch.keys.begin(), patch.keys.end(), key) != patch.keys.end(); }
std::string NativeType(DesktopWidgetType type)
{
    switch (type)
    {
    case DesktopWidgetType::Collection: return "collection";
    case DesktopWidgetType::FileCategories: return "file-categories";
    case DesktopWidgetType::FolderMapping: return "folder-mapping";
    case DesktopWidgetType::CollectionGroup: return "collection-group";
    case DesktopWidgetType::FileGroup: return "file-group";
    default: throw Error("invalid_target", "The target must be an existing native storage widget");
    }
}
std::wstring NativeDefaultTitle(const DesktopWidget& widget)
{
    if (!widget.scriptTitle.empty()) return widget.scriptTitle;
    switch (widget.type)
    {
    case DesktopWidgetType::Collection: return _LW("widget.collection");
    case DesktopWidgetType::FileCategories: return _LW("widget.desktop_files");
    case DesktopWidgetType::CollectionGroup: return _LW("widget.collection_group");
    case DesktopWidgetType::FileGroup: return _LW("widget.file_group");
    case DesktopWidgetType::FolderMapping:
    {
        auto path = std::filesystem::path(widget.sourceFolderPath);
        if (path.filename().empty()) path = path.parent_path();
        const auto name = path.filename().wstring();
        return name.empty() ? (!widget.sourceFolderPath.empty() ? widget.sourceFolderPath : !widget.title.empty() ? widget.title : _LW("widget.collection")) : name;
    }
    default: return widget.title;
    }
}
void NativeInvalidate(WidgetContainer* view)
{
    if (!view) return;
    if (auto* fileGroup = dynamic_cast<FileGroup*>(view)) fileGroup->InvalidateHostedView();
    else if (auto* collectionGroup = dynamic_cast<CollectionGroup*>(view)) collectionGroup->InvalidateFilterCache();
    else if (auto* mapping = dynamic_cast<FolderMapping*>(view)) mapping->InvalidateFilterCache();
    else if (auto* categories = dynamic_cast<FileCategories*>(view)) categories->InvalidateCategoryCache();
    else view->InvalidateSlots();
}
NativeWidgetConfiguration NativeConfiguration(const DesktopWidget& widget)
{
    NativeWidgetConfiguration value;
    value.title = WideToUtf8(widget.title);
#define NATIVE_FIELD(name) value.name = widget.name
    NATIVE_FIELD(showTitle); NATIVE_FIELD(bottomBarHover); NATIVE_FIELD(showOnHoverOnly); NATIVE_FIELD(keepWhenDesktopHidden);
    NATIVE_FIELD(privacyMode); NATIVE_FIELD(singleCellIconMode); NATIVE_FIELD(scrollContainerMode); NATIVE_FIELD(largeFolderTitleless);
    NATIVE_FIELD(titleBarCollapsed); NATIVE_FIELD(titleBarExpandOnHover); NATIVE_FIELD(showSearchBox); NATIVE_FIELD(fanPopup);
    NATIVE_FIELD(listMode); NATIVE_FIELD(listColumns); NATIVE_FIELD(detailShowModified); NATIVE_FIELD(detailShowType); NATIVE_FIELD(detailShowSize);
    NATIVE_FIELD(detailModifiedPosition); NATIVE_FIELD(detailTypePosition); NATIVE_FIELD(detailSizePosition); NATIVE_FIELD(dateHeaders); NATIVE_FIELD(showFileCategories);
    NATIVE_FIELD(autoCollect);
#undef NATIVE_FIELD
    value.demoIconCategory = WideToUtf8(widget.demoIconCategory);
    value.sortColumn = std::string(snowdesktop::list_detail_rules::ToString(widget.type == DesktopWidgetType::FolderMapping
        ? snowdesktop::list_detail_rules::FromLegacyFolderSortMode(widget.folderSortMode) : widget.contentSortColumn));
    value.sortAscending = widget.type == DesktopWidgetType::FolderMapping ? widget.folderSortAscending : widget.contentSortAscending;
    value.autoCollectCategoryIds = NativeStrings(widget.autoCollectCategoryIds);
    value.activeCategoryId = WideToUtf8(widget.activeCategoryId);
    value.contentCategoryId = value.activeCategoryId;
    value.categoryTabOrder = NativeStrings(widget.categoryTabOrder);
    return value;
}
}

snowdesktop::control_settings::NativeWidgetSettingsState DesktopApp::CaptureNativeWidgetSettings(std::wstring_view id)
{
    using namespace snowdesktop;
    const auto index = FindWidgetIndexById(std::wstring(id));
    if (index >= widgets_.size()) throw Error("invalid_target", "Native widget ID no longer exists");
    auto& owner = widgets_[index];
    NativeWidgetSettingsState state;
    state.type = NativeType(owner.type);
    state.id = WideToUtf8(owner.id);
    WidgetContainer* view = nullptr;
    for (const auto& container : containers_)
        if (auto* candidate = dynamic_cast<WidgetContainer*>(container.get()); candidate && candidate->GetWidgetData() == &owner)
        { view = candidate; break; }
    // Closed Dock/icon entrances still expose their persisted content settings.
    // A temporary native view resolves the same active child as the real UI.
    const bool liveView = view != nullptr;
    std::unique_ptr<Widget> temporary;
    if (!view) { temporary = CreateWidget(&owner, this); view = dynamic_cast<WidgetContainer*>(temporary.get()); }
    DesktopWidget* content = view ? view->GetContentViewData() : &owner;
    if (!content) content = &owner;
    DesktopWidget* source = &owner;
    if (auto* fileGroup = dynamic_cast<FileGroup*>(view))
    {
        state.activeChildId = WideToUtf8(fileGroup->GetActiveSourceId());
        const auto active = FindWidgetIndexById(fileGroup->GetActiveSourceId());
        if (active < widgets_.size()) source = &widgets_[active];
    }
    else if (auto* collectionGroup = dynamic_cast<CollectionGroup*>(view)) state.activeChildId = WideToUtf8(collectionGroup->GetActiveCollectionId());
    DesktopWidget* sort = content;
    state.contentTargetId = WideToUtf8(content->id);
    state.sourceTargetId = WideToUtf8(source->id);
    state.sortTargetId = WideToUtf8(sort->id);
    auto& values = state.values;
    values.title = WideToUtf8(owner.title);
#define OWNER_BOOL(name) values.name = owner.name
    OWNER_BOOL(showTitle); OWNER_BOOL(bottomBarHover); OWNER_BOOL(showOnHoverOnly); OWNER_BOOL(keepWhenDesktopHidden);
    OWNER_BOOL(privacyMode); OWNER_BOOL(singleCellIconMode); OWNER_BOOL(scrollContainerMode); OWNER_BOOL(largeFolderTitleless);
    OWNER_BOOL(titleBarCollapsed); OWNER_BOOL(titleBarExpandOnHover); OWNER_BOOL(showSearchBox); OWNER_BOOL(fanPopup);
#undef OWNER_BOOL
    values.demoIconCategory = WideToUtf8(owner.demoIconCategory);
#define CONTENT_FIELD(name) values.name = content->name
    CONTENT_FIELD(listMode); CONTENT_FIELD(listColumns); CONTENT_FIELD(detailShowModified); CONTENT_FIELD(detailShowType); CONTENT_FIELD(detailShowSize);
    CONTENT_FIELD(detailModifiedPosition); CONTENT_FIELD(detailTypePosition); CONTENT_FIELD(detailSizePosition); CONTENT_FIELD(dateHeaders); CONTENT_FIELD(showFileCategories);
#undef CONTENT_FIELD
    values.listColumns = list_detail_rules::ListColumns(*content);
    values.sortColumn = std::string(list_detail_rules::ToString(sort->type == DesktopWidgetType::FolderMapping
        ? list_detail_rules::FromLegacyFolderSortMode(sort->folderSortMode) : sort->contentSortColumn));
    values.sortAscending = sort->type == DesktopWidgetType::FolderMapping ? sort->folderSortAscending : sort->contentSortAscending;
    values.autoCollect = source->autoCollect;
    values.autoCollectCategoryIds = NativeStrings(source->autoCollectCategoryIds);
    values.activeCategoryId = WideToUtf8(owner.activeCategoryId);
    values.contentCategoryId = WideToUtf8(source->activeCategoryId);
    values.categoryTabOrder = NativeStrings(source->categoryTabOrder);
    state.customTitle = WideToUtf8(owner.customTitle);
    state.defaults.title = WideToUtf8(NativeDefaultTitle(owner));
    state.sourceFolderPath = WideToUtf8(source->sourceFolderPath);
    state.placement = Object({{"pageId", String(WideToUtf8(owner.gridCell.pageId))}, {"column", Number(owner.gridCell.column)},
        {"row", Number(owner.gridCell.row)}, {"columns", Number(owner.gridSpan.columns)}, {"rows", Number(owner.gridSpan.rows)}});
    state.editable = desktopItemsReady_ && !gridPages_.empty() && !layoutReload_.Pending() && !exitRequested_ && !reloading_ &&
        !pageMutationActive_ && !dragSession_.HasContext() && !dragDropController_.IsTransportActive() && widgetAction_ == WidgetAction::None;
    state.editableKeys = {"title", "showTitle", "bottomBarHover", "showOnHoverOnly", "keepWhenDesktopHidden", "privacyMode",
        "listMode", "listColumns", "detailShowModified", "detailShowType", "detailShowSize", "detailModifiedPosition", "detailTypePosition", "detailSizePosition", "sortColumn", "sortAscending"};
    if (single_cell_widget::IsDesktopSingleCell(owner)) state.editableKeys.push_back("singleCellIconMode");
    const bool noncompactCollection = owner.type == DesktopWidgetType::Collection && (owner.gridSpan.columns > 1 || owner.gridSpan.rows > 1);
    if (noncompactCollection)
    {
        state.editableKeys.push_back("scrollContainerMode");
        if (!owner.scrollContainerMode) state.editableKeys.push_back("largeFolderTitleless");
    }
    if (storage_title_bar::UsesTop(owner, CurrentPersonalization().scrollableTitleBarOnTop))
    { state.editableKeys.push_back("titleBarCollapsed"); state.editableKeys.push_back("titleBarExpandOnHover"); }
    if (owner.type != DesktopWidgetType::Collection) state.editableKeys.push_back("showSearchBox");
    if (content->type == DesktopWidgetType::FileCategories || content->type == DesktopWidgetType::FolderMapping || content->type == DesktopWidgetType::FileGroup)
    { state.editableKeys.push_back("dateHeaders"); state.editableKeys.push_back("showFileCategories"); }
    if (owner.gridCell.pageId == kDockPageId && (owner.type == DesktopWidgetType::Collection || owner.type == DesktopWidgetType::FolderMapping))
        state.editableKeys.push_back("fanPopup");
    for (const auto& category : GetCategoryOrder(categorySettings_)) if (category != L"all") state.categoryIds.push_back(WideToUtf8(category));
    if (source->type == DesktopWidgetType::FileCategories)
    { state.editableKeys.push_back("autoCollect"); state.editableKeys.push_back("autoCollectCategoryIds"); state.collectAvailable = true; }
    const auto sourceCategories = [&](DesktopWidget* data) {
        if (data->type == DesktopWidgetType::FileCategories) { FileCategories probe(data, this); return NativeStrings(probe.CachedVisibleCategoryIds()); }
        if (data->type == DesktopWidgetType::FolderMapping) { FolderMapping probe(data, this); return NativeStrings(probe.GetVisibleCategoryIds()); }
        return std::vector<std::string>{};
    };
    state.contentCategoryIds = sourceCategories(source);
    if (source->type == DesktopWidgetType::FileCategories || source->type == DesktopWidgetType::FolderMapping)
        state.categoryTabIds = NativeStrings(GetCategoryOrder(categorySettings_));
    if (IsGroupWidgetType(owner.type))
    {
        state.childIds = NativeStrings(owner.childWidgetIds);
        if (auto* group = dynamic_cast<CollectionGroup*>(view)) state.activeCategoryIds = NativeStrings(group->GetVisibleCollectionIds());
        if (auto* group = dynamic_cast<FileGroup*>(view)) state.activeCategoryIds = NativeStrings(group->GetVisibleSourceIds());
        if (owner.type == DesktopWidgetType::FileGroup)
        { state.editableKeys.push_back("contentCategoryId"); state.editableKeys.push_back("categoryTabOrder"); }
        state.editableKeys.push_back("activeCategoryId");
    }
    else if (owner.type == DesktopWidgetType::FileCategories || owner.type == DesktopWidgetType::FolderMapping)
    {
        state.activeCategoryIds = state.contentCategoryIds;
        state.editableKeys.push_back("activeCategoryId"); state.editableKeys.push_back("categoryTabOrder");
    }
    if (generalSettings_.demoModeEnabled && demoIdentityAssetsAvailable_ && owner.type == DesktopWidgetType::Collection)
    {
        state.editableKeys.push_back("demoIconCategory");
        for (const auto& category : demo_collection_rules::kCategories) state.demoIconCategories.push_back(WideToUtf8(std::wstring(category.id)));
    }
    auto* search = liveView ? dynamic_cast<ScrollingItemWidget*>(view) : nullptr;
    if (dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ == owner.id && dockFolderPopupContainer_) search = dockFolderPopupContainer_.get();
    state.searchAvailable = search && owner.showSearchBox;
    if (search) state.searchText = WideToUtf8(search->GetSearchText());
    auto ownership = Array();
    for (const auto& widget : widgets_)
    {
        auto entry = Object({{"id", String(WideToUtf8(widget.id))}, {"autoCollect", Boolean(widget.autoCollect)},
            {"categories", Array()}});
        if (widget.id == owner.id || widget.id == content->id || widget.id == source->id)
            entry.object.emplace("configuration", NativeWidgetConfigurationJson(NativeConfiguration(widget)));
        for (const auto& category : widget.autoCollectCategoryIds) entry.object.at("categories").array.push_back(String(WideToUtf8(category)));
        ownership.array.push_back(std::move(entry));
    }
    state.ownershipRevision = std::to_string(CapturePageLayoutSnapshot().revision) + ":" + Revision(ownership);
    return state;
}

bool DesktopApp::ApplyNativeWidgetSettings(const snowdesktop::control_settings::NativeWidgetSettingsState& expected,
    const snowdesktop::control_settings::NativeWidgetSettingsPatch& patch)
{
    using namespace snowdesktop;
    const auto current = CaptureNativeWidgetSettings(Utf8ToWide(expected.id));
    if (NativeWidgetSettingsRevision(current) != NativeWidgetSettingsRevision(expected)) throw Error("stale_settings", "The native widget or active child changed");
    if (!current.editable) throw Error("busy", "The native desktop model is busy");
    ValidateNativeWidgetSettingsPatch(current, patch);
    // Drain an older asynchronous snapshot before constructing a candidate.
    FlushQueuedLayoutSave();
    if (layoutSavePending_ && !SaveLayoutSlots(false)) return false;
    if (NativeWidgetSettingsRevision(CaptureNativeWidgetSettings(Utf8ToWide(expected.id))) != NativeWidgetSettingsRevision(expected))
        throw Error("stale_settings", "The native layout changed while finishing an earlier save");
    const auto oldWidgets = widgets_;
    const auto oldDock = dockEntries_;
    const auto oldGrid = gridPages_;
    const auto oldIds = savedPageIds_;
    const auto oldNames = savedPageNames_;
    const auto oldColumns = savedPageColumns_;
    const auto oldRows = savedPageRows_;
    const auto oldRecords = layoutRecords_;
    const auto oldPopupTabOrder = dockFolderPopupWidget_.categoryTabOrder;
    const auto oldLastPage = lastMonitorPageId_;
    const auto oldPageRevision = pageMutationRevision_;
    const int oldOffset = pageOffset_;
    const bool oldPending = layoutSavePending_;
    std::unordered_map<std::wstring, std::wstring> searchQueries;
    for (const auto& container : containers_)
        if (auto* search = dynamic_cast<ScrollingItemWidget*>(container.get()); search && search->GetWidgetData())
            searchQueries.emplace(search->GetWidgetData()->id, search->GetSearchText());
    const bool rebuild = patch.collectNow || NativeKey(patch, "autoCollect") || NativeKey(patch, "autoCollectCategoryIds") ||
        NativeKey(patch, "sortColumn") || NativeKey(patch, "sortAscending");
    struct ItemState
    {
        GridCell cell; GridSpan span; std::optional<LargeIconConfig> largeIcon; int slot; RECT bounds;
        category_collection_rules::ShortcutTarget shortcutTarget;
    };
    std::vector<ItemState> oldItems;
    for (const auto& item : items_) oldItems.push_back({item.gridCell, item.gridSpan, item.largeIcon, item.slot, item.bounds, item.shortcutTarget});
    const auto ownerIndex = FindWidgetIndexById(Utf8ToWide(expected.id));
    const auto contentIndex = FindWidgetIndexById(Utf8ToWide(expected.contentTargetId));
    const auto sourceIndex = FindWidgetIndexById(Utf8ToWide(expected.sourceTargetId));
    const auto sortIndex = FindWidgetIndexById(Utf8ToWide(expected.sortTargetId));
    if (ownerIndex >= widgets_.size() || contentIndex >= widgets_.size() || sourceIndex >= widgets_.size() || sortIndex >= widgets_.size())
        throw Error("stale_settings", "A native configuration owner was removed");
    const auto& value = patch.values;
    bool saved = false;
    const auto rollback = [&] {
        widgets_ = oldWidgets; dockEntries_ = oldDock; gridPages_ = oldGrid;
        savedPageIds_ = oldIds; savedPageNames_ = oldNames; savedPageColumns_ = oldColumns; savedPageRows_ = oldRows;
        layoutRecords_ = oldRecords; lastMonitorPageId_ = oldLastPage; pageOffset_ = oldOffset; pageMutationRevision_ = oldPageRevision;
        dockFolderPopupWidget_.categoryTabOrder = oldPopupTabOrder;
        for (std::size_t i = 0; i < oldItems.size(); ++i)
        {
            items_[i].gridCell = oldItems[i].cell; items_[i].gridSpan = oldItems[i].span; items_[i].largeIcon = oldItems[i].largeIcon;
            items_[i].slot = oldItems[i].slot; items_[i].bounds = oldItems[i].bounds; items_[i].shortcutTarget = oldItems[i].shortcutTarget;
        }
        layoutSavePending_ = oldPending;
        RefreshCollectedKeysCache();
    };
    try
    {
        auto& owner = widgets_[ownerIndex];
        auto& content = widgets_[contentIndex];
        auto& source = widgets_[sourceIndex];
        if (NativeKey(patch, "title"))
        {
            owner.customTitle = Utf8ToWide(value.title); owner.userRenamed = !owner.customTitle.empty();
            owner.title = owner.customTitle.empty() ? NativeDefaultTitle(owner) : owner.customTitle;
        }
#define APPLY_OWNER(name) if (NativeKey(patch, #name)) owner.name = value.name
        APPLY_OWNER(showTitle); APPLY_OWNER(bottomBarHover); APPLY_OWNER(showOnHoverOnly); APPLY_OWNER(keepWhenDesktopHidden);
        APPLY_OWNER(privacyMode); APPLY_OWNER(singleCellIconMode); APPLY_OWNER(scrollContainerMode); APPLY_OWNER(largeFolderTitleless);
        APPLY_OWNER(titleBarCollapsed); APPLY_OWNER(titleBarExpandOnHover); APPLY_OWNER(showSearchBox); APPLY_OWNER(fanPopup);
#undef APPLY_OWNER
        if (NativeKey(patch, "demoIconCategory")) owner.demoIconCategory = Utf8ToWide(value.demoIconCategory);
#define APPLY_CONTENT(name) if (NativeKey(patch, #name)) content.name = value.name
        APPLY_CONTENT(listMode); APPLY_CONTENT(detailShowModified); APPLY_CONTENT(detailShowType); APPLY_CONTENT(detailShowSize);
        APPLY_CONTENT(detailModifiedPosition); APPLY_CONTENT(detailTypePosition); APPLY_CONTENT(detailSizePosition);
        APPLY_CONTENT(dateHeaders); APPLY_CONTENT(showFileCategories);
#undef APPLY_CONTENT
        const bool detailEdit = std::any_of(patch.keys.begin(), patch.keys.end(), [](const auto& key) { return key == "listColumns" || key.starts_with("detail"); });
        if (detailEdit)
        {
            content.listColumns = value.listColumns;
            content.detailShowModified = value.detailShowModified; content.detailShowType = value.detailShowType; content.detailShowSize = value.detailShowSize;
            content.showDetails = list_detail_rules::HasMetadataColumns(content.detailShowModified, content.detailShowType, content.detailShowSize);
        }
        if (NativeKey(patch, "activeCategoryId")) owner.activeCategoryId = Utf8ToWide(value.activeCategoryId);
        if (NativeKey(patch, "contentCategoryId")) source.activeCategoryId = Utf8ToWide(value.contentCategoryId);
        if (NativeKey(patch, "categoryTabOrder")) source.categoryTabOrder = NativeWideStrings(value.categoryTabOrder);
        if (dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ == owner.id)
            dockFolderPopupWidget_.categoryTabOrder = owner.categoryTabOrder;
        if (!patch.reorderedChildIds.empty()) owner.childWidgetIds = NativeWideStrings(patch.reorderedChildIds);
        if (NativeKey(patch, "autoCollect")) source.autoCollect = value.autoCollect;
        if (NativeKey(patch, "autoCollectCategoryIds")) source.autoCollectCategoryIds = NativeWideStrings(value.autoCollectCategoryIds);
        if (NativeKey(patch, "autoCollect") || NativeKey(patch, "autoCollectCategoryIds"))
        { EnforceAutoCollectFileCategorySelection(sourceIndex); ApplyAutoCollectFileCategoryWidgets(); }
        if (patch.collectNow) CollectFileCategoryWidget(sourceIndex, false);
        if (NativeKey(patch, "sortColumn") || NativeKey(patch, "sortAscending"))
        {
            const int mode = value.sortColumn == "name" ? 0 : value.sortColumn == "type" ? 1 : value.sortColumn == "modified" ? 2 : value.sortColumn == "size" ? 3 : -1;
            SortWidgetContents(sortIndex, mode, value.sortAscending, false);
        }
        const bool resetScroll = rebuild || std::any_of(patch.keys.begin(), patch.keys.end(), [](const auto& key) {
            return key == "listMode" || key == "listColumns" || key.starts_with("detail") || key == "dateHeaders" ||
                key == "showFileCategories" || key == "showSearchBox" || key == "activeCategoryId" || key == "contentCategoryId" ||
                key == "categoryTabOrder" || key == "scrollContainerMode";
        });
        if (resetScroll) owner.scrollOffset = content.scrollOffset = 0;
        if (NativeKey(patch, "categoryTabOrder") || NativeKey(patch, "activeCategoryId") || NativeKey(patch, "showFileCategories"))
            owner.tabScrollOffset = source.tabScrollOffset = 0;
        // Layout can release items, create overflow pages and remove guide
        // placeholders. All of those authoritative fields are in the snapshot.
        if (rebuild) LayoutItems();
        saved = SaveLayoutSlots(false);
        if (!saved) rollback();
    }
    catch (...) { rollback(); RebuildContainersAndItems(); throw; }
    if (rebuild || !saved) RebuildContainersAndItems();
    for (const auto& container : containers_)
    {
        auto* view = dynamic_cast<WidgetContainer*>(container.get());
        if (!view || !view->GetWidgetData()) continue;
        auto* data = view->GetWidgetData();
        if (auto* search = dynamic_cast<ScrollingItemWidget*>(view))
        {
            const auto query = searchQueries.find(data->id);
            if (query != searchQueries.end() && data->showSearchBox) search->SetSearchText(query->second);
            if (!data->showSearchBox) search->ClearSearchText();
        }
        NativeInvalidate(view);
    }
    if (saved)
    {
        const auto index = FindWidgetIndexById(Utf8ToWide(expected.id));
        if (index < widgets_.size())
        {
            auto& owner = widgets_[index];
            hoverExpandedWidgetIds_.erase(owner.id);
            if (NativeKey(patch, "titleBarCollapsed")) hoverExpansionSuppressedWidgetId_ = owner.titleBarCollapsed ? owner.id : L"";
            if (NativeKey(patch, "titleBarExpandOnHover")) UpdateWidgetHoverExpansion(lastMousePoint_);
            if (NativeKey(patch, "titleBarCollapsed")) UpdateHostInputImePosition();
            if (dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ == owner.id)
            {
                // The popup is a projection; preserve its file resources and
                // apply the same native configuration after the durable commit.
                dockFolderPopupWidget_.title = owner.title;
                dockFolderPopupWidget_.listMode = owner.listMode; dockFolderPopupWidget_.listColumns = owner.listColumns;
                dockFolderPopupWidget_.showSearchBox = owner.showSearchBox; dockFolderPopupWidget_.showFileCategories = owner.showFileCategories;
                dockFolderPopupWidget_.dateHeaders = owner.dateHeaders; dockFolderPopupWidget_.fanPopup = owner.fanPopup;
                dockFolderPopupWidget_.categoryTabOrder = owner.categoryTabOrder; dockFolderPopupWidget_.activeCategoryId = owner.activeCategoryId;
                dockFolderPopupWidget_.detailShowModified = owner.detailShowModified; dockFolderPopupWidget_.detailShowType = owner.detailShowType; dockFolderPopupWidget_.detailShowSize = owner.detailShowSize;
                dockFolderPopupWidget_.showDetails = owner.showDetails;
                dockFolderPopupWidget_.detailModifiedPosition = owner.detailModifiedPosition; dockFolderPopupWidget_.detailTypePosition = owner.detailTypePosition; dockFolderPopupWidget_.detailSizePosition = owner.detailSizePosition;
                dockFolderPopupWidget_.contentSortColumn = owner.contentSortColumn; dockFolderPopupWidget_.contentSortAscending = owner.contentSortAscending;
                if (dockFolderPopupContainer_)
                { if (!owner.showSearchBox) dockFolderPopupContainer_->ClearSearchText(); dockFolderPopupContainer_->InvalidateFilterCache(); }
                popupScrollOffset_ = 0; RefreshDockFolderPopupGeometry();
            }
            if (!dockFolderPopupOpen_ && popupWidgetIndex_ == index)
            { popupScrollOffset_ = 0; InvalidateCollectionPopupContent(); RefreshOpenCollectionPopupGeometry(); }
            if (NativeKey(patch, "fanPopup") && ((!dockFolderPopupOpen_ && popupWidgetIndex_ == index) ||
                    (dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ == owner.id)))
            {
                ResetCollectionPopupFanScroll(); popupFanShowAll_ = popupFanActionFocused_ = false; ResetCollectionPopupAnimationCache();
                popupAnimation_.ShowImmediately();
                popupAnimation_.Configure(animation::RuntimePopupEffect() == animation::Fade,
                    animation::RuntimeDurationScale() * (owner.fanPopup ? 2.4 : 1.0), !owner.fanPopup);
            }
            (void)QueueDesktopWidgetComposition(owner.id);
        }
        demoCollectionIdentityCache_.clear();
    }
    InvalidateDragStaticScene();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateFloatingPopupWindow(false); InvalidateFloatingDockWindow(false);
    return saved;
}

bool DesktopApp::NativeWidgetSearchAction(std::wstring_view id, const std::wstring& query)
{
    const auto state = CaptureNativeWidgetSettings(id);
    if (!state.editable || !state.searchAvailable) return false;
    const auto index = FindWidgetIndexById(std::wstring(id));
    ScrollingItemWidget* search = nullptr;
    for (const auto& container : containers_)
        if (auto* candidate = dynamic_cast<ScrollingItemWidget*>(container.get()); candidate && candidate->GetWidgetData() == &widgets_[index])
        { search = candidate; break; }
    if (dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ == id && dockFolderPopupContainer_) search = dockFolderPopupContainer_.get();
    if (!search) return false;
    search->SetSearchText(query);
    NativeInvalidate(search);
    widgets_[index].scrollOffset = 0;
    if (dockFolderPopupOpen_ && dockFolderPopupMappingWidgetId_ == id) { popupScrollOffset_ = 0; RefreshDockFolderPopupGeometry(); }
    else if (popupWidgetIndex_ == index) { popupScrollOffset_ = 0; RefreshOpenCollectionPopupGeometry(); }
    InvalidateDragStaticScene();
    (void)QueueDesktopWidgetComposition(widgets_[index].id);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateFloatingPopupWindow(false);
    return true;
}
