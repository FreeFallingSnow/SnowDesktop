#include "app.h"
#include "../menu_fluent_glyphs.h"

// Collection/file-group dwell activation and popup tab switching.

bool DesktopApp::CanCurrentDragUseCollectionPopup() const
{
    const DragSourceList& sourceList =
        dragSession_.SourceList();
    return snowdesktop::dock_drop_rules::
        CanUseCollectionPopup(
            dragSession_.IsActive(),
            dragDropController_.IsExternalDragActive(),
            sourceList.Empty(),
            sourceList.hasWidgets,
            sourceList.hasCollectionGroupEntries,
            sourceList.hasFileGroupEntries);
}

void DesktopApp::EnsureCollectionPopupDwellTimerArmed()
{
    if (collectionPopupDwellTimerArmed_ ||
        !hwnd_ || !IsWindow(hwnd_))
        return;
    collectionPopupDwellTimerArmed_ =
        SetTimer(
            hwnd_, kCollectionPopupDwellTimerId,
            kCollectionPopupDwellIntervalMs, nullptr) != 0;
}

void DesktopApp::CancelCollectionPopupDwell()
{
    if (!collectionPopupDwellTimerArmed_ &&
        popupDwellController_.IsIdle())
        return;
    popupDwellController_.Reset();
    const bool wasArmed = collectionPopupDwellTimerArmed_;
    collectionPopupDwellTimerArmed_ = false;
    if (wasArmed && hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
}

void DesktopApp::EnsureCollectionGroupTabDwellTimerArmed()
{
    if (collectionGroupTabDwellTimerArmed_ ||
        !hwnd_ || !IsWindow(hwnd_))
        return;
    collectionGroupTabDwellTimerArmed_ =
        SetTimer(
            hwnd_, kCollectionGroupTabDwellTimerId,
            kCollectionGroupTabDwellIntervalMs, nullptr) != 0;
}

void DesktopApp::CancelCollectionGroupTabDwell()
{
    if (!collectionGroupTabDwellTimerArmed_ &&
        collectionGroupTabDwellWidgetIndex_ ==
            static_cast<size_t>(-1) &&
        collectionGroupTabDwellId_.empty() &&
        collectionGroupTabDwellTick_ == 0)
        return;
    collectionGroupTabDwellWidgetIndex_ =
        static_cast<size_t>(-1);
    collectionGroupTabDwellId_.clear();
    collectionGroupTabDwellTick_ = 0;
    const bool wasArmed =
        collectionGroupTabDwellTimerArmed_;
    collectionGroupTabDwellTimerArmed_ = false;
    if (wasArmed && hwnd_ && IsWindow(hwnd_))
    {
        KillTimer(
            hwnd_, kCollectionGroupTabDwellTimerId);
    }
}

void DesktopApp::UpdateCollectionPopupDwell(POINT point)
{
    lastMousePoint_ = point;
    if (!CanCurrentDragUseCollectionPopup() ||
        SuppressDesktopWidgetDragTargets())
    {
        CancelCollectionPopupDwell();
        return;
    }

    // The popup is rendered above Dock and desktop widgets. Do not let its
    // contents start a dwell timer for an opener tile hidden underneath it.
    if (popupDwellController_.CancelIfOccluded(
            IsPointInsideOpenPopup(point)))
    {
        CancelCollectionPopupDwell();
        return;
    }

    size_t hoveredCollection = static_cast<size_t>(-1);
    if (DockContainer* dock = GetDockContainerAtPoint(point))
    {
        if (DockEntryItem* entry = dock->EntryAtPoint(point);
            entry && entry->GetEntryType() == DockEntryType::Collection)
        {
            hoveredCollection = FindWidgetIndexById(entry->GetReference());
        }
    }

    for (auto& c : containers_)
    {
        if (hoveredCollection < widgets_.size()) break;
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(c.get()))
            continue;
        auto* collection = dynamic_cast<Collection*>(c.get());
        if (!collection) continue;

        RECT buttonRect = collection->GetAllButtonRect();
        if (IsRectEmptyRect(buttonRect) || !PtInRect(&buttonRect, point))
            continue;

        DesktopWidget* data = collection->GetWidgetData();
        for (size_t wi = 0; wi < widgets_.size(); ++wi)
        {
            if (&widgets_[wi] == data)
            {
                hoveredCollection = wi;
                break;
            }
        }
        break;
    }

    if (hoveredCollection == static_cast<size_t>(-1) ||
        hoveredCollection == popupWidgetIndex_)
    {
        CancelCollectionPopupDwell();
        return;
    }

    DWORD now = GetTickCount();
    const bool candidateChanged =
        popupDwellController_.Track(
            hoveredCollection, now);
    EnsureCollectionPopupDwellTimerArmed();
    if (candidateChanged)
        return;

    TryOpenDwellCollectionPopup(now);
}

