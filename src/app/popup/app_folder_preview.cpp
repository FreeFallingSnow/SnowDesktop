#include "app/app.h"

#include "common/l10n.h"
#include "common/utils.h"
#include "layout/collection_popup_layout.h"
#include "layout/folder_preview_header_layout.h"
#include "settings/animation_settings.h"
#include "widgets/widget_folder_search.h"

#include <cmath>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
struct PreviewItem
{
    std::wstring path;
    std::wstring desktopKey;
    bool knownDirectory = false;
};

PreviewItem GetPreviewItem(const Item* item)
{
    if (const auto* icon = dynamic_cast<const DesktopIcon*>(item))
        if (const auto* desktop = icon->GetDesktopItem())
            return {desktop->parsingName, desktop->layoutKey};
    if (const auto* icon = dynamic_cast<const FolderEntryIcon*>(item))
        if (const auto* entry = icon->GetFolderEntry())
            return {entry->fullPath, {}, entry->isDirectory};
    return {};
}

std::wstring FolderName(const std::wstring& path)
{
    const auto value = std::filesystem::path(path).lexically_normal();
    if (!value.filename().empty()) return value.filename().wstring();
    if (!value.parent_path().filename().empty()) return value.parent_path().filename().wstring();
    return path;
}
}

bool DesktopApp::TryFolderPreviewDoubleClick(POINT point, WPARAM keyState)
{
    using namespace snowdesktop::folder_preview;
    if (dockTrayPopup_ || (keyState & MK_SHIFT)) return false;
    const bool forcePreview = (keyState & MK_CONTROL) != 0;
    const DesktopWidget* source = nullptr;
    const DesktopWidget* host = nullptr;
    std::optional<RECT> inlineBounds;
    const bool inPopup = IsPointOccludedByOpenPopup(point);
    const bool ordinaryDockFolder = inPopup && dockFolderPopupOpen_ &&
        (folderPreview_ ? folderPreview_->sourceWidgetId.empty() && folderPreview_->folderHome.has_value()
                        : dockFolderPopupMappingWidgetId_.empty());
    PreviewItem target;
    const auto resolveInView = [&](ScrollingItemWidget& view) {
        const RECT viewport = view.GetContentViewportRect();
        if (!PtInRect(&viewport, point)) return;
        source = view.GetWidgetData();
        for (const auto& slot : view.GetSlots())
        {
            if (!slot) continue;
            const RECT bounds = slot->GetBounds();
            if (!PtInRect(&bounds, point)) continue;
            const Item* item = slot->GetItem();
            target = GetPreviewItem(item);
            if (auto* group = dynamic_cast<FileGroup*>(&view))
            {
                auto* memberSource = group->GetSourceContainerForItem(item);
                source = memberSource ? memberSource->GetWidgetData() : nullptr;
            }
            return;
        }
    };
    if (inPopup)
    {
        if (!IsCollectionPopupInteractive()) return false;
        const auto* popup = GetOpenPopupWidget();
        if (!popup) return false;
        if (auto* group = GetGroupPopupView())
        {
            host = group->GetWidgetData();
            const RECT frame = GetCategorizedPopupFrame(GetCollectionPopupRect(*popup));
            group->SetPopupFrame(&frame);
            resolveInView(*group);
        }
        else if (dockFolderPopupOpen_ || popup->type == DesktopWidgetType::FileCategories)
        {
            if (dockFolderPopupOpen_)
            {
                const size_t index = FindWidgetIndexById(
                    folderPreview_ ? folderPreview_->sourceWidgetId : dockFolderPopupMappingWidgetId_);
                if (!dockFolderPopupAvailable_ || (index >= widgets_.size() && !ordinaryDockFolder)) return false;
                if (index < widgets_.size()) source = &widgets_[index];
            }
            else
                source = host = popup;
            const RECT bounds = GetCollectionPopupRect(*popup);
            const RECT content = GetCollectionPopupContentRect(bounds);
            for (size_t i = 0; i < GetPopupItemCount(*popup); ++i)
            {
                const RECT item = GetCollectionPopupItemRect(bounds, i);
                RECT clipped{};
                if (!IntersectRect(&clipped, &item, &content) ||
                    !HitTestCollectionPopupItem(bounds, i, point))
                    continue;
                if (dockFolderPopupOpen_)
                {
                    const auto& entry = popup->folderEntries[GetPopupFolderEntryIndex(*popup, i)];
                    target = {entry.fullPath, {}, entry.isDirectory};
                }
                else
                {
                    const auto keys = GetPopupItemKeys(*popup);
                    if (i >= keys.size()) break;
                    const size_t index = FindItemIndexByKey(keys[i]);
                    if (index < items_.size())
                        target = {items_[index].parsingName, items_[index].layoutKey};
                }
                break;
            }
        }
    }
    else
    {
        if (GetOpenPopupWidget() || GetDockContainerAtPoint(point)) return false;
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            auto* view = dynamic_cast<ScrollingItemWidget*>(it->get());
            if (!view) continue;
            const auto* data = view->GetWidgetData();
            if (!data || IsGroupedWidget(*data) || view->IsCollapsed() ||
                view->IsSingleCellPresentation() ||
                (desktopIconsHidden_ && !data->keepWhenDesktopHidden))
                continue;
            const RECT frame = view->GetFrameRect();
            if (!PtInRect(&frame, point)) continue;
            host = data;
            inlineBounds = frame;
            if (Supports(data->type) || data->type == DesktopWidgetType::FileGroup)
                resolveInView(*view);
            break;
        }
    }
    if ((!ordinaryDockFolder && (!source || !Supports(source->type))) ||
        (!forcePreview && !personalizationSettings_.folderDoubleClickPreview) ||
        target.path.empty() || !std::filesystem::path(target.path).is_absolute())
        return false;

    const auto sourceId = source ? source->id : std::wstring{};
    const auto hostId = host ? host->id : std::wstring{};
    const auto expectedPopupPath = folderPreview_ ? dockFolderPopupWidget_.sourceFolderPath : std::wstring{};
    const auto dockFolderSourceId = dockFolderPopupOpen_ ? dockFolderPopupSourceId_ : std::wstring{};
    const auto finish = [this, target, sourceId, hostId, inlineBounds, inPopup, expectedPopupPath, forcePreview, dockFolderSourceId]
        (snowdesktop::item_location::FolderTarget resolved) {
        if (exitRequested_ || dragSession_.HasContext() || dragDropController_.IsTransportActive()) return;
        const size_t index = FindWidgetIndexById(sourceId);
        if ((!sourceId.empty() && index >= widgets_.size()) ||
            (!forcePreview && !personalizationSettings_.folderDoubleClickPreview)) return;
        if (inPopup != (GetOpenPopupWidget() != nullptr) ||
            (inPopup && !IsCollectionPopupInteractive()) ||
            (folderPreview_ && dockFolderPopupWidget_.sourceFolderPath != expectedPopupPath))
            return;
        const size_t hostIndex = FindWidgetIndexById(hostId);
        if (!hostId.empty() && hostIndex >= widgets_.size()) return;
        if (inlineBounds)
        {
            if (hostIndex >= widgets_.size()) return;
            const RECT hostFrame = GetStandaloneWidgetFrameRect(widgets_[hostIndex]);
            if (!EqualRect(&hostFrame, &*inlineBounds)) return;
        }
        if (inPopup && !folderPreview_)
        {
            const auto* popup = GetOpenPopupWidget();
            if (dockFolderPopupOpen_ ? dockFolderPopupSourceId_ != dockFolderSourceId
                                    : !dockFolderSourceId.empty() || !popup || popup->id != hostId)
                return;
        }
        if (resolved.path.empty() ||
            (!resolved.available && !target.knownDirectory))
        {
            const size_t item = FindItemIndexByKey(target.desktopKey);
            const bool accepted = !target.desktopKey.empty()
                ? item < items_.size() && LaunchDesktopItem(item)
                : LaunchPathWithShortcutPolicy(hwnd_, target.path);
            if (accepted && !folderPreview_)
            {
                ClearSelection();
                if (inPopup) CloseCollectionPopup();
                else InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }

        bool fromFan = false;
        if (!folderPreview_)
        {
            Session session;
            session.sourceWidgetId = sourceId;
            session.hostWidgetId = hostId;
            session.inlineBounds = inlineBounds;
            if (inlineBounds)
            {
                for (const auto& container : containers_)
                {
                    const auto* view = dynamic_cast<const WidgetContainer*>(container.get());
                    if (!view || view->GetWidgetData() != &widgets_[hostIndex]) continue;
                    session.SetInlineGeometry(*view);
                    break;
                }
                session.inlineListMode = widgets_[index].listMode;
            }
            if (const auto* popup = GetOpenPopupWidget())
            {
                session.homeBounds = GetCollectionPopupRect(*popup);
                fromFan = UsesCollectionPopupFan(*popup);
                if (!fromFan)
                    session.pendingPopupResize = GetPresentedCollectionPopupGeometry();
            }
            session.homeScroll = popupScrollOffset_;
            session.homeFanScroll = popupFanScroll_;
            session.homeFanShowAll = popupFanShowAll_;
            session.homeFanActionFocused = popupFanActionFocused_;
            for (const auto& item : items_)
                if (item.selected && (!forcePreview || item.layoutKey != target.desktopKey))
                    session.selectedDesktopKeys.push_back(item.layoutKey);
            const auto keepSelection = [&](const FolderEntry& entry) {
                return entry.selected && (!forcePreview || !PathsEqualInsensitive(entry.fullPath, target.path));
            };
            for (const auto& widget : widgets_)
                for (const auto& entry : widget.folderEntries)
                    if (keepSelection(entry)) session.selectedFolderPaths.emplace_back(widget.id, entry.fullPath);
            for (const auto& container : containers_)
                if (const auto* view = dynamic_cast<const ScrollingItemWidget*>(container.get());
                    view && view->folderSearch_ && view->GetWidgetData())
                    for (const auto& row : view->folderSearch_->rows)
                        if (row.recursive && keepSelection(*row.recursive))
                            session.selectedFolderPaths.emplace_back(
                                view->GetWidgetData()->id, row.recursive->fullPath);
            if (dockFolderPopupOpen_)
            {
                CancelDockFolderPopupIconLoads();
                if (forcePreview)
                    for (auto& entry : dockFolderPopupWidget_.folderEntries)
                        entry.selected = keepSelection(entry);
                session.folderHome.emplace(FolderPopupHome{
                    std::move(dockFolderPopupWidget_), std::move(dockFolderPopupContainer_),
                    std::move(dockFolderPopupSourceId_), std::move(dockFolderPopupMappingWidgetId_),
                    dockFolderPopupKnownItemCount_, dockFolderPopupAvailable_, dockFolderPopupLoading_});
            }
            folderPreview_.emplace(std::move(session));
            ClearSelection();
        }
        OpenFolderPreviewPath(resolved.path, fromFan);
    };
    folderReadWork_.Cancel(kResolveWorkKey);
    if (target.knownDirectory)
    {
        finish({target.path, snowdesktop::item_location::FolderTargetKind::Directory, true});
        return true;
    }
    return folderReadWork_.Submit(kResolveWorkKey, [path = target.path] {
        return snowdesktop::item_location::ResolveFolderTarget(path);
    }, finish, hwnd_, kBackgroundShellReadyMessage);
}

