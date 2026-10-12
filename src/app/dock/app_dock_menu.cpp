#include "app/app.h"
#include "ui/menu/menu_fluent_glyphs.h"
#include "settings/bar_settings_rules.h"
#include "dock/dock_settings_rules.h"
#include "dock_platform_helpers.h"
#include "dock_running_app_pin_rules.h"
#include "dock_explorer_pin.h"
#include "app/shell/shell_icon_request.h"
#include "desktop/desktop_source.h"
#include "desktop/desktop_style_presets.h"
#include "layout/logical_slot_picker_rules.h"
#include <commctrl.h>

// Dock and running-application context menus.

namespace
{
constexpr UINT kDockStatusBarSettingsCommand = 1;
constexpr UINT kDockTaskManagerCommand = 2;
constexpr UINT kDockMonitorScopeFirst = 41830;
constexpr UINT kDockScaleFirst = 41840;
constexpr int kDockScalePercent[]{50, 75, 100, 125, 150, 175, 200};
constexpr UINT kMergedDockScaleFirst = 41850;
constexpr UINT kMergedBarHeightFirst = 41860;
constexpr UINT kMergedBarDetailedSettings = 41869;
constexpr int kMergedBarHeights[]{32, 40, 48, 56, 64, 72, 80, 96};
constexpr UINT kDockRevealGestureFirst = 41870;
constexpr UINT kDockReserveDesktop = 41872;
constexpr UINT kDockReserveMaximized = 41873;
constexpr UINT kDockStartButton = 41874;
constexpr UINT kDockQuickSearchButton = 41875;
constexpr UINT kDesktopStyleFirst = 41880;
constexpr wchar_t const* kDesktopStyles[]{L"native", L"taskbar-dock", L"island", L"merged", L"side"};
constexpr char const* kDesktopStyleLabels[]{"settings.desktopStyle.native.title", "settings.desktopStyle.taskbar-dock.title",
    "settings.desktopStyle.island.title", "settings.desktopStyle.merged.title", "settings.desktopStyle.side.title"};
constexpr char const* kDesktopStyleAnimations[]{nullptr, nullptr, "settings.desktopStyle.island.animations",
    "settings.desktopStyle.merged.animations", "settings.desktopStyle.side.animations"};
static_assert(std::size(kDesktopStyles) == std::size(kDesktopStyleLabels) &&
    std::size(kDesktopStyles) == std::size(kDesktopStyleAnimations));

bool ConfirmBarMenuAction(HWND owner, const wchar_t* title, const wchar_t* message,
    const wchar_t* accept, const wchar_t* cancel)
{
    const TASKDIALOG_BUTTON buttons[]{{IDYES, accept}, {IDCANCEL, cancel}};
    POINT anchor{};
    GetCursorPos(&anchor);
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = owner;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle = L"SnowDesktop";
    dialog.pszMainInstruction = title;
    dialog.pszContent = message;
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = IDCANCEL;
    dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(&anchor);
    dialog.pfCallback = +[](HWND window, UINT notification, WPARAM, LPARAM, LONG_PTR data) -> HRESULT {
        if (notification == TDN_CREATED)
        {
            const auto point = *reinterpret_cast<const POINT*>(data);
            MONITORINFO monitor{sizeof(monitor)};
            RECT bounds{};
            UINT flags = SWP_NOSIZE | SWP_SHOWWINDOW;
            int x = 0, y = 0;
            if (GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor) && GetWindowRect(window, &bounds))
            {
                x = monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - bounds.right + bounds.left) / 2;
                y = monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - bounds.bottom + bounds.top) / 2;
            }
            else flags |= SWP_NOMOVE;
            SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0, flags);
            SetForegroundWindow(window);
        }
        return S_OK;
    };
    int selected = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&dialog, &selected, nullptr, nullptr)) && selected == IDYES;
}
}

