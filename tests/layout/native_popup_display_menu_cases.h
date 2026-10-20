#pragma once
#include "data/layout_storage.h"

namespace native_popup_display_menu_cases
{
using namespace native_drag_commit_cases;

inline MENUITEMINFOW Item(HMENU menu, UINT position)
{
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_DATA | MIIM_FTYPE;
    Require(GetMenuItemInfoW(menu, position, TRUE, &info) != FALSE, "read the actual native popup menu item");
    return info;
}

// Build the actual background-menu section and execute its real command service.
// No menu is displayed, no shell launch is invoked, and all storage is isolated.
inline void Run()
{
    Profile profile;
    native_list_hover_cases::Rendering rendering;
    for (const bool mapped : {false, true})
    {
        context = "popup display menu mapped=" + std::to_string(mapped);
        const auto folder = profile.root / (mapped ? L"mapped" : L"direct");
        fs::create_directories(folder);
        { std::ofstream(folder / L"probe.txt") << "isolated menu fixture"; }
        auto& app = profile.Model(false, folder / L"layout");
        Access::InitListTitleRendering(app, rendering.device.Get(), rendering.context.Get());
        Access::SeedPopupDisplayMenu(app, folder, mapped);
        HMENU menu = CreatePopupMenu();
        Require(menu != nullptr, "allocate an actual native popup menu");
        Access::AppendPopupDisplayMenu(app, menu);
        Require(GetMenuItemCount(menu) == 8 && Item(menu, 0).wID == kContextWidgetOpenFolder &&
            Item(menu, 1).wID == kContextPopupFan && Item(menu, 2).wID == kContextWidgetToggleListMode &&
            Item(menu, 5).wID == kContextWidgetToggleFileCategories &&
            Item(menu, 6).wID == kContextWidgetToggleSearchBox && Item(menu, 7).wID == kContextWidgetToggleDateGroup,
            "the blank-popup menu exposes all eight requested display actions in screenshot order");
        const auto columns = Item(menu, 3), details = Item(menu, 4);
        Require(columns.hSubMenu && details.hSubMenu && (columns.fState & MFS_DISABLED) &&
            (details.fState & MFS_DISABLED), "grid mode keeps list-only choices disabled");
        Require(GetMenuItemCount(columns.hSubMenu) == 4, "list columns offer exactly one through four");
        for (UINT i = 0; i < 4; ++i)
        {
            const auto choice = Item(columns.hSubMenu, i);
            Require(choice.wID == kContextWidgetListColumnsFirst + i && choice.dwItemData == 0 &&
                !(choice.fType & MFT_OWNERDRAW), "column choices retain native checkmarks without an extra icon");
        }
        DestroyMenu(menu); Access::ClearPopupMenuIcons(app);
        const auto execute = [&](UINT command) {
            Require(Access::PopupDisplayCommand(app, command), "the popup display command must be handled");
        };
        execute(kContextWidgetToggleListMode);
        execute(kContextWidgetDetailType);
        Require(Access::PopupDisplay(app).listColumns == 1 && Access::PopupDisplay(app).detailShowType,
            "metadata switches the popup back to one column");
        execute(kContextWidgetListColumnsLast);
        Require(Access::PopupDisplay(app).listColumns == 4 && !Access::PopupDisplay(app).showDetails,
            "four columns disable metadata in the actual popup state");
        execute(kContextPopupFan);
        Require(Access::PopupDisplay(app).fanPopup, "fan toggle changes the real popup mode");
        execute(kContextPopupFan);
        execute(kContextWidgetToggleFileCategories);
        execute(kContextWidgetToggleSearchBox);
        execute(kContextWidgetToggleDateGroup);
        const auto& popup = Access::PopupDisplay(app);
        Require(popup.showFileCategories && popup.showSearchBox && popup.dateHeaders,
            "category, search and date commands change the actual popup flags");
        snowdesktop::layout_storage::Document document;
        const auto loaded = snowdesktop::layout_storage::LoadDocument(Access::LayoutPath(app), document);
        Require(loaded.status == snowdesktop::layout_storage::LoadStatus::LoadedPrimary,
            "popup menu changes are written by the real host save boundary");
        if (mapped)
        {
            Require(!document.widgets.empty() && document.widgets[0].dateHeaders &&
                document.widgets[0].showSearchBox && document.widgets[0].showFileCategories &&
                document.widgets[0].listColumns == 4,
                "mapped popup display changes persist on the source widget");
        }
        else
        {
            Require(document.dockEntries.size() == 1 && document.dockEntries[0].dateHeaders &&
                document.dockEntries[0].showSearchBox && document.dockEntries[0].showFileCategories &&
                document.dockEntries[0].listColumns == 4,
                "direct folder display changes persist on its Dock entry");
            auto* view = Access::DatePopupView(app);
            Require(view != nullptr, "date grouping uses the real native folder layout");
            const auto first = Access::PopupDisplayItemRect(app, 0);
            const auto own = view->GetMemberLayoutRect(0), viewport = view->GetContentViewportRect();
            Require(EqualRect(&first, &own) && first.top > viewport.top,
                "popup hit geometry includes the visible date header and matches native item drawing");
            Access::PaintListTitles(app, *view, rendering.context.Get(), {LONG_MIN, LONG_MIN});
            Access::Load(app);
            Require(Access::DockEntries(app).size() == 1 && Access::DockEntries(app)[0].dateHeaders &&
                Access::DockEntries(app)[0].listColumns == 4 && Access::DockEntries(app)[0].showSearchBox &&
                Access::DockEntries(app)[0].showFileCategories,
                "the real Dock restore codec retains popup display choices after layout reload");
        }
        Access::ClearListTitleRendering(app);
        Require(Access::NoPresentationWindows(app), "popup menu regression never creates presentation windows");
    }
    std::cout << "Native popup display menus, mutual exclusion, date layout and source persistence passed\n";
}
}