void DesktopApp::ShowFolderPreviewDirectory(const std::wstring& path, bool fromFan)
{
    const RECT previous = popupRect_;
    CancelRenameClick();
    CancelPointerPressWithoutCaptureRelease();
    ReleaseCapturePreservingPointerState();
    CancelPopupHover(true);
    ClearListTitleHover();
    ClearPopupDragTarget();
    ClearPopupMouseDownItem();
    ResetCollectionPopupAnimationCache();
    ResetCollectionPopupFanScroll();
    popupFanShowAll_ = popupFanActionFocused_ = false;
    popupAnimation_.ShowImmediately();
    AdvanceFloatingPopupContentGeneration();
    CancelDockFolderPopupIconLoads();
    ClearDockFolderPopupEntries();
    dockFolderPopupContainer_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    dockFolderPopupMappingWidgetId_.clear();
    dockFolderPopupOpen_ = true;
    dockFolderPopupAvailable_ = false;
    dockFolderPopupLoading_ = true;
    dockFolderPopupKnownItemCount_ = 0;
    popupScrollOffset_ = 0;
    const auto key = snowdesktop::shell_refresh::FolderKey(path);
    dockFolderPopupSourceId_ = L"folder-preview:" + key;
    const auto sort = folderPreviewSort_.Get(key);
    auto& view = dockFolderPopupWidget_;
    view = {};
    view.type = DesktopWidgetType::FolderMapping;
    view.id = kDockFolderPopupWidgetId;
    view.gridCell.pageId = kDockPageId;
    view.sourceFolderPath = path;
    view.title = FolderName(path);
    view.folderSortMode = sort.mode;
    view.folderSortAscending = sort.ascending;
    view.contentSortColumn = snowdesktop::list_detail_rules::FromLegacyFolderSortMode(sort.mode);
    view.contentSortAscending = sort.ascending;
    const size_t source = FindWidgetIndexById(folderPreview_->sourceWidgetId);
    const auto* sourceView = source < widgets_.size() ? &widgets_[source]
        : folderPreview_->folderHome ? &folderPreview_->folderHome->data : nullptr;
    if (sourceView)
    {
        view.listMode = sourceView->listMode;
        view.listColumns = sourceView->listColumns;
    }
    if (folderPreview_->inlineBounds)
    {
        const size_t host = FindWidgetIndexById(folderPreview_->hostWidgetId);
        if (host < widgets_.size())
        {
            popupPageId_ = widgets_[host].gridCell.pageId;
            view.gridSpan = widgets_[host].gridSpan;
        }
        view.listMode = folderPreview_->inlineListMode;
        view.dateHeaders = folderPreview_->inlineDateHeaders;
    }
    SyncFolderChangeNotifications();
    RefreshDockFolderPopup(nullptr, !fromFan);
    if (fromFan)
    {
        RefreshDockFolderPopupGeometry(false);
        StartCollectionPopupAnimation(false, true);
        InvalidateCollectionPopupDesktopScene(previous);
        UpdateFloatingPopupWindowBounds(true);
    }
}

