#include "app/app.h"
#include "app/dock/dock_folder_popup_read.h"
#include "app/navigation/quick_navigation_state.h"

#include "common/l10n.h"
#include "common/utils.h"
#include "layout/collection_popup_layout.h"
#include "layout/folder_preview_header_layout.h"
#include "settings/animation_settings.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "widget/runtime/widget_item_layout.h"
#include "widgets/folder_member_order.h"
#include "widgets/widget_folder_search.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
std::wstring FolderName(const std::wstring& path)
{
    const auto value = std::filesystem::path(path).lexically_normal();
    if (!value.filename().empty()) return value.filename().wstring();
    if (!value.parent_path().filename().empty()) return value.parent_path().filename().wstring();
    return path;
}

std::wstring EscapePathMenuLabel(std::wstring label)
{
    for (size_t amp = label.find(L'&'); amp != std::wstring::npos; amp = label.find(L'&', amp + 2))
        label.insert(amp, 1, L'&');
    return label;
}

std::wstring FolderPreviewViewKey(const std::wstring& hostId, const std::wstring& sourceId,
    const std::wstring& dockSourceId)
{
    if (!hostId.empty()) return L"widget:" + hostId;
    if (!sourceId.empty()) return L"widget:" + sourceId;
    return dockSourceId.empty() ? std::wstring{} : L"dock:" + dockSourceId;
}

bool UsesTemporaryFolderPreview(DesktopWidgetType type)
{
    return type == DesktopWidgetType::Collection || type == DesktopWidgetType::CollectionGroup;
}

POINT ItemCenter(RECT bounds)
{
    return {bounds.left + (bounds.right - bounds.left) / 2,
        bounds.top + (bounds.bottom - bounds.top) / 2};
}
}

