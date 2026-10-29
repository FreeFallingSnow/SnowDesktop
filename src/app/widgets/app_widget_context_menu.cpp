#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "widgets/single_cell_widget_rules.h"
#include "app/menus/sort_menu.h"
#include "platform/demo_collection_rules.h"
#include "layout/collection_titleless_rules.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "ui/menu/right_click_contract.h"
#include "widget/packages/widget_removal.h"
#include "widgets/collection_group_rules.h"
#include "widgets/file_category_auto_collect_rules.h"
#include "widgets/storage_title_bar_layout.h"
#include "settings/control_native_widget_settings.h"

#include <commctrl.h>
#include <functional>

// Widget editor, group-tab and generic widget context menus.

namespace
{
constexpr UINT kDockChooseIcon = 43770;
constexpr UINT kDockRestoreIcon = 43771;
constexpr UINT kDockBeautifyIcon = 43786, kDockInheritIcon = 43787;
bool ShowWidgetRemovalConfirmation(HWND owner, POINT anchor,
    const std::wstring& widgetTitle)
{
    const auto content = _LFW("app.widget.removal.content", widgetTitle);
    const TASKDIALOG_BUTTON buttons[] = {
        { IDYES, _LW("app.settings.delete") },
        { IDCANCEL, _LW("app.settings.cancel") },
    };
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = owner;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle = L"SnowDesktop";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszMainInstruction = _LW("app.widget.removal.title");
    dialog.pszContent = content.c_str();
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(&anchor);
    dialog.pfCallback = +[](HWND window, UINT notification,
        WPARAM, LPARAM, LONG_PTR data) -> HRESULT {
        if (notification != TDN_CREATED) return S_OK;
        const auto point = *reinterpret_cast<const POINT*>(data);
        RECT rect{};
        MONITORINFO monitor{ sizeof(monitor) };
        UINT flags = SWP_NOSIZE | SWP_SHOWWINDOW;
        int x = 0;
        int y = 0;
        if (GetWindowRect(window, &rect) && GetMonitorInfoW(
                MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor))
        {
            const auto position = snowdesktop::widget_runtime::
                CenterConsentDialogInWorkArea(monitor.rcWork.left,
                    monitor.rcWork.top, monitor.rcWork.right,
                    monitor.rcWork.bottom, rect.right - rect.left,
                    rect.bottom - rect.top);
            x = position.x;
            y = position.y;
        }
        else flags |= SWP_NOMOVE;
        SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0, flags);
        snowdesktop::private_pointer::SetInputForeground(window);
        return S_OK;
    };
    int selected = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)) &&
        selected == IDYES;
}
}

void DesktopApp::ShowWidgetEditorHost(size_t widgetIndex)
{
    if (!settingsWindow_ || widgetIndex >= widgets_.size()) return;
    const auto& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::LuaScript) return;
    ShowSettingsWindow(snowdesktop::SettingsRoute::ForWidget(widget.id));
}

/**
 * @brief 显示窗口小部件的右键上下文菜单
 * @param screenPoint 屏幕坐标点
 * @param widgetIndex 小部件索引
 */
void DesktopApp::ShowCollectionGroupTabContextMenu(
    POINT screenPoint, size_t groupIndex,
    const std::wstring& collectionId)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::CollectionGroup)
        return;
    const size_t childIndex =
        FindWidgetIndexById(collectionId);
    if (childIndex >= widgets_.size() ||
        widgets_[childIndex].type !=
            DesktopWidgetType::Collection ||
        std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            collectionId) ==
            widgets_[groupIndex].childWidgetIds.end())
        return;

    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(
        menu, MF_STRING,
        kContextWidgetRename,
        _LW("app.menu.rename"));
    SetMenuItemIcon(
        menu, kContextWidgetRename, L"\U000F0A39",
        MenuIconFont::FluentRegular);
    RestoreInteractionInputFocus();
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command == kContextWidgetRename)
    {
        ClearSelection();
        widgets_[childIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        auto it = std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            collectionId);
        keyboardNavMemberIndex_ =
            it == widgets_[groupIndex].childWidgetIds.end()
                ? 0
                : static_cast<int>(
                    std::distance(
                        widgets_[groupIndex].childWidgetIds.begin(),
                        it));
        keyboardNavCollectionGroupTabs_ = true;
        BeginRenameSelected();
    }
    else
    {
        RestoreInteractionInputFocus();
    }
}

void DesktopApp::ShowFileGroupSourceTabContextMenu(
    POINT screenPoint, size_t groupIndex,
    const std::wstring& childId)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::FileGroup)
        return;
    const size_t childIndex =
        FindWidgetIndexById(childId);
    if (childIndex >= widgets_.size() ||
        (widgets_[childIndex].type !=
             DesktopWidgetType::FileCategories &&
         widgets_[childIndex].type !=
             DesktopWidgetType::FolderMapping) ||
        std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            childId) ==
            widgets_[groupIndex].childWidgetIds.end())
        return;

    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING,
        kContextWidgetRename,
        _LW("app.menu.rename"));
    SetMenuItemIcon(
        menu, kContextWidgetRename, L"\U000F0A39",
        MenuIconFont::FluentRegular);
    RestoreInteractionInputFocus();
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command == kContextWidgetRename)
    {
        ClearSelection();
        widgets_[childIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        const auto it = std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            childId);
        keyboardNavMemberIndex_ =
            it == widgets_[groupIndex].childWidgetIds.end()
                ? 0
                : static_cast<int>(
                    std::distance(
                        widgets_[groupIndex].
                            childWidgetIds.begin(), it));
        keyboardNavCollectionGroupTabs_ = true;
        BeginRenameSelected();
    }
    else
    {
        RestoreInteractionInputFocus();
    }
}