void DesktopApp::NavigateFolderPreviewBack()
{
    using namespace snowdesktop::folder_preview;
    folderReadWork_.Cancel(kResolveWorkKey);
    if (!folderPreview_ || dragSession_.HasContext() || dragDropController_.IsTransportActive()) return;
    if (renameController_.IsActive()) CommitRename(false);
    if (!folderPreview_) return;
    if (folderPreview_->pages.size() == 1)
    {
        RestoreFolderPreviewHome(true);
        return;
    }
    if (!folderPreview_->inlineBounds)
        folderPreview_->pendingPopupResize = GetPresentedCollectionPopupGeometry();
    folderPreview_->pages.pop_back();
    ShowFolderPreviewDirectory(folderPreview_->pages.back().path);
}

void DesktopApp::OpenFolderPreviewPath(const std::wstring& path, bool fromFan)
{
    folderReadWork_.Cancel(snowdesktop::folder_preview::kResolveWorkKey);
    if (!folderPreview_->pages.empty())
    {
        if (snowdesktop::shell_refresh::FolderKey(path) ==
            snowdesktop::shell_refresh::FolderKey(folderPreview_->pages.back().path)) return;
        folderPreview_->pages.back().scroll = popupScrollOffset_;
        if (!folderPreview_->inlineBounds)
            folderPreview_->pendingPopupResize = GetPresentedCollectionPopupGeometry();
    }
    folderPreview_->pages.push_back({path});
    ShowFolderPreviewDirectory(folderPreview_->pages.back().path, fromFan);
}

