#include "app/app.h"
#include "drag_drop/pending_drop_rules.h"
#include "drag_drop/pending_drop_completion.h"
#include "drag_drop/pending_drop_placement.h"
#include "drag_drop/desktop_output_placement.h"
#include "icons/large_icon_renderer.h"
#include "desktop_preview_source.h"
#include "desktop_preview_cache.h"
#include "widgets/folder_member_order.h"

// Desktop drop-preview rendering, caching and deferred placement.

std::uint64_t DesktopApp::BeginFileDropRead(const DropPreviewList& preview, bool present,
    const std::wstring& widgetId, const std::wstring& folderPath, bool popup,
    const std::vector<std::wstring>& knownPaths)
{
    PendingLandingCache cache;
    if (preview.pinMaterializedItemsToDock)
    {
        cache.dockMonitor = preview.dockMonitor;
        cache.dockInsertIndex = preview.dockInsertIndex;
    }
    else if (preview.targetKind == DropTargetKind::FolderMapping)
    {
        PendingFolderPlacement folder;
        folder.widgetId = popup ? dockFolderPopupMappingWidgetId_ : widgetId;
        folder.popupSourceId = popup ? dockFolderPopupSourceId_ : std::wstring{};
        folder.sourceFolderPath = folderPath;
        folder.insertIndex = preview.insertIndex;
        folder.sourcePaths = knownPaths;
        cache.folderPlacements.push_back(std::move(folder));
    }
    else
    {
        auto landings = preview.landings;
        if (knownPaths.size() > landings.size() && !landings.empty())
        {
            if (landings.front().kind == DropLandingKind::DesktopCell)
                landings = BuildExternalDesktopPreviewList(landings.front().cell, knownPaths.size()).landings;
            else if (landings.front().kind == DropLandingKind::WidgetIndex)
                while (landings.size() < knownPaths.size())
                {
                    auto next = landings.back();
                    ++next.sourceIndex;
                    ++next.insertIndex;
                    landings.push_back(std::move(next));
                }
        }
        for (const auto& landing : landings)
        {
            PendingLandingEntry entry;
            entry.kind = landing.kind;
            entry.sourceIndex = landing.sourceIndex;
            entry.cell = landing.cell;
            entry.span = landing.span;
            entry.plannedPageSize = landing.plannedPageSize;
            entry.insertIndex = landing.insertIndex;
            entry.widgetId = landing.widgetId;
            cache.entries.push_back(std::move(entry));
        }
    }
    DragSourceList sources;
    for (const auto& path : knownPaths)
    {
        DragSourceEntry entry;
        entry.sourceIndex = sources.entries.size();
        entry.filePath = path;
        entry.displayName = FileNameFromPath(path);
        sources.entries.push_back(std::move(entry));
    }
    return BeginFileDropVisual(std::move(cache), present, &sources, preview.action);
}

std::uint64_t DesktopApp::BeginFileDropVisual(PendingLandingCache cache, bool present,
    const DragSourceList* sources, DropAction action)
{
    if (cache.entries.empty() && cache.folderPlacements.empty() && !cache.dockMonitor) return 0;
    const auto id = ++nextFileDropVisual_;
    const bool dock = cache.dockMonitor != nullptr;
    cache.visualId = id;
    cache.movingFiles = action == DropAction::Move;
    if (sources) CaptureFileDropAppearance(cache, *sources);
    cache.UpdateMovedSourceKeys();
    cache.waitingForFiles = true;
    cache.active = true;
    pendingLandingCaches_.push_back(std::move(cache));
    fileDropLayouts_.clear();
    InvalidateFileDropVisual(pendingLandingCaches_.back(), true);
    if (present && hwnd_)
    {
        PresentDesktopPointerUpdate();
        if (dock) InvalidateFloatingDockWindow(true);
        else if (ShouldShowFloatingPopupWindow()) InvalidateFloatingPopupWindow(true);
        FlushPendingCompositionCommit();
    }
    return id;
}