/**
 * @brief 尝试在停留时间达标后打开集合弹窗
 * @param now 当前时间（毫秒）
 * @return 是否成功打开了弹窗
 */
bool DesktopApp::TryOpenDwellCollectionPopup(DWORD now)
{
    if (!CanCurrentDragUseCollectionPopup() ||
        SuppressDesktopWidgetDragTargets())
    {
        CancelCollectionPopupDwell();
        return false;
    }
    // Recheck at timer delivery so a candidate captured before another popup
    // opened cannot replace it through the foreground popup.
    if (popupDwellController_.CancelIfOccluded(
            IsPointInsideOpenPopup(lastMousePoint_)))
    {
        CancelCollectionPopupDwell();
        return false;
    }
    const size_t candidate =
        popupDwellController_.Candidate();
    if (candidate >= widgets_.size())
    {
        CancelCollectionPopupDwell();
        return false;
    }
    if (desktopIconsHidden_ &&
        !widgets_[candidate].keepWhenDesktopHidden)
    {
        CancelCollectionPopupDwell();
        return false;
    }
    if (candidate == popupWidgetIndex_ ||
        widgets_[candidate].type !=
            DesktopWidgetType::Collection)
    {
        CancelCollectionPopupDwell();
        return false;
    }
    if (!popupDwellController_.IsReady(
            now, kCollectionPopupDwellDelayMs))
        return false;

    size_t widgetIndex = candidate;
    CancelCollectionPopupDwell();
    OpenCollectionPopupAt(widgetIndex, lastMousePoint_);
    UpdateWindow(hwnd_);
    return true;
}

void DesktopApp::UpdateCollectionGroupTabDwell(
    POINT point)
{
    auto clearDwell = [&]() {
        CancelCollectionGroupTabDwell();
    };

    const DragSourceList& sourceList =
        dragSession_.SourceList();
    if (!dragSession_.IsActive() ||
        !(sourceList.hasDesktopIcons ||
          sourceList.hasFolderEntries ||
          sourceList.hasExternalFiles) ||
        sourceList.hasCollectionGroupEntries ||
        sourceList.hasFileGroupEntries)
    {
        clearDwell();
        return;
    }

    if (IsPointInsideOpenPopup(point))
    {
        clearDwell();
        return;
    }

    size_t hoveredGroup = static_cast<size_t>(-1);
    std::wstring hoveredId;
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        if (desktopIconsHidden_ &&
            !IsRetainedContainer(it->get()))
            continue;
        DesktopWidget* data = nullptr;
        std::wstring id;
        if (auto* group =
                dynamic_cast<CollectionGroup*>(it->get());
            group && sourceList.hasDesktopIcons)
        {
            id = group->CategoryIdAtPoint(point);
            if (id.empty() ||
                id == group->GetActiveCollectionId())
                continue;
            data = group->GetWidgetData();
        }
        else if (auto* fileGroup =
                     dynamic_cast<FileGroup*>(it->get()))
        {
            const std::wstring sourceId =
                fileGroup->SourceIdAtPoint(point);
            if (!sourceId.empty() &&
                sourceId != fileGroup->GetActiveSourceId())
            {
                id = L"source:" + sourceId;
            }
            else
            {
                const std::wstring categoryId =
                    fileGroup->CategoryIdAtPoint(point);
                ScrollingItemWidget* active =
                    fileGroup->GetActiveSourceContainer();
                DesktopWidget* activeData = active
                    ? active->GetWidgetData() : nullptr;
                if (categoryId.empty() ||
                    (activeData &&
                     activeData->activeCategoryId ==
                        categoryId))
                    continue;
                id = L"category:" + categoryId;
            }
            data = fileGroup->GetWidgetData();
        }
        else
        {
            continue;
        }
        if (!data) continue;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (&widgets_[i] != data) continue;
            hoveredGroup = i;
            hoveredId = std::move(id);
            break;
        }
        if (hoveredGroup < widgets_.size())
            break;
    }

    if (hoveredGroup >= widgets_.size() ||
        hoveredId.empty())
    {
        clearDwell();
        return;
    }

    if (collectionGroupTabDwellWidgetIndex_ !=
            hoveredGroup ||
        collectionGroupTabDwellId_ != hoveredId)
    {
        collectionGroupTabDwellWidgetIndex_ =
            hoveredGroup;
        collectionGroupTabDwellId_ =
            std::move(hoveredId);
        collectionGroupTabDwellTick_ =
            GetTickCount();
    }
    EnsureCollectionGroupTabDwellTimerArmed();
}

