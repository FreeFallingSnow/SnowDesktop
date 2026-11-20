#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "common/utils.h"
#include "diagnostics/crashlog.h"
#include "widget/runtime/widget_item_layout.h"
#include "app/layout/grid_geometry.h"
#include "app/navigation/quick_navigation_state.h"
#include <algorithm>
#include <cstdlib>
#include <tuple>

// Keyboard drag hints, Shell verbs and desktop-grid navigation.

void DesktopApp::RefreshDragHintFromKeyboard()
{
    if (widgetAction_ == WidgetAction::Move)
    {
        OnMouseMoveAt(0, lastMousePoint_);
        PresentPointerInteractionFrame();
        return;
    }
    if (!dragSession_.IsActive() &&
        !dragDropController_.IsTransportActive()) return;

    int mods = 0;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
    if (snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(
            mods,
            dragDropController_.IsExternalDragActive()
                ? DropAction::Copy : DropAction::Move);

    if (GetWidgetPairDragSource().widgetIndex < widgets_.size() &&
        !dragDropController_.IsTransportActive())
    {
        RefreshDragTargetAt(dragSession_.CurrentPoint(), mods);
        PresentPointerInteractionFrame();
        return;
    }

    std::wstring hint = GetDockDragOutRemovalHint(dragSession_.CurrentPoint());
    if (hint.empty()) hint = GetCurrentDragTargetHint(mods);

    if (dragDropController_.IsTransportActive())
    {
        POINT screenPoint = dragSession_.CurrentPoint();
        ClientToScreen(hwnd_, &screenPoint);
        ShowDragHintWindowScreen(screenPoint, hint);
        OnPaint();
        InvalidateFloatingDockWindow(true);
    }
    else
    {
        ShowDragHintWindow(dragSession_.CurrentPoint(), hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

/**
 * @brief 对选中的桌面项调用 Shell 动词（如 "copy"）
 * @param verb Shell 动词字符串
 */
void DesktopApp::InvokeSelectedShellVerb(const char* verb)
{
    std::vector<PCUITEMID_CHILD> pidls;
    for (const auto& item : items_)
    {
        if (item.selected && item.childPidl.value)
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.value));
    }
    if (pidls.empty()) return;

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IContextMenu, nullptr,
        reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(hr) || !contextMenu) return;

    CMINVOKECOMMANDINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = ShellDialogOwnerHwnd();
    info.lpVerb = verb;
    info.nShow = SW_SHOWNORMAL;
    if (SafeInvokeCommand(contextMenu.Get(), &info))
        ReloadItems();
}

namespace
{
enum class DesktopDirectionSurface { File, Widget, Dock, StatusBar };
struct DesktopDirectionTarget
{
    DesktopDirectionSurface surface;
    size_t index;
    RECT bounds; // Screen coordinates across all monitors.
    bool focused;
};

using DirectionRank = std::tuple<bool, long long, long long, long long>;

std::optional<DirectionRank> RankInDirection(RECT from, RECT to, WPARAM key)
{
    const bool horizontal = key == VK_LEFT || key == VK_RIGHT;
    const bool forward = key == VK_RIGHT || key == VK_DOWN;
    if (!horizontal && key != VK_UP && key != VK_DOWN)
        return std::nullopt;

    if (!horizontal)
    {
        from = {from.top, from.left, from.bottom, from.right};
        to = {to.top, to.left, to.bottom, to.right};
    }
    const long long primary = forward
        ? static_cast<long long>(to.left) + to.right - from.left - from.right
        : static_cast<long long>(from.left) + from.right - to.left - to.right;
    if (primary <= 0) return std::nullopt;

    const bool projects = from.top < to.bottom && to.top < from.bottom;
    const long long secondary = std::abs(static_cast<long long>(to.top) + to.bottom - from.top - from.bottom);
    return DirectionRank{!projects, projects ? primary : primary + 2 * secondary, primary, secondary};
}

bool IsDesktopGridTarget(const DesktopDirectionTarget& target)
{
    return target.surface == DesktopDirectionSurface::File ||
        target.surface == DesktopDirectionSurface::Widget;
}
}

void DesktopApp::NavigateDesktopGrid(WPARAM arrowKey)
{
    if (desktopPassthroughActive_) return;
    const bool horizontal = arrowKey == VK_LEFT || arrowKey == VK_RIGHT;
    const auto screenBounds = [this](RECT bounds) {
        OffsetRect(&bounds, virtualLeft_, virtualTop_);
        return bounds;
    };
    const auto fileAvailable = [this](const DesktopItem& item) {
        return !item.name.empty() && (!desktopIconsHidden_ || IsRetainedLargeIcon(item)) &&
            !IsDockExclusiveItemKey(item.layoutKey) &&
            !collectedKeysCache_.contains(ToUpperInvariant(item.layoutKey)) &&
            !IsPendingMovedFile(item.parsingName, item.layoutKey);
    };
    const auto widgetAvailable = [this](const DesktopWidget& widget) {
        return !IsGroupedWidget(widget) && !IsDockExclusiveWidgetId(widget.id) &&
            (!desktopIconsHidden_ || widget.keepWhenDesktopHidden);
    };
    const auto gridTargets = [&] {
        std::vector<DesktopDirectionTarget> targets;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            const auto& item = items_[i];
            if (!fileAvailable(item) || IsRectEmpty(&item.bounds) ||
                (!gridPages_.empty() && !FindGridPage(gridPages_, item.gridCell.pageId))) continue;
            const RECT bounds = item.largeIcon ? GetLargeIconFrameRect(item) : item.bounds;
            targets.push_back({DesktopDirectionSurface::File, i, screenBounds(bounds), item.selected});
        }
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            const auto& widget = widgets_[i];
            if (!widgetAvailable(widget) || IsRectEmpty(&widget.bounds) ||
                (!gridPages_.empty() && !FindGridPage(gridPages_, widget.gridCell.pageId))) continue;
            const RECT bounds = GetStandaloneWidgetFrameRect(widget);
            if (!IsRectEmpty(&bounds))
                targets.push_back({DesktopDirectionSurface::Widget, i, screenBounds(bounds), widget.selected});
        }
        return targets;
    };

    auto targets = gridTargets();
    struct DockTarget { DockContainer* dock; DockContainer::KeyboardTarget target; };
    std::vector<DockTarget> dockTargets;
    const bool verticalDock = dockSettings_.position == DockPosition::Left ||
        dockSettings_.position == DockPosition::Right;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        for (auto& target : dock->KeyboardTargets(horizontal != verticalDock))
        {
            targets.push_back({DesktopDirectionSurface::Dock, dockTargets.size(),
                screenBounds(target.bounds), target.focused});
            dockTargets.push_back({dock, std::move(target)});
        }
    }

    const auto barTargets = statusBar_
        ? statusBar_->KeyboardTargets(horizontal) : std::vector<snowdesktop::StatusBarKeyboardTarget>{};
    for (size_t i = 0; i < barTargets.size(); ++i)
        targets.push_back({DesktopDirectionSurface::StatusBar, i, barTargets[i].bounds, barTargets[i].focused});

    const auto focus = [&](const DesktopDirectionTarget& target) {
        ClearSelection();
        switch (target.surface)
        {
        case DesktopDirectionSurface::File:
            items_[target.index].selected = true;
            break;
        case DesktopDirectionSurface::Widget:
            widgets_[target.index].selected = true;
            keyboardNavVisualFocus_ = true;
            break;
        case DesktopDirectionSurface::Dock:
        {
            const auto& destination = dockTargets[target.index];
            destination.dock->SetKeyboardFocus(destination.target);
            if (auto* host = FindPersistentDockHost(destination.dock))
                InvalidateFloatingDockWindow(*host);
            break;
        }
        case DesktopDirectionSurface::StatusBar:
            if (statusBar_) statusBar_->FocusFromDesktop(barTargets[target.index]);
            break;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    const auto nextPage = [&](const std::wstring& sourcePage, RECT origin) {
        if (!horizontal || sourcePage.empty() || sourcePage != lastMonitorPageId_ || gridPages_.empty())
            return false;
        const int delta = arrowKey == VK_RIGHT ? 1 : -1;
        int offset = pageOffset_;
        for (;;)
        {
            const int adjacent = NextNonEmptyOffset(offset, delta);
            if (adjacent == offset) return false;
            offset = adjacent;
            const size_t pageIndex = gridPages_.size() - 1 + static_cast<size_t>(offset);
            if (pageIndex >= savedPageIds_.size()) return false;
            const auto& page = savedPageIds_[pageIndex];
            const bool hasFile = std::any_of(items_.begin(), items_.end(), [&](const auto& item) {
                return item.gridCell.pageId == page && fileAvailable(item);
            });
            const bool hasWidget = std::any_of(widgets_.begin(), widgets_.end(), [&](const auto& widget) {
                return widget.gridCell.pageId == page && widgetAvailable(widget);
            });
            if (hasFile || hasWidget) break;
        }

        ClearSelection();
        JumpToPageOffset(offset);
        const auto* page = FindGridPage(gridPages_, lastMonitorPageId_);
        if (!page) return true;
        const LONG edge = arrowKey == VK_RIGHT ? page->workArea.left : page->workArea.right;
        origin.left = edge - 1;
        origin.right = edge + 1;
        const auto candidates = gridTargets();
        const DesktopDirectionTarget* best = nullptr;
        std::optional<DirectionRank> bestRank;
        for (const auto& candidate : candidates)
        {
            const auto& candidatePage = candidate.surface == DesktopDirectionSurface::File
                ? items_[candidate.index].gridCell.pageId : widgets_[candidate.index].gridCell.pageId;
            if (candidatePage != page->id) continue;
            const auto rank = RankInDirection(origin, candidate.bounds, arrowKey);
            if (rank && (!bestRank || *rank < *bestRank))
            {
                best = &candidate;
                bestRank = rank;
            }
        }
        if (best) focus(*best);
        return true;
    };

    const auto current = std::find_if(targets.begin(), targets.end(), [](const auto& target) {
        return target.focused;
    });
    if (current == targets.end())
    {
        POINT cursor{};
        snowdesktop::private_pointer::CursorPosition(&cursor);
        const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        const auto initialRank = [&](const DesktopDirectionTarget& target) {
            const bool otherMonitor = MonitorFromRect(&target.bounds, MONITOR_DEFAULTTONEAREST) != monitor;
            return std::tuple{otherMonitor, !IsDesktopGridTarget(target), target.bounds.left, target.bounds.top};
        };
        const auto first = std::min_element(targets.begin(), targets.end(), [&](const auto& a, const auto& b) {
            return initialRank(a) < initialRank(b);
        });
        if (first == targets.end() || std::get<0>(initialRank(*first)) || !IsDesktopGridTarget(*first))
        {
            const auto* flipPage = FindGridPage(gridPages_, lastMonitorPageId_);
            if (flipPage && MonitorFromRect(&flipPage->bounds, MONITOR_DEFAULTTONEAREST) == monitor &&
                nextPage(flipPage->id, RECT{cursor.x, cursor.y, cursor.x + 1, cursor.y + 1}))
                return;
        }
        if (first != targets.end()) focus(*first);
        return;
    }

    const DesktopDirectionTarget* best = nullptr;
    std::optional<DirectionRank> bestRank;
    for (const auto& candidate : targets)
    {
        if (&candidate == &*current) continue;
        const auto rank = RankInDirection(current->bounds, candidate.bounds, arrowKey);
        if (rank && (!bestRank || *rank < *bestRank))
        {
            best = &candidate;
            bestRank = rank;
        }
    }
    if ((!bestRank || std::get<0>(*bestRank)) && IsDesktopGridTarget(*current))
    {
        const auto& sourcePage = current->surface == DesktopDirectionSurface::File
            ? items_[current->index].gridCell.pageId : widgets_[current->index].gridCell.pageId;
        if (nextPage(sourcePage, current->bounds)) return;
    }
    if (best) focus(*best);
}

bool DesktopApp::ClearDesktopSurfaceKeyboardFocus()
{
    bool changed = statusBar_ && statusBar_->ClearDesktopKeyboardFocus();
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock || !dock->ClearKeyboardFocus()) continue;
        if (auto* host = FindPersistentDockHost(dock)) InvalidateFloatingDockWindow(*host);
        InvalidateRect(hwnd_, nullptr, FALSE);
        changed = true;
    }
    return changed;
}