PendingLandingCache DesktopApp::TakeFileDropVisual(std::uint64_t id)
{
    if (!id) return {};
    const auto found = std::find_if(pendingLandingCaches_.begin(), pendingLandingCaches_.end(),
        [id](const auto& cache) { return cache.waitingForFiles && cache.visualId == id; });
    if (found == pendingLandingCaches_.end()) return {};
    auto cache = std::move(*found);
    pendingLandingCaches_.erase(found);
    fileDropLayouts_.clear();
    cache.waitingForFiles = false;
    InvalidateFileDropVisual(cache, true);
    return cache;
}

void DesktopApp::InvalidateFileDropVisual(const PendingLandingCache& cache, bool layoutChanged)
{
    if (!hwnd_) return;
    if (cache.dockMonitor)
    {
        for (const auto& container : containers_)
            if (auto* dock = dynamic_cast<DockContainer*>(container.get()); dock && dock->GetMonitor() == cache.dockMonitor)
            {
                if (layoutChanged) dock->InvalidateSlots();
                const RECT bounds = dock->GetAnimationVisualBounds();
                InvalidateRect(hwnd_, &bounds, FALSE);
                if (auto* host = FindPersistentDockHost(dock)) InvalidateFloatingDockWindow(*host, false);
            }
    }
    RECT dirty{};
    std::unordered_set<std::wstring> owners;
    bool popup = false;
    if (cache.movingFiles)
    {
        owners.insert(cache.originWidgetId);
        std::unordered_set<std::wstring> folders;
        for (const auto& file : cache.files)
        {
            UnionRect(&dirty, &dirty, &file.sourceBounds);
            folders.insert(snowdesktop::shell_refresh::FolderKey(std::filesystem::path(file.sourcePath).parent_path().wstring()));
        }
        for (const auto& widget : widgets_)
            if (widget.type == DesktopWidgetType::FolderMapping && folders.contains(snowdesktop::shell_refresh::FolderKey(widget.sourceFolderPath)))
                owners.insert(widget.id);
        popup = dockFolderPopupOpen_ && folders.contains(snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
    }
    for (const auto& entry : cache.entries)
    {
        if (entry.kind == DropLandingKind::DesktopCell)
        {
            const RECT rect = GetGridRect(gridPages_, entry.cell, entry.span);
            UnionRect(&dirty, &dirty, &rect);
        }
        else owners.insert(entry.widgetId);
    }
    for (const auto& folder : cache.folderPlacements)
    {
        owners.insert(folder.widgetId);
        popup |= !folder.popupSourceId.empty() && folder.popupSourceId == dockFolderPopupSourceId_;
    }
    if (const auto* open = GetOpenPopupWidget()) popup |= owners.contains(open->id);
    if (popup && folderPreview_ && folderPreview_->inlineBounds) owners.insert(folderPreview_->hostWidgetId);
    for (const auto& owner : owners)
    {
        const auto group = FindCollectionGroupIndexForChild(owner);
        const auto index = group < widgets_.size() ? group : FindWidgetIndexById(owner);
        if (index >= widgets_.size()) continue;
        const auto& widget = widgets_[index];
        popup |= GetOpenPopupWidget() == &widget;
        UnionRect(&dirty, &dirty, &widget.bounds);
    }
    if (layoutChanged && !dragSession_.HasContext())
    {
        for (const auto& container : containers_)
        {
            auto* view = dynamic_cast<WidgetContainer*>(container.get());
            if (!view || !view->GetWidgetData()) continue;
            const auto* content = view->GetContentViewData();
            if (!owners.contains(view->GetWidgetData()->id) && !(content && owners.contains(content->id))) continue;
            if (auto* group = dynamic_cast<FileGroup*>(view))
                if (auto* source = group->GetActiveSourceContainer()) source->InvalidateSlots();
            view->InvalidateSlots();
        }
        if (popup && dockFolderPopupContainer_) dockFolderPopupContainer_->InvalidateSlots();
    }
    if (popup)
    {
        UnionRect(&dirty, &dirty, &popupRect_);
        InvalidateCollectionPopupContent();
        InvalidateFloatingPopupWindow(false);
    }
    if (!IsRectEmpty(&dirty)) InvalidateRect(hwnd_, &dirty, FALSE);
}

void DesktopApp::DrawFileDropPlaceholders(ID2D1DeviceContext* context, const DesktopWidget* widget,
    WidgetContainer* view, const RECT* popup)
{
    if (pendingLandingCaches_.empty()) return;
    if (!widget)
    {
        for (const auto& request : pendingLandingCaches_)
        {
            if (!request.visualId || request.dockMonitor) continue;
            for (const auto& entry : request.entries)
            {
                if (entry.kind != DropLandingKind::DesktopCell) continue;
                const auto* file = request.FindFile(entry.sourceIndex);
                DrawPendingFile(context, GetGridRect(gridPages_, entry.cell, entry.span),
                    file ? file->appearance.get() : nullptr);
            }
        }
        return;
    }

    const auto& layout = FileDropMemberLayout(*widget, popup ? nullptr : view);
    if (layout.gaps.empty()) return;
    const RECT viewport = popup ? GetCollectionPopupContentRect(*popup) : view->GetContentViewportRect();
    const bool light = popup ? collectionPopupLightTheme_ : view->UsesLightContentTheme();
    const bool list = popup ? UsesCollectionPopupList(*widget) :
        view->GetContentViewData() && view->GetContentViewData()->listMode;
    const bool privateView = widget->privacyMode && !PtInRect(&viewport, lastMousePoint_);
    context->PushAxisAlignedClip(ToD2DRect(viewport), D2D1_ANTIALIAS_MODE_ALIASED);
    for (const auto& gap : layout.gaps)
    {
        const bool single = !popup && view->IsSingleCellPresentation();
        const RECT rect = popup ? FileDropPopupCell(*popup, gap.position) :
            single ? view->GetBodyRect() : view->GetMemberLayoutRect(gap.position);
        RECT visible{};
        if (IntersectRect(&visible, &rect, &viewport))
            DrawPendingFile(context, rect, privateView ? nullptr : gap.file.get(), view, list, light);
        if (single) break;
    }
    context->PopAxisAlignedClip();
}

bool DesktopApp::ApplyPendingFolderPlacements(DesktopWidget& targetWidget,
    const std::wstring& widgetId, const std::wstring& popupSourceId)
{
    fileDropLayouts_.clear();
    bool changed = false;
    for (auto& cache : pendingLandingCaches_)
        changed = ApplyPendingFolderPlacements(cache, targetWidget, widgetId, popupSourceId) || changed;
    std::erase_if(pendingLandingCaches_, [](const auto& cache) { return !cache.active; });
    return changed;
}

bool DesktopApp::ApplyPendingFolderPlacements(
    PendingLandingCache& cache, DesktopWidget& targetWidget,
    const std::wstring& widgetId,
    const std::wstring& popupSourceId)
{
    return snowdesktop::pending_drop::ApplyFolder(cache, targetWidget, widgetId, popupSourceId,
        MatchPendingName, ToUpperInvariant, PathsEqualInsensitive);
}

void DesktopApp::ApplyPendingFolderPreviewPlacements(
    const snowdesktop::shell_refresh::FolderSnapshot& snapshot)
{
    if (!snapshot.complete || pendingLandingCaches_.empty()) return;
    fileDropLayouts_.clear();
    const auto key = snowdesktop::shell_refresh::FolderKey(snapshot.path);
    const auto popupId = L"folder-preview:" + key;
    const auto matches = [&](const PendingFolderPlacement& placement) {
        return placement.popupSourceId == popupId;
    };
    if (std::none_of(pendingLandingCaches_.begin(), pendingLandingCaches_.end(),
            [&](const auto& cache) {
                return !cache.waitingForFiles && std::any_of(cache.folderPlacements.begin(), cache.folderPlacements.end(), matches);
            })) return;

    auto sort = folderPreviewSort_.Get(key);
    if (sort.mode != snowdesktop::folder_sort_rules::kManual)
    {
        // A position selected in the currently displayed order is valid even
        // before the directory has its first manual sort preference.
        for (auto& cache : pendingLandingCaches_)
        {
            if (cache.waitingForFiles) continue;
            std::erase_if(cache.folderPlacements, [&](const PendingFolderPlacement& placement) {
                if (!matches(placement)) return false;
                const auto& atDrop = placement.previewSortAtDrop;
                return !atDrop || atDrop->mode != sort.mode || atDrop->ascending != sort.ascending;
            });
            cache.active = !cache.entries.empty() || !cache.folderPlacements.empty();
        }
        std::erase_if(pendingLandingCaches_, [](const auto& cache) { return !cache.active; });
    }

    DesktopWidget target;
    target.sourceFolderPath = snapshot.path;
    target.folderEntries = snapshot.entries;
    target.folderSortMode = sort.mode;
    target.folderSortAscending = sort.ascending;
    target.itemKeys = std::move(sort.order);
    snowdesktop::folder_member_order::Refresh(target, ToUpperInvariant);
    if (ApplyPendingFolderPlacements(target, {}, popupId) &&
        !folderPreviewSort_.Set(key, {target.folderSortMode, target.folderSortAscending, std::move(target.itemKeys)}))
        MessageBeep(MB_ICONWARNING);
}

void DesktopApp::DrawDesktopDropPreviewList(ID2D1DeviceContext* ctx,
    const DropPreviewList& preview)
{
    if (!ctx) return;
    const snowdesktop::desktop_preview_source::Lookup sourceLookup(items_, dragSession_.IsActive());
    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell) continue;
        GridSpan span{
            std::max(1, landing.span.columns),
            std::max(1, landing.span.rows)
        };
        RECT targetRect = GetGridRect(gridPages_, landing.cell, span);
        if (dragSession_.IsActive())
        {
            const auto& sources = dragSession_.SourceList().entries;
            const auto source = std::find_if(sources.begin(), sources.end(),
                [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
            const auto* sourceItem = source != sources.end() ? sourceLookup.Find(*source) : nullptr;
            if (sourceItem)
            {
                DesktopWidget geometry; geometry.bounds = targetRect; geometry.gridCell = landing.cell;
                if (const auto* page = FindGridPage(gridPages_, landing.cell.pageId)) geometry.cellScale = GetGridPageCuScale(*page);
                const auto config = snowdesktop::large_icon_render_rules::ResolveComponentRadius(
                    EffectiveLargeIconConfig(*sourceItem), CurrentPersonalization().cornerRadius);
                snowdesktop::large_icon_renderer::DrawPlacementPreview(ctx, config, GetStandaloneWidgetFrameRect(geometry),
                    GetItemLayoutScale(targetRect), true, &targetRect);
                continue;
            }
        }
        DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
    }
}

/**
 * @brief 获取或重建缓存的桌面放置预览。
 *
 * 拖拽渲染每帧调用 DrawDropPreview → BuildDropPreviewList → BuildDesktopLandings，
 * 后者遍历全部 items/widgets 搜索空位。当鼠标位置/动作/目标不变时复用缓存，
 * 避免每帧重建导致卡顿（尤其阶段2-4全页/跨页/新建页搜索）。
 */
const DropPreviewList& DesktopApp::GetCachedDesktopDropPreview(
    bool hasItemDrag, const DragSourceList& sourceList,
    Container* target, Slot* slot, HitRegion region, int mods, POINT dragPoint)
{
    const size_t externalCount = hasItemDrag ? 0 : static_cast<size_t>(std::max(1, dragDropController_.ExternalSummary().fileCount));
    return snowdesktop::desktop_preview_cache::Read(
        {cachedDropPreview_, cachedDropPreviewPoint_, cachedDropPreviewMods_, cachedDropPreviewTarget_, cachedDropPreviewSlot_,
            cachedDropPreviewRegion_, cachedDropPreviewHasItems_, cachedDropPreviewSourceCount_,
            cachedDropPreviewSceneRevision_, cachedDropPreviewExternalCount_},
        {hasItemDrag, dragPoint, mods, target, slot, region, sourceList.entries.size(), dragSession_.StaticSceneRevision(), externalCount},
        [&] { return BuildDropPreviewList(sourceList, target, slot, region, mods, dragPoint); },
        [&]() -> DropPreviewList {
            GridCell targetCell = CellFromPoint(dragPoint);
            if (targetCell.pageId.empty()) return {};
            return BuildExternalDesktopPreviewList(targetCell, externalCount);
        });
}

/**
 * @brief 应用缓存的放置结果，将新创建的文件分配到正确的网格位置或组件中。
 */
void DesktopApp::ApplyPendingPlacement()
{
    fileDropLayouts_.clear();
    bool changed = false;
    std::unordered_set<std::wstring> claimed;
    for (auto& cache : pendingLandingCaches_)
        changed = ApplyPendingPlacement(cache, claimed) || changed;
    std::erase_if(pendingLandingCaches_, [](const auto& cache) { return !cache.active; });
    if (changed)
    {
        LayoutItems();
        RequestLayoutSave();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

bool DesktopApp::ApplyPendingPlacement(PendingLandingCache& cache,
    std::unordered_set<std::wstring>& claimed)
{
    if (!cache.active || cache.waitingForFiles || cache.entries.empty()) return false;
    auto usedSlots = snowdesktop::desktop_output_placement::Used(widgets_, items_, claimed, cache.existingDesktopKeys,
        ToUpperInvariant, [this](const auto& widget) { return IsGroupedWidget(widget); },
        [this](const auto& item) { return IsItemInAnyWidget(item); });

    auto findWidgetContainer = [&](const std::wstring& widgetId) -> WidgetContainer* {
        for (auto& container : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
            if (data && data->id == widgetId)
                return widget;
        }
        const size_t groupedIndex =
            FindCollectionGroupIndexForChild(widgetId);
        if (groupedIndex < widgets_.size())
            for (auto& container : containers_)
            {
                auto* widget =
                    dynamic_cast<WidgetContainer*>(
                        container.get());
                if (widget &&
                    widget->GetWidgetData() ==
                        &widgets_[groupedIndex])
                    return widget;
            }
        return nullptr;
    };

    // Mapped-folder snapshots arrive independently of the desktop read.
    // Only their fresh enumeration can reconcile folder placements. Using
    // the current widget entries here could consume a batch before its files
    // appear in that old directory list.
    const bool entriesChanged = snowdesktop::pending_drop::ApplyEntries(cache, widgets_, items_, claimed,
        ToUpperInvariant,
        [](const DesktopItem& item, const std::wstring& sourceName) {
            return MatchPendingName(item.name, sourceName) ||
                (!item.parsingName.empty() && MatchPendingName(FileNameFromPath(item.parsingName), sourceName));
        },
        [&](const PendingLandingEntry& landing, const std::wstring& key) {
            auto* widget = findWidgetContainer(landing.widgetId);
            return !widget || landing.action == DropAction::Link || widget->AllowsDesktopKey(key);
        },
        [&](DesktopItem& item, const PendingLandingEntry& landing) {
            snowdesktop::desktop_output_placement::Services services;
            services.reference = [this]() -> const GridPage* {
                auto monitorOrder = BuildMonitorRenderOrder();
                return !monitorOrder.empty()
                    ? &gridPages_[monitorOrder.back()] : GetFirstPageGridPage();
            };
            services.findFree = [this](GridSpan span, auto& used, GridCell& found, const auto& page, int slot) {
                return TryFindFreeCell(span, used, found, page, slot);
            };
            services.slot = [this](const auto& cell) { return SlotFromCell(gridPages_, cell); };
            return snowdesktop::desktop_output_placement::Place(
                {savedPageIds_, savedPageColumns_, savedPageRows_, gridPages_}, item, landing, usedSlots, services);
        },
        [&](const std::wstring& id) {
            RefreshCollectedKeysCache();
            if (auto* widget = findWidgetContainer(id)) widget->InvalidateSlots();
        });
    return entriesChanged;
}

// ── 网格全局函数 ──────────────────────────────────────────

/**
 * @brief 根据页面 ID 在页面列表中查找对应的网格页面。
 * @param pages 页面列表。
 * @param pageId 页面 ID。
 * @return 找到的页面指针，未找到返回 nullptr。
 */