void DesktopApp::ShowLuaLogicalSlotItemContextMenu(
    POINT screenPoint, const std::wstring& widgetId,
    const std::string& slotId, const std::string& itemId,
    bool collection, size_t itemIndex, size_t itemCount,
    bool canRemove)
{
    if (!widgetEngine_) return;
    PrepareMenuIconsForPoint(screenPoint);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (collection)
    {
        AppendMenuW(menu,
            MF_STRING | (itemIndex == 0 ? MF_GRAYED : 0),
            kContextLuaLogicalSlotMovePrevious,
            _LW("app.interact.logical_slot_move_previous"));
        AppendMenuW(menu,
            MF_STRING |
                (itemIndex + 1 >= itemCount ? MF_GRAYED : 0),
            kContextLuaLogicalSlotMoveNext,
            _LW("app.interact.logical_slot_move_next"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        SetMenuItemIcon(menu, kContextLuaLogicalSlotMovePrevious,
            L"\uE74A", MenuIconFont::FluentRegular);
        SetMenuItemIcon(menu, kContextLuaLogicalSlotMoveNext,
            L"\uE74B", MenuIconFont::FluentRegular);
    }
    AppendMenuW(menu, MF_STRING | (canRemove ? 0 : MF_GRAYED),
        kContextLuaLogicalSlotRemove,
        _LW("app.interact.logical_slot_remove"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING,
        kContextWidgetOpenComponentPanel,
        _LW("app.interact.open_component_panel"));
    SetMenuItemIcon(menu, kContextLuaLogicalSlotRemove,
        L"\uF34C", MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextWidgetOpenComponentPanel,
        snowdesktop::menu_fluent_glyphs::kChevronRight,
        MenuIconFont::FluentRegular);

    RestoreInteractionInputFocus();
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    if (command == kContextWidgetOpenComponentPanel)
    {
        const size_t widgetIndex = FindWidgetIndexById(widgetId);
        if (widgetIndex < widgets_.size())
        {
            ShowWidgetContextMenu(screenPoint, widgetIndex,
                std::nullopt, std::nullopt, "desktop", true);
        }
        else
        {
            RestoreInteractionInputFocus();
        }
        return;
    }

    snowdesktop::widget_runtime::LogicalSlotChange change;
    std::string error;
    bool handled = false;
    bool succeeded = true;
    if (command == kContextLuaLogicalSlotMovePrevious && itemIndex > 0)
    {
        handled = true;
        succeeded = widgetEngine_->RuntimeMoveHostLogicalSlotItem(
            widgetId, slotId, itemId, itemIndex - 1, change, error);
    }
    else if (command == kContextLuaLogicalSlotMoveNext &&
        itemIndex + 1 < itemCount)
    {
        handled = true;
        succeeded = widgetEngine_->RuntimeMoveHostLogicalSlotItem(
            widgetId, slotId, itemId, itemIndex + 1, change, error);
    }
    else if (command == kContextLuaLogicalSlotRemove && canRemove)
    {
        handled = true;
        succeeded = widgetEngine_->RuntimeRemoveHostLogicalSlotItem(
            widgetId, slotId, itemId, change, error);
    }
    if (handled && !succeeded)
    {
        widgetEngine_->RuntimeRecordError(widgetId,
            "logical slot host menu: " + error);
        MessageBeep(MB_ICONWARNING);
    }
    if (handled) InvalidateRect(hwnd_, nullptr, FALSE);
    RestoreInteractionInputFocus();
}

void DesktopApp::ShowWidgetContextMenu(
    POINT screenPoint, size_t widgetIndex,
    std::optional<RECT> dockRenameAnchor,
    std::optional<POINT> luaLocalPoint,
    std::string_view luaSurface,
    bool forceComponentMenu)
{
    if (widgetIndex >= widgets_.size()) return;
    PrepareMenuIconsForPoint(screenPoint);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const auto setFluentIcon = [this](
        HMENU targetMenu, UINT_PTR item, const wchar_t* glyph) {
        SetMenuItemIcon(targetMenu, item, glyph,
            MenuIconFont::FluentRegular);
    };

    const auto& widget = widgets_[widgetIndex];
    size_t effectiveSourceIndex = widgetIndex;
    const auto menuWidgetId = widget.id;
    const auto applyNativeSettings = [&](snowdesktop::control::Value changes) {
        try
        {
            const auto state = CaptureNativeWidgetSettings(menuWidgetId);
            const auto patch = snowdesktop::control_settings::ParseNativeWidgetSettingsPatch(state, changes);
            const bool saved = ApplyNativeWidgetSettings(state, patch);
            if (!saved) MessageBeep(MB_ICONWARNING);
            return saved;
        }
        catch (const snowdesktop::control::Error&) { MessageBeep(MB_ICONWARNING); return false; }
    };
    const auto menuPackageId = widget.packageId;
    const auto menuWidgetType = widget.type;
    const bool dockIconMenu = ((dockRenameAnchor.has_value() && widget.gridCell.pageId == kDockPageId) ||
        (snowdesktop::single_cell_widget::IsSingleCell(widget) && widget.singleCellIconMode)) &&
        (widget.type == DesktopWidgetType::Collection || widget.type == DesktopWidgetType::FileCategories ||
         widget.type == DesktopWidgetType::CollectionGroup || widget.type == DesktopWidgetType::FileGroup ||
         widget.type == DesktopWidgetType::FolderMapping);
    if (widget.type == DesktopWidgetType::FileGroup)
    {
        // The group displays its first visible source while no active source is
        // stored, so menu source options must resolve to that displayed source
        // instead of falling back to the group itself.
        for (const auto& container : containers_)
        {
            const auto* group = dynamic_cast<const FileGroup*>(container.get());
            if (!group || group->GetWidgetData() != &widget) continue;
            effectiveSourceIndex =
                FindWidgetIndexById(group->GetActiveSourceId());
            break;
        }
        if (effectiveSourceIndex >= widgets_.size())
            effectiveSourceIndex =
                FindWidgetIndexById(widget.activeCategoryId);
        if (effectiveSourceIndex >= widgets_.size())
        {
            for (const auto& childId : widget.childWidgetIds)
            {
                const size_t childIndex = FindWidgetIndexById(childId);
                if (childIndex < widgets_.size() &&
                    (widgets_[childIndex].type ==
                         DesktopWidgetType::FileCategories ||
                     widgets_[childIndex].type ==
                         DesktopWidgetType::FolderMapping))
                {
                    effectiveSourceIndex = childIndex;
                    break;
                }
            }
        }
        if (effectiveSourceIndex >= widgets_.size())
            effectiveSourceIndex = widgetIndex;
    }
    const DesktopWidget& effectiveSource =
        widgets_[effectiveSourceIndex];
    size_t contentSortTargetIndex = effectiveSourceIndex;
    if (widget.type == DesktopWidgetType::FileGroup)
    {
        for (const auto& container : containers_)
        {
            const auto* group = dynamic_cast<const FileGroup*>(
                container.get());
            if (group && group->GetWidgetData() == &widget &&
                group->IsGroupSearchActive())
            {
                contentSortTargetIndex = widgetIndex;
                break;
            }
        }
    }
    const auto& contentView = *ResolveWidgetContentViewData(&widgets_[widgetIndex]);
    const auto statusLabel = [](const wchar_t* title,
                                const wchar_t* status) {
        std::wstring label = title;
        label += L"\t";
        label += status;
        return label;
    };
    const auto visibilityLabel = [&](const wchar_t* title,
                                     bool visible) {
        return statusLabel(title,
            visible
                ? _LW("app.interact.shown")
                : _LW("app.interact.hidden"));
    };
    const std::wstring displayTypeLabel = statusLabel(
        _LW("app.interact.display_type"),
        contentView.listMode
            ? _LW("app.interact.list_view_state")
            : _LW("app.interact.icon_view_state"));
    HMENU detailsMenu = nullptr;
    HMENU autoCollectMenu = nullptr;
    std::vector<std::wstring> autoCollectCategories;
    const auto appendAutoCollectMenu = [&]() {
        autoCollectMenu = CreatePopupMenu();
        if (!autoCollectMenu) return;
        AppendMenuW(autoCollectMenu,
            MF_STRING | (effectiveSource.autoCollect &&
                effectiveSource.autoCollectCategoryIds.empty() ? MF_CHECKED : 0),
            kContextWidgetAutoCollectAll, _LW("app.interact.auto_collect_all"));
        setFluentIcon(autoCollectMenu, kContextWidgetAutoCollectAll,
            snowdesktop::menu_fluent_glyphs::kCollectItems);
        AppendMenuW(autoCollectMenu, MF_SEPARATOR, 0, nullptr);
        for (const auto& category : GetCategoryOrder(categorySettings_))
        {
            if (category == L"all") continue;
            const UINT commandId = kContextWidgetAutoCollectCategoryFirst +
                static_cast<UINT>(autoCollectCategories.size());
            if (commandId > kContextWidgetAutoCollectCategoryLast) break;
            autoCollectCategories.push_back(category);
            const bool checked = effectiveSource.autoCollect &&
                snowdesktop::file_category_auto_collect_rules::Matches(
                    effectiveSource, category);
            AppendMenuW(autoCollectMenu, MF_STRING | (checked ? MF_CHECKED : 0),
                commandId, GetCategoryLabel(categorySettings_, category).c_str());
            setFluentIcon(autoCollectMenu, commandId,
                snowdesktop::menu_fluent_glyphs::kCategoryBar);
        }
        AppendMenuW(autoCollectMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(autoCollectMenu,
            MF_STRING | (!effectiveSource.autoCollect ? MF_CHECKED : 0),
            kContextWidgetToggleAutoCollect, _LW("app.interact.auto_collect_off"));
        setFluentIcon(autoCollectMenu, kContextWidgetToggleAutoCollect,
            snowdesktop::menu_fluent_glyphs::kAutoCollect);
        const auto label = statusLabel(_LW("app.interact.auto_collect"),
            effectiveSource.autoCollect ? _LW("app.interact.on") : _LW("app.interact.off"));
        AppendMenuW(menu, MF_POPUP,
            reinterpret_cast<UINT_PTR>(autoCollectMenu), label.c_str());
        setFluentIcon(menu, reinterpret_cast<UINT_PTR>(autoCollectMenu),
            snowdesktop::menu_fluent_glyphs::kAutoCollect);
    };
    const auto appendDetailsMenu = [&]() {
        HMENU columnsMenu = CreatePopupMenu();
        if (columnsMenu)
        {
            const int selected = snowdesktop::list_detail_rules::ListColumns(contentView);
            for (int columns = 1; columns <= 4; ++columns)
            {
                const UINT id = kContextWidgetListColumnsFirst + columns - 1;
                AppendMenuW(columnsMenu, MF_STRING | (columns == selected ? MF_CHECKED : 0),
                    id, std::to_wstring(columns).c_str());
            }
            AppendMenuW(menu, MF_POPUP | (contentView.listMode ? 0 : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(columnsMenu), _LW("app.interact.list_columns"));
            setFluentIcon(menu, reinterpret_cast<UINT_PTR>(columnsMenu),
                snowdesktop::menu_fluent_glyphs::kContentLayout);
        }
        detailsMenu = CreatePopupMenu();
        if (!detailsMenu) return;
        AppendMenuW(detailsMenu,
            MF_STRING | MF_CHECKED | MF_GRAYED,
            kContextWidgetDetailName,
            _LW("widget.details.name"));
        AppendMenuW(detailsMenu,
            MF_STRING |
                (contentView.detailShowModified ? MF_CHECKED : 0),
            kContextWidgetDetailModified,
            _LW("widget.details.modified"));
        AppendMenuW(detailsMenu,
            MF_STRING |
                (contentView.detailShowType ? MF_CHECKED : 0),
            kContextWidgetDetailType,
            _LW("widget.details.type"));
        AppendMenuW(detailsMenu,
            MF_STRING |
                (contentView.detailShowSize ? MF_CHECKED : 0),
            kContextWidgetDetailSize,
            _LW("widget.details.size"));
        AppendMenuW(menu,
            MF_POPUP | (contentView.listMode ? 0 : MF_GRAYED),
            reinterpret_cast<UINT_PTR>(detailsMenu),
            _LW("app.interact.show_details"));
    };
    const std::wstring categoryVisibilityLabel = visibilityLabel(
        _LW("app.interact.file_categories"),
        contentView.showFileCategories);
    const std::wstring searchVisibilityLabel = visibilityLabel(
        _LW("app.interact.search_box"),
        widget.showSearchBox);
    const std::wstring dateVisibilityLabel = visibilityLabel(
        _LW("app.interact.date_header"),
        contentView.dateHeaders);
    const std::wstring collectionModeLabel = statusLabel(
        _LW("app.interact.display_mode"),
        widget.scrollContainerMode
            ? _LW("app.interact.popup_container")
            : _LW("app.interact.large_folder"));
    const auto appendFanToggle = [&]() {
        if (widget.gridCell.pageId != kDockPageId && popupWidgetIndex_ != widgetIndex) return;
        const auto label = statusLabel(_LW("app.interact.popup_fan"),
            widget.fanPopup ? _LW("app.interact.on") : _LW("app.interact.off"));
        AppendMenuW(menu, MF_STRING, kContextPopupFan, label.c_str());
    };
    std::vector<LuaWidgetMenuItem> luaMenuItems;
    std::vector<LuaWidgetMenuItem> luaMenuActions;
    auto luaMenuScope = snowdesktop::right_click_contract::
        LuaWidgetMenuScope::Widget;
    bool luaElementMenu = false;
    std::optional<POINT> resolvedLuaLocalPoint;
    HMENU demoCategoryMenu = nullptr;

    if (snowdesktop::single_cell_widget::IsSingleCell(widget) && widget.gridCell.pageId != kDockPageId)
    {
        const auto label = statusLabel(_LW("app.interact.single_cell_display"),
            widget.singleCellIconMode ? _LW("app.interact.single_cell_icon")
                : _LW("app.interact.single_cell_component"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleSingleCellMode, label.c_str());
        setFluentIcon(menu, kContextWidgetToggleSingleCellMode,
            snowdesktop::menu_fluent_glyphs::kCollection);
    }

    if (widget.type == DesktopWidgetType::Collection)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpen, _LW("app.interact.open_all"));
        appendFanToggle();
        if (generalSettings_.demoModeEnabled &&
            demoIdentityAssetsAvailable_)
        {
            demoCategoryMenu = CreatePopupMenu();
            if (demoCategoryMenu)
            {
                UINT flags = MF_STRING;
                if (widget.demoIconCategory.empty()) flags |= MF_CHECKED;
                AppendMenuW(demoCategoryMenu, flags,
                    kContextWidgetDemoCategoryFirst,
                    _LW("app.demo_category.auto"));
                for (size_t index = 0;
                    index < snowdesktop::demo_collection_rules::
                        kCategories.size(); ++index)
                {
                    const auto& category =
                        snowdesktop::demo_collection_rules::
                            kCategories[index];
                    flags = MF_STRING;
                    if (snowdesktop::demo_collection_rules::
                            EqualsAsciiInsensitive(
                                widget.demoIconCategory, category.id))
                        flags |= MF_CHECKED;
                    AppendMenuW(demoCategoryMenu, flags,
                        kContextWidgetDemoCategoryFirst + 1 +
                            static_cast<UINT>(index),
                        _LW(category.titleKey));
                }
                AppendMenuW(menu, MF_POPUP,
                    reinterpret_cast<UINT_PTR>(demoCategoryMenu),
                    _LW("app.demo_category.icon_category"));
            }
        }
        const bool compactCollection =
            widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1;
        if (!compactCollection)
        {
            AppendMenuW(menu, MF_STRING,
                kContextWidgetToggleCollectionMode,
                collectionModeLabel.c_str());
        }
        // Every collection can open a popup, including compact and large-folder
        // collections whose inline content keeps its own grid presentation.
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        if (effectiveSource.type ==
            DesktopWidgetType::FileCategories)
        {
            AppendMenuW(menu,
                HasPasteableFileClipboardData()
                    ? MF_STRING : MF_STRING | MF_GRAYED,
                kContextPasteCommand, _LW("app.menu.paste"));
            AppendMenuW(menu, MF_STRING,
                kContextNewMenu, _LW("app.menu.new"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING,
                kContextWidgetManualCollect,
                _LW("app.interact.collect_now"));
            appendAutoCollectMenu();
        }
        else if (effectiveSource.type ==
                 DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_STRING,
                kContextWidgetOpenFolder,
                _LW("app.interact.open_folder"));
            AppendMenuW(menu,
                HasPasteableFileClipboardData()
                    ? MF_STRING : MF_STRING | MF_GRAYED,
                kContextPasteCommand, _LW("app.menu.paste"));
        }
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleFileCategories,
            categoryVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleDateGroup,
            dateVisibilityLabel.c_str());
        if (effectiveSource.type ==
            DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_STRING,
                kContextNewMenu, _LW("app.menu.new"));
            AppendMenuW(menu, MF_STRING,
                kContextMoreCommand,
                _LW("app.menu.more_options"));
        }
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        AppendMenuW(menu,
            HasPasteableFileClipboardData()
                ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextPasteCommand, _LW("app.menu.paste"));
        AppendMenuW(menu, MF_STRING,
            kContextNewMenu, _LW("app.menu.new"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextWidgetManualCollect, _LW("app.interact.collect_now"));
        appendAutoCollectMenu();
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleFileCategories,
            categoryVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleDateGroup,
            dateVisibilityLabel.c_str());
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpenFolder, _LW("app.interact.open_folder"));
        appendFanToggle();
        AppendMenuW(menu,
            HasPasteableFileClipboardData()
                ? MF_STRING : MF_STRING | MF_GRAYED,
            kContextPasteCommand, _LW("app.menu.paste"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
            displayTypeLabel.c_str());
        appendDetailsMenu();
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleFileCategories,
            categoryVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            searchVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleDateGroup,
            dateVisibilityLabel.c_str());
        AppendMenuW(menu, MF_STRING, kContextNewMenu, _LW("app.menu.new"));
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, _LW("app.menu.more_options"));
    }
    else if (widget.type == DesktopWidgetType::LuaScript)
    {
        if (widgetEngine_)
        {
            widgetEngine_->EnsureWidgetLoaded(widget.id, widget.packageId);
            POINT clientPoint = screenPoint;
            ScreenToClient(hwnd_, &clientPoint);
            const RECT frame = GetStandaloneWidgetFrameRect(widget);
            const POINT localPoint = luaLocalPoint.value_or(POINT{
                clientPoint.x - frame.left,
                clientPoint.y - frame.top });
            resolvedLuaLocalPoint = localPoint;
            luaMenuItems = widgetEngine_->GetContextMenu(widget.id,
                localPoint.x, localPoint.y, luaSurface,
                forceComponentMenu);
            const bool hasElementAction =
                snowdesktop::right_click_contract::
                    HasLuaElementMenuAction(luaMenuItems);
            luaMenuScope = snowdesktop::right_click_contract::
                ResolveLuaWidgetMenuScope(hasElementAction);
            luaElementMenu = luaMenuScope ==
                snowdesktop::right_click_contract::
                    LuaWidgetMenuScope::Element;
            if (!luaElementMenu)
            {
                AppendMenuW(menu, MF_STRING, kContextWidgetEdit,
                    _LW("app.interact.detailed_settings"));
            }
            const auto setLuaItemIcon = [this, &widget](HMENU targetMenu,
                                                 UINT_PTR itemId,
                                                 const LuaWidgetMenuItem& item) {
                if (!item.imageResourceName.empty())
                {
                    const auto* source = widgetEngine_->
                        RuntimeFindPackageImageSource(widget.id,
                            item.imageResourceName);
                    if (source)
                        SetMenuItemImage(targetMenu, itemId, *source);
                    return;
                }
                if (item.icon.empty()) return;
                const std::wstring icon = Utf8ToWide(item.icon);
                const bool useFluent =
                    _stricmp(item.iconFont.c_str(), "fluent") == 0 ||
                    _stricmp(item.iconFont.c_str(), "fluent-regular") == 0;
                SetMenuItemIcon(targetMenu, itemId, icon.c_str(),
                    useFluent
                        ? MenuIconFont::FluentRegular
                        : MenuIconFont::FontAwesomeSolid);
            };
            std::function<void(HMENU,
                const std::vector<LuaWidgetMenuItem>&)> appendLuaMenuItems;
            appendLuaMenuItems = [&](HMENU targetMenu,
                                     const std::vector<LuaWidgetMenuItem>& items) {
                for (const auto& item : items)
                {
                    if (item.separator)
                    {
                        AppendMenuW(targetMenu, MF_SEPARATOR, 0, nullptr);
                        continue;
                    }
                    UINT flags = (item.enabled ? 0 : MF_GRAYED) |
                        (item.checked ? MF_CHECKED : 0);
                    if (!item.children.empty())
                    {
                        HMENU submenu = CreatePopupMenu();
                        if (!submenu) continue;
                        appendLuaMenuItems(submenu, item.children);
                        const UINT_PTR submenuId =
                            reinterpret_cast<UINT_PTR>(submenu);
                        if (!AppendMenuW(targetMenu, flags | MF_POPUP,
                                submenuId,
                                Utf8ToWide(item.label).c_str()))
                        {
                            DestroyMenu(submenu);
                            continue;
                        }
                        setLuaItemIcon(targetMenu, submenuId, item);
                        continue;
                    }
                    if (kContextLuaWidgetMenuFirst +
                            static_cast<UINT>(luaMenuActions.size()) >
                        kContextLuaWidgetMenuLast)
                        continue;
                    const UINT commandId = kContextLuaWidgetMenuFirst +
                        static_cast<UINT>(luaMenuActions.size());
                    if (!AppendMenuW(targetMenu, flags | MF_STRING,
                            commandId, Utf8ToWide(item.label).c_str()))
                        continue;
                    setLuaItemIcon(targetMenu, commandId, item);
                    luaMenuActions.push_back(item);
                }
            };
            appendLuaMenuItems(menu, luaMenuItems);
            if (snowdesktop::right_click_contract::
                    ShouldOfferComponentPanelShortcut(luaMenuScope))
            {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING,
                    kContextWidgetOpenComponentPanel,
                    _LW("app.interact.open_component_panel"));
            }
        }
    }

    HMENU sortMenu = nullptr;
    if (widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::Collection ||
        widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup)
    {
        sortMenu = snowdesktop::sort_menu::Create(
            [](const char* key) { return _LW(key); });
        if (sortMenu)
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), _LW("app.menu.sort_by"));
    }

    const UINT hoverToggleCommand = widget.showOnHoverOnly
        ? kContextWidgetShowOnHoverOff
        : kContextWidgetShowOnHoverOn;
    const UINT keepToggleCommand = widget.keepWhenDesktopHidden
        ? kContextWidgetKeepWhenHiddenOff
        : kContextWidgetKeepWhenHiddenOn;
    const UINT privacyToggleCommand = widget.privacyMode
        ? kContextWidgetPrivacyModeOff
        : kContextWidgetPrivacyModeOn;
    const bool showLargeFolderTitlelessOption =
        widget.type == DesktopWidgetType::Collection &&
        snowdesktop::collection_titleless_rules::
            IsLargeFolderMode(
                widget.scrollContainerMode,
                widget.gridSpan.columns,
                widget.gridSpan.rows);
    const bool showExpandOnHoverOption = snowdesktop::storage_title_bar::UsesTop(
        widget, CurrentPersonalization().scrollableTitleBarOnTop);
    if (!luaElementMenu)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (CanRenameWidget(widget))
        {
            AppendMenuW(menu, MF_STRING, kContextWidgetRename, _LW("app.menu.rename"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        auto toggleLabel = [](const wchar_t* title, bool enabled) {
            std::wstring label = title;
            label += L"\t";
            label += enabled
                ? _LW("app.interact.on")
                : _LW("app.interact.off");
            return label;
        };
        const std::wstring hoverLabel = toggleLabel(
            _LW("app.interact.hover_only"), widget.showOnHoverOnly);
        const std::wstring keepLabel = toggleLabel(
            _LW("app.interact.keep_when_hidden"),
            widget.keepWhenDesktopHidden);
        AppendMenuW(menu, MF_STRING, hoverToggleCommand, hoverLabel.c_str());
        if (showExpandOnHoverOption)
        {
            const std::wstring expandLabel = toggleLabel(
                _LW("app.interact.expand_on_hover"), widget.titleBarExpandOnHover);
            AppendMenuW(menu, MF_STRING, kContextWidgetToggleExpandOnHover,
                expandLabel.c_str());
        }
        AppendMenuW(menu, MF_STRING, keepToggleCommand, keepLabel.c_str());
        if (widget.type == DesktopWidgetType::Collection ||
            widget.type == DesktopWidgetType::FileCategories ||
            widget.type == DesktopWidgetType::FolderMapping ||
            widget.type == DesktopWidgetType::CollectionGroup ||
            widget.type == DesktopWidgetType::FileGroup)
        {
            const std::wstring privacyLabel = toggleLabel(
                _LW("app.interact.privacy_mode"), widget.privacyMode);
            AppendMenuW(menu, MF_STRING,
                privacyToggleCommand, privacyLabel.c_str());
        }
        if (showLargeFolderTitlelessOption)
        {
            const std::wstring titlelessLabel = toggleLabel(
                _LW("app.interact.large_folder_titleless"),
                widget.largeFolderTitleless);
            AppendMenuW(menu, MF_STRING,
                kContextWidgetToggleLargeFolderTitleless,
                titlelessLabel.c_str());
        }
        AppendMenuW(menu, MF_STRING, kContextWidgetDelete,
            _LW("app.interact.delete_widget"));
    }

    setFluentIcon(menu, kContextPopupFan, snowdesktop::menu_fluent_glyphs::kFanExpansion);
    setFluentIcon(menu, kContextWidgetOpen, L"\uF582");
    setFluentIcon(menu, kContextWidgetManualCollect,
        snowdesktop::menu_fluent_glyphs::kCollectItems);
    setFluentIcon(menu, kContextWidgetToggleListMode,
        snowdesktop::menu_fluent_glyphs::kContentLayout);
    if (detailsMenu)
        setFluentIcon(menu,
            reinterpret_cast<UINT_PTR>(detailsMenu), L"\uF168");
    setFluentIcon(menu, kContextWidgetToggleDateGroup,
        snowdesktop::menu_fluent_glyphs::kDateHeader);
    setFluentIcon(menu, kContextWidgetOpenFolder, L"\uF42E");
    setFluentIcon(menu, kContextWidgetToggleFileCategories,
        snowdesktop::menu_fluent_glyphs::kCategoryBar);
    setFluentIcon(menu, kContextWidgetToggleSearchBox, L"\uF68F");
    SetMenuItemIcon(menu, kContextPasteCommand, L"");
    setFluentIcon(menu, kContextNewMenu,
        snowdesktop::menu_fluent_glyphs::kNewItem);
    setFluentIcon(menu, kContextMoreCommand,
        snowdesktop::menu_fluent_glyphs::kMoreOptions);
    setFluentIcon(menu, kContextWidgetEdit, L"\uF6A9");
    setFluentIcon(menu, kContextWidgetOpenComponentPanel,
        snowdesktop::menu_fluent_glyphs::kChevronRight);
    setFluentIcon(menu, kContextWidgetRename, L"\U000F0A39");
    setFluentIcon(menu, kContextWidgetDelete, L"\uF34C");
    SetMenuItemQuickAction(menu, kContextWidgetEdit);
    SetMenuItemQuickAction(menu, kContextNewMenu);
    SetMenuItemQuickAction(menu, kContextPasteCommand);
    SetMenuItemQuickAction(menu, kContextWidgetRename);
    SetMenuItemQuickAction(menu, kContextWidgetDelete);
    setFluentIcon(menu, hoverToggleCommand, L"\uE5F2");
    if (showExpandOnHoverOption)
        setFluentIcon(menu, kContextWidgetToggleExpandOnHover,
            snowdesktop::menu_fluent_glyphs::kChevronRight);
    setFluentIcon(menu, keepToggleCommand, L"\uF359");
    if (widget.type == DesktopWidgetType::Collection ||
        widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup)
    {
        setFluentIcon(menu, privacyToggleCommand,
            widget.privacyMode ? L"\uE78F" : L"\uE795");
    }
    if (showLargeFolderTitlelessOption)
    {
        setFluentIcon(menu,
            kContextWidgetToggleLargeFolderTitleless,
            snowdesktop::menu_fluent_glyphs::kCompactGrid);
    }
    if (sortMenu)
    {
        setFluentIcon(menu, reinterpret_cast<UINT_PTR>(sortMenu),
            snowdesktop::menu_fluent_glyphs::kSort);
        snowdesktop::sort_menu::SetIcons(sortMenu,
            [this](HMENU target, UINT commandId, const wchar_t* glyph) {
                SetMenuItemIcon(target, commandId, glyph, MenuIconFont::FluentRegular);
            });
    }
    if (widget.type == DesktopWidgetType::Collection)
        setFluentIcon(menu, kContextWidgetToggleCollectionMode,
            snowdesktop::menu_fluent_glyphs::kCollection);
    if (demoCategoryMenu)
        setFluentIcon(menu,
            reinterpret_cast<UINT_PTR>(demoCategoryMenu), L"\uF18B");

    if (dockIconMenu)
    {
        const auto iconMenu = CreatePopupMenu();
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), _LW("iconLibrary.personalization"));
        setFluentIcon(menu, reinterpret_cast<UINT_PTR>(iconMenu), L"\uF592");
        AppendMenuW(iconMenu, MF_STRING, kDockChooseIcon, _LW("iconLibrary.change"));
        setFluentIcon(iconMenu, kDockChooseIcon, L"\uF489"); // image_24_regular
        if (!widget.dockIconAssetId.empty())
        {
            AppendMenuW(iconMenu, MF_STRING, kDockRestoreIcon, _LW("iconLibrary.restore"));
            setFluentIcon(iconMenu, kDockRestoreIcon, L"\uF19A"); // arrow_undo_24_regular
        }
        AppendMenuW(iconMenu, MF_STRING, kDockBeautifyIcon, _LW("iconAppearance.edit"));
        setFluentIcon(iconMenu, kDockBeautifyIcon, L"\uF592");
        if (iconAppearanceOverrides_.contains(L"widget:" + widget.id))
        {
            AppendMenuW(iconMenu, MF_STRING, kDockInheritIcon, _LW("iconAppearance.inherit"));
            setFluentIcon(iconMenu, kDockInheritIcon, L"\uF19A");
        }
    }

    RestoreInteractionInputFocus();
    snowdesktop::shell_extensions::Request shellRequest;
    if (effectiveSourceIndex < widgets_.size() &&
        widgets_[effectiveSourceIndex].type == DesktopWidgetType::FolderMapping &&
        !widgets_[effectiveSourceIndex].sourceFolderPath.empty())
    {
        shellRequest.paths.push_back(widgets_[effectiveSourceIndex].sourceFolderPath);
        shellRequest.background = true;
        shellRequest.extended = (snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000) != 0;
    }
    // Toggling an auto-collect option is a continuous configuration step: keep
    // the submenu open, refresh its check marks in place, and apply the
    // resulting collection/layout work once the menu session ends.
    bool autoCollectPending = false;
    DesktopWidget autoCollectCandidate;
    autoCollectCandidate.autoCollect = effectiveSource.autoCollect;
    autoCollectCandidate.autoCollectCategoryIds = effectiveSource.autoCollectCategoryIds;
    const auto applyAutoCollectMenuCommand = [&](UINT menuCommand,
        std::vector<snowdesktop::modern_menu::Item>& items) -> bool {
        if (effectiveSourceIndex >= widgets_.size() ||
            widgets_[effectiveSourceIndex].type != DesktopWidgetType::FileCategories)
            return false;
        auto& source = autoCollectCandidate;
        if (menuCommand == kContextWidgetAutoCollectAll)
        {
            source.autoCollect = true;
            source.autoCollectCategoryIds.clear();
        }
        else if (menuCommand == kContextWidgetToggleAutoCollect)
        {
            source.autoCollect = false;
        }
        else if (menuCommand >= kContextWidgetAutoCollectCategoryFirst &&
            menuCommand <= kContextWidgetAutoCollectCategoryLast)
        {
            const size_t categoryIndex =
                menuCommand - kContextWidgetAutoCollectCategoryFirst;
            if (categoryIndex >= autoCollectCategories.size()) return false;
            snowdesktop::file_category_auto_collect_rules::ToggleCategory(
                source, autoCollectCategories[categoryIndex],
                GetCategoryOrder(categorySettings_));
        }
        else
            return false;

        autoCollectPending = true;
        for (auto& item : items)
        {
            const bool holdsAutoCollect = std::any_of(
                item.children.begin(), item.children.end(),
                [](const auto& child) {
                    return child.command == kContextWidgetAutoCollectAll ||
                        child.command == kContextWidgetToggleAutoCollect ||
                        (child.command >= kContextWidgetAutoCollectCategoryFirst &&
                            child.command <= kContextWidgetAutoCollectCategoryLast);
                });
            if (!holdsAutoCollect) continue;
            for (auto& child : item.children)
            {
                if (child.command == kContextWidgetAutoCollectAll)
                {
                    child.checked = source.autoCollect &&
                        source.autoCollectCategoryIds.empty();
                }
                else if (child.command == kContextWidgetToggleAutoCollect)
                {
                    child.checked = !source.autoCollect;
                }
                else if (child.command >= kContextWidgetAutoCollectCategoryFirst &&
                    child.command <= kContextWidgetAutoCollectCategoryLast)
                {
                    const size_t categoryIndex =
                        child.command - kContextWidgetAutoCollectCategoryFirst;
                    child.checked = source.autoCollect &&
                        categoryIndex < autoCollectCategories.size() &&
                        snowdesktop::file_category_auto_collect_rules::Matches(
                            source, autoCollectCategories[categoryIndex]);
                }
            }
            item.label = statusLabel(_LW("app.interact.auto_collect"),
                source.autoCollect ? _LW("app.interact.on")
                                   : _LW("app.interact.off"));
            break;
        }
        return true;
    };
    UINT command = ShowModernMenu(
        menu, screenPoint, hwnd_, dockRenameAnchor.has_value(), false, nullptr,
        applyAutoCollectMenuCommand, {}, {}, &shellRequest);
    DestroyMenu(menu);
    ClearMenuIcons();

    if (autoCollectPending && effectiveSourceIndex < widgets_.size() &&
        widgets_[effectiveSourceIndex].type == DesktopWidgetType::FileCategories)
    {
        // The submenu stays open for consecutive changes; collect and relayout
        // once after the menu session so the desktop does not rebuild under it.
        const auto sourceId = widgets_[effectiveSourceIndex].id;
        const auto sortId = widgets_[contentSortTargetIndex].id;
        auto categories = snowdesktop::control::Array();
        for (const auto& category : autoCollectCandidate.autoCollectCategoryIds)
            categories.array.push_back(snowdesktop::control::String(WideToUtf8(category)));
        (void)applyNativeSettings(snowdesktop::control::Object({
            {"autoCollect", snowdesktop::control::Boolean(autoCollectCandidate.autoCollect)},
            {"autoCollectCategoryIds", std::move(categories)}}));
        widgetIndex = FindWidgetIndexById(menuWidgetId);
        effectiveSourceIndex = FindWidgetIndexById(sourceId);
        contentSortTargetIndex = FindWidgetIndexById(sortId);
        if (widgetIndex >= widgets_.size() || effectiveSourceIndex >= widgets_.size()) return;
    }

    if (dockIconMenu && (command == kDockBeautifyIcon || command == kDockInheritIcon))
    {
        RestoreDesktopWindowLayer();
        const auto target = L"widget:" + menuWidgetId;
        if (command == kDockBeautifyIcon) OpenIconAppearance(target);
        else { snowdesktop::IconAppearanceRequest request; request.action = "inherit"; request.target = target; (void)EditIconAppearance(request); }
        return;
    }
    if (dockIconMenu && (command == kDockChooseIcon || command == kDockRestoreIcon))
    {
        RestoreDesktopWindowLayer();
        if (command == kDockChooseIcon) OpenDockIconLibrary(menuWidgetId);
        else if (!SetDockIconAsset(menuWidgetId, {}))
            MessageBoxW(hwnd_, _LW("iconLibrary.saveFailed"), L"SnowDesktop", MB_OK | MB_ICONWARNING);
        return;
    }

    if (command == kContextWidgetOpenComponentPanel && luaElementMenu)
    {
        RestoreDesktopWindowLayer();
        ShowWidgetContextMenu(screenPoint, widgetIndex,
            dockRenameAnchor, resolvedLuaLocalPoint, luaSurface, true);
        return;
    }

    if (command >= kContextLuaWidgetMenuFirst && command <= kContextLuaWidgetMenuLast)
    {
        // Finish restoring the desktop menu owner before dispatching the Lua
        // action. The action may synchronously open another interaction
        // surface (for example the logical-slot picker), which must keep the
        // focus it establishes.
        RestoreDesktopWindowLayer();
        RestoreInteractionInputFocus();
        size_t itemIndex = static_cast<size_t>(command - kContextLuaWidgetMenuFirst);
        if (itemIndex < luaMenuActions.size() && widgetEngine_)
        {
            widgetEngine_->InvokeMenu(
                widgets_[widgetIndex].id, luaMenuActions[itemIndex]);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    if (command >= kContextWidgetDemoCategoryFirst && command <= kContextWidgetDemoCategoryLast)
    {
        std::string category;
        const auto selected = static_cast<size_t>(command - kContextWidgetDemoCategoryFirst);
        if (selected > 0 && selected <= snowdesktop::demo_collection_rules::kCategories.size())
            category = WideToUtf8(std::wstring(snowdesktop::demo_collection_rules::kCategories[selected - 1].id));
        (void)applyNativeSettings(snowdesktop::control::Object({{"demoIconCategory", snowdesktop::control::String(category)}}));
        RestoreDesktopWindowLayer(); RestoreInteractionInputFocus();
        return;
    }
    bool needsDesktopFocus = true;
    if (const auto sort = snowdesktop::sort_menu::ResolveCommand(command))
        SortWidgetContents(contentSortTargetIndex, sort->mode, sort->ascending);

    switch (command)
    {
    case kContextWidgetToggleSingleCellMode:
        SelectWidgetOnly(widgetIndex);
        (void)applyNativeSettings(snowdesktop::control::Object({{"singleCellIconMode", snowdesktop::control::Boolean(!widgets_[widgetIndex].singleCellIconMode)}}));
        break;
    case kContextWidgetOpen:
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        OpenCollectionPopupAt(widgetIndex, clientPoint, L"");
        break;
    }
    case kContextWidgetOpenFolder:
        needsDesktopFocus = false;
        if (widgets_[effectiveSourceIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[effectiveSourceIndex].sourceFolderPath.empty())
            shellLaunchWorker_.Enqueue(
                hwnd_, widgets_[effectiveSourceIndex].sourceFolderPath);
        break;
    case kContextPopupFan:
        (void)applyNativeSettings(snowdesktop::control::Object({{"fanPopup", snowdesktop::control::Boolean(!widgets_[widgetIndex].fanPopup)}}));
        break;
    case kContextWidgetListColumnsFirst:
    case kContextWidgetListColumnsFirst + 1:
    case kContextWidgetListColumnsFirst + 2:
    case kContextWidgetListColumnsLast:
    case kContextWidgetToggleListMode:
    {
        const auto state = CaptureNativeWidgetSettings(menuWidgetId);
        if (command == kContextWidgetToggleListMode)
            (void)applyNativeSettings(snowdesktop::control::Object({{"listMode", snowdesktop::control::Boolean(!state.values.listMode)}}));
        else if (state.values.listMode)
            (void)applyNativeSettings(snowdesktop::control::Object({{"listColumns", snowdesktop::control::Number(static_cast<int>(command - kContextWidgetListColumnsFirst) + 1)}}));
        break;
    }
    case kContextWidgetDetailModified:
    case kContextWidgetDetailType:
    case kContextWidgetDetailSize:
    {
        const auto state = CaptureNativeWidgetSettings(menuWidgetId);
        if (!state.values.listMode) break;
        const auto* key = command == kContextWidgetDetailModified ? "detailShowModified" : command == kContextWidgetDetailType ? "detailShowType" : "detailShowSize";
        const bool enabled = command == kContextWidgetDetailModified ? state.values.detailShowModified : command == kContextWidgetDetailType ? state.values.detailShowType : state.values.detailShowSize;
        (void)applyNativeSettings(snowdesktop::control::Object({{key, snowdesktop::control::Boolean(!enabled)}}));
        break;
    }
    case kContextWidgetToggleDateGroup:
    {
        const auto state = CaptureNativeWidgetSettings(menuWidgetId);
        (void)applyNativeSettings(snowdesktop::control::Object({{"dateHeaders", snowdesktop::control::Boolean(!state.values.dateHeaders)}}));
        break;
    }
    case kContextWidgetToggleFileCategories:
    {
        const auto state = CaptureNativeWidgetSettings(menuWidgetId);
        (void)applyNativeSettings(snowdesktop::control::Object({{"showFileCategories", snowdesktop::control::Boolean(!state.values.showFileCategories)}}));
        break;
    }
    case kContextWidgetToggleSearchBox:
        (void)applyNativeSettings(snowdesktop::control::Object({{"showSearchBox", snowdesktop::control::Boolean(!widgets_[widgetIndex].showSearchBox)}}));
        break;
    case kContextWidgetManualCollect:
    {
        const auto state = CaptureNativeWidgetSettings(menuWidgetId);
        snowdesktop::control_settings::NativeWidgetSettingsPatch patch;
        patch.values = state.values; patch.collectNow = true;
        if (!ApplyNativeWidgetSettings(state, patch)) MessageBeep(MB_ICONWARNING);
        break;
    }
    case kContextWidgetRename:
        needsDesktopFocus = false;
        if (CanRenameWidget(widgets_[widgetIndex]))
        {
            SelectWidgetOnly(widgetIndex);
            BeginRenameSelected(dockRenameAnchor);
        }
        break;
    case kContextWidgetEdit:
        needsDesktopFocus = false;
        ShowWidgetEditorHost(widgetIndex);
        break;
    case kContextWidgetDelete:
    {
        widgetIndex = FindWidgetIndexById(menuWidgetId);
        if (widgetIndex >= widgets_.size() ||
            widgets_[widgetIndex].packageId != menuPackageId ||
            widgets_[widgetIndex].type != menuWidgetType)
            break;
        const bool requiresConfirmation =
            widgets_[widgetIndex].type == DesktopWidgetType::LuaScript &&
            (!widgetEngine_ || widgetEngine_->RequiresRemovalConfirmation(
                widgets_[widgetIndex].id, widgets_[widgetIndex].packageId));
        const auto removalIndex = snowdesktop::widget_runtime::ConfirmWidgetRemoval(
            widgets_, widgetIndex, requiresConfirmation,
            [&](const std::wstring& title) {
                return ShowWidgetRemovalConfirmation(hwnd_, screenPoint, title);
            });
        if (!removalIndex) break;
        widgetIndex = *removalIndex;
        for (size_t i = 0; i < dockEntries_.size(); ++i)
            if (IsWidgetDockEntryType(dockEntries_[i].type) &&
                dockEntries_[i].reference == widgets_[widgetIndex].id)
                for (const auto& container : containers_)
                    if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
                        dock->PrepareEntryRemoval(i);
        // widgets_ 的下标会在删除后整体移动，不能让关闭动画继续引用
        // 即将失效的弹窗数据源。
        if (GetOpenPopupWidget())
        {
            CloseCollectionPopup();
            FinalizeCloseCollectionPopup();
        }
        const std::wstring deletedWidgetId = widgets_[widgetIndex].id;
        const bool deletedLuaInstance =
            widgets_[widgetIndex].type == DesktopWidgetType::LuaScript;
        if (widgets_[widgetIndex].type == DesktopWidgetType::CollectionGroup)
        {
            if (popupWidgetIndex_ < widgets_.size() &&
                std::find(
                    widgets_[widgetIndex].childWidgetIds.begin(),
                    widgets_[widgetIndex].childWidgetIds.end(),
                    widgets_[popupWidgetIndex_].id) !=
                    widgets_[widgetIndex].childWidgetIds.end())
                CloseCollectionPopup();
            ReleaseCollectionGroupChildren(widgetIndex);
        }
        else if (widgets_[widgetIndex].type ==
                 DesktopWidgetType::FileGroup)
        {
            ReleaseFileGroupChildren(widgetIndex);
        }
        ReleaseDesktopItemsFromWidget(widgetIndex);
        for (auto& group : widgets_)
        {
            if (group.id == deletedWidgetId ||
                (group.type !=
                     DesktopWidgetType::CollectionGroup &&
                 group.type !=
                     DesktopWidgetType::FileGroup))
                continue;
            std::erase(
                group.childWidgetIds,
                deletedWidgetId);
            group.activeCategoryId =
                snowdesktop::collection_group_rules::
                    ResolveActiveItem(
                        group.childWidgetIds,
                        group.activeCategoryId);
        }
        // 成员落点已在删除前逐个分配；移除组件后重新进入桌面容器。
        widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(widgetIndex));
        std::erase_if(dockEntries_, [&](const DockEntry& entry) {
            return (IsWidgetDockEntryType(entry.type)) &&
                entry.reference == deletedWidgetId;
        });
        EnsureNavTabOrder();
        // 删除组件可能使页面变空（溢出区空页后面有非空页时应立即清理顺延）
        ApplyPageMapping();
        LayoutItems();
        RebuildContainersAndItems();
        // Remove the model and rebuild its wrappers before retiring pixels.
        // Runtime disposal can invalidate surfaces or pump paints; it must
        // observe no owner and no retained content/backdrop to redraw.
        RemoveDesktopWidgetComposition(deletedWidgetId);
        if (deletedLuaInstance && widgetEngine_)
            widgetEngine_->DeleteWidgetInstance(deletedWidgetId);
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    }
    case kContextPasteCommand:
        if (effectiveSourceIndex < widgets_.size())
        {
            if (widgets_[effectiveSourceIndex].type ==
                    DesktopWidgetType::FolderMapping &&
                !widgets_[effectiveSourceIndex].
                    sourceFolderPath.empty())
            {
                PasteClipboardToFolderMapping(
                    effectiveSourceIndex);
            }
            else if (widgets_[effectiveSourceIndex].type ==
                     DesktopWidgetType::FileCategories)
            {
                PasteClipboardToDesktop();
            }
        }
        break;
    case kContextNewMenu:
        if (effectiveSourceIndex < widgets_.size() &&
            widgets_[effectiveSourceIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[effectiveSourceIndex].
                sourceFolderPath.empty())
        {
            ShowNewMenuAndInvoke(screenPoint,
                widgets_[effectiveSourceIndex].
                    sourceFolderPath);
            RequestShellRefresh();
        }
        else if (effectiveSourceIndex < widgets_.size() &&
                 widgets_[effectiveSourceIndex].type ==
                     DesktopWidgetType::FileCategories)
        {
            wchar_t desktopPath[MAX_PATH]{};
            if (snowdesktop::desktop_source::CopyDirectory(desktopPath))
            {
                ShowNewMenuAndInvoke(
                    screenPoint, desktopPath, false,
                    widgets_[effectiveSourceIndex].id);
                RequestShellRefresh();
            }
        }
        break;
    case kContextMoreCommand:
        needsDesktopFocus = false;
        if (effectiveSourceIndex < widgets_.size() &&
            widgets_[effectiveSourceIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[effectiveSourceIndex].
                sourceFolderPath.empty())
        {
            ShowShellContextMenuForPath(
                widgets_[effectiveSourceIndex].
                    sourceFolderPath, screenPoint);
        }
        break;
    case kContextWidgetShowOnHoverOn:
        if (snowdesktop::single_cell_widget::Supports(menuWidgetType))
        { (void)applyNativeSettings(snowdesktop::control::Object({{"showOnHoverOnly", snowdesktop::control::Boolean(true)}})); break; }
        widgets_[widgetIndex].showOnHoverOnly = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleExpandOnHover:
        if (snowdesktop::single_cell_widget::Supports(menuWidgetType))
        { (void)applyNativeSettings(snowdesktop::control::Object({{"titleBarExpandOnHover", snowdesktop::control::Boolean(!widgets_[widgetIndex].titleBarExpandOnHover)}})); break; }
        if (snowdesktop::storage_title_bar::UsesTop(widgets_[widgetIndex],
                CurrentPersonalization().scrollableTitleBarOnTop))
        {
            auto& target = widgets_[widgetIndex];
            target.titleBarExpandOnHover = !target.titleBarExpandOnHover;
            UpdateWidgetHoverExpansion(lastMousePoint_);
            SaveLayoutSlots();
            InvalidateDragStaticScene();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    case kContextWidgetShowOnHoverOff:
        if (snowdesktop::single_cell_widget::Supports(menuWidgetType))
        { (void)applyNativeSettings(snowdesktop::control::Object({{"showOnHoverOnly", snowdesktop::control::Boolean(false)}})); break; }
        widgets_[widgetIndex].showOnHoverOnly = false;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetKeepWhenHiddenOn:
        if (snowdesktop::single_cell_widget::Supports(menuWidgetType))
        { (void)applyNativeSettings(snowdesktop::control::Object({{"keepWhenDesktopHidden", snowdesktop::control::Boolean(true)}})); break; }
        widgets_[widgetIndex].keepWhenDesktopHidden = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetKeepWhenHiddenOff:
        if (snowdesktop::single_cell_widget::Supports(menuWidgetType))
        { (void)applyNativeSettings(snowdesktop::control::Object({{"keepWhenDesktopHidden", snowdesktop::control::Boolean(false)}})); break; }
        widgets_[widgetIndex].keepWhenDesktopHidden = false;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetPrivacyModeOn:
        (void)applyNativeSettings(snowdesktop::control::Object({{"privacyMode", snowdesktop::control::Boolean(true)}}));
        break;
    case kContextWidgetPrivacyModeOff:
        (void)applyNativeSettings(snowdesktop::control::Object({{"privacyMode", snowdesktop::control::Boolean(false)}}));
        break;
    case kContextWidgetToggleLargeFolderTitleless:
        (void)applyNativeSettings(snowdesktop::control::Object({{"largeFolderTitleless", snowdesktop::control::Boolean(!widgets_[widgetIndex].largeFolderTitleless)}}));
        break;
    case kContextWidgetToggleCollectionMode:
        (void)applyNativeSettings(snowdesktop::control::Object({{"scrollContainerMode", snowdesktop::control::Boolean(!widgets_[widgetIndex].scrollContainerMode)}}));
        break;
    default:
        break;
    }
    RestoreDesktopWindowLayer();
    if (needsDesktopFocus)
        RestoreInteractionInputFocus();
}

// Tray implementation lives in app_tray.cpp.