void DesktopApp::RestoreFolderPreviewHome(bool present)
{
    if (!folderPreview_) return;
    folderReadWork_.Cancel(snowdesktop::folder_preview::kResolveWorkKey);
    const auto previousPopup = present && !folderPreview_->inlineBounds
        ? std::optional{GetPresentedCollectionPopupGeometry()} : std::nullopt;
    auto session = std::move(*folderPreview_);
    folderPreview_.reset();
    const RECT previous = popupRect_;
    ClearPopupMouseDownItem();
    ClearPopupDragTarget();
    ResetCollectionPopupAnimationCache();
    CancelDockFolderPopupIconLoads();
    ClearDockFolderPopupEntries();
    dockFolderPopupContainer_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    dockFolderPopupOpen_ = session.folderHome.has_value();
    dockFolderPopupSourceId_.clear();
    dockFolderPopupMappingWidgetId_.clear();
    dockFolderPopupAvailable_ = dockFolderPopupLoading_ = false;
    dockFolderPopupKnownItemCount_ = 0;
    if (present)
    {
        ClearSelection();
        for (const auto& key : session.selectedDesktopKeys)
        {
            const size_t index = FindItemIndexByKey(key);
            if (index < items_.size()) items_[index].selected = true;
        }
        std::unordered_map<std::wstring_view, std::unordered_set<std::wstring_view>> paths;
        for (const auto& [owner, path] : session.selectedFolderPaths)
            paths[owner].insert(path);
        const auto selected = [&](std::wstring_view owner, std::wstring_view path) {
            const auto found = paths.find(owner);
            return found != paths.end() && found->second.contains(path);
        };
        for (auto& widget : widgets_)
            for (auto& entry : widget.folderEntries)
                if (selected(widget.id, entry.fullPath)) entry.selected = true;
        for (const auto& container : containers_)
            if (const auto* view = dynamic_cast<const ScrollingItemWidget*>(container.get());
                view && view->folderSearch_ && view->GetWidgetData())
                for (const auto& row : view->folderSearch_->rows)
                    if (row.recursive && selected(view->GetWidgetData()->id, row.recursive->fullPath))
                        row.recursive->selected = true;
    }
    if (session.folderHome)
    {
        auto& home = *session.folderHome;
        dockFolderPopupWidget_ = std::move(home.data);
        dockFolderPopupContainer_ = std::move(home.view);
        dockFolderPopupSourceId_ = std::move(home.dockSourceId);
        dockFolderPopupMappingWidgetId_ = std::move(home.mappingId);
        dockFolderPopupKnownItemCount_ = home.knownCount;
        dockFolderPopupAvailable_ = home.available;
        dockFolderPopupLoading_ = home.loading;
    }
    else
    {
        dockFolderPopupWidget_ = {};
        popupWidgetIndex_ = session.inlineBounds ? static_cast<size_t>(-1)
                                                : FindWidgetIndexById(session.hostWidgetId);
    }
    popupScrollOffset_ = session.homeScroll;
    popupFanScroll_ = session.homeFanScroll;
    popupFanShowAll_ = session.homeFanShowAll;
    popupFanActionFocused_ = session.homeFanActionFocused;
    popupRect_ = session.homeBounds;
    if (session.inlineBounds)
    {
        popupPageId_.clear();
        popupAnimation_.ResetHidden();
    }
    SyncFolderChangeNotifications();
    if (!present) return;
    AdvanceFloatingPopupContentGeneration();
    if (dockFolderPopupOpen_) QueueFolderRead(dockFolderPopupWidget_.sourceFolderPath);
    InvalidateCollectionPopupContent();
    if (previousPopup) StartFolderPreviewPopupResize(*previousPopup);
    RefreshOpenCollectionPopupGeometry();
    InvalidateCollectionPopupDesktopScene(previous);
}