bool DesktopApp::TryActivateCollectionGroupTab(
    DWORD now)
{
    if (!dragSession_.IsActive() ||
        collectionGroupTabDwellWidgetIndex_ >=
            widgets_.size() ||
        collectionGroupTabDwellId_.empty())
    {
        CancelCollectionGroupTabDwell();
        return false;
    }
    if (now - collectionGroupTabDwellTick_ <
            kCollectionGroupTabDwellDelayMs)
        return false;

    if (IsPointInsideOpenPopup(lastMousePoint_))
    {
        CancelCollectionGroupTabDwell();
        return false;
    }

    DesktopWidget& data =
        widgets_[collectionGroupTabDwellWidgetIndex_];
    const bool collectionGroup =
        data.type == DesktopWidgetType::CollectionGroup;
    const bool fileGroup =
        data.type == DesktopWidgetType::FileGroup;
    if (!collectionGroup && !fileGroup)
    {
        CancelCollectionGroupTabDwell();
        return false;
    }

    WidgetContainer* groupedContainer = nullptr;
    for (auto& container : containers_)
    {
        auto* candidate =
            dynamic_cast<WidgetContainer*>(
                container.get());
        if (candidate &&
            candidate->GetWidgetData() == &data)
        {
            groupedContainer = candidate;
            break;
        }
    }
    if (!groupedContainer)
    {
        CancelCollectionGroupTabDwell();
        return false;
    }

    bool activated = false;
    if (collectionGroup)
    {
        auto* group =
            dynamic_cast<CollectionGroup*>(
                groupedContainer);
        const std::wstring id =
            collectionGroupTabDwellId_;
        if (group &&
            group->CategoryIdAtPoint(lastMousePoint_) == id &&
            std::find(data.childWidgetIds.begin(),
                data.childWidgetIds.end(), id) !=
                data.childWidgetIds.end())
        {
            data.activeCategoryId = id;
            data.scrollOffset = 0;
            group->InvalidateFilterCache();
            activated = true;
        }
    }
    else
    {
        auto* group =
            dynamic_cast<FileGroup*>(groupedContainer);
        constexpr std::wstring_view sourcePrefix =
            L"source:";
        constexpr std::wstring_view categoryPrefix =
            L"category:";
        if (group &&
            collectionGroupTabDwellId_.starts_with(
                sourcePrefix))
        {
            const std::wstring id =
                collectionGroupTabDwellId_.substr(
                    sourcePrefix.size());
            if (group->SourceIdAtPoint(lastMousePoint_) == id &&
                std::find(data.childWidgetIds.begin(),
                    data.childWidgetIds.end(), id) !=
                    data.childWidgetIds.end())
            {
                data.activeCategoryId = id;
                data.scrollOffset = 0;
                group->InvalidateHostedView();
                activated = true;
            }
        }
        else if (group &&
                 collectionGroupTabDwellId_.starts_with(
                    categoryPrefix))
        {
            const std::wstring id =
                collectionGroupTabDwellId_.substr(
                    categoryPrefix.size());
            ScrollingItemWidget* active =
                group->GetActiveSourceContainer();
            DesktopWidget* activeData = active
                ? active->GetWidgetData() : nullptr;
            if (activeData &&
                group->CategoryIdAtPoint(lastMousePoint_) == id)
            {
                activeData->activeCategoryId = id;
                data.scrollOffset = 0;
                group->InvalidateHostedView();
                activated = true;
            }
        }
    }
    if (!activated)
    {
        CancelCollectionGroupTabDwell();
        return false;
    }
    CancelCollectionGroupTabDwell();
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = {-1, -1};
    dragSession_.InvalidateStaticScene();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, FALSE);

    int mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        mods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        mods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        mods |= MK_SHIFT;
    RefreshDragTargetAt(lastMousePoint_, mods);
    return true;
}

