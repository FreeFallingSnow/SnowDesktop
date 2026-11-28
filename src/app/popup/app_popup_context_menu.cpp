#include "app/app.h"
#include "common/l10n.h"
#include "layout/collection_popup_layout.h"
#include "layout/list_detail_rules.h"
#include "settings/animation_settings.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "widgets/storage_title_bar_layout.h"

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
    append(kContextWidgetOpenFolder,
        _LW(folderPreview_ ? "app.interact.open_in_explorer" : "app.interact.open_folder"),
        nullptr, L"\uF42E");
    if (!folderPreview_ && popupAnchoredToDock_ &&
        snowdesktop::collection_popup_layout::SupportsFanOnDock(popupDockPosition_))
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
    if (!folderPreview_)
    {
        append(kContextWidgetToggleFileCategories, _LW("app.interact.file_categories"),
            widget.showFileCategories ? _LW("app.interact.shown") : _LW("app.interact.hidden"),
            snowdesktop::menu_fluent_glyphs::kCategoryBar);
        append(kContextWidgetToggleSearchBox, _LW("app.interact.search_box"),
            widget.showSearchBox ? _LW("app.interact.shown") : _LW("app.interact.hidden"), L"\uF68F");
    }
    append(kContextWidgetToggleDateGroup, _LW("app.interact.date_header"),
        widget.dateHeaders ? _LW("app.interact.shown") : _LW("app.interact.hidden"),
        snowdesktop::menu_fluent_glyphs::kDateHeader);
    if (const auto* host = GetFolderPreviewComponent())
    {
        const auto state = [](bool enabled) { return _LW(enabled ? "app.interact.on" : "app.interact.off"); };
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        append(host->showOnHoverOnly ? kContextWidgetShowOnHoverOff : kContextWidgetShowOnHoverOn,
            _LW("app.interact.hover_only"), state(host->showOnHoverOnly),
            snowdesktop::menu_fluent_glyphs::kWidgetHoverOnly);
        if (snowdesktop::storage_title_bar::UsesTop(*host, CurrentPersonalization().scrollableTitleBarOnTop))
            append(kContextWidgetToggleExpandOnHover, _LW("app.interact.expand_on_hover"),
                state(host->titleBarExpandOnHover), snowdesktop::menu_fluent_glyphs::kChevronRight);
        append(host->keepWhenDesktopHidden ? kContextWidgetKeepWhenHiddenOff : kContextWidgetKeepWhenHiddenOn,
            _LW("app.interact.keep_when_hidden"), state(host->keepWhenDesktopHidden),
            snowdesktop::menu_fluent_glyphs::kWidgetKeepWhenHidden);
        append(host->privacyMode ? kContextWidgetPrivacyModeOff : kContextWidgetPrivacyModeOn,
            _LW("app.interact.privacy_mode"), state(host->privacyMode),
            host->privacyMode ? snowdesktop::menu_fluent_glyphs::kWidgetPrivacyOn
                              : snowdesktop::menu_fluent_glyphs::kWidgetPrivacyOff);
    }
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
    if (auto* host = GetFolderPreviewComponent())
    {
        const bool behavior = [&] {
            switch (command)
            {
            case kContextWidgetShowOnHoverOn: case kContextWidgetShowOnHoverOff:
                host->showOnHoverOnly = command == kContextWidgetShowOnHoverOn; return true;
            case kContextWidgetKeepWhenHiddenOn: case kContextWidgetKeepWhenHiddenOff:
                host->keepWhenDesktopHidden = command == kContextWidgetKeepWhenHiddenOn; return true;
            case kContextWidgetToggleExpandOnHover:
                if (!snowdesktop::storage_title_bar::UsesTop(*host, CurrentPersonalization().scrollableTitleBarOnTop))
                    return false;
                host->titleBarExpandOnHover = !host->titleBarExpandOnHover; return true;
            case kContextWidgetPrivacyModeOn: case kContextWidgetPrivacyModeOff:
                widget.privacyMode = host->privacyMode = command == kContextWidgetPrivacyModeOn; return true;
            default: return false;
            }
        }();
        if (behavior)
        {
            SaveLayoutSlots();
            UpdateWidgetHoverExpansion(lastMousePoint_);
            InvalidateDragStaticScene();
            InvalidateRect(hwnd_, nullptr, FALSE);
            InvalidateCollectionPopupContent();
            return true;
        }
    }
    if (folderPreview_ && (command == kContextPopupFan || command == kContextWidgetToggleSearchBox ||
        command == kContextWidgetToggleFileCategories)) return false;
    switch (command)
    {
    case kContextPopupFan:
        if (!popupAnchoredToDock_ ||
            !snowdesktop::collection_popup_layout::SupportsFanOnDock(popupDockPosition_))
            return false;
        widget.fanPopup = !widget.fanPopup;
        break;
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
    if (folderPreview_) CommitFolderPreviewDisplaySettings();
    else CommitDockFolderPopupStateToSource();
    InvalidateCollectionPopupContent();
    RefreshDockFolderPopupGeometry();
    return true;
}

void DesktopApp::CommitFolderPreviewDisplaySettings()
{
    if (!folderPreview_ || folderPreview_->IsTemporary()) return;
    const auto& widget = dockFolderPopupWidget_;
    const auto copyDisplay = [&](auto& target) {
        target.listMode = widget.listMode;
        target.listColumns = widget.listColumns;
        target.dateHeaders = widget.dateHeaders;
        target.detailShowModified = widget.detailShowModified;
        target.detailShowType = widget.detailShowType;
        target.detailShowSize = widget.detailShowSize;
        target.detailModifiedPosition = widget.detailModifiedPosition;
        target.detailTypePosition = widget.detailTypePosition;
        target.detailSizePosition = widget.detailSizePosition;
        if constexpr (requires { target.showDetails; }) target.showDetails = widget.showDetails;
    };
    if (auto* owner = GetFolderPreviewComponent())
    {
        auto* settings = ResolveWidgetContentViewData(owner);
        copyDisplay(*settings);
        for (auto& container : containers_)
        {
            auto* view = dynamic_cast<ScrollingItemWidget*>(container.get());
            if (!view || (view->GetWidgetData() != owner && view->GetWidgetData() != settings)) continue;
            if (auto* group = dynamic_cast<FileGroup*>(view)) group->InvalidateHostedView();
            else if (auto* mapping = dynamic_cast<FolderMapping*>(view)) mapping->InvalidateFilterCache();
            else view->InvalidateSlots();
        }
    }
    if (folderPreview_->folderHome)
    {
        auto& home = *folderPreview_->folderHome;
        copyDisplay(home.data);
        if (home.view) home.view->InvalidateFilterCache();
        if (home.mappingId.empty())
            for (auto& entry : dockEntries_)
                if (IsFolderDockEntry(entry) && std::to_wstring(static_cast<int>(entry.type)) +
                    L":" + ToUpperInvariant(entry.reference) == home.dockSourceId)
                {
                    copyDisplay(entry);
                    break;
                }
    }
    folderPreview_->inlineListMode = widget.listMode;
    folderPreview_->inlineDateHeaders = widget.dateHeaders;
    SaveFolderPreviewView();
    SaveLayoutSlots();
}