bool DesktopApp::ActivateDesktopSurfaceKeyboardFocus(bool repeated)
{
    if (statusBar_ && statusBar_->InvokeDesktopKeyboardFocus(repeated)) return true;
    for (const auto& container : containers_)
    {
        auto* dock = dynamic_cast<DockContainer*>(container.get());
        if (!dock) continue;
        const auto focused = dock->FocusedKeyboardTarget();
        if (!focused) continue;
        if (repeated) return true;

        RECT anchor = dock->GetElementVisualRect(focused->bounds, lastMousePoint_);
        const POINT point{(anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2};
        OffsetRect(&anchor, virtualLeft_, virtualTop_);
        const auto restore = [this, monitor = dock->GetMonitor(), key = focused->key] {
            if (exitRequested_ || desktopPassthroughActive_) return;
            for (const auto& candidate : containers_)
            {
                auto* source = dynamic_cast<DockContainer*>(candidate.get());
                if (!source || source->GetMonitor() != monitor) continue;
                const auto targets = source->KeyboardTargets();
                const auto match = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
                    return target.key == key;
                });
                if (match == targets.end()) continue;
                ClearSelection();
                source->SetKeyboardFocus(*match);
                if (auto* host = FindPersistentDockHost(source)) InvalidateFloatingDockWindow(*host);
                InvalidateRect(hwnd_, nullptr, FALSE);
                RestoreInteractionInputFocus();
                return;
            }
        };
        if (auto* host = FindPersistentDockHost(dock)) SelectPersistentDockHost(host);
        using Kind = DockContainer::KeyboardTarget::Kind;
        switch (focused->kind)
        {
        case Kind::Start:
            ToggleWindowsStartMenu(dock);
            break;
        case Kind::Search:
            OpenQuickNavigation(QuickNavigationInvocationSource::DockSearch,
                POINT{point.x + virtualLeft_, point.y + virtualTop_});
            if (quickNavigation_->open) quickNavigation_->keyboardFocusReturn = restore;
            break;
        case Kind::Running:
            if (focused->index < dockUnpinnedRunningApps_.size())
                ActivateOrToggleDockWindow(dockUnpinnedRunningApps_[focused->index].window,
                    std::nullopt, nullptr, anchor);
            break;
        case Kind::Frequent:
            ActivateOrToggleDockItem(focused->index, std::nullopt, nullptr, anchor);
            break;
        case Kind::Entry:
            if (focused->index < dockEntries_.size())
            {
                const auto& entry = dockEntries_[focused->index];
                if (IsLogicalDockEntryType(entry.type))
                {
                    const size_t index = FindWidgetIndexById(entry.reference);
                    if (index < widgets_.size()) OpenCollectionPopupAt(index, point);
                }
                else if (IsFolderDockEntry(entry))
                    OpenDockFolderPopupAt(focused->index, point);
                else
                {
                    const size_t index = FindItemIndexByKey(entry.reference);
                    if (index < items_.size()) ActivateOrToggleDockItem(index, std::nullopt, nullptr, anchor);
                }
            }
            break;
        }
        if (IsCollectionPopupInteractive()) collectionPopupKeyboardReturn_ = restore;
        return true;
    }
    return false;
}