void DesktopApp::DiscardFolderPreview()
{
    folderReadWork_.Cancel(snowdesktop::folder_preview::kResolveWorkKey);
    if (!folderPreview_) return;
    // Regular popup close also releases the suspended view and its frame.
    RestoreFolderPreviewHome(false);
}

const DesktopWidget* DesktopApp::GetInlineFolderPreviewData(const DesktopWidget& host) const
{
    return folderPreview_ && folderPreview_->inlineBounds && folderPreview_->hostWidgetId == host.id
        ? &dockFolderPopupWidget_ : nullptr;
}

snowdesktop::folder_preview::NavigationRow DesktopApp::GetFolderPreviewNavigationRow(const RECT& panel) const
{
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    const float scale = folderPreview_->inlineBounds ? folderPreview_->inlineScale : metrics.scale;
    const LONG top = folderPreview_->inlineBounds ? folderPreview_->inlineRowTop
        : panel.top + metrics.headerHeight;
    return snowdesktop::folder_preview::ResolveNavigationRow(panel, top, GetCategorizedWidgetTabHeight(), scale);
}

std::vector<snowdesktop::folder_preview::Breadcrumb> DesktopApp::GetFolderPreviewBreadcrumbs(const RECT& popup) const
{
    const auto row = GetFolderPreviewNavigationRow(popup);
    const float scale = row.scale;
    const RECT rect = row.breadcrumb;
    if (IsRectEmpty(&rect)) return {};
    const auto directory = std::filesystem::path(dockFolderPopupWidget_.sourceFolderPath).lexically_normal();
    auto path = directory.root_path();
    const auto relative = directory.relative_path();
    auto component = relative.begin();
    // A UNC share is the first navigable directory on a network path.
    if (directory.root_name().native().starts_with(L"\\\\") && component != relative.end())
    {
        path /= *component;
        ++component;
    }
    using snowdesktop::folder_preview::Breadcrumb;
    std::vector<Breadcrumb> crumbs{{{}, path.wstring(), path.wstring(), {}}};
    for (; component != relative.end(); ++component)
    {
        if (component->empty()) continue;
        path /= *component;
        crumbs.push_back({{}, path.wstring(), component->wstring(), {}});
    }
    using snowdesktop::collection_popup_layout::ScaleDimension;
    const LONG width = rect.right - rect.left;
    const LONG padding = ScaleDimension(10, scale);
    const LONG separator = ScaleDimension(10, scale);
    std::vector<LONG> widths;
    widths.reserve(crumbs.size());
    LONG total = 0;
    for (const auto& crumb : crumbs)
    {
        LONG labelWidth = ScaleDimension(7, scale) * static_cast<LONG>(crumb.label.size());
        if (dwriteFactory_ && collectionPopupControlTextFormat_)
        {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(crumb.label.c_str(), static_cast<UINT32>(crumb.label.size()),
                collectionPopupControlTextFormat_.Get(), 4096.0f, 128.0f, &layout)))
            {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(layout->GetMetrics(&metrics)))
                    labelWidth = static_cast<LONG>(std::ceil(metrics.width));
            }
        }
        const LONG segmentWidth = labelWidth + padding + (widths.empty() ? 0 : separator);
        widths.push_back(segmentWidth);
        total += segmentWidth;
    }
    if (total > width && crumbs.size() > 1)
    {
        widths.front() = std::min(widths.front(), width / 3);
        if (crumbs.size() > 2)
        {
            const LONG overflowWidth = std::min<LONG>(ScaleDimension(30, scale), width / 3);
            const LONG remaining = width - widths.front() - overflowWidth;
            const LONG current = std::min(widths.back(), remaining);
            LONG unused = remaining - current;
            size_t firstVisible = crumbs.size() - 1;
            while (firstVisible > 2 && widths[firstVisible - 1] <= unused)
                unused -= widths[--firstVisible];
            std::vector<std::wstring> hidden;
            for (size_t i = 1; i < firstVisible; ++i)
                hidden.push_back(std::move(crumbs[i].path));
            crumbs.erase(crumbs.begin() + 1, crumbs.begin() + firstVisible);
            widths.erase(widths.begin() + 1, widths.begin() + firstVisible);
            crumbs.insert(crumbs.begin() + 1, Breadcrumb{{}, {}, L"…", std::move(hidden)});
            widths.insert(widths.begin() + 1, overflowWidth);
        }
    }
    LONG left = rect.left;
    for (size_t i = 0; i < crumbs.size(); ++i)
    {
        const LONG right = std::min(rect.right, left + widths[i]);
        crumbs[i].bounds = {left, rect.top, right, rect.bottom};
        left = right;
    }
    return crumbs;
}

