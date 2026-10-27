#include "app/app.h"
#include "ui/menu/menu_fluent_glyphs.h"

void DesktopApp::AppendDockFolderPopupDisplayMenu(HMENU menu)
{
    const auto& widget = dockFolderPopupWidget_;
    const auto status = [](const wchar_t* title, const wchar_t* value) {
        return std::wstring(title) + L"\t" + value;
    };
    const auto append = [&](UINT id, const wchar_t* title, const wchar_t* value, const wchar_t* glyph) {
        const auto label = value ? status(title, value) : std::wstring(title);
        AppendMenuW(menu, MF_STRING, id, label.c_str());
        SetMenuItemIcon(menu, id, glyph, MenuIconFont::FluentRegular);
    };
    append(kContextWidgetOpenFolder, _LW("app.interact.open_folder"), nullptr, L"\uF42E");
    append(kContextPopupFan, _LW("app.interact.popup_fan"),
        widget.fanPopup ? _LW("app.interact.on") : _LW("app.interact.off"),
        snowdesktop::menu_fluent_glyphs::kFanExpansion);
    append(kContextWidgetToggleListMode, _LW("app.interact.display_type"),
        widget.listMode ? _LW("app.interact.list_view_state") : _LW("app.interact.icon_view_state"),
        snowdesktop::menu_fluent_glyphs::kContentLayout);
    const UINT mode = MF_POPUP | (widget.listMode ? 0 : MF_GRAYED);
    if (HMENU columns = CreatePopupMenu())
    {
        for (int count = 1; count <= 4; ++count)
            AppendMenuW(columns, MF_STRING | (snowdesktop::list_detail_rules::ListColumns(widget) == count ? MF_CHECKED : 0),
                kContextWidgetListColumnsFirst + count - 1, std::to_wstring(count).c_str());
        AppendMenuW(menu, mode, reinterpret_cast<UINT_PTR>(columns), _LW("app.interact.list_columns"));
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(columns),
            snowdesktop::menu_fluent_glyphs::kContentLayout, MenuIconFont::FluentRegular);
    }
    if (HMENU details = CreatePopupMenu())
    {
        AppendMenuW(details, MF_STRING | MF_CHECKED | MF_GRAYED, kContextWidgetDetailName, _LW("widget.details.name"));
        AppendMenuW(details, MF_STRING | (widget.detailShowModified ? MF_CHECKED : 0),
            kContextWidgetDetailModified, _LW("widget.details.modified"));
        AppendMenuW(details, MF_STRING | (widget.detailShowType ? MF_CHECKED : 0),
            kContextWidgetDetailType, _LW("widget.details.type"));
        AppendMenuW(details, MF_STRING | (widget.detailShowSize ? MF_CHECKED : 0),
            kContextWidgetDetailSize, _LW("widget.details.size"));
        AppendMenuW(menu, mode, reinterpret_cast<UINT_PTR>(details), _LW("app.interact.show_details"));
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(details),
            L"\uF168", MenuIconFont::FluentRegular);
    }
    append(kContextWidgetToggleFileCategories, _LW("app.interact.file_categories"),
        widget.showFileCategories ? _LW("app.interact.shown") : _LW("app.interact.hidden"),
        snowdesktop::menu_fluent_glyphs::kCategoryBar);
    append(kContextWidgetToggleSearchBox, _LW("app.interact.search_box"),
        widget.showSearchBox ? _LW("app.interact.shown") : _LW("app.interact.hidden"), L"\uF68F");
    append(kContextWidgetToggleDateGroup, _LW("app.interact.date_header"),
        widget.dateHeaders ? _LW("app.interact.shown") : _LW("app.interact.hidden"),
        snowdesktop::menu_fluent_glyphs::kDateHeader);
}

bool DesktopApp::ExecuteDockFolderPopupDisplayCommand(UINT command)
{
    if (!dockFolderPopupOpen_) return false;
    auto& widget = dockFolderPopupWidget_;
    if (command == kContextWidgetOpenFolder)
    {
        LaunchPathWithShortcutPolicy(hwnd_, widget.sourceFolderPath);
        return true;
    }
    switch (command)
    {
    case kContextPopupFan: widget.fanPopup = !widget.fanPopup; break;
    case kContextWidgetToggleListMode: widget.listMode = !widget.listMode; break;
    case kContextWidgetToggleSearchBox: widget.showSearchBox = !widget.showSearchBox; break;
    case kContextWidgetToggleFileCategories: widget.showFileCategories = !widget.showFileCategories; break;
    case kContextWidgetToggleDateGroup: widget.dateHeaders = !widget.dateHeaders; break;
    case kContextWidgetListColumnsFirst:
    case kContextWidgetListColumnsFirst + 1:
    case kContextWidgetListColumnsFirst + 2:
    case kContextWidgetListColumnsLast:
        if (!widget.listMode) return true;
        snowdesktop::list_detail_rules::SetListColumns(widget,
            static_cast<int>(command - kContextWidgetListColumnsFirst) + 1);
        break;
    case kContextWidgetDetailModified:
    case kContextWidgetDetailType:
    case kContextWidgetDetailSize:
        if (!widget.listMode) return true;
        if (command == kContextWidgetDetailModified) widget.detailShowModified = !widget.detailShowModified;
        else if (command == kContextWidgetDetailType) widget.detailShowType = !widget.detailShowType;
        else widget.detailShowSize = !widget.detailShowSize;
        widget.listColumns = 1;
        {
            const auto positions = snowdesktop::list_detail_rules::NormalizePositions(
                widget.detailShowModified, widget.detailShowType, widget.detailShowSize,
                {widget.detailModifiedPosition, widget.detailTypePosition, widget.detailSizePosition});
            widget.detailModifiedPosition = positions.modified;
            widget.detailTypePosition = positions.type;
            widget.detailSizePosition = positions.size;
        }
        break;
    default: return false;
    }
    widget.showDetails = snowdesktop::list_detail_rules::HasMetadataColumns(
        widget.detailShowModified, widget.detailShowType, widget.detailShowSize);
    ClearListTitleHover();
    widget.scrollOffset = popupScrollOffset_ = 0;
    ResetCollectionPopupFanScroll();
    popupFanShowAll_ = popupFanActionFocused_ = false;
    if (command == kContextPopupFan)
    {
        ResetCollectionPopupAnimationCache();
        popupAnimation_.ShowImmediately();
        popupAnimation_.Configure(
            snowdesktop::animation::RuntimePopupEffect() == snowdesktop::animation::Fade,
            snowdesktop::animation::RuntimeDurationScale() * (widget.fanPopup ? 2.4 : 1.0), !widget.fanPopup);
    }
    if (dockFolderPopupContainer_)
    {
        if (!widget.showSearchBox) dockFolderPopupContainer_->ClearSearchText();
        if (!widget.showFileCategories) dockFolderPopupContainer_->EndCategoryTabDrag(false);
        dockFolderPopupContainer_->InvalidateFilterCache();
    }
    CommitDockFolderPopupStateToSource();
    InvalidateCollectionPopupContent();
    RefreshDockFolderPopupGeometry();
    return true;
}