/**
 * @brief 滚动组件以确保指定索引的成员项可见
 * @param widgetIndex 组件索引
 * @param memberIndex 成员索引
 *
 * 计算目标成员在组件内容区域中的近似纵向位置，
 * 如果超出当前滚动视口则调整 scrollOffset。
 */

void DesktopApp::ScrollWidgetToMember(size_t widgetIndex, int memberIndex)
{
    if (widgetIndex >= widgets_.size() || memberIndex < 0) return;
    auto& widget = widgets_[widgetIndex];

    WidgetContainer* wc = nullptr;
    for (auto& c : containers_)
    {
        auto* w = dynamic_cast<WidgetContainer*>(c.get());
        if (w && w->GetWidgetData() == &widget) { wc = w; break; }
    }
    if (!wc) return;

    int maxScroll = wc->GetMaxScrollOffset();
    if (maxScroll <= 0) return;

    auto applyScroll = [&](int scroll) {
        scroll = std::clamp(scroll, 0, maxScroll);
        if (scroll == wc->GetScrollOffset()) return;
        auto* search = dynamic_cast<ScrollingItemWidget*>(wc);
        if (!search || !search->SetFolderSearchScrollOffset(scroll)) widget.scrollOffset = scroll;
        if (auto* group = dynamic_cast<FileGroup*>(wc))
            group->InvalidateHostedView();
        else
            wc->InvalidateSlots();
    };

    // Drawing, hit testing and keyboard reveal all ask the component for the
    // same local-track rectangle. This also accounts for search/date headers
    // and for FileGroup-hosted source geometry.
    const RECT viewport = wc->GetContentViewportRect();
    const RECT target = wc->GetMemberLayoutRect(
        static_cast<size_t>(memberIndex));
    if (!IsRectEmptyRect(target) && !IsRectEmptyRect(viewport))
    {
        applyScroll(snowdesktop::widget_item_layout::
            ScrollOffsetToReveal(viewport, target,
                wc->GetScrollOffset(), maxScroll));
        return;
    }

    // Fallback for non-item surfaces that cannot provide member geometry.
    int columns = std::max(1, widget.gridSpan.columns);
    const auto* contentView = wc->GetContentViewData();
    const bool listMode = contentView && (contentView->listMode ||
        (contentView->type != DesktopWidgetType::Collection && contentView->dateHeaders));

    size_t memberCount = (widget.type == DesktopWidgetType::FolderMapping)
        ? widget.folderEntries.size()
        : widget.itemKeys.size();
    if (widget.type == DesktopWidgetType::FileCategories)
    {
        auto* fc = dynamic_cast<FileCategories*>(wc);
        if (fc) memberCount = fc->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        auto* mapping = dynamic_cast<FolderMapping*>(wc);
        if (mapping) memberCount = mapping->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        auto* group = dynamic_cast<FileGroup*>(wc);
        if (group) memberCount = group->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        auto* group = dynamic_cast<CollectionGroup*>(wc);
        if (group) memberCount = group->GetSlotCount();
    }
    if (memberCount <= 1) return;

    int totalRows = listMode
        ? static_cast<int>(memberCount)
        : (static_cast<int>(memberCount) + columns - 1) / columns;
    int row = listMode ? memberIndex : memberIndex / columns;

    int scroll = (totalRows <= 1) ? 0
        : static_cast<int>((static_cast<int64_t>(row) * maxScroll) / (totalRows - 1));

    applyScroll(scroll);
}