HMENU DesktopApp::CreateDockContextMenu(bool includeStatusBar)
{
    HMENU menu = CreatePopupMenu();
    HMENU positionMenu = CreatePopupMenu();
    HMENU layoutMenu = CreatePopupMenu();
    HMENU monitorMenu = CreatePopupMenu();
    HMENU scaleMenu = CreatePopupMenu();
    HMENU revealMenu = CreatePopupMenu();
    HMENU independentMenu = includeStatusBar ? CreatePopupMenu() : menu;
    if (!menu || !positionMenu || !layoutMenu || !monitorMenu || !scaleMenu || !revealMenu || !independentMenu)
    {
        if (positionMenu) DestroyMenu(positionMenu);
        if (layoutMenu) DestroyMenu(layoutMenu);
        if (monitorMenu) DestroyMenu(monitorMenu);
        if (scaleMenu) DestroyMenu(scaleMenu);
        if (revealMenu) DestroyMenu(revealMenu);
        if (includeStatusBar && independentMenu) DestroyMenu(independentMenu);
        if (menu) DestroyMenu(menu);
        return nullptr;
    }

    const auto addChoice = [&](HMENU target, UINT command, const wchar_t* title,
                              bool selected, const wchar_t* glyph) {
        std::wstring label = title;
        if (selected) { label += L"\t"; label += _LW("statusBar.menu.current"); }
        AppendMenuW(target, MF_STRING, command, label.c_str());
        SetMenuItemIcon(target, command, glyph, MenuIconFont::FluentRegular);
    };
    const auto addDesktopStyles = [&](HMENU parent) {
        const HMENU styles = CreatePopupMenu();
        if (!styles) return;
        if (!AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(styles), _LW("settings.desktopStyle.title")))
        { DestroyMenu(styles); return; }
        SetMenuItemIcon(parent, reinterpret_cast<UINT_PTR>(styles), L"\uF359", MenuIconFont::FluentRegular);
        for (UINT index = 0; index < std::size(kDesktopStyles); ++index)
        {
            AppendMenuW(styles, MF_STRING, kDesktopStyleFirst + index, _LW(kDesktopStyleLabels[index]));
        }
    };
    addChoice(positionMenu, kContextDockPositionBottom, _LW("app.dock.bottom"), dockSettings_.position == DockPosition::Bottom, L"\uF148");
    addChoice(positionMenu, kContextDockPositionTop, _LW("app.dock.top"), dockSettings_.position == DockPosition::Top, L"\uF19B");
    addChoice(positionMenu, kContextDockPositionLeft, _LW("app.dock.left"), dockSettings_.position == DockPosition::Left, L"\uF15B");
    addChoice(positionMenu, kContextDockPositionRight, _LW("app.dock.right"), dockSettings_.position == DockPosition::Right, L"\uF181");
    addChoice(layoutMenu, kContextDockLayoutIsland, _LW("app.dock.island"), !dockSettings_.edgeAttached, L"\uF8B5");
    addChoice(layoutMenu, kContextDockLayoutEdge, _LW("app.dock.edge"), dockSettings_.edgeAttached, L"\uF359");

    AppendMenuW(independentMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(positionMenu), _LW("app.dock.position"));
    AppendMenuW(independentMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu), _LW("app.dock.layout"));
    AppendMenuW(independentMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(monitorMenu), _LW("settings.dock.monitor"));
    AppendMenuW(independentMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(scaleMenu), _LW("app.settings.dock_thickness"));
    const bool edgeRevealEnabled = snowdesktop::dock_settings_rules::IsFloatingEdgeSwipeEnabled(
        dockSettings_.showOnlyWhenSummoned, dockSettings_.floatingEdgeSwipeEnabled);
    const HMENU revealParent = includeStatusBar && !edgeRevealEnabled ? independentMenu : menu;
    AppendMenuW(revealParent, MF_POPUP,
        reinterpret_cast<UINT_PTR>(revealMenu), _LW("settings.dock.edgeRevealGesture"));
    addChoice(revealMenu, kDockRevealGestureFirst, _LW("settings.dock.gestureSwipe"),
        dockSettings_.edgeRevealGesture == 0, L"\uF8E5");
    addChoice(revealMenu, kDockRevealGestureFirst + 1, _LW("settings.dock.gestureHover"),
        dockSettings_.edgeRevealGesture == 1, L"\uE5F2");
    const char* monitorLabels[]{"app.dock.first_screen", "app.dock.last_screen", "app.dock.all_screens"};
    for (UINT index = 0; index < std::size(monitorLabels); ++index)
    {
        addChoice(monitorMenu, kDockMonitorScopeFirst + index, _LW(monitorLabels[index]),
            static_cast<UINT>(dockSettings_.monitorScope) == index,
            index == 2 ? L"\uEB96" : L"\uF359");
    }
    for (UINT index = 0; index < std::size(kDockScalePercent); ++index)
    {
        const auto label = std::to_wstring(kDockScalePercent[index]) + L"%";
        addChoice(scaleMenu, kDockScaleFirst + index, label.c_str(),
            std::abs(dockSettings_.thicknessScale * 100 - kDockScalePercent[index]) < .5f, L"\uF8C4");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    auto toggleLabel = [](const wchar_t* title, bool enabled) {
        std::wstring label = title;
        label += L"\t";
        label += enabled
            ? _LW("app.interact.on")
            : _LW("app.interact.off");
        return label;
    };
    const std::wstring frequentLabel = toggleLabel(
        _LW("app.dock.show_frequent"),
        dockSettings_.showFrequentItems);
    const auto addToggle = [&](HMENU target, UINT command, const wchar_t* title, bool enabled,
                               const wchar_t* glyph, bool available = true, bool visibility = false) {
        const auto label = visibility ? std::wstring(title) + L"\t" +
            _LW(enabled ? "app.interact.shown" : "app.interact.hidden") : toggleLabel(title, enabled);
        AppendMenuW(target, MF_STRING | (available ? 0 : MF_GRAYED), command, label.c_str());
        SetMenuItemIcon(target, command, glyph, MenuIconFont::FluentRegular);
    };
    addToggle(menu, kDockStartButton, _LW("app.dock.start_button"), dockSettings_.showWindowsButton,
        L"\uF133", true, true);
    addToggle(menu, kDockQuickSearchButton, _LW("app.dock.quick_search"), dockSettings_.showQuickSearchButton,
        L"\uF690", true, true); // search_20
    addToggle(menu, kContextDockSingleClickLaunch, _LW("app.dock.single_click_launch"),
        dockSettings_.singleClickLaunchItems, L"\uE445"); // cursor_click_20
    addToggle(menu, kContextDockShowOnlyWhenSummoned, _LW("settings.dock.showOnlyWhenSummoned"),
        dockSettings_.showOnlyWhenSummoned, L"\uE5F2"); // eye_20
    addToggle(menu, kContextDockWindowPreviews, _LW("settings.dock.windowPreviews"),
        dockSettings_.showWindowPreviews, L"\uF8B5"); // window_20
    addToggle(independentMenu, kDockReserveDesktop, _LW("settings.dock.allowDesktopContentOverlap"),
        !dockSettings_.allowDesktopContentOverlap, L"\uF359");
    addToggle(independentMenu, kDockReserveMaximized, _LW("settings.dock.reserveScreenSpace"),
        dockSettings_.reserveScreenSpace, L"\uF8B5");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING,
        kContextDockShowFrequentItems, frequentLabel.c_str());

    const UINT keepToggleCommand =
        dockSettings_.keepWhenDesktopHidden
            ? kContextDockKeepWhenHiddenOff
            : kContextDockKeepWhenHiddenOn;
    const std::wstring keepLabel = toggleLabel(
        _LW("app.dock.keep_when_hidden"),
        dockSettings_.keepWhenDesktopHidden);
    AppendMenuW(menu, MF_STRING,
        keepToggleCommand, keepLabel.c_str());

    if (!includeStatusBar)
    {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        addDesktopStyles(menu);
        AppendMenuW(menu, MF_STRING, kContextDockDetailedSettings, _LW("app.dock.detailed"));
        SetMenuItemIcon(menu, kContextDockDetailedSettings, L"\uF6A9", MenuIconFont::FluentRegular);
    }
    SetMenuItemIcon(independentMenu, reinterpret_cast<UINT_PTR>(positionMenu), L"\uF8E5", MenuIconFont::FluentRegular);
    SetMenuItemIcon(independentMenu, reinterpret_cast<UINT_PTR>(layoutMenu), L"\uF587", MenuIconFont::FluentRegular);
    SetMenuItemIcon(independentMenu, reinterpret_cast<UINT_PTR>(monitorMenu), L"\uF359", MenuIconFont::FluentRegular);
    SetMenuItemIcon(independentMenu, reinterpret_cast<UINT_PTR>(scaleMenu), L"\uF8C4", MenuIconFont::FluentRegular);
    SetMenuItemIcon(revealParent, reinterpret_cast<UINT_PTR>(revealMenu), L"\uE445", MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, kContextDockShowFrequentItems,
        dockSettings_.showFrequentItems
            ? snowdesktop::menu_fluent_glyphs::kShowFrequent
            : snowdesktop::menu_fluent_glyphs::kHideFrequent,
        MenuIconFont::FluentRegular);
    SetMenuItemIcon(menu, keepToggleCommand,
        snowdesktop::menu_fluent_glyphs::kKeepWhenDesktopHidden,
        MenuIconFont::FluentRegular);
    if (includeStatusBar)
    {
        const HMENU root = CreatePopupMenu();
        if (!root) { DestroyMenu(menu); DestroyMenu(independentMenu); return nullptr; }
        AppendMenuW(root, MF_STRING, kDockTaskManagerCommand, _LW("statusBar.taskManager"));
        AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
        // Dock actions belong directly to the merged bar's settings level.
        const HMENU mergedMenu = menu;
        const HMENU appearanceMenu = CreatePopupMenu();
        if (!appearanceMenu)
        {
            DestroyMenu(menu); DestroyMenu(independentMenu); DestroyMenu(root); return nullptr;
        }
        if (!AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(mergedMenu), _LW("settings.bars.mergedBar")))
        { DestroyMenu(appearanceMenu); DestroyMenu(menu); DestroyMenu(independentMenu); DestroyMenu(root); return nullptr; }
        SetMenuItemIcon(root, kDockTaskManagerCommand, L"\uE49D", MenuIconFont::FluentRegular);
        SetMenuItemIcon(root, reinterpret_cast<UINT_PTR>(mergedMenu), L"\uF587", MenuIconFont::FluentRegular);
        if (!InsertMenuW(mergedMenu, 0, MF_BYPOSITION | MF_POPUP,
            reinterpret_cast<UINT_PTR>(appearanceMenu), _LW("settings.bars.positionAndAppearance")))
        { DestroyMenu(appearanceMenu); DestroyMenu(independentMenu); DestroyMenu(root); return nullptr; }
        if (edgeRevealEnabled)
            InsertMenuW(mergedMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        SetMenuItemIcon(mergedMenu, reinterpret_cast<UINT_PTR>(appearanceMenu), L"\uF592", MenuIconFont::FluentRegular);
        addDesktopStyles(root);
        const HMENU heightMenu = CreatePopupMenu();
        const HMENU iconMenu = CreatePopupMenu();
        if (!heightMenu || !iconMenu)
        {
            if (heightMenu) DestroyMenu(heightMenu);
            if (iconMenu) DestroyMenu(iconMenu);
            DestroyMenu(independentMenu);
            DestroyMenu(root);
            return nullptr;
        }
        AppendMenuW(appearanceMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(heightMenu), _LW("settings.bars.height"));
        AppendMenuW(appearanceMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), _LW("settings.bars.iconSize"));
        for (UINT index = 0; index < std::size(kMergedBarHeights); ++index)
        {
            const auto label = std::to_wstring(kMergedBarHeights[index]);
            addChoice(heightMenu, kMergedBarHeightFirst + index, label.c_str(),
                dockSettings_.mergedBarHeight == kMergedBarHeights[index], L"\uF8C4");
        }
        for (UINT index = 0; index < 3; ++index)
        {
            const auto label = std::to_wstring(kDockScalePercent[index]) + L"%";
            addChoice(iconMenu, kMergedDockScaleFirst + index, label.c_str(),
                std::abs(dockSettings_.mergedIconScale * 100 - kDockScalePercent[index]) < .5f, L"\uF8C4");
        }
        AppendMenuW(mergedMenu, MF_SEPARATOR, 0, nullptr);
        if (!AppendMenuW(mergedMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(independentMenu), _LW("app.settings.dock_bar")))
        { DestroyMenu(independentMenu); DestroyMenu(root); return nullptr; }
        SetMenuItemIcon(mergedMenu, reinterpret_cast<UINT_PTR>(independentMenu), L"\uF133", MenuIconFont::FluentRegular);
        SetMenuItemIcon(appearanceMenu, reinterpret_cast<UINT_PTR>(heightMenu), L"\uF8C4", MenuIconFont::FluentRegular);
        SetMenuItemIcon(appearanceMenu, reinterpret_cast<UINT_PTR>(iconMenu), L"\uF133", MenuIconFont::FluentRegular);
        return root;
    }
    return menu;
}