std::optional<DesktopApp::FolderPreviewTarget> DesktopApp::FindFolderPreviewTarget(POINT point, bool keyboard)
{
    using namespace snowdesktop::folder_preview;
    if (dockTrayPopup_) return std::nullopt;
    const DesktopWidget* source = nullptr;
    const DesktopWidget* host = nullptr;
    FolderPreviewTarget target;
    target.inPopup = keyboard ? IsCollectionPopupKeyboardTarget() : IsPointOccludedByOpenPopup(point);
    if (keyboard && (popupFanActionFocused_ || (!dockFolderPopupOpen_ &&
        (keyboardNavSearchBox_ || keyboardNavCollectionGroupTabs_ || keyboardNavFileGroupCategoryTabs_))))
        return std::nullopt;
    const bool browsingPreview = target.inPopup && folderPreview_;
    const bool ordinaryDockFolder = target.inPopup && dockFolderPopupOpen_ &&
        !browsingPreview && dockFolderPopupMappingWidgetId_.empty();
    const auto setDesktopItem = [&](const DesktopItem& item) {
        target.path = item.parsingName;
        target.desktopKey = item.layoutKey;
        target.directoryShortcut = item.shortcutTarget.directory;
    };
    const auto setFolderEntry = [&](const FolderEntry& entry) {
        target.path = entry.fullPath;
        target.knownDirectory = entry.isDirectory;
        target.directoryShortcut = entry.shortcutTarget.directory;
    };
    const auto resolveInView = [&](ScrollingItemWidget& view) {
        const RECT viewport = view.GetContentViewportRect();
        if (!keyboard && !PtInRect(&viewport, point)) return;
        source = view.GetWidgetData();
        for (const auto& slot : view.GetSlots())
        {
            if (!slot) continue;
            const RECT bounds = slot->GetBounds();
            const Item* item = slot->GetItem();
            if (keyboard ? (!item || !item->IsSelected()) : !PtInRect(&bounds, point)) continue;
            if (!target.path.empty()) { target.path.clear(); return; }
            target.anchor = ItemCenter(bounds);
            if (const auto* icon = dynamic_cast<const DesktopIcon*>(item))
            {
                if (const auto* desktop = icon->GetDesktopItem()) setDesktopItem(*desktop);
            }
            else if (const auto* folderIcon = dynamic_cast<const FolderEntryIcon*>(item))
            {
                if (const auto* entry = folderIcon->GetFolderEntry()) setFolderEntry(*entry);
            }
            if (auto* group = dynamic_cast<FileGroup*>(&view))
            {
                auto* memberSource = group->GetSourceContainerForItem(item);
                source = memberSource ? memberSource->GetWidgetData() : nullptr;
            }
            if (!keyboard) return;
        }
    };
    if (target.inPopup)
    {
        if (!IsCollectionPopupInteractive()) return std::nullopt;
        const auto* popup = GetOpenPopupWidget();
        if (!popup) return std::nullopt;
        if (auto* group = GetGroupPopupView())
        {
            host = group->GetWidgetData();
            const RECT frame = GetCategorizedPopupFrame(GetCollectionPopupRect(*popup));
            group->SetPopupFrame(&frame);
            resolveInView(*group);
        }
        else if (dockFolderPopupOpen_ || popup->type == DesktopWidgetType::FileCategories ||
                 popup->type == DesktopWidgetType::Collection)
        {
            if (dockFolderPopupOpen_)
            {
                const size_t index = FindWidgetIndexById(
                    folderPreview_ ? folderPreview_->sourceWidgetId : dockFolderPopupMappingWidgetId_);
                if (!dockFolderPopupAvailable_ ||
                    (index >= widgets_.size() && !ordinaryDockFolder && !browsingPreview))
                    return std::nullopt;
                if (index < widgets_.size()) source = &widgets_[index];
            }
            else
                source = host = popup;
            const RECT bounds = GetCollectionPopupRect(*popup);
            const size_t count = GetPopupItemCount(*popup);
            const auto& keys = GetPopupItemKeysView(*popup);
            for (size_t i = 0; i < count; ++i)
            {
                if (!keyboard && !HitTestCollectionPopupItem(bounds, i, point)) continue;
                if (dockFolderPopupOpen_)
                {
                    const auto index = GetPopupFolderEntryIndex(*popup, i);
                    if (index >= popup->folderEntries.size()) continue;
                    const auto& entry = popup->folderEntries[index];
                    if (keyboard && !entry.selected) continue;
                    if (!target.path.empty()) return std::nullopt;
                    setFolderEntry(entry);
                }
                else
                {
                    if (i >= keys.size()) break;
                    const size_t index = FindItemIndexByKey(keys[i]);
                    if (index >= items_.size() || (keyboard && !items_[index].selected)) continue;
                    if (!target.path.empty()) return std::nullopt;
                    setDesktopItem(items_[index]);
                }
                if (!keyboard) break;
            }
        }
    }
    else
    {
        if ((GetOpenPopupWidget() && !(folderPreview_ && folderPreview_->inlineBounds)) ||
            (!keyboard && GetDockContainerAtPoint(point))) return std::nullopt;
        if (keyboard && keyboardNavInsideWidget_ && keyboardNavWidgetIndex_ >= widgets_.size())
            return std::nullopt;
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
            if (keyboard ? (!keyboardNavInsideWidget_ || data != &widgets_[keyboardNavWidgetIndex_])
                         : !PtInRect(&frame, point)) continue;
            host = data;
            if (Supports(data->type) || data->type == DesktopWidgetType::FileGroup)
            {
                target.inlineBounds = frame;
                resolveInView(*view);
            }
            else if (UsesTemporaryFolderPreview(data->type))
                resolveInView(*view);
            break;
        }
        if (!host)
        {
            const DesktopItem* item = nullptr;
            if (keyboard)
            {
                if (keyboardNavInsideWidget_ || std::any_of(widgets_.begin(), widgets_.end(),
                        [](const DesktopWidget& widget) { return widget.selected; })) return std::nullopt;
                for (const auto& candidate : items_)
                    if (candidate.selected && !IsItemInAnyWidget(candidate) &&
                        (!desktopIconsHidden_ || IsRetainedLargeIcon(candidate)))
                    {
                        if (item) return std::nullopt;
                        item = &candidate;
                    }
            }
            else if (HitTestStandaloneWidgetIndex(point) == static_cast<size_t>(-1))
            {
                if (const auto* icon = HitTestIcon(point)) item = icon->GetDesktopItem();
            }
            if (item)
            {
                setDesktopItem(*item);
                target.anchor = ItemCenter(item->largeIcon ? GetLargeIconFrameRect(*item) : item->bounds);
            }
        }
    }
    target.temporary = !browsingPreview && !ordinaryDockFolder &&
        (!host || UsesTemporaryFolderPreview(host->type));
    if (target.path.empty() || !std::filesystem::path(target.path).is_absolute() ||
        (!browsingPreview && !ordinaryDockFolder && !target.temporary &&
            (!source || !Supports(source->type))))
        return std::nullopt;

    if (source && !target.temporary) target.sourceId = source->id;
    if (host) target.hostId = host->id;
    return target;
}

void DesktopApp::AppendFolderPreviewOpenMenuItem(HMENU menu, POINT point, const std::wstring& path)
{
    const auto target = FindFolderPreviewTarget(point);
    if (!target || !PathsEqualInsensitive(target->path, path)) return;
    if (!target->knownDirectory)
    {
        const DWORD attributes = target->desktopKey.empty()
            ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
        const bool directory = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
        const bool directoryShortcut = target->directoryShortcut &&
            _wcsicmp(std::filesystem::path(path).extension().c_str(), L".lnk") == 0;
        if (!directory && !directoryShortcut) return;
    }
    if (AppendMenuW(menu, MF_STRING, kContextFolderPreviewOpen, _LW("app.folderPreview.open")))
        SetMenuItemIcon(menu, kContextFolderPreviewOpen, snowdesktop::menu_fluent_glyphs::kFolderPreview,
            MenuIconFont::FluentRegular);
}

bool DesktopApp::TryFolderPreviewDoubleClick(POINT point, WPARAM keyState, const std::wstring& menuPath)
{
    return TryFolderPreviewActivation(point, keyState, menuPath, false);
}