/**
 * @brief 在组件内部导航成员项
 * @param arrowKey 方向键虚拟键码
 *
 * 根据组件类型（Collection、FileCategories、FolderMapping）的列数布局，
 * 在组件成员项之间进行上下左右 2D 导航。list 模式的 FileCategories 使用线性上下移动。
 */
void DesktopApp::NavigateWidgetMembers(WPARAM arrowKey)
{
    keyboardNavVisualFocus_ = true;
    if (keyboardNavWidgetIndex_ >= widgets_.size()) return;
    const auto clearSelectionForKeyboardNavigation = [this]() {
        ClearSelection();
        keyboardNavVisualFocus_ = true;
    };
    const size_t navigationWidgetIndex =
        keyboardNavWidgetIndex_;
    auto& widget = widgets_[keyboardNavWidgetIndex_];
    const auto memberColumns = [&]() {
        for (const auto& container : containers_)
        {
            const auto* view = dynamic_cast<const WidgetContainer*>(container.get());
            if (view && view->GetWidgetData() == &widget)
                return view->GetMemberColumnCount();
        }
        return std::max(1, widget.gridSpan.columns);
    };

    if (widget.type == DesktopWidgetType::FileGroup)
    {
        FileGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<FileGroup*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                group = candidate;
                break;
            }
        }
        if (!group) return;
        const auto& visibleSourceIds =
            group->GetVisibleSourceIds();
        const std::vector<std::wstring> sourceIds(
            visibleSourceIds.begin(),
            visibleSourceIds.end());
        if (sourceIds.empty()) return;
        const size_t groupIndex =
            keyboardNavWidgetIndex_;
        auto activeSourceIt = std::find(
            sourceIds.begin(), sourceIds.end(),
            group->GetActiveSourceId());
        const size_t activeSource =
            activeSourceIt == sourceIds.end()
                ? 0
                : static_cast<size_t>(std::distance(
                    sourceIds.begin(), activeSourceIt));

        const bool searchAvailable =
            !IsRectEmptyRect(group->GetSearchBoxRect());
        auto focusSearch = [&]() {
            if (!searchAvailable) return;
            group->SetSearchFocused(false);
            clearSelectionForKeyboardNavigation();
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ = -1;
            keyboardNavSearchBox_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        auto focusSource = [&](size_t index,
            bool activate) {
            if (index >= sourceIds.size()) return;
            group->SetSearchFocused(false);
            clearSelectionForKeyboardNavigation();
            if (activate)
            {
                widget.activeCategoryId =
                    sourceIds[index];
                if (!group->SetFolderSearchScrollOffset(0)) widget.scrollOffset = 0;
                group->InvalidateHostedView();
            }
            const size_t childIndex =
                FindWidgetIndexById(sourceIds[index]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = true;
            keyboardNavFileGroupCategoryTabs_ = false;
            group->EnsureSourceTabVisible(index);
            if (activate) SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        const bool showCategoryRow =
            group->GetContentViewData()->showFileCategories &&
            !(widget.showSearchBox &&
              !group->GetSearchText().empty());
        const std::vector<std::wstring> categoryIds =
            showCategoryRow
                ? group->GetHostedVisibleCategoryIds()
                : std::vector<std::wstring>{};
        auto* activeContainer =
            group->GetActiveSourceContainer();
        DesktopWidget* activeData = activeContainer
            ? activeContainer->GetWidgetData() : nullptr;
        auto activeCategoryIt = activeData
            ? std::find(categoryIds.begin(),
                categoryIds.end(),
                activeData->activeCategoryId)
            : categoryIds.end();
        const size_t activeCategory =
            activeCategoryIt == categoryIds.end()
                ? 0
                : static_cast<size_t>(std::distance(
                    categoryIds.begin(), activeCategoryIt));

        auto focusCategory = [&](size_t index,
            bool activate) {
            if (!activeData ||
                index >= categoryIds.size())
                return;
            group->SetSearchFocused(false);
            clearSelectionForKeyboardNavigation();
            if (activate)
            {
                activeData->activeCategoryId =
                    categoryIds[index];
                widget.scrollOffset = 0;
                group->InvalidateHostedView();
            }
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = true;
            group->EnsureCategoryTabVisible(index);
            if (activate) SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        const bool groupSearching =
            group->IsGroupSearchActive();
        const auto itemKeys = groupSearching
            ? std::vector<std::wstring>{}
            : group->GetHostedVisibleItemKeys();
        const auto folderIndices = groupSearching
            ? std::vector<size_t>{}
            : group->GetHostedVisibleFolderIndices();
        const size_t itemCount = groupSearching
            ? group->GetSlotCount()
            : (!itemKeys.empty()
                ? itemKeys.size()
                : folderIndices.size());
        auto selectItem = [&](size_t index) {
            if (index >= itemCount) return;
            group->SetSearchFocused(false);
            clearSelectionForKeyboardNavigation();
            if (groupSearching)
            {
                Item* item =
                    group->GetMemberItem(index);
                if (!item) return;
                item->SetSelected(true);
            }
            else if (!itemKeys.empty())
            {
                const size_t itemIndex =
                    FindItemIndexByKey(itemKeys[index]);
                if (itemIndex >= items_.size()) return;
                items_[itemIndex].selected = true;
            }
            else if (activeData)
            {
                const size_t entryIndex =
                    folderIndices[index];
                if (entryIndex >=
                    activeData->folderEntries.size())
                    return;
                activeData->folderEntries[
                    entryIndex].selected = true;
            }
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            ScrollWidgetToMember(groupIndex,
                static_cast<int>(index));
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        if (keyboardNavSearchBox_)
        {
            if (arrowKey == VK_DOWN)
            {
                if (!groupSearching)
                    focusSource(activeSource, false);
                else if (itemCount > 0)
                    selectItem(0);
            }
            return;
        }

        if (keyboardNavCollectionGroupTabs_)
        {
            size_t current =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        sourceIds.size() - 1)
                    : activeSource;
            if (arrowKey == VK_LEFT && current > 0)
                focusSource(current - 1, true);
            else if (arrowKey == VK_RIGHT &&
                     current + 1 < sourceIds.size())
                focusSource(current + 1, true);
            else if (arrowKey == VK_DOWN)
            {
                if (!categoryIds.empty())
                    focusCategory(activeCategory, false);
                else if (itemCount > 0)
                    selectItem(0);
            }
            else if (arrowKey == VK_UP)
                focusSearch();
            return;
        }
        if (keyboardNavFileGroupCategoryTabs_)
        {
            if (categoryIds.empty())
            {
                focusSource(activeSource, false);
                return;
            }
            size_t current =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        categoryIds.size() - 1)
                    : activeCategory;
            if (arrowKey == VK_LEFT && current > 0)
                focusCategory(current - 1, true);
            else if (arrowKey == VK_RIGHT &&
                     current + 1 < categoryIds.size())
                focusCategory(current + 1, true);
            else if (arrowKey == VK_UP)
                focusSource(activeSource, false);
            else if (arrowKey == VK_DOWN &&
                     itemCount > 0)
                selectItem(0);
            return;
        }

        if (itemCount == 0)
        {
            if (groupSearching)
                return;
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else
                focusSource(activeSource, false);
            return;
        }
        size_t current =
            keyboardNavMemberIndex_ >= 0
                ? std::min(
                    static_cast<size_t>(
                        keyboardNavMemberIndex_),
                    itemCount - 1)
                : 0;
        const int columns = memberColumns();
        const bool firstVisualRow = columns > 1
            ? group->GetMemberLayoutRect(current).top == group->GetMemberLayoutRect(0).top
            : current < static_cast<size_t>(columns);
        if (arrowKey == VK_UP && firstVisualRow)
        {
            if (groupSearching)
            {
                focusSearch();
                return;
            }
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else
                focusSource(activeSource, false);
            return;
        }
        int next = static_cast<int>(current);
        if (arrowKey == VK_UP) next -= columns;
        else if (arrowKey == VK_DOWN) next += columns;
        else if (arrowKey == VK_LEFT) --next;
        else if (arrowKey == VK_RIGHT) ++next;
        else return;
        if (columns > 1 && (arrowKey == VK_UP || arrowKey == VK_DOWN))
            next = snowdesktop::widget_item_layout::VerticalNeighbor(itemCount,
                static_cast<int>(current), arrowKey == VK_DOWN,
                [&](size_t index) { return group->GetMemberLayoutRect(index); });
        if (next >= 0 &&
            static_cast<size_t>(next) < itemCount)
            selectItem(static_cast<size_t>(next));
        return;
    }

    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        CollectionGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<CollectionGroup*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                group = candidate;
                break;
            }
        }
        if (!group) return;

        const auto& visibleChildIds =
            group->GetVisibleCollectionIds();
        const std::vector<std::wstring> childIds(
            visibleChildIds.begin(),
            visibleChildIds.end());
        const size_t groupWidgetIndex =
            keyboardNavWidgetIndex_;
        auto activeIt = std::find(
            childIds.begin(), childIds.end(),
            group->GetActiveCollectionId());
        size_t activeTab = activeIt == childIds.end()
            ? 0
            : static_cast<size_t>(
                std::distance(childIds.begin(), activeIt));

        const bool searchAvailable =
            !IsRectEmptyRect(group->GetSearchBoxRect());
        auto focusSearch = [&]() {
            if (!searchAvailable) return;
            group->SetSearchFocused(false);
            clearSelectionForKeyboardNavigation();
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupWidgetIndex;
            keyboardNavMemberIndex_ = -1;
            keyboardNavSearchBox_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        auto focusTab = [&](size_t tabIndex,
            bool activate) {
            if (tabIndex >= childIds.size()) return;
            group->SetSearchFocused(false);
            clearSelectionForKeyboardNavigation();
            if (activate)
            {
                widget.activeCategoryId =
                    childIds[tabIndex];
                widget.scrollOffset = 0;
                group->InvalidateFilterCache();
            }
            const size_t childIndex =
                FindWidgetIndexById(childIds[tabIndex]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ =
                groupWidgetIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(tabIndex);
            keyboardNavCollectionGroupTabs_ = true;
            group->EnsureTabVisible(tabIndex);
            if (activate)
                SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        if (keyboardNavSearchBox_)
        {
            if (arrowKey == VK_DOWN &&
                !childIds.empty())
                focusTab(activeTab, false);
            return;
        }

        if (keyboardNavCollectionGroupTabs_)
        {
            if (childIds.empty()) return;
            size_t currentTab =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        childIds.size() - 1)
                    : activeTab;
            if (arrowKey == VK_LEFT)
            {
                if (currentTab == 0) return;
                focusTab(currentTab - 1, true);
            }
            else if (arrowKey == VK_RIGHT)
            {
                if (currentTab + 1 >=
                    childIds.size())
                    return;
                focusTab(currentTab + 1, true);
            }
            else if (arrowKey == VK_DOWN)
            {
                const auto& keys =
                    group->GetVisibleItemKeys();
                if (keys.empty()) return;
                const size_t itemIndex =
                    FindItemIndexByKey(keys.front());
                if (itemIndex >= items_.size()) return;
                clearSelectionForKeyboardNavigation();
                items_[itemIndex].selected = true;
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ =
                    groupWidgetIndex;
                keyboardNavMemberIndex_ = 0;
                keyboardNavCollectionGroupTabs_ =
                    false;
                ScrollWidgetToMember(
                    keyboardNavWidgetIndex_, 0);
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
            }
            else if (arrowKey == VK_UP)
                focusSearch();
            return;
        }

        if (arrowKey == VK_UP && !childIds.empty())
        {
            const auto& keys =
                group->GetVisibleItemKeys();
            int current = keyboardNavMemberIndex_;
            for (size_t i = 0; i < keys.size(); ++i)
            {
                const size_t itemIndex =
                    FindItemIndexByKey(keys[i]);
                if (itemIndex < items_.size() &&
                    items_[itemIndex].selected)
                {
                    current = static_cast<int>(i);
                    break;
                }
            }
            const int columns = memberColumns();
            if (current >= 0 && current < columns)
            {
                focusTab(activeTab, false);
                return;
            }
        }
    }

    size_t memberCount = 0;
    int columns = 1;
    bool isListMode = false;

    switch (widget.type)
    {
    case DesktopWidgetType::CollectionGroup:
        memberCount = 0;
        columns = memberColumns();
        if (ResolveWidgetContentViewData(&widget)->listMode)
        {
            isListMode = columns == 1;
        }
        break;
    case DesktopWidgetType::Collection:
    case DesktopWidgetType::FileCategories:
        memberCount = widget.itemKeys.size();
        columns = memberColumns();
        if (widget.listMode || widget.dateHeaders)
        {
            isListMode = columns == 1;
        }
        break;
    case DesktopWidgetType::FolderMapping:
        memberCount = widget.folderEntries.size();
        columns = memberColumns();
        if (widget.listMode || widget.dateHeaders)
        {
            isListMode = columns == 1;
        }
        break;
    default:
        return;   // LuaScript、Guide 无内部导航
    }

    // Collection 弹窗打开时，按弹窗实际列数进行 2D 导航
    if ((widget.type == DesktopWidgetType::Collection ||
            widget.type == DesktopWidgetType::FileCategories) &&
        popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
        popupWidgetIndex_ < widgets_.size())
    {
        columns = GetCollectionPopupColumnCount(popupRect_);
        isListMode = UsesCollectionPopupList(widget) && columns == 1;
    }

    // FileCategories：获取当前可见项目键列表（受搜索/分类标签页过滤）
    std::vector<std::wstring> visibleKeys;
    std::vector<size_t> visibleFolderIndices;
    std::vector<std::wstring> categoryIds;
    ScrollingItemWidget* categorizedWidget = nullptr;
    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        for (auto& c : containers_)
        {
            auto* group =
                dynamic_cast<CollectionGroup*>(c.get());
            if (group && group->GetWidgetData() == &widget)
            {
                const auto& keys =
                    group->GetVisibleItemKeys();
                visibleKeys.assign(keys.begin(), keys.end());
                break;
            }
        }
        memberCount = visibleKeys.size();
        if (memberCount == 0) return;
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widget)
            {
                auto* fc = dynamic_cast<FileCategories*>(wc);
                if (fc)
                {
                    categorizedWidget = fc;
                    const auto& rk = fc->GetSearchResultKeys();
                    visibleKeys.assign(rk.begin(), rk.end());
                    if (widget.showFileCategories &&
                        (!widget.showSearchBox ||
                            !fc->IsSearchActive()))
                    {
                        const auto& ids =
                            fc->CachedVisibleCategoryIds();
                        categoryIds.assign(
                            ids.begin(), ids.end());
                    }
                }
                break;
            }
        }
        if (popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
            !UsesCategorizedPopupControls(widget))
            visibleKeys = GetPopupItemKeys(widget);
        memberCount = visibleKeys.size();
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        for (auto& c : containers_)
        {
            auto* mapping = dynamic_cast<FolderMapping*>(c.get());
            if (mapping && mapping->GetWidgetData() == &widget)
            {
                categorizedWidget = mapping;
                const auto& indices = mapping->GetVisibleEntryIndices();
                visibleFolderIndices.assign(indices.begin(), indices.end());
                if (widget.showFileCategories)
                {
                    const auto& ids =
                        mapping->GetVisibleCategoryIds();
                    categoryIds.assign(
                        ids.begin(), ids.end());
                }
                break;
            }
        }
        memberCount = visibleFolderIndices.size();
    }
    else if (memberCount == 0)
    {
        return;
    }

    const bool folderSearch = categorizedWidget && categorizedWidget->UsesFolderSearch();
    if (folderSearch)
    {
        memberCount = categorizedWidget->FolderSearchCount();
        visibleKeys.clear(); visibleFolderIndices.clear(); categoryIds.clear();
    }
    const bool searchAvailable =
        categorizedWidget &&
        !IsRectEmptyRect(
            categorizedWidget->GetSearchBoxRect());
    auto focusSearch = [&]() {
        if (!searchAvailable) return;
        categorizedWidget->SetSearchFocused(false);
        clearSelectionForKeyboardNavigation();
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = navigationWidgetIndex;
        keyboardNavMemberIndex_ = -1;
        keyboardNavSearchBox_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    auto activeCategoryIt = std::find(
        categoryIds.begin(), categoryIds.end(),
        widget.activeCategoryId);
    const size_t activeCategory =
        activeCategoryIt == categoryIds.end()
            ? 0
            : static_cast<size_t>(std::distance(
                categoryIds.begin(), activeCategoryIt));
    auto focusCategory = [&](size_t index,
        bool activate) {
        if (!categorizedWidget ||
            index >= categoryIds.size())
            return;
        categorizedWidget->SetSearchFocused(false);
        clearSelectionForKeyboardNavigation();
        if (activate)
        {
            widget.activeCategoryId = categoryIds[index];
            widget.scrollOffset = 0;
            if (auto* mapping =
                    dynamic_cast<FolderMapping*>(
                        categorizedWidget))
                mapping->InvalidateFilterCache();
            else
                categorizedWidget->InvalidateSlots();
        }
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = navigationWidgetIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(index);
        keyboardNavFileGroupCategoryTabs_ = true;
        categorizedWidget->EnsureCategoryTabVisible(index);
        if (activate) SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    auto selectVisibleItem = [&](size_t index) {
        if (index >= memberCount) return;
        if (folderSearch)
        {
            categorizedWidget->SetSearchFocused(false);
            clearSelectionForKeyboardNavigation();
            auto* item = categorizedWidget->FolderSearchItem(index, true);
            if (!item) return;
            item->SetSelected(true);
            keyboardNavInsideWidget_ = true; keyboardNavWidgetIndex_ = navigationWidgetIndex;
            keyboardNavMemberIndex_ = static_cast<int>(index);
            ScrollWidgetToMember(navigationWidgetIndex, static_cast<int>(index));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        size_t selectedItemIndex =
            static_cast<size_t>(-1);
        size_t selectedEntryIndex =
            static_cast<size_t>(-1);
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            selectedEntryIndex =
                visibleFolderIndices[index];
            if (selectedEntryIndex >=
                widget.folderEntries.size())
                return;
        }
        else
        {
            selectedItemIndex =
                FindItemIndexByKey(visibleKeys[index]);
            if (selectedItemIndex >= items_.size())
                return;
        }
        if (categorizedWidget)
            categorizedWidget->SetSearchFocused(false);
        clearSelectionForKeyboardNavigation();
        if (selectedEntryIndex <
            widget.folderEntries.size())
            widget.folderEntries[
                selectedEntryIndex].selected = true;
        else
            items_[selectedItemIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = navigationWidgetIndex;
        keyboardNavMemberIndex_ =
            widget.type == DesktopWidgetType::FolderMapping
                ? static_cast<int>(selectedEntryIndex)
                : static_cast<int>(index);
        ScrollWidgetToMember(
            keyboardNavWidgetIndex_,
            static_cast<int>(index));
        InvalidateRect(hwnd_, nullptr, FALSE);
    };

    if (keyboardNavSearchBox_)
    {
        if (arrowKey == VK_DOWN)
        {
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else if (memberCount > 0)
                selectVisibleItem(0);
        }
        return;
    }
    if (keyboardNavFileGroupCategoryTabs_)
    {
        if (categoryIds.empty())
        {
            if (searchAvailable)
                focusSearch();
            return;
        }
        size_t current = keyboardNavMemberIndex_ >= 0
            ? std::min(
                static_cast<size_t>(keyboardNavMemberIndex_),
                categoryIds.size() - 1)
            : activeCategory;
        if (arrowKey == VK_LEFT && current > 0)
            focusCategory(current - 1, true);
        else if (arrowKey == VK_RIGHT &&
                 current + 1 < categoryIds.size())
            focusCategory(current + 1, true);
        else if (arrowKey == VK_UP)
            focusSearch();
        else if (arrowKey == VK_DOWN &&
                 memberCount > 0)
            selectVisibleItem(0);
        return;
    }

    if (memberCount == 0)
    {
        if (!categoryIds.empty())
            focusCategory(activeCategory, false);
        else
            focusSearch();
        return;
    }

    int currentIdx = keyboardNavMemberIndex_;
    if (currentIdx < 0) currentIdx = 0;

    if (folderSearch)
    {
        const auto selected = categorizedWidget->FolderSearchSelectedIndices();
        if (!selected.empty()) currentIdx = static_cast<int>(selected.front());
    }
    // FileCategories：基于当前可见选中项确定导航起点
    if (!visibleKeys.empty())
    {
        int foundIdx = -1;
        for (size_t i = 0; i < visibleKeys.size(); ++i)
        {
            size_t itemIdx = FindItemIndexByKey(visibleKeys[i]);
            if (itemIdx != static_cast<size_t>(-1) && items_[itemIdx].selected)
            {
                foundIdx = static_cast<int>(i);
                break;
            }
        }
        if (foundIdx >= 0)
            currentIdx = foundIdx;
        else
            currentIdx = 0;
    }
    else if (!visibleFolderIndices.empty())
    {
        int foundIdx = -1;
        for (size_t i = 0; i < visibleFolderIndices.size(); ++i)
        {
            size_t entryIndex = visibleFolderIndices[i];
            if (entryIndex < widget.folderEntries.size() &&
                widget.folderEntries[entryIndex].selected)
            {
                foundIdx = static_cast<int>(i);
                break;
            }
        }
        currentIdx = foundIdx >= 0 ? foundIdx : 0;
    }

    int currentCol = currentIdx % columns;
    int currentRow = currentIdx / columns;
    int totalRows = static_cast<int>((memberCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));

    int nextRow = currentRow;
    int nextCol = currentCol;
    WidgetContainer* navigationView = nullptr;
    for (const auto& container : containers_)
    {
        auto* view = dynamic_cast<WidgetContainer*>(container.get());
        if (view && view->GetWidgetData() == &widget) { navigationView = view; break; }
    }
    const bool flatPopup = popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
        popupWidgetIndex_ < widgets_.size() && !GetGroupPopupView();
    const auto memberBounds = [&](size_t index) {
        return flatPopup ? GetCollectionPopupItemRect(popupRect_, index) :
            navigationView ? navigationView->GetMemberLayoutRect(index) : RECT{};
    };
    const bool geometricRows = columns > 1 && !IsRectEmptyRect(memberBounds(currentIdx));
    const bool firstVisualRow = geometricRows
        ? memberBounds(currentIdx).top == memberBounds(0).top : currentRow == 0;
    const int verticalNext = geometricRows && (arrowKey == VK_UP || arrowKey == VK_DOWN)
        ? snowdesktop::widget_item_layout::VerticalNeighbor(memberCount, currentIdx,
            arrowKey == VK_DOWN, memberBounds) : -1;

    if (arrowKey == VK_UP && firstVisualRow &&
        (!categoryIds.empty() || searchAvailable))
    {
        if (!categoryIds.empty())
            focusCategory(activeCategory, false);
        else
            focusSearch();
        return;
    }

    switch (arrowKey)
    {
    case VK_UP:
        if (verticalNext >= 0) break;
        nextRow = currentRow - 1;
        if (nextRow < 0) return;
        break;
    case VK_DOWN:
        if (verticalNext >= 0) break;
        nextRow = currentRow + 1;
        if (nextRow >= totalRows) return;
        break;
    case VK_LEFT:
        if (isListMode)
        {
            if (currentIdx <= 0) return;
        }
        else
        {
            nextCol = currentCol - 1;
            if (nextCol < 0)
            {
                // 换行：跳到上一行最后一列
                nextRow = currentRow - 1;
                if (nextRow < 0) return;
                nextCol = columns - 1;
            }
        }
        break;
    case VK_RIGHT:
        if (isListMode)
        {
            if (static_cast<size_t>(currentIdx) + 1 >= memberCount) return;
        }
        else
        {
            nextCol = currentCol + 1;
            if (nextCol >= columns)
            {
                // 换行：跳到下一行第一列
                nextRow = currentRow + 1;
                if (nextRow >= totalRows) return;
                nextCol = 0;
            }
        }
        break;
    default:
        return;
    }

    int nextIdx;
    if (verticalNext >= 0)
        nextIdx = verticalNext;
    else if (isListMode)
    {
        if (arrowKey == VK_UP || arrowKey == VK_LEFT)
            nextIdx = currentIdx - 1;
        else
            nextIdx = currentIdx + 1;
        if (nextIdx < 0 || static_cast<size_t>(nextIdx) >= memberCount) return;
    }
    else
    {
        nextIdx = nextRow * columns + nextCol;
        if (nextIdx < 0 || static_cast<size_t>(nextIdx) >= static_cast<int>(memberCount)) return;
    }

    if (nextIdx == currentIdx) return;

    if (folderSearch) { selectVisibleItem(static_cast<size_t>(nextIdx)); return; }

    // 取消旧成员选中
    if (currentIdx >= 0)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            if (static_cast<size_t>(currentIdx) <
                visibleFolderIndices.size())
            {
                size_t entryIndex =
                    visibleFolderIndices[static_cast<size_t>(currentIdx)];
                if (entryIndex < widget.folderEntries.size())
                    widget.folderEntries[entryIndex].selected = false;
            }
        }
        else if (!visibleKeys.empty())
        {
            // 清除所有可见项选中状态，防止多选残留
            for (const auto& key : visibleKeys)
            {
                size_t itemIdx = FindItemIndexByKey(key);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
        else
        {
            const auto& keys = widget.itemKeys;
            if (static_cast<size_t>(currentIdx) < keys.size())
            {
                size_t itemIdx = FindItemIndexByKey(keys[static_cast<size_t>(currentIdx)]);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
    }

    // 选中新成员
    if (widget.type == DesktopWidgetType::FolderMapping)
    {
        size_t entryIndex =
            visibleFolderIndices[static_cast<size_t>(nextIdx)];
        if (entryIndex >= widget.folderEntries.size()) return;
        widget.folderEntries[entryIndex].selected = true;
    }
    else if (!visibleKeys.empty())
    {
        size_t itemIdx = FindItemIndexByKey(visibleKeys[static_cast<size_t>(nextIdx)]);
        if (itemIdx != static_cast<size_t>(-1))
            items_[itemIdx].selected = true;
    }
    else
    {
        const auto& keys = widget.itemKeys;
        size_t itemIdx = FindItemIndexByKey(keys[static_cast<size_t>(nextIdx)]);
        if (itemIdx != static_cast<size_t>(-1))
            items_[itemIdx].selected = true;
    }

    keyboardNavMemberIndex_ =
        widget.type == DesktopWidgetType::FolderMapping
            ? static_cast<int>(
                visibleFolderIndices[static_cast<size_t>(nextIdx)])
            : nextIdx;

    // 确保选中的成员项可见
    ScrollWidgetToMember(keyboardNavWidgetIndex_, nextIdx);

    // Collection 大文件夹模式：超出外露区域时进入弹窗。
    if (widget.type == DesktopWidgetType::Collection && !widget.scrollContainerMode)
    {
        int cols = std::max(1, widget.gridSpan.columns);
        int rows = std::max(1, widget.gridSpan.rows);
        Collection* collection = nullptr;
        for (const auto& container : containers_)
        {
            auto* candidate =
                dynamic_cast<Collection*>(container.get());
            if (!candidate ||
                candidate->GetWidgetData() != &widget)
                continue;
            collection = candidate;
            break;
        }
        const size_t inlineCap = collection
            ? collection->GetSlotCount()
            : ((cols <= 1 && rows <= 1)
                ? 4
                : static_cast<size_t>(cols * rows - 1));
        if (static_cast<size_t>(nextIdx) >= inlineCap)
        {
            if (popupWidgetIndex_ != keyboardNavWidgetIndex_)
            {
                POINT popupAnchor{
                    widget.bounds.right - 1,
                    widget.bounds.bottom - 1,
                };
                if (collection)
                {
                    const RECT allButton =
                        collection->GetAllButtonRect();
                    if (!IsRectEmptyRect(allButton))
                    {
                        popupAnchor = {
                            allButton.left +
                                (allButton.right - allButton.left) / 2,
                            allButton.top +
                                (allButton.bottom - allButton.top) / 2,
                        };
                    }
                }
                OpenCollectionPopupAt(keyboardNavWidgetIndex_,
                    popupAnchor);
            }
        }
    }

    // 弹窗滚动跟随键盘选中的项目。
    if (popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
        popupWidgetIndex_ < widgets_.size() && !GetGroupPopupView())
    {
        RECT rPopup = popupRect_;
        RECT content = GetCollectionPopupContentRect(rPopup);
        int viewH = std::max(1, static_cast<int>(content.bottom - content.top));
        const RECT targetRect = GetCollectionPopupItemRect(
            rPopup, static_cast<size_t>(nextIdx));
        const int targetTop = targetRect.top - content.top +
            popupScrollOffset_;
        const int targetBottom = targetRect.bottom - content.top +
            popupScrollOffset_;
        int maxPopupScroll = GetCollectionPopupMaxScrollOffset(
            widgets_[keyboardNavWidgetIndex_], rPopup);
        if (targetTop < popupScrollOffset_)
            popupScrollOffset_ = targetTop;
        else if (targetBottom > popupScrollOffset_ + viewH)
            popupScrollOffset_ = targetBottom - viewH;
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0, maxPopupScroll);
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}