bool DesktopApp::HandleFolderPreviewHeaderPress(POINT point)
{
    if (!folderPreview_ || !IsCollectionPopupInteractive()) return false;
    if (folderPreview_->inlineBounds &&
        (PtInRect(&folderPreview_->inlineCollapseButton, point) ||
            PtInRect(&folderPreview_->inlineResizeHandle, point)))
    {
        // Continue the original press through the component's own controls.
        return false;
    }
    const RECT popup = GetCollectionPopupRect(dockFolderPopupWidget_);
    const auto* motion = GetCollectionPopupResizeTransition();
    const RECT panel = motion ? motion->Panel(popup) : popup;
    const auto row = GetFolderPreviewNavigationRow(panel);
    if (PtInRect(&row.back, point))
    {
        NavigateFolderPreviewBack();
        return true;
    }
    if (PtInRect(&folderPreview_->inlineExplorerButton, point))
    {
        shellLaunchWorker_.Enqueue(hwnd_, dockFolderPopupWidget_.sourceFolderPath);
        return true;
    }
    if (PtInRect(&row.home, point))
    {
        RestoreFolderPreviewHome(true);
        return true;
    }
    if (PtInRect(&row.breadcrumb, point))
    {
        for (const auto& crumb : GetFolderPreviewBreadcrumbs(panel))
            if (PtInRect(&crumb.bounds, point))
            {
                if (crumb.hiddenParents.empty()) OpenFolderPreviewPath(crumb.path);
                else if (HMENU menu = CreatePopupMenu())
                {
                    POINT screenPoint{crumb.bounds.left, crumb.bounds.bottom};
                    ClientToScreen(hwnd_, &screenPoint);
                    PrepareMenuIconsForPoint(screenPoint);
                    for (size_t i = 0; i < crumb.hiddenParents.size(); ++i)
                        AppendMenuW(menu, MF_STRING, i + 1, FolderName(crumb.hiddenParents[i]).c_str());
                    const auto originalPath = dockFolderPopupWidget_.sourceFolderPath;
                    const UINT choice = ShowModernMenu(menu, screenPoint, hwnd_);
                    DestroyMenu(menu);
                    ClearMenuIcons();
                    if (choice > 0 && choice <= crumb.hiddenParents.size() && folderPreview_ &&
                        dockFolderPopupWidget_.sourceFolderPath == originalPath)
                        OpenFolderPreviewPath(crumb.hiddenParents[choice - 1]);
                }
                return true;
            }
        return true;
    }
    if (!folderPreview_->inlineBounds || !PtInRect(&folderPreview_->inlineTitleBar, point)) return false;
    const size_t hostIndex = FindWidgetIndexById(folderPreview_->hostWidgetId);
    WidgetHit hit = WidgetHit::None;
    if (hostIndex < widgets_.size())
    {
        for (const auto& container : containers_)
            if (auto* host = dynamic_cast<WidgetContainer*>(container.get());
                host && host->GetWidgetData() == &widgets_[hostIndex])
            {
                hit = host->HitTestWidget(point);
                break;
            }
    }
    if (hit != WidgetHit::DateHeaderToggleBtn && hit != WidgetHit::ListToggleBtn) return false;
    if (hit == WidgetHit::DateHeaderToggleBtn)
        dockFolderPopupWidget_.dateHeaders = folderPreview_->inlineDateHeaders = !folderPreview_->inlineDateHeaders;
    else
        dockFolderPopupWidget_.listMode = folderPreview_->inlineListMode = !folderPreview_->inlineListMode;
    if (dockFolderPopupContainer_) dockFolderPopupContainer_->InvalidateFilterCache();
    popupScrollOffset_ = 0;
    (void)QueueDesktopWidgetComposition(folderPreview_->hostWidgetId);
    return true;
}