bool DesktopApp::TryFolderPreviewActivation(POINT point, WPARAM keyState, const std::wstring& menuPath, bool keyboard)
{
    using namespace snowdesktop::folder_preview;
    const bool fromMenu = !menuPath.empty();
    if (!fromMenu && (keyState & MK_SHIFT)) return false;
    const bool clearTriggerSelection = !fromMenu && (keyState & MK_CONTROL);
    const bool forcePreview = fromMenu || clearTriggerSelection;
    if (!forcePreview && !personalizationSettings_.folderDoubleClickPreview) return false;
    auto hit = FindFolderPreviewTarget(point, keyboard);
    if (!hit || (fromMenu && !PathsEqualInsensitive(hit->path, menuPath))) return false;
    if (keyboard && hit->desktopKey.empty() && !hit->knownDirectory &&
        _wcsicmp(std::filesystem::path(hit->path).extension().c_str(), L".lnk") != 0) return false;
    auto target = std::move(*hit);

    const auto expectedPopupPath = folderPreview_ ? dockFolderPopupWidget_.sourceFolderPath : std::wstring{};
    const auto dockFolderSourceId = dockFolderPopupOpen_ ? dockFolderPopupSourceId_ : std::wstring{};
    const auto popupGeneration = floatingPopupMouseHookGeneration_;
    const auto finish = [this, target, expectedPopupPath, forcePreview, clearTriggerSelection, fromMenu,
                            dockFolderSourceId, popupGeneration]
        (snowdesktop::item_location::FolderTarget resolved) {
        const auto& sourceId = target.sourceId;
        const auto& hostId = target.hostId;
        const auto& inlineBounds = target.inlineBounds;
        const auto& path = target.path;
        const auto& desktopKey = target.desktopKey;
        const bool inPopup = target.inPopup;
        if (exitRequested_ || dragSession_.HasContext() || dragDropController_.IsTransportActive()) return;
        const size_t index = FindWidgetIndexById(sourceId);
        if ((!sourceId.empty() && index >= widgets_.size()) ||
            (!forcePreview && !personalizationSettings_.folderDoubleClickPreview)) return;
        const bool openingInline = !inPopup && inlineBounds.has_value();
        const bool openingStandalone = !inPopup && target.temporary;
        const bool existingInline = folderPreview_ && folderPreview_->inlineBounds;
        if ((!inPopup && GetOpenPopupWidget() && !existingInline) ||
            (inPopup && (!IsCollectionPopupInteractive() || popupGeneration != floatingPopupMouseHookGeneration_)) ||
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
            if (fromMenu) return;
            const size_t item = FindItemIndexByKey(desktopKey);
            const bool accepted = !desktopKey.empty()
                ? item < items_.size() && LaunchDesktopItem(item)
                : LaunchPathWithShortcutPolicy(hwnd_, path);
            if (accepted && (!folderPreview_ || openingInline || openingStandalone))
            {
                ClearSelection();
                if (inPopup) CloseCollectionPopup();
                else InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }

        if (!desktopKey.empty() && FindItemIndexByKey(desktopKey) >= items_.size()) return;
        if ((openingInline || openingStandalone) && existingInline) SuspendInlineFolderPreview();
        bool fromFan = false;
        if (!folderPreview_)
        {
            Session session;
            session.sourceWidgetId = sourceId;
            if (!openingStandalone) session.hostWidgetId = hostId;
            if (!target.temporary) session.viewKey = FolderPreviewViewKey(hostId, sourceId, dockFolderSourceId);
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
                const auto* settings = ResolveWidgetContentViewData(&widgets_[hostIndex]);
                session.inlineListMode = settings->listMode;
                session.inlineDateHeaders = settings->dateHeaders;
            }
            if (const auto* popup = GetOpenPopupWidget())
            {
                session.homeBounds = GetCollectionPopupRect(*popup);
                fromFan = UsesCollectionPopupFan(*popup);
                if (!fromFan)
                    session.pendingPopupResize = GetPresentedCollectionPopupGeometry();
            }
            if (openingStandalone)
            {
                pendingCollectionPopupOpen_.reset();
                popupWidgetIndex_ = static_cast<size_t>(-1);
                popupHasAnchor_ = true;
                popupAnchoredToDock_ = false;
                collectionPopupDockHost_ = nullptr;
                popupAnchorPoint_ = target.anchor;
                const auto* page = GridPageFromPoint(target.anchor);
                popupPageId_ = page ? page->id : std::wstring{};
                popupCategoryId_.clear();
            }
            session.homeScroll = popupScrollOffset_;
            session.homeFanScroll = popupFanScroll_;
            session.homeFanShowAll = popupFanShowAll_;
            session.homeFanActionFocused = popupFanActionFocused_;
            for (const auto& item : items_)
                if (item.selected && (!clearTriggerSelection || item.layoutKey != desktopKey))
                    session.selectedDesktopKeys.push_back(item.layoutKey);
            const auto keepSelection = [&](const FolderEntry& entry) {
                return entry.selected && (!clearTriggerSelection || !PathsEqualInsensitive(entry.fullPath, path));
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
                if (clearTriggerSelection)
                    for (auto& entry : dockFolderPopupWidget_.folderEntries)
                        entry.selected = keepSelection(entry);
                SuspendDockFolderPreviewHome(session);
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

void DesktopApp::ShowFolderPreviewDirectory(const std::wstring& path, bool fromFan, bool present)
{
    const RECT previous = popupRect_;
    const bool keepTemporaryDisplay = folderPreview_->IsTemporary() && dockFolderPopupOpen_;
    CancelRenameClick();
    if (!dragSession_.HasContext() && !dragDropController_.IsTransportActive() &&
        !dragDropController_.IsExternalDragActive())
    {
        CancelPointerPressWithoutCaptureRelease();
        ReleaseCapturePreservingPointerState();
    }
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
    auto sort = folderPreviewSort_.Get(key);
    auto& view = dockFolderPopupWidget_;
    if (!keepTemporaryDisplay) view = {};
    view.fanPopup = false;
    view.scrollOffset = 0;
    view.type = DesktopWidgetType::FolderMapping;
    view.id = kDockFolderPopupWidgetId;
    view.gridCell.pageId = kDockPageId;
    view.sourceFolderPath = path;
    view.title = _LW("app.folderPreview.title");
    view.folderSortMode = sort.mode;
    view.folderSortAscending = sort.ascending;
    view.itemKeys = std::move(sort.order);
    view.contentSortColumn = snowdesktop::list_detail_rules::FromLegacyFolderSortMode(sort.mode);
    view.contentSortAscending = sort.ascending;
    const auto* host = GetFolderPreviewComponent();
    const auto* sourceView = host ? ResolveWidgetContentViewData(host)
        : folderPreview_->folderHome ? &folderPreview_->folderHome->data : nullptr;
    if (sourceView)
    {
        view.listMode = sourceView->listMode;
        view.listColumns = sourceView->listColumns;
        view.showDetails = sourceView->showDetails;
        view.detailShowModified = sourceView->detailShowModified;
        view.detailShowType = sourceView->detailShowType;
        view.detailShowSize = sourceView->detailShowSize;
        view.detailModifiedPosition = sourceView->detailModifiedPosition;
        view.detailTypePosition = sourceView->detailTypePosition;
        view.detailSizePosition = sourceView->detailSizePosition;
        view.dateHeaders = sourceView->dateHeaders;
    }
    if (host) view.privacyMode = host->privacyMode;
    if (folderPreview_->inlineBounds)
    {
        const size_t hostIndex = FindWidgetIndexById(folderPreview_->hostWidgetId);
        if (hostIndex < widgets_.size())
        {
            popupPageId_ = widgets_[hostIndex].gridCell.pageId;
            view.gridSpan = widgets_[hostIndex].gridSpan;
        }
        view.listMode = folderPreview_->inlineListMode;
        view.dateHeaders = folderPreview_->inlineDateHeaders;
    }
    SaveFolderPreviewView();
    SyncFolderChangeNotifications();
    RefreshDockFolderPopup(nullptr, present && !fromFan);
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
    if (present && folderPreview_->IsTemporary() &&
        FindWidgetIndexById(folderPreview_->hostWidgetId) >= widgets_.size())
    {
        CloseCollectionPopup(false);
        return;
    }
    if (present)
    {
        if (!folderPreview_->IsTemporary() && !folderPreviewViews_.Set(folderPreview_->viewKey, {}))
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, L"Could not clear folder preview state", DiagnosticLogLevel::Warning);
    }
    else SaveFolderPreviewView();
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
        ApplyFolderCutState(dockFolderPopupWidget_.folderEntries);
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
    if (folderPreview_->inlineBounds)
    {
        SuspendInlineFolderPreview();
        return;
    }
    // Regular popup close also releases the suspended view and its frame.
    RestoreFolderPreviewHome(false);
}

void DesktopApp::SaveFolderPreviewView()
{
    if (!folderPreview_ || folderPreview_->viewKey.empty() || folderPreview_->pages.empty()) return;
    if (!dockFolderPopupLoading_) folderPreview_->pages.back().scroll = popupScrollOffset_;
    if (!folderPreviewViews_.Set(folderPreview_->viewKey, {folderPreview_->sourceWidgetId,
            folderPreview_->pages.back(), folderPreview_->inlineListMode, folderPreview_->inlineDateHeaders}))
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Menus, L"Could not save folder preview state", DiagnosticLogLevel::Warning);
}

void DesktopApp::SuspendDockFolderPreviewHome(snowdesktop::folder_preview::Session& session)
{
    if (!dockFolderPopupOpen_) return;
    PreserveDockFolderPopupDragSourceForTransition();
    CancelDockFolderPopupIconLoads();
    session.folderHome.emplace(snowdesktop::folder_preview::FolderPopupHome{
        std::move(dockFolderPopupWidget_), std::move(dockFolderPopupContainer_),
        std::move(dockFolderPopupSourceId_), std::move(dockFolderPopupMappingWidgetId_),
        dockFolderPopupKnownItemCount_, dockFolderPopupAvailable_, dockFolderPopupLoading_});
}

bool DesktopApp::ResumeFolderPreviewForPopup(const std::wstring& hostId, const std::wstring& sourceId)
{
    const auto* home = GetOpenPopupWidget();
    if (folderPreview_ || !home) return false;
    const auto key = FolderPreviewViewKey(hostId, sourceId, dockFolderPopupSourceId_);
    auto saved = folderPreviewViews_.Get(key);
    if (!saved || !saved->page) return false;
    snowdesktop::folder_preview::Session session;
    session.viewKey = key;
    session.hostWidgetId = hostId;
    session.sourceWidgetId = sourceId.empty() ? std::move(saved->sourceWidgetId) : sourceId;
    session.pages.push_back(std::move(*saved->page));
    session.homeBounds = GetCollectionPopupRect(*home);
    session.homeScroll = popupScrollOffset_;
    session.homeFanScroll = popupFanScroll_;
    session.homeFanShowAll = popupFanShowAll_;
    session.homeFanActionFocused = popupFanActionFocused_;
    SuspendDockFolderPreviewHome(session);
    folderPreview_.emplace(std::move(session));
    ShowFolderPreviewDirectory(folderPreview_->pages.back().path, false, false);
    return true;
}

void DesktopApp::ResumeInlineFolderPreview()
{
    std::erase_if(inactiveInlineFolderPreviews_, [this](const auto& preview) {
        const size_t index = FindWidgetIndexById(preview.first);
        if (index < widgets_.size() && !IsGroupedWidget(widgets_[index]) &&
            !snowdesktop::single_cell_widget::IsSingleCell(widgets_[index]) && !IsRectEmptyRect(widgets_[index].bounds))
            return false;
        for (const auto& entry : preview.second.model.data.folderEntries) EraseD2DIconCacheForBitmap(entry.iconBitmap);
        return true;
    });
    if (exitRequested_ || folderPreview_ || GetOpenPopupWidget() || pendingCollectionPopupOpen_ ||
        dragSession_.HasContext() || dragDropController_.IsTransportActive() || !hwnd_)
        return;
    for (const auto& container : containers_)
    {
        const auto* host = dynamic_cast<const ScrollingItemWidget*>(container.get());
        const auto* data = host ? host->GetWidgetData() : nullptr;
        if (!data || inactiveInlineFolderPreviews_.contains(data->id) || IsGroupedWidget(*data) ||
            host->IsSingleCellPresentation() || IsRectEmptyRect(data->bounds) ||
            (!snowdesktop::folder_preview::Supports(data->type) && data->type != DesktopWidgetType::FileGroup)) continue;
        const auto key = FolderPreviewViewKey(data->id, {}, {});
        auto saved = folderPreviewViews_.Get(key);
        if (!saved || !saved->page) continue;
        snowdesktop::folder_preview::Session session;
        session.viewKey = key;
        session.hostWidgetId = data->id;
        session.sourceWidgetId = std::move(saved->sourceWidgetId);
        if (FindWidgetIndexById(session.sourceWidgetId) >= widgets_.size())
        {
            if (!snowdesktop::folder_preview::Supports(data->type)) continue;
            session.sourceWidgetId = data->id;
        }
        session.pages.push_back(std::move(*saved->page));
        session.inlineListMode = saved->listMode;
        session.inlineDateHeaders = saved->dateHeaders;
        session.keyboardFocused = false;
        session.SetInlineGeometry(*host);
        folderPreview_.emplace(std::move(session));
        ShowFolderPreviewDirectory(folderPreview_->pages.back().path, false, false);
        SuspendInlineFolderPreview();
    }
}

void DesktopApp::SwapInlineFolderPreview(snowdesktop::folder_preview::InlinePreview& view)
{
    using std::swap;
    swap(view.session, folderPreview_);
    swap(view.model.data, dockFolderPopupWidget_);
    swap(view.model.view, dockFolderPopupContainer_);
    swap(view.model.dockSourceId, dockFolderPopupSourceId_);
    swap(view.model.mappingId, dockFolderPopupMappingWidgetId_);
    swap(view.model.knownCount, dockFolderPopupKnownItemCount_);
    swap(view.model.available, dockFolderPopupAvailable_);
    swap(view.model.loading, dockFolderPopupLoading_);
    swap(view.pageId, popupPageId_);
    swap(view.bounds, popupRect_);
    swap(view.scroll, popupScrollOffset_);
    swap(view.open, dockFolderPopupOpen_);
}

DesktopApp::InlineFolderPreviewProjection::InlineFolderPreviewProjection(DesktopApp* app, const std::wstring& hostId)
    : app_(app)
{
    if (app_ && app_->folderPreview_ && app_->folderPreview_->inlineBounds && app_->folderPreview_->hostWidgetId == hostId)
        return;
    if (app_)
        if (auto found = app_->inactiveInlineFolderPreviews_.find(hostId); found != app_->inactiveInlineFolderPreviews_.end())
        {
            view_ = &found->second;
            app_->SwapInlineFolderPreview(*view_);
        }
}

DesktopApp::InlineFolderPreviewProjection::~InlineFolderPreviewProjection()
{
    if (view_) app_->SwapInlineFolderPreview(*view_);
}

void DesktopApp::SuspendInlineFolderPreview()
{
    if (!folderPreview_ || !folderPreview_->inlineBounds) return;
    SaveFolderPreviewView();
    folderPreview_->keyboardFocused = false;
    folderReadWork_.Cancel(snowdesktop::folder_preview::kResolveWorkKey);
    PreserveDockFolderPopupDragSourceForTransition();
    CancelDockFolderPopupIconLoads();
    ClearPopupMouseDownItem();
    ClearPopupDragTarget();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    for (auto& entry : dockFolderPopupWidget_.folderEntries) entry.selected = false;
    auto& saved = inactiveInlineFolderPreviews_[folderPreview_->hostWidgetId];
    SwapInlineFolderPreview(saved);
    popupAnimation_.ResetHidden();
}

const DesktopWidget* DesktopApp::FindInactiveInlineFolderPreviewHostAtPoint(POINT point) const
{
    if (inactiveInlineFolderPreviews_.empty() || GetDockContainerAtPoint(point)) return nullptr;
    for (auto it = widgets_.rbegin(); it != widgets_.rend(); ++it)
    {
        if (IsGroupedWidget(*it) || (desktopIconsHidden_ && !it->keepWhenDesktopHidden)) continue;
        const RECT frame = GetStandaloneWidgetFrameRect(*it);
        if (PtInRect(&frame, point))
            return inactiveInlineFolderPreviews_.contains(it->id) ? &*it : nullptr;
    }
    return nullptr;
}

void DesktopApp::ActivateInlineFolderPreviewAtPoint(POINT point, bool duringDrag)
{
    if (inactiveInlineFolderPreviews_.empty() || quickNavigation_->open ||
        (GetOpenPopupWidget() && !(folderPreview_ && folderPreview_->inlineBounds))) return;
    if (!duringDrag && (dragSession_.HasContext() || dragDropController_.IsTransportActive())) return;
    const auto* target = FindInactiveInlineFolderPreviewHostAtPoint(point);
    if (!target) return;
    if (folderPreview_) SuspendInlineFolderPreview();
    // Inserting the previously active preview can rehash the map.
    const auto ready = inactiveInlineFolderPreviews_.find(target->id);
    SwapInlineFolderPreview(ready->second);
    inactiveInlineFolderPreviews_.erase(ready);
    ApplyFolderCutState(dockFolderPopupWidget_.folderEntries);
    folderPreview_->keyboardFocused = !duringDrag;
    for (const auto& container : containers_)
        if (const auto* host = dynamic_cast<const WidgetContainer*>(container.get());
            host && host->GetWidgetData() == target)
        {
            folderPreview_->SetInlineGeometry(*host);
            break;
        }
    popupAnimation_.ShowImmediately();
    RefreshDockFolderPopupGeometry(false);
    QueueFolderRead(dockFolderPopupWidget_.sourceFolderPath);
}

void DesktopApp::SyncFolderPreviewSortToOtherViews()
{
    const auto key = snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath);
    const auto sort = folderPreviewSort_.Get(key);
    for (auto& [host, preview] : inactiveInlineFolderPreviews_)
    {
        auto& model = preview.model;
        auto& data = model.data;
        if (snowdesktop::shell_refresh::FolderKey(data.sourceFolderPath) != key) continue;
        data.folderSortMode = sort.mode;
        data.folderSortAscending = sort.ascending;
        data.contentSortColumn = snowdesktop::list_detail_rules::FromLegacyFolderSortMode(sort.mode);
        data.contentSortAscending = sort.ascending;
        data.itemKeys = sort.order;
        if (model.available) snowdesktop::folder_member_order::Refresh(data, ToUpperInvariant);
        if (model.view) model.view->InvalidateFilterCache();
        (void)QueueDesktopWidgetComposition(host);
    }
}

void DesktopApp::RefreshInactiveInlineFolderPreviews(const snowdesktop::shell_refresh::Snapshot& snapshot)
{
    for (auto& [host, preview] : inactiveInlineFolderPreviews_)
    {
        auto& model = preview.model;
        const auto folder = snapshot.folders.find(snowdesktop::shell_refresh::FolderKey(model.data.sourceFolderPath));
        if (folder == snapshot.folders.end()) continue;
        if (!snowdesktop::dock_folder_popup_read::Refresh(model.data.sourceFolderPath, &folder->second,
            model.available, model.loading, [](const auto&) {}, [this, &model](const auto& listing) {
                EnumerateFolderMappingEntries(model.data, false, &listing);
            }, false)) continue;
        if (!model.available)
        {
            for (const auto& entry : model.data.folderEntries) EraseD2DIconCacheForBitmap(entry.iconBitmap);
            model.data.folderEntries.clear();
        }
        model.knownCount = model.data.folderEntries.size();
        if (model.view) model.view->InvalidateFilterCache();
        (void)QueueDesktopWidgetComposition(host);
    }
}

const DesktopWidget* DesktopApp::GetInlineFolderPreviewData(const DesktopWidget& host) const
{
    return folderPreview_ && folderPreview_->inlineBounds && folderPreview_->hostWidgetId == host.id
        ? &dockFolderPopupWidget_ : nullptr;
}

DesktopWidget* DesktopApp::GetFolderPreviewComponent()
{
    if (!folderPreview_ || folderPreview_->IsTemporary()) return nullptr;
    const auto& id = folderPreview_->hostWidgetId.empty()
        ? folderPreview_->sourceWidgetId : folderPreview_->hostWidgetId;
    const size_t index = FindWidgetIndexById(id);
    return index < widgets_.size() ? &widgets_[index] : nullptr;
}

snowdesktop::folder_preview::NavigationRow DesktopApp::GetFolderPreviewNavigationRow(const RECT& panel) const
{
    namespace layout = snowdesktop::collection_popup_layout;
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    RECT row = layout::ResolveBodyRect(panel, metrics);
    row.top = std::min<LONG>(panel.bottom, panel.top + metrics.paddingX);
    float scale = metrics.scale;
    int height = metrics.controlRowHeight;
    if (folderPreview_->inlineBounds)
    {
        scale = folderPreview_->inlineScale;
        row = panel;
        const LONG inset = ScaleWidgetCu(10.0f, scale) + ScaleWidgetCu(2.0f, scale);
        row.left = std::min(panel.left + inset, (panel.left + panel.right) / 2);
        row.right = std::max(row.left, panel.right - inset);
        for (const auto& container : containers_)
            if (const auto* host = dynamic_cast<const ScrollingItemWidget*>(container.get()); host &&
                host->GetWidgetData() && host->GetWidgetData()->id == folderPreview_->hostWidgetId)
            {
                const RECT search = host->GetCategorizedSearchBoxRect(true);
                if (!IsRectEmpty(&search))
                {
                    row.left = search.left;
                    row.right = search.right;
                }
                break;
            }
        const bool bottomTitle = folderPreview_->inlineRowTop == panel.top;
        const int topGap = bottomTitle ? ScaleWidgetCu(12.0f, scale) : ScaleWidgetCu(4.0f, scale);
        row.top = std::clamp(folderPreview_->inlineRowTop + topGap, panel.top, panel.bottom);
        height = layout::ResolveControlRowHeight(GetCategorizedWidgetTabHeight(), scale);
    }
    row.bottom = std::min<LONG>(row.bottom, row.top + height);
    return snowdesktop::folder_preview::ResolveNavigationRow(row, scale);
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

UINT DesktopApp::TrackFolderPreviewPathMenu(HMENU menu, POINT screenPoint, bool textOnly)
{
    const auto* owner = GetFolderPreviewComponent();
    const std::wstring hostId = owner ? owner->id : std::wstring{};
    const auto previousPin = interactionPinnedWidgetId_;
    if (!hostId.empty()) interactionPinnedWidgetId_ = hostId;
    const UINT choice = ShowModernMenu(menu, screenPoint, hwnd_, false, false, nullptr,
        {}, {}, {}, nullptr, {}, false, 0, 0, {}, textOnly);
    if (!hostId.empty() && interactionPinnedWidgetId_ == hostId)
    {
        interactionPinnedWidgetId_ = previousPin;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    DestroyMenu(menu);
    ClearMenuIcons();
    return choice;
}

bool DesktopApp::HandleFolderPreviewBreadcrumbContextMenu(POINT point, bool show)
{
    if (!folderPreview_ || !IsCollectionPopupInteractive() || !IsPointOccludedByOpenPopup(point)) return false;
    const RECT panel = GetPresentedCollectionPopupGeometry().panel;
    const auto row = GetFolderPreviewNavigationRow(panel);
    if (!PtInRect(&row.breadcrumb, point)) return false;
    const auto crumbs = GetFolderPreviewBreadcrumbs(panel);
    const auto crumb = std::find_if(crumbs.begin(), crumbs.end(), [&](const auto& candidate) {
        return PtInRect(&candidate.bounds, point) != FALSE;
    });
    if (crumb == crumbs.end()) return false;
    if (!show) return true;
    const bool overflow = !crumb->hiddenParents.empty();
    const auto paths = overflow ? crumb->hiddenParents : std::vector{crumb->path};
    HMENU menu = CreatePopupMenu();
    if (!menu) return true;
    POINT screenPoint = point;
    ClientToScreen(hwnd_, &screenPoint);
    PrepareMenuIconsForPoint(screenPoint);

    namespace glyphs = snowdesktop::menu_fluent_glyphs;
    enum class Action { Copy, Shortcut, Mapping, Open };
    struct ActionItem { const char* label; const wchar_t* glyph; };
    constexpr std::array actions{
        ActionItem{"app.folderPreview.copyAddress", glyphs::kCopy},
        ActionItem{"app.nav.send_to_desktop", glyphs::kLinkAdd},
        ActionItem{"app.folderPreview.createDesktopMapping", glyphs::kFolderMapping},
        ActionItem{"app.interact.open_in_explorer", glyphs::kFolderOpen},
    };
    for (size_t index = 0; index < paths.size(); ++index)
    {
        HMENU current = overflow ? CreatePopupMenu() : menu;
        if (!current) continue;
        if (overflow)
        {
            const UINT_PTR submenu = reinterpret_cast<UINT_PTR>(current);
            if (!AppendMenuW(menu, MF_POPUP, submenu, EscapePathMenuLabel(FolderName(paths[index])).c_str()))
            {
                DestroyMenu(current);
                continue;
            }
            SetMenuItemIcon(menu, submenu, glyphs::kDesktopFiles, MenuIconFont::FluentRegular);
        }
        for (size_t action = 0; action < actions.size(); ++action)
        {
            const UINT command = static_cast<UINT>(index * actions.size() + action + 1);
            if (AppendMenuW(current, MF_STRING, command, _LW(actions[action].label)))
                SetMenuItemIcon(current, command, actions[action].glyph, MenuIconFont::FluentRegular);
        }
    }
    const UINT choice = TrackFolderPreviewPathMenu(menu, screenPoint, false);
    if (!choice || exitRequested_) return true;
    const auto index = (choice - 1) / actions.size();
    if (index >= paths.size()) return true;
    const auto& path = paths[index];
    switch (static_cast<Action>((choice - 1) % actions.size()))
    {
    case Action::Copy:
        CopyPathsToClipboard({path});
        break;
    case Action::Shortcut:
        if (!CreateDesktopShortcutForPath(path, true, FolderName(path))) MessageBeep(MB_ICONWARNING);
        break;
    case Action::Mapping:
    {
        const bool queued = shellVisualWork_.Submit(L"folder-preview:map:" + snowdesktop::shell_refresh::FolderKey(path),
            [path] { return snowdesktop::item_location::ResolveFolderTarget(path); },
            [this, screenPoint](snowdesktop::item_location::FolderTarget target) {
                if (exitRequested_ || !hwnd_) return;
                if (!target) { MessageBeep(MB_ICONWARNING); return; }
                DesktopWidget widget;
                widget.id = MakeNewWidgetId();
                const auto id = widget.id;
                widget.type = DesktopWidgetType::FolderMapping;
                widget.title = FolderName(target.path);
                widget.showTitle = true;
                widget.sourceFolderPath = std::move(target.path);
                lastContextMenuScreenPoint_ = screenPoint;
                AddWidgetToGrid(std::move(widget), {3, 3});
                const auto created = FindWidgetIndexById(id);
                if (created >= widgets_.size()) { MessageBeep(MB_ICONWARNING); return; }
                EnumerateFolderMappingEntries(widgets_[created]);
                ClearWidgetAddedHint();
                ShowWidgetAddedHint();
            }, hwnd_, kBackgroundShellReadyMessage);
        if (!queued) MessageBeep(MB_ICONWARNING);
        break;
    }
    case Action::Open:
        shellLaunchWorker_.Enqueue(hwnd_, path);
        break;
    }
    return true;
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
    if (PtInRect(&row.home, point) ||
        (folderPreview_->inlineBounds && PtInRect(&folderPreview_->inlineTitle, point)))
    {
        RestoreFolderPreviewHome(true);
        return true;
    }
    if (PtInRect(&row.breadcrumb, point))
    {
        const auto crumbs = GetFolderPreviewBreadcrumbs(panel);
        for (size_t i = 0; i < crumbs.size(); ++i)
        {
            const auto& crumb = crumbs[i];
            if (PtInRect(&crumb.bounds, point))
            {
                if (i != 0 && crumb.hiddenParents.empty()) OpenFolderPreviewPath(crumb.path);
                else if (HMENU menu = CreatePopupMenu())
                {
                    auto destinations = crumb.hiddenParents;
                    if (i == 0)
                    {
                        const DWORD drives = GetLogicalDrives();
                        for (unsigned index = 0; index < 26; ++index)
                            if (drives & (1UL << index))
                                destinations.push_back({static_cast<wchar_t>(L'A' + index), L':', L'\\'});
                        if (std::none_of(destinations.begin(), destinations.end(), [&](const auto& drive) {
                            return PathsEqualInsensitive(drive, crumb.path);
                        })) destinations.insert(destinations.begin(), crumb.path);
                    }
                    POINT screenPoint{crumb.bounds.left, crumb.bounds.bottom};
                    ClientToScreen(hwnd_, &screenPoint);
                    PrepareMenuIconsForPoint(screenPoint);
                    for (size_t n = 0; n < destinations.size(); ++n)
                    {
                        const auto& path = destinations[n];
                        AppendMenuW(menu, MF_STRING, n + 1, EscapePathMenuLabel(i == 0 ? path : FolderName(path)).c_str());
                    }
                    const auto originalPath = dockFolderPopupWidget_.sourceFolderPath;
                    const auto viewKey = folderPreview_->viewKey;
                    const UINT choice = TrackFolderPreviewPathMenu(menu, screenPoint, true);
                    if (choice > 0 && choice <= destinations.size() && folderPreview_ && folderPreview_->viewKey == viewKey &&
                        dockFolderPopupWidget_.sourceFolderPath == originalPath)
                        OpenFolderPreviewPath(destinations[choice - 1]);
                }
                return true;
            }
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
    if (hit == WidgetHit::DateHeaderToggleBtn)
        return ExecuteDockFolderPopupDisplayCommand(kContextWidgetToggleDateGroup);
    if (hit == WidgetHit::ListToggleBtn)
        return ExecuteDockFolderPopupDisplayCommand(kContextWidgetToggleListMode);
    return false;
}

const snowdesktop::popup_resize_transition::Transition* DesktopApp::GetCollectionPopupResizeTransition() const
{
    if (folderPreview_ && folderPreview_->inlineBounds) return nullptr;
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

float DesktopApp::GetFolderPreviewInsertionPad(const RECT& popup, size_t index, bool after) const
{
    const size_t count = GetPopupItemCount(dockFolderPopupWidget_);
    if (index >= count || (after ? index + 1 == count : index == 0)) return 0.0f;
    const size_t first = after ? index : index - 1;
    const bool verticalBar = !UsesCollectionPopupList(dockFolderPopupWidget_) ||
        GetCollectionPopupColumnCount(popup) > 1;
    return snowdesktop::widget_item_layout::InsertionBoundaryPad(
        GetCollectionPopupItemRect(popup, first),
        GetCollectionPopupItemRect(popup, first + 1), verticalBar);
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