// ── Collection popup ─────────────────────────────────────────

/**
 * @brief 在替换 Dock 文件夹弹窗前，将活动拖拽来源提升为稳定快照。
 *
 * Dock 文件夹弹窗使用临时 Widget、Container 和 Item 包装器。悬停打开
 * 另一个 Dock 弹窗时这些对象会被销毁，因此必须先复制被拖拽条目并一次性
 * 重绑定 DragSession 中的全部运行时指针。
 */
void DesktopApp::
ShowDockFolderPopupContextMenu(
    POINT screenPoint,
    std::optional<size_t> memberIndex)
{
    if (!dockFolderPopupOpen_) return;

    PrepareMenuIconsForPoint(screenPoint);

    const bool itemMenu =
        memberIndex &&
        *memberIndex <
            dockFolderPopupWidget_.
                folderEntries.size();
    const std::vector<std::wstring>
        selectedPaths =
            GetSelectedFolderEntryPaths();
    const bool singleSelection =
        selectedPaths.size() == 1;
    const bool hasSelection =
        !selectedPaths.empty();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    HMENU sortMenu = nullptr;
    HMENU nameMenu = nullptr;
    HMENU typeMenu = nullptr;
    HMENU dateMenu = nullptr;

    if (itemMenu)
    {
        AppendMenuW(
            menu,
            hasSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextOpenCommand,
            _LW("app.menu.open"));
        AppendMenuW(
            menu,
            hasSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextCopyPathCommand,
            _LW("app.menu.copy_path"));
        AppendMenuW(
            menu,
            singleSelection &&
                    snowdesktop::
                        item_location::
                            CanReveal(
                                selectedPaths.front())
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextRevealLocationCommand,
            _LW(
                "app.menu.open_file_location"));
        AppendMenuW(
            menu, MF_SEPARATOR,
            0, nullptr);
        AppendMenuW(
            menu,
            singleSelection &&
                    IsAdministratorRunnablePath(
                        selectedPaths.front())
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextRunAsAdministratorCommand,
            _LW("app.menu.run_as_administrator"));
        AppendMenuW(
            menu,
            singleSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextPropertiesCommand,
            _LW("app.menu.properties"));
        AppendMenuW(
            menu, MF_SEPARATOR,
            0, nullptr);
        AppendMenuW(
            menu,
            singleSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextRenameCommand,
            _LW("app.menu.rename"));
        AppendMenuW(
            menu, MF_SEPARATOR,
            0, nullptr);
        AppendMenuW(
            menu,
            hasSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextCutCommand,
            _LW("app.menu.cut"));
        AppendMenuW(
            menu,
            hasSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextCopyCommand,
            _LW("app.menu.copy"));
        AppendMenuW(
            menu,
            hasSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextDeleteCommand,
            _LW("app.settings.delete"));
        AppendMenuW(
            menu, MF_SEPARATOR,
            0, nullptr);
        AppendMenuW(
            menu,
            singleSelection
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextMoreCommand,
            _LW(
                "app.menu.more_options"));
    }
    else
    {
        ComPtr<IDataObject> clipObject;
        bool canPaste = false;
        if (dockFolderPopupAvailable_ &&
            SUCCEEDED(
                OleGetClipboard(
                    &clipObject)) &&
            clipObject)
        {
            FORMATETC format{
                CF_HDROP, nullptr,
                DVASPECT_CONTENT, -1,
                TYMED_HGLOBAL
            };
            canPaste = SUCCEEDED(
                clipObject->
                    QueryGetData(&format));
        }
        AppendMenuW(
            menu,
            canPaste
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextPasteCommand,
            _LW("app.menu.paste"));
        AppendMenuW(
            menu,
            dockFolderPopupAvailable_
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextNewMenu,
            _LW("app.menu.new"));
        AppendMenuW(
            menu,
            dockFolderPopupAvailable_
                ? MF_STRING
                : MF_STRING | MF_GRAYED,
            kContextMoreCommand,
            _LW("app.menu.more_options"));
        AppendMenuW(
            menu, MF_SEPARATOR,
            0, nullptr);

        sortMenu = CreatePopupMenu();
        nameMenu = CreatePopupMenu();
        typeMenu = CreatePopupMenu();
        dateMenu = CreatePopupMenu();
        if (sortMenu && nameMenu &&
            typeMenu && dateMenu)
        {
            auto addDirections = [](
                HMENU parent,
                HMENU child,
                UINT ascendingCommand,
                UINT descendingCommand,
                const wchar_t* label) {
                AppendMenuW(
                    child, MF_STRING,
                    ascendingCommand,
                    _LW(
                        "app.menu.sort_asc"));
                AppendMenuW(
                    child, MF_STRING,
                    descendingCommand,
                    _LW(
                        "app.menu.sort_desc"));
                AppendMenuW(
                    parent, MF_POPUP,
                    reinterpret_cast<
                        UINT_PTR>(child),
                    label);
            };
            addDirections(
                sortMenu, nameMenu,
                kContextWidgetSortByName,
                kContextWidgetSortByNameDesc,
                _LW("app.menu.sort_name"));
            addDirections(
                sortMenu, typeMenu,
                kContextWidgetSortByType,
                kContextWidgetSortByTypeDesc,
                _LW("app.menu.sort_type"));
            addDirections(
                sortMenu, dateMenu,
                kContextWidgetSortByDate,
                kContextWidgetSortByDateDesc,
                _LW(
                    "app.interact.sort_date"));
            AppendMenuW(
                menu, MF_POPUP,
                reinterpret_cast<UINT_PTR>(
                    sortMenu),
                _LW("app.menu.sort_by"));
        }
        else
        {
            if (sortMenu)
                DestroyMenu(sortMenu);
            if (nameMenu)
                DestroyMenu(nameMenu);
            if (typeMenu)
                DestroyMenu(typeMenu);
            if (dateMenu)
                DestroyMenu(dateMenu);
            sortMenu = nullptr;
            nameMenu = nullptr;
            typeMenu = nullptr;
            dateMenu = nullptr;
        }
    }

    SetMenuItemIcon(
        menu, kContextOpenCommand,
        L"");
    SetMenuItemIcon(
        menu,
        kContextRevealLocationCommand,
        L"");
    SetMenuItemIcon(
        menu, kContextCopyPathCommand,
        snowdesktop::menu_fluent_glyphs::kCopy,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(
        menu, kContextRunAsAdministratorCommand,
        snowdesktop::menu_fluent_glyphs::kShield,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(
        menu, kContextPropertiesCommand,
        snowdesktop::menu_fluent_glyphs::kInfo,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(
        menu, kContextRenameCommand,
        L"");
    SetMenuItemIcon(
        menu, kContextCutCommand,
        L"");
    SetMenuItemIcon(
        menu, kContextCopyCommand,
        L"");
    SetMenuItemIcon(
        menu, kContextDeleteCommand,
        L"");
    SetMenuItemIcon(
        menu, kContextPasteCommand,
        L"");
    SetMenuItemIcon(
        menu, kContextNewMenu,
        snowdesktop::menu_fluent_glyphs::kNewItem,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(
        menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions,
        MenuIconFont::FluentRegular);
    SetMenuItemQuickAction(menu, kContextRenameCommand);
    SetMenuItemQuickAction(menu, kContextCutCommand);
    SetMenuItemQuickAction(menu, kContextCopyCommand);
    SetMenuItemQuickAction(menu, kContextDeleteCommand);
    SetMenuItemQuickAction(menu, kContextPasteCommand);
    SetMenuItemQuickAction(menu, kContextNewMenu);
    if (sortMenu)
    {
        SetMenuItemIcon(
            menu,
            reinterpret_cast<UINT_PTR>(
                sortMenu),
            snowdesktop::menu_fluent_glyphs::kSort,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(sortMenu,
            reinterpret_cast<UINT_PTR>(nameMenu),
            snowdesktop::menu_fluent_glyphs::kSortName,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(nameMenu, kContextWidgetSortByName,
            snowdesktop::menu_fluent_glyphs::kSortNameAscending,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(nameMenu, kContextWidgetSortByNameDesc,
            snowdesktop::menu_fluent_glyphs::kSortNameDescending,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(sortMenu,
            reinterpret_cast<UINT_PTR>(typeMenu),
            snowdesktop::menu_fluent_glyphs::kSortType,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(typeMenu, kContextWidgetSortByType,
            snowdesktop::menu_fluent_glyphs::kSortTypeAscending,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(typeMenu, kContextWidgetSortByTypeDesc,
            snowdesktop::menu_fluent_glyphs::kSortTypeDescending,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(sortMenu,
            reinterpret_cast<UINT_PTR>(dateMenu),
            snowdesktop::menu_fluent_glyphs::kSortDate,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(dateMenu, kContextWidgetSortByDate,
            snowdesktop::menu_fluent_glyphs::kSortDateAscending,
            MenuIconFont::FluentRegular);
        SetMenuItemIcon(dateMenu, kContextWidgetSortByDateDesc,
            snowdesktop::menu_fluent_glyphs::kSortDateDescending,
            MenuIconFont::FluentRegular);
    }

    SetForegroundWindow(hwnd_);
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();

    switch (command)
    {
    case kContextOpenCommand:
        for (const auto& path :
             GetSelectedFolderEntryPaths())
            shellLaunchWorker_.Enqueue(
                hwnd_, path);
        break;
    case kContextRevealLocationCommand:
        if (selectedPaths.size() == 1)
            snowdesktop::item_location::
                Reveal(
                    hwnd_,
                    selectedPaths.front());
        break;
    case kContextCopyPathCommand:
        CopyPathsToClipboard(selectedPaths);
        break;
    case kContextRunAsAdministratorCommand:
        if (selectedPaths.size() == 1 &&
            IsAdministratorRunnablePath(selectedPaths.front()))
            RunPathAsAdministrator(selectedPaths.front());
        break;
    case kContextPropertiesCommand:
        if (selectedPaths.size() == 1)
            ShowPathProperties(selectedPaths.front());
        break;
    case kContextRenameCommand:
        if (singleSelection)
        {
            size_t selectedIndex =
                static_cast<size_t>(-1);
            for (size_t i = 0;
                i <
                    dockFolderPopupWidget_.
                        folderEntries.size();
                ++i)
            {
                if (dockFolderPopupWidget_.
                        folderEntries[i].
                            selected)
                {
                    selectedIndex = i;
                    break;
                }
            }
            if (selectedIndex <
                dockFolderPopupWidget_.
                    folderEntries.size())
                BeginRenameDockFolderPopupEntry(
                    selectedIndex);
        }
        break;
    case kContextCutCommand:
        CopyCutSelectedFolderEntries(
            true);
        break;
    case kContextCopyCommand:
        CopyCutSelectedFolderEntries(
            false);
        break;
    case kContextDeleteCommand:
        DeleteSelectedFolderEntries(
            false);
        break;
    case kContextMoreCommand:
        if (itemMenu &&
            selectedPaths.size() == 1)
        {
            ShowShellItemContextMenuForPath(
                selectedPaths.front(),
                screenPoint);
            break;
        }
        ShowShellContextMenuForPath(
            dockFolderPopupWidget_.
                sourceFolderPath,
            screenPoint);
        RefreshDockFolderPopup();
        break;
    case kContextPasteCommand:
        PasteClipboardToFolderPath(
            dockFolderPopupWidget_.
                sourceFolderPath);
        break;
    case kContextNewMenu:
        ShowNewMenuAndInvoke(
            screenPoint,
            dockFolderPopupWidget_.
                sourceFolderPath);
        for (size_t i = 0;
            i < widgets_.size(); ++i)
        {
            if (widgets_[i].type ==
                DesktopWidgetType::
                    FolderMapping)
                RefreshFolderMappingWidget(
                    i);
        }
        RefreshDockFolderPopup();
        break;
    case kContextWidgetSortByName:
        SortDockFolderPopupContents(
            snowdesktop::
                folder_sort_rules::kName,
            true);
        break;
    case kContextWidgetSortByNameDesc:
        SortDockFolderPopupContents(
            snowdesktop::
                folder_sort_rules::kName,
            false);
        break;
    case kContextWidgetSortByType:
        SortDockFolderPopupContents(
            snowdesktop::
                folder_sort_rules::kType,
            true);
        break;
    case kContextWidgetSortByTypeDesc:
        SortDockFolderPopupContents(
            snowdesktop::
                folder_sort_rules::kType,
            false);
        break;
    case kContextWidgetSortByDate:
        SortDockFolderPopupContents(
            snowdesktop::
                folder_sort_rules::
                    kModified,
            true);
        break;
    case kContextWidgetSortByDateDesc:
        SortDockFolderPopupContents(
            snowdesktop::
                folder_sort_rules::
                    kModified,
            false);
        break;
    default:
        break;
    }
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
}