const snowdesktop::popup_resize_transition::Transition* DesktopApp::GetCollectionPopupResizeTransition() const
{
    if (folderPreviewPopupResize_) return folderPreviewPopupResize_.get();
    return dockFolderPopupOpen_ && dockFolderPopupContainer_
        ? dockFolderPopupContainer_->PopupResizeTransition().get() : nullptr;
}

snowdesktop::popup_resize_transition::Geometry DesktopApp::GetPresentedCollectionPopupGeometry() const
{
    const auto* widget = GetOpenPopupWidget();
    if (!widget) return {};
    const RECT panel = GetCollectionPopupRect(*widget);
    const RECT content = GetCollectionPopupContentRect(panel);
    if (const auto* motion = GetCollectionPopupResizeTransition())
        return {motion->Panel(panel), motion->Content(content, panel)};
    return {panel, content};
}

void DesktopApp::StartFolderPreviewPopupResize(snowdesktop::popup_resize_transition::Geometry before)
{
    using namespace snowdesktop::popup_resize_transition;
    const auto* widget = GetOpenPopupWidget();
    if (!widget || IsRectEmpty(&before.panel) ||
        snowdesktop::animation::RuntimePopupEffect() == snowdesktop::animation::NoEffect)
        return;
    const RECT panel = GetCollectionPopupRect(*widget);
    if (IsRectEmpty(&panel) || EqualRect(&before.panel, &panel)) return;
    const Geometry target{panel, GetCollectionPopupContentRect(panel)};
    auto transition = std::make_shared<Transition>(before, target,
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(), snowdesktop::animation::RuntimeDurationScale());
    folderPreviewPopupResize_ = transition;
    const auto token = uiAnimationScheduler_.StartAnimation(snowdesktop::UiAnimationSurface::Popup,
        [this, weak = std::weak_ptr<Transition>(transition)](double now) {
            const auto current = weak.lock();
            if (!current || folderPreviewPopupResize_ != current) return false;
            const bool keep = IsCollectionPopupInteractive() && !exitRequested_ &&
                snowdesktop::animation::RuntimePopupEffect() != snowdesktop::animation::NoEffect &&
                current->Advance(now);
            if (!keep) folderPreviewPopupResize_.reset();
            UpdateFloatingPopupWindowBounds(!keep, false);
            InvalidateFloatingPopupWindow(false);
            return keep;
        });
    if (!token) folderPreviewPopupResize_.reset();
}