void DesktopApp::ShowDockContextMenu(POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    if (statusBar_ && hwnd_ && ScreenToClient(hwnd_, &clientPoint))
    {
        const DockContainer* dock = GetDockContainerAtPoint(clientPoint);
        if (dock && dock->IsMergedWithStatusBar())
        {
            const HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
            if (const HWND owner = statusBar_->InteractionWindow(monitor))
            {
                // Both parts of the merged strip share the same activation
                // hold and menu lifetime; never restore the desktop band here.
                ActivateStatusBar(snowdesktop::StatusBarAction::Menu, owner,
                    RECT{screenPoint.x, screenPoint.y, screenPoint.x + 1, screenPoint.y + 1});
            }
            return;
        }
    }
    PrepareMenuIconsForPoint(screenPoint);
    HMENU menu = CreateDockContextMenu(false);
    if (!menu) return;
    RestoreInteractionInputFocus();
    const UINT command = ShowModernMenu(menu, screenPoint, hwnd_, true);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();
    if (command != kContextDockDetailedSettings)
        RestoreInteractionInputFocus();
    ExecuteDockContextMenuCommand(command, hwnd_);
}

void DesktopApp::ExecuteDockContextMenuCommand(UINT command, HWND owner)
{
    if (command >= kDesktopStyleFirst && command < kDesktopStyleFirst + std::size(kDesktopStyles))
    {
        if (!settingsController_) return;
        const auto index = command - kDesktopStyleFirst;
        const std::wstring preset = kDesktopStyles[index];
        // The status-bar interaction window can disappear when the layout is
        // switched. Keep dialogs owned by the stable desktop host instead.
        const HWND dialogOwner = hwnd_;
        snowdesktop::SettingsHostActions::Request request;
        request.action = snowdesktop::SettingsHostActions::Action::ApplyDesktopStylePreset;
        request.value = preset;
        if (preset == L"native" && settingsController_->Snapshot()->values.general.dockEnabled)
        {
            if (!ConfirmBarMenuAction(dialogOwner, _LW("settings.dock.disableConfirm.title"),
                _LW("settings.dock.disableConfirm.description"), _LW("settings.desktopStyle.applyLayout"),
                _LW("settings.dialog.cancel"))) return;
            request.boolValue = true;
        }
        auto result = settingsController_->InvokeHostAction(request);
        if (result.Succeeded()) result = settingsController_->FlushPending();
        if (!result.Succeeded())
        { MessageBoxW(dialogOwner, result.message.c_str(), L"SnowDesktop", MB_OK | MB_ICONERROR); return; }
        if (snowdesktop::NeedsDesktopStyleAnimations(preset, settingsController_->Snapshot()->values.dock))
        {
            const auto description = std::wstring(_LW("settings.desktopStyle.animationPrompt.description")) + L"\n\n" + _LW(kDesktopStyleAnimations[index]);
            if (ConfirmBarMenuAction(dialogOwner, _LW("settings.desktopStyle.animationPrompt.title"), description.c_str(),
                _LW("settings.desktopStyle.applyAnimations"), _LW("settings.desktopStyle.animationPrompt.keep")))
            {
                request.action = snowdesktop::SettingsHostActions::Action::ApplyDesktopStyleAnimations;
                result = settingsController_->InvokeHostAction(request);
                if (result.Succeeded()) result = settingsController_->FlushPending();
                if (!result.Succeeded()) MessageBoxW(dialogOwner, result.message.c_str(), L"SnowDesktop", MB_OK | MB_ICONERROR);
            }
        }
        return;
    }
    DockSettings updated = dockSettings_;
    bool layoutChanged = false;
    switch (command)
    {
    case kContextDockPositionBottom:
    case kContextDockPositionTop:
    case kContextDockPositionLeft:
    case kContextDockPositionRight:
        updated.position = static_cast<DockPosition>(
            command - kContextDockPositionBottom);
        layoutChanged = updated.position != dockSettings_.position;
        break;
    case kContextDockLayoutIsland:
        updated.edgeAttached = false;
        layoutChanged = updated.edgeAttached != dockSettings_.edgeAttached;
        break;
    case kContextDockLayoutEdge:
        updated.edgeAttached = true;
        layoutChanged = updated.edgeAttached != dockSettings_.edgeAttached;
        break;
    case kContextDockShowFrequentItems:
        updated.showFrequentItems = !updated.showFrequentItems;
        layoutChanged = true;
        break;
    case kContextDockSingleClickLaunch:
        updated.singleClickLaunchItems = !updated.singleClickLaunchItems;
        break;
    case kDockStartButton:
        updated.showWindowsButton = !updated.showWindowsButton;
        layoutChanged = true;
        break;
    case kDockQuickSearchButton:
        updated.showQuickSearchButton = !updated.showQuickSearchButton;
        layoutChanged = true;
        break;
    case kDockReserveDesktop:
    case kDockReserveMaximized:
        if (command == kDockReserveDesktop)
            updated.allowDesktopContentOverlap = !updated.allowDesktopContentOverlap;
        else updated.reserveScreenSpace = !updated.reserveScreenSpace;
        layoutChanged = true;
        break;
    case kContextDockShowOnlyWhenSummoned:
        updated.showOnlyWhenSummoned = !updated.showOnlyWhenSummoned;
        layoutChanged = true;
        break;
    case kContextDockWindowPreviews:
        updated.showWindowPreviews = !updated.showWindowPreviews;
        if (!updated.showWindowPreviews)
            HideDockWindowPreview();
        break;
    case kContextDockKeepWhenHiddenOn:
        updated.keepWhenDesktopHidden = true;
        break;
    case kContextDockKeepWhenHiddenOff:
        updated.keepWhenDesktopHidden = false;
        break;
    case kContextDockDetailedSettings:
    case kMergedBarDetailedSettings:
        if (settingsController_)
        {
            (void)settingsController_->SynchronizeGeneral(generalSettings_);
            (void)settingsController_->SynchronizeDock(dockSettings_);
        }
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::Dock, command == kMergedBarDetailedSettings ? "dock.mergedBarHeight" : ""));
        return;
    case kDockStatusBarSettingsCommand:
        ShowSettingsWindow(snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::StatusBar));
        return;
    case kDockTaskManagerCommand:
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", L"taskmgr.exe",
                nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            MessageBeep(MB_ICONWARNING);
        return;
    default:
        if (command >= kDockRevealGestureFirst && command < kDockRevealGestureFirst + 2)
        {
            updated.edgeRevealGesture = static_cast<int>(command - kDockRevealGestureFirst);
            layoutChanged = updated.edgeRevealGesture != dockSettings_.edgeRevealGesture;
        }
        else if (command >= kDockMonitorScopeFirst && command < kDockMonitorScopeFirst + 3)
        {
            updated.monitorScope = static_cast<DockMonitorScope>(command - kDockMonitorScopeFirst);
            layoutChanged = updated.monitorScope != dockSettings_.monitorScope;
        }
        else if (command >= kDockScaleFirst && command < kDockScaleFirst + std::size(kDockScalePercent))
        {
            updated.thicknessScale = ClampDockScale(kDockScalePercent[command - kDockScaleFirst] / 100.f);
            layoutChanged = updated.thicknessScale != dockSettings_.thicknessScale;
        }
        else if (command >= kMergedDockScaleFirst && command < kMergedDockScaleFirst + 3)
        {
            updated.mergedIconScale = ClampMergedDockIconScale(kDockScalePercent[command - kMergedDockScaleFirst] / 100.f);
            layoutChanged = updated.mergedIconScale != dockSettings_.mergedIconScale;
        }
        else if (command >= kMergedBarHeightFirst && command < kMergedBarHeightFirst + std::size(kMergedBarHeights))
        {
            updated.mergedBarHeight = kMergedBarHeights[command - kMergedBarHeightFirst];
            layoutChanged = updated.mergedBarHeight != dockSettings_.mergedBarHeight;
        }
        else return;
        break;
    }

    dockSettings_ = updated;
    SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
    if (settingsController_)
        (void)settingsController_->SynchronizeDock(dockSettings_);
    if (layoutChanged)
    {
        ApplyFloatingDockHotkey();
        UpdateLayoutWorkArea();
        LayoutItems();
        InvalidateDragStaticScene();
    }
    // Visibility preferences also belong to layout backups, even when the
    // current geometry does not change.
    SaveLayoutSlots();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::ShowDockRunningAppContextMenu(
    POINT screenPoint, size_t runningIndex)
{
    if (runningIndex >=
        dockUnpinnedRunningApps_.size())
        return;

    const DockRunningAppInfo running =
        dockUnpinnedRunningApps_[runningIndex];
    if (!running.presence.Interactive()) return;
    const bool explorer = snowdesktop::shortcut_application_rules::
        IsExplorerExecutable(running.executablePath) &&
        snowdesktop::dock_explorer_pin::IsFolderWindow(running.window);
    DockAppIdentity identity;
    identity.executablePath =
        running.executablePath;
    identity.appUserModelId =
        running.appUserModelId;
    identity.kind =
        !identity.appUserModelId.empty()
        ? DockAppIdentityKind::Applications
        : DockAppIdentityKind::Executable;
    if (identity.kind ==
            DockAppIdentityKind::Executable &&
        identity.executablePath.empty())
        return;
    shellVisualWork_.Cancel(L"dock-running-menu:");

    struct CatalogApplication
    {
        QuickNavigationAppEntry entry;
        snowdesktop::dock_running_app_pin::ApplicationIdentity identity;
        std::wstring folderPath;
        std::shared_ptr<CatalogApplication> currentFolder;
        std::vector<snowdesktop::dock_explorer_pin::FolderPinSource> pinSources;
        bool folderAlreadyPinned = false;
    };

    auto readPinSources = [this] {
        std::vector<snowdesktop::dock_explorer_pin::FolderPinSource> sources;
        for (const auto& entry : dockEntries_)
        {
            if (entry.type == DockEntryType::FolderMapping)
            {
                const auto index = FindWidgetIndexById(entry.reference);
                if (index < widgets_.size())
                    sources.push_back({widgets_[index].sourceFolderPath, L"M:" + entry.reference});
            }
            else if (entry.type == DockEntryType::DesktopItem && !IsRecycleBinDockEntry(entry))
            {
                const auto index = FindItemIndexByKey(entry.reference);
                if (index < items_.size())
                    sources.push_back({items_[index].parsingName,
                        L"I:" + entry.reference + L"\n" + snowdesktop::shell_icon_request::Stamp(items_[index])});
                else sources.push_back({entry.reference, L"I:" + entry.reference});
            }
        }
        return sources;
    };

    // Own the target across the native menu loop: application indexing and
    // desktop refresh may replace their vectors while the menu is open.
    auto showMenu = [this, running, identity, screenPoint, explorer, readPinSources](
        std::shared_ptr<CatalogApplication> application) {
        if (!hwnd_ || !IsWindow(hwnd_) ||
            (explorer && !snowdesktop::dock_explorer_pin::IsFolderWindow(running.window)) ||
            std::none_of(dockUnpinnedRunningApps_.begin(),
                dockUnpinnedRunningApps_.end(), [&](const auto& current) {
                    return current.presence.Interactive() && current.identityKey == running.identityKey;
                }) || dragSession_.HasContext() ||
            dragDropController_.IsTransportActive())
            return;

        if (explorer && application && readPinSources() != application->pinSources)
        {
            // A pin changed while Shell was reading. Rebuild from a new snapshot
            // rather than expose a stale duplicate action.
            const auto current = std::find_if(dockUnpinnedRunningApps_.begin(),
                dockUnpinnedRunningApps_.end(), [&](const auto& value) {
                    return value.identityKey == running.identityKey;
                });
            if (current != dockUnpinnedRunningApps_.end())
                ShowDockRunningAppContextMenu(screenPoint,
                    static_cast<size_t>(current - dockUnpinnedRunningApps_.begin()));
            return;
        }

        std::wstring matchingDesktopKey;
        // Explorer folder launchers can share its EXE/AUMID. Always create
        // the explicit default launcher rather than move an unrelated link.
        if (!explorer)
            if (const auto itemIndex = FindDesktopItemForDockRunningApp(running);
                itemIndex && *itemIndex < items_.size())
                matchingDesktopKey = items_[*itemIndex].layoutKey;

        PrepareMenuIconsForPoint(screenPoint);

        HMENU menu = CreatePopupMenu();
        if (!menu)
            return;
        if (explorer && (!application || !application->folderAlreadyPinned))
        {
            const bool hasFolder = application && application->currentFolder;
            std::wstring label = _LW("app.dock.pin_current_folder_to_files");
            if (hasFolder)
            {
                std::wstring name;
                for (const auto ch : application->currentFolder->entry.name)
                {
                    name += ch;
                    if (ch == L'&') name += ch;
                }
                label += L" (" + name + L")";
            }
            AppendMenuW(menu, MF_STRING | (hasFolder ? 0 : MF_GRAYED),
                kContextDockPinCurrentFolder, label.c_str());
            SetMenuItemIcon(menu, kContextDockPinCurrentFolder,
                snowdesktop::menu_fluent_glyphs::kPin, MenuIconFont::FluentRegular);
        }
        if (!matchingDesktopKey.empty() || application)
        {
            AppendMenuW(
                menu, MF_STRING,
                kContextDockPinMoveToDock,
                explorer ? _LW("app.dock.pin_explorer_to_fixed")
                         : _LW("app.dock.pin_move_to_dock"));
            if (!matchingDesktopKey.empty())
                AppendMenuW(
                    menu, MF_STRING,
                    kContextDockCreateMapping,
                    _LW("app.dock.create_mapping"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            SetMenuItemIcon(
                menu, kContextDockPinMoveToDock,
                snowdesktop::menu_fluent_glyphs::kPin,
                MenuIconFont::FluentRegular);
            if (!matchingDesktopKey.empty())
                SetMenuItemIcon(
                    menu, kContextDockCreateMapping,
                    snowdesktop::menu_fluent_glyphs::kLinkAdd,
                    MenuIconFont::FluentRegular);
        }
        AppendMenuW(
            menu, MF_STRING,
            kContextDockCloseApplication,
            _LW("app.dock.close_application"));
        SetMenuItemIcon(
            menu, kContextDockCloseApplication,
            L"");

        DismissDockWindowPreviewUntilLeave();
        RestoreInteractionInputFocus();
        const UINT command = ShowModernMenu(menu, screenPoint, hwnd_, true);
        DestroyMenu(menu);
        ClearMenuIcons();
        RestoreDesktopWindowLayer();
        RestoreInteractionInputFocus();
        if (!hwnd_ || !IsWindow(hwnd_) || dragSession_.HasContext() ||
            dragDropController_.IsTransportActive())
            return;

        auto adoptRunning = [this, runningKey = running.identityKey](size_t itemIndex) {
            AdoptDockRunningPresentation(runningKey, itemIndex);
        };
        // The native menu pumps messages. Prefer a desktop source that appeared
        // while it was open, rather than creating a second shortcut for that app.
        if (command == kContextDockPinMoveToDock && !explorer)
            if (const auto itemIndex = FindDesktopItemForDockRunningApp(running);
                itemIndex && *itemIndex < items_.size())
                matchingDesktopKey = items_[*itemIndex].layoutKey;

        if ((command == kContextDockPinMoveToDock ||
                command == kContextDockCreateMapping) &&
            !matchingDesktopKey.empty())
        {
            const size_t itemIndex =
                FindItemIndexByKey(matchingDesktopKey);
            POINT clientPoint = screenPoint;
            if (itemIndex < items_.size() &&
                ScreenToClient(hwnd_, &clientPoint))
            {
                if (DockContainer* dock =
                        GetDockContainerAtPoint(clientPoint))
                {
                    DesktopIcon source(
                        &items_[itemIndex], nullptr, this);
                    const int mods =
                        command == kContextDockCreateMapping
                        ? MK_CONTROL : 0;
                    CommitDockDrop(
                        { &source }, nullptr, dock,
                        dock->GetInsertIndexAtPoint(clientPoint),
                        mods);
                    if (std::any_of(dockEntries_.begin(), dockEntries_.end(),
                            [&](const auto& entry) {
                                return entry.type == DockEntryType::DesktopItem &&
                                    entry.reference == ToUpperInvariant(matchingDesktopKey);
                            }))
                        adoptRunning(itemIndex);
                    SaveLayoutSlots();
                    ApplyPageMapping();
                    LayoutItems();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return;
        }

        auto pinApplication = [this, screenPoint, runningKey = running.identityKey](
            const std::shared_ptr<CatalogApplication>& target) {
            POINT clientPoint = screenPoint;
            if (!hwnd_ || !ScreenToClient(hwnd_, &clientPoint)) return;
            DockContainer* dock = GetDockContainerAtPoint(clientPoint);
            if (!dock) return;
            DockRunningPinTarget pinTarget;
            pinTarget.entry.name = target->entry.name;
            pinTarget.entry.parsingName = target->entry.parsingName;
            pinTarget.entry.absolutePidl.reset(ILCloneFull(target->entry.absolutePidl.get()));
            pinTarget.identity.executablePath = target->identity.executablePath;
            pinTarget.identity.appUserModelId = target->identity.appUserModelId;
            pinTarget.folderPath = target->folderPath;
            if (!PinDockRunningTarget(runningKey, pinTarget, dock->GetInsertIndexAtPoint(clientPoint)))
                MessageBeep(MB_ICONWARNING);
        };
        if (command == kContextDockPinCurrentFolder)
        {
            if (!explorer || !application || !application->currentFolder ||
                application->folderAlreadyPinned) return;
            const auto folder = application->currentFolder;
            const auto sources = readPinSources();
            shellVisualWork_.Submit(L"dock-folder-pin:" + running.identityKey,
                [folder, sources] {
                    return snowdesktop::dock_explorer_pin::AlreadyPinnedFolder(folder->folderPath, sources);
                }, [this, folder, sources, readPinSources, pinApplication](bool duplicate) {
                    // Check again after the native menu pumps messages. If the
                    // Dock changed during this read, leave it untouched.
                    if (duplicate || !hwnd_ || !IsWindow(hwnd_) || exitRequested_ ||
                        readPinSources() != sources) return;
                    pinApplication(folder);
                }, hwnd_, kBackgroundShellReadyMessage);
            return;
        }
        if (command == kContextDockPinMoveToDock && application)
        {
            pinApplication(application);
            return;
        }

        if (command ==
            kContextDockCloseApplication)
            CloseDockApplicationWindows(identity);
    };

    // Desktop sources retain both existing actions without waiting for Shell
    // providers. Catalog-only sources get one action that creates a new link.
    if (!explorer && FindDesktopItemForDockRunningApp(running))
    {
        showMenu(nullptr);
        return;
    }

    auto applications = std::make_shared<std::vector<QuickNavigationAppEntry>>();
    if (!explorer && quickNavigationAppsIndexed_)
    {
        for (const auto& entry : quickNavigationAppEntries_)
        {
            QuickNavigationAppEntry copy;
            copy.name = entry.name;
            copy.parsingName = entry.parsingName;
            copy.absolutePidl.reset(ILCloneFull(entry.absolutePidl.get()));
            if (copy.absolutePidl.get()) applications->push_back(std::move(copy));
        }
    }
    const bool indexed = quickNavigationAppsIndexed_;
    const std::wstring explorerName = _LW("app.dock.explorer_name");
    const auto pinSources = readPinSources();
    const bool submitted = shellVisualWork_.Submit(
        L"dock-running-menu:" + running.identityKey,
        [applications, indexed, running, explorer, explorerName, pinSources] {
            if (explorer)
            {
                auto application = std::make_shared<CatalogApplication>();
                const auto path = snowdesktop::dock_explorer_pin::ExecutablePath();
                PIDLIST_ABSOLUTE target = nullptr;
                if (path.empty() || FAILED(SHParseDisplayName(path.c_str(), nullptr,
                        &target, 0, nullptr)) || !target)
                    return std::shared_ptr<CatalogApplication>{};
                application->entry.absolutePidl.reset(target);
                application->entry.name = explorerName;
                application->entry.parsingName = path;
                application->identity.executablePath = NormalizeDockExecutablePath(path);
                application->pinSources = pinSources;
                if (const auto folder = snowdesktop::dock_explorer_pin::ReadCurrentFolder(running.window))
                {
                    target = nullptr;
                    if (SUCCEEDED(SHParseDisplayName(folder->path.c_str(), nullptr,
                            &target, 0, nullptr)) && target)
                    {
                        auto current = std::make_shared<CatalogApplication>();
                        current->entry.absolutePidl.reset(target);
                        current->entry.name = folder->name;
                        current->entry.parsingName = folder->path;
                        current->folderPath = folder->path;
                        application->folderAlreadyPinned = snowdesktop::dock_explorer_pin::
                            AlreadyPinnedFolder(folder->path, pinSources);
                        application->currentFolder = std::move(current);
                    }
                }
                return application;
            }
            if (!indexed)
            {
                HIMAGELIST systemImageList = nullptr;
                *applications = BuildQuickNavigationAppIndex(
                    nullptr, systemImageList, 32);
            }
            std::vector<snowdesktop::dock_running_app_pin::ApplicationIdentity>
                identities;
            identities.reserve(applications->size());
            for (const auto& entry : *applications)
            {
                auto app = snowdesktop::dock_running_app_pin::ReadApplicationIdentity(
                    entry.absolutePidl.get());
                app.appUserModelId = ToUpperInvariant(app.appUserModelId);
                app.executablePath = NormalizeDockExecutablePath(app.executablePath);
                if (app.appUserModelId.empty())
                {
                    const auto target = snowdesktop::logical_slot_picker_rules::
                        NormalizeApplicationLaunchTarget(entry.parsingName);
                    constexpr std::wstring_view appsFolderMarker = L"APPSFOLDER\\";
                    const auto marker = ToUpperInvariant(target).find(appsFolderMarker);
                    if (marker != std::wstring::npos)
                        app.appUserModelId = ToUpperInvariant(
                            target.substr(marker + appsFolderMarker.size()));
                }
                identities.push_back(std::move(app));
            }
            const auto match = snowdesktop::dock_running_app_pin::FindApplication(
                identities, running.executablePath, running.appUserModelId,
                running.ancestorExecutablePaths);
            if (!match) return std::shared_ptr<CatalogApplication>{};
            auto application = std::make_shared<CatalogApplication>();
            application->entry = std::move((*applications)[*match]);
            application->identity = std::move(identities[*match]);
            return application;
        }, [showMenu, screenPoint, foreground = GetForegroundWindow()](
            std::shared_ptr<CatalogApplication> application) {
            POINT cursor{};
            // A slow Shell provider must not open an obsolete context menu
            // after the user has moved to another target or another window.
            if (GetForegroundWindow() != foreground || !GetCursorPos(&cursor) ||
                std::abs(cursor.x - screenPoint.x) > GetSystemMetrics(SM_CXDRAG) ||
                std::abs(cursor.y - screenPoint.y) > GetSystemMetrics(SM_CYDRAG))
                return;
            showMenu(std::move(application));
        }, hwnd_, kBackgroundShellReadyMessage);
    if (!submitted) showMenu(nullptr);
}

/**
 * @brief 连续显示行列调整菜单。
 * @details 标准 Win32 菜单执行命令后会结束。这里在每次调整完成后立即按
 *          最新行列数重建菜单，方便用户连续增加或减少行列。
 */
