#include "app.h"
#include "../modern_menu.h"

// Converts the existing HMENU command model into fully custom popup windows.

namespace
{

bool TranslateLegacyBuiltinGlyph(
    const wchar_t* glyph, std::wstring& translated)
{
    if (!glyph || glyph[0] == L'\0' || glyph[1] != L'\0')
        return false;
    const wchar_t* fluent = nullptr;
    switch (glyph[0])
    {
    case 0xE494: fluent = L"\uF41C"; break; // folder add
    case 0xF002: fluent = L"\uF68F"; break; // search
    case 0xF00A: fluent = L"\uF462"; break; // grid
    case 0xF00C: fluent = L"\uF294"; break; // checkmark
    case 0xF00D: fluent = L"\uF369"; break; // dismiss
    case 0xF013: fluent = L"\uF6A9"; break; // settings
    case 0xF017: fluent = L"\uF21D"; break; // calendar clock
    case 0xF019: fluent = L"\uF150"; break; // download
    case 0xF021: fluent = L"\uF13D"; break; // refresh
    case 0xF023: fluent = L"\uE78F"; break; // lock
    case 0xF031: fluent = L"\uF7E4"; break; // font
    case 0xF032: fluent = L"\uF7A4"; break; // bold
    case 0xF03A: fluent = L"\uF4ED"; break; // list
    case 0xF044: fluent = L"\uF669"; break; // rename
    case 0xF067: fluent = L"\uF109"; break; // add
    case 0xF068: fluent = L"\uEBD0"; break; // subtract
    case 0xF06E: fluent = L"\uE5F2"; break; // eye
    case 0xF070: fluent = L"\uE5F5"; break; // eye off
    case 0xF073: fluent = L"\uF21D"; break; // calendar
    case 0xF07B: fluent = L"\uF418"; break; // folder
    case 0xF07C: fluent = L"\uF42E"; break; // folder open
    case 0xF08D: fluent = L"\uF601"; break; // pin
    case 0xF08E: fluent = L"\uF582"; break; // open
    case 0xF09C: fluent = L"\uE795"; break; // lock open
    case 0xF0AE: fluent = L"\uF4ED"; break; // list/checklist
    case 0xF0B2: fluent = L"\uF8E5"; break; // move
    case 0xF0C4: fluent = L"\uF33A"; break; // cut
    case 0xF0C5: fluent = L"\uF32B"; break; // copy
    case 0xF0CA: fluent = L"\uE6CA"; break; // grouped list
    case 0xF0EA: fluent = L"\uF2D5"; break; // paste
    case 0xF108: fluent = L"\uF359"; break; // desktop
    case 0xF12E: fluent = L"\uE9E9"; break; // puzzle piece
    case 0xF141: fluent = L"\uE824"; break; // more horizontal
    case 0xF15B: fluent = L"\uF378"; break; // document
    case 0xF15D: fluent = L"\uF802"; break; // sort ascending
    case 0xF160: fluent = L"\uF803"; break; // sort descending
    case 0xF177: fluent = L"\uF15B"; break; // arrow left
    case 0xF178: fluent = L"\uF181"; break; // arrow right
    case 0xF1B3: fluent = L"\uF2F1"; break; // collections
    case 0xF1D8: fluent = L"\uF699"; break; // send
    case 0xF1DE: fluent = L"\uF587"; break; // options
    case 0xF2ED: fluent = L"\uF34C"; break; // delete
    case 0xF2F1: fluent = L"\uF13D"; break; // restart
    case 0xF337: fluent = L"\uEC45"; break; // spacing
    case 0xF35D: fluent = L"\uF582"; break; // open
    case 0xF53F: fluent = L"\uF2F5"; break; // color
    case 0xF58D: fluent = L"\uF8CB"; break; // layout/list
    case 0xF802: fluent = L"\uF418"; break; // folder category
    default: return false;
    }
    translated = fluent;
    return true;
}

snowdesktop::MenuQuickIcon ResolveQuickIcon(UINT_PTR command)
{
    using snowdesktop::MenuQuickIcon;
    if (command == kContextPasteCommand)
        return MenuQuickIcon::Paste;
    if (command == kContextNewMenu)
        return MenuQuickIcon::NewItem;
    if (command == kContextRefreshCommand)
        return MenuQuickIcon::Refresh;
    if (command == kContextCutCommand)
        return MenuQuickIcon::Cut;
    if (command == kContextCopyCommand)
        return MenuQuickIcon::Copy;
    if (command == kContextRenameCommand ||
        command == kContextWidgetRename)
        return MenuQuickIcon::Rename;
    if (command == kContextDeleteCommand ||
        command == kContextWidgetDelete)
        return MenuQuickIcon::Delete;
    if (command == kContextWidgetEdit)
        return MenuQuickIcon::Settings;
    return MenuQuickIcon::FontGlyph;
}

bool IsWindowsAppLightThemeEnabled()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
            &value, &size) != ERROR_SUCCESS)
        return true;
    return value != 0;
}

bool TryGetTaskbarRectAtPoint(POINT screenPoint, RECT& taskbarRect)
{
    HWND window = WindowFromPoint(screenPoint);
    while (window)
    {
        wchar_t className[64]{};
        GetClassNameW(window, className,
            static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Shell_TrayWnd") == 0 ||
            wcscmp(className, L"Shell_SecondaryTrayWnd") == 0)
        {
            return GetWindowRect(window, &taskbarRect) != FALSE;
        }
        window = GetParent(window);
    }
    return false;
}

enum class TrayEdge
{
    Bottom,
    Top,
    Left,
    Right,
};

TrayEdge ResolveTrayEdge(
    const RECT& surface, const MONITORINFO& monitorInfo)
{
    const int surfaceWidth = surface.right - surface.left;
    const int surfaceHeight = surface.bottom - surface.top;
    const int monitorWidth =
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    const int monitorHeight =
        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
    if (surfaceWidth >= monitorWidth / 2 &&
        surfaceWidth >= surfaceHeight)
    {
        return surface.top + surface.bottom >=
                monitorInfo.rcMonitor.top + monitorInfo.rcMonitor.bottom
            ? TrayEdge::Bottom : TrayEdge::Top;
    }
    if (surfaceHeight >= monitorHeight / 2 &&
        surfaceHeight > surfaceWidth)
    {
        return surface.left + surface.right >=
                monitorInfo.rcMonitor.left + monitorInfo.rcMonitor.right
            ? TrayEdge::Right : TrayEdge::Left;
    }

    // A small top-level surface is the Windows hidden-icons flyout.  Follow
    // the taskbar edge reserved by the work area; Windows 11 uses the bottom
    // edge when an auto-hidden taskbar leaves no work-area inset.
    if (monitorInfo.rcWork.bottom < monitorInfo.rcMonitor.bottom)
        return TrayEdge::Bottom;
    if (monitorInfo.rcWork.top > monitorInfo.rcMonitor.top)
        return TrayEdge::Top;
    if (monitorInfo.rcWork.right < monitorInfo.rcMonitor.right)
        return TrayEdge::Right;
    if (monitorInfo.rcWork.left > monitorInfo.rcMonitor.left)
        return TrayEdge::Left;
    return TrayEdge::Bottom;
}

void PlaceMenuAwayFromTraySurface(
    snowdesktop::modern_menu::Options& options,
    POINT screenPoint,
    const RECT& surface,
    const MONITORINFO& monitorInfo,
    int gap)
{
    switch (ResolveTrayEdge(surface, monitorInfo))
    {
    case TrayEdge::Bottom:
    {
        const int edge = std::min(
            surface.top, monitorInfo.rcWork.bottom) - gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::AboveAnchorRect;
        options.anchorRect = {
            screenPoint.x, edge, screenPoint.x + 1, edge };
        break;
    }
    case TrayEdge::Top:
    {
        const int edge = std::max(
            surface.bottom, monitorInfo.rcWork.top) + gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::BelowAnchorRect;
        options.anchorRect = {
            screenPoint.x, edge, screenPoint.x + 1, edge };
        break;
    }
    case TrayEdge::Right:
    {
        const int edge = std::min(
            surface.left, monitorInfo.rcWork.right) - gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::LeftOfAnchorRect;
        options.anchorRect = {
            edge, screenPoint.y, edge, screenPoint.y + 1 };
        break;
    }
    case TrayEdge::Left:
    {
        const int edge = std::max(
            surface.right, monitorInfo.rcWork.left) + gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::RightOfAnchorRect;
        options.anchorRect = {
            edge, screenPoint.y, edge, screenPoint.y + 1 };
        break;
    }
    }
}

void ConfigureTrayMenuPlacement(
    snowdesktop::modern_menu::Options& options,
    POINT screenPoint,
    const RECT* capturedTraySurface)
{
    HMONITOR monitor = MonitorFromPoint(
        screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return;

    const int gap = std::max(1, MulDiv(
        8, static_cast<int>(options.dpi), USER_DEFAULT_SCREEN_DPI));
    if (capturedTraySurface)
    {
        PlaceMenuAwayFromTraySurface(options, screenPoint,
            *capturedTraySurface, monitorInfo, gap);
        return;
    }
    RECT taskbar{};
    if (TryGetTaskbarRectAtPoint(screenPoint, taskbar))
    {
        PlaceMenuAwayFromTraySurface(options, screenPoint,
            taskbar, monitorInfo, gap);
        return;
    }

    // Overflow trays and auto-hidden taskbars may not expose a taskbar HWND at
    // the click point.  In that case, place the menu away from the nearest
    // monitor edge and still keep an eight-pixel gap from the work area.
    const RECT& work = monitorInfo.rcWork;
    const int leftDistance = std::abs(screenPoint.x - work.left);
    const int rightDistance = std::abs(work.right - screenPoint.x);
    const int topDistance = std::abs(screenPoint.y - work.top);
    const int bottomDistance = std::abs(work.bottom - screenPoint.y);
    const int nearest = std::min({
        leftDistance, rightDistance, topDistance, bottomDistance });
    if (nearest == bottomDistance)
    {
        const int edge = work.bottom - gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::AboveAnchorRect;
        options.anchorRect = {
            screenPoint.x, edge, screenPoint.x + 1, edge };
    }
    else if (nearest == topDistance)
    {
        const int edge = work.top + gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::BelowAnchorRect;
        options.anchorRect = {
            screenPoint.x, edge, screenPoint.x + 1, edge };
    }
    else if (nearest == rightDistance)
    {
        const int edge = work.right - gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::LeftOfAnchorRect;
        options.anchorRect = {
            edge, screenPoint.y, edge, screenPoint.y + 1 };
    }
    else
    {
        const int edge = work.left + gap;
        options.rootPlacement =
            snowdesktop::modern_menu::RootPlacement::RightOfAnchorRect;
        options.anchorRect = {
            edge, screenPoint.y, edge, screenPoint.y + 1 };
    }
}

} // namespace

void DesktopApp::PrepareMenuIconsForPoint(POINT screenPoint)
{
    ClearMenuIcons();

    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    HMONITOR monitor = MonitorFromPoint(
        screenPoint, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 0;
    UINT dpiY = 0;
    if (monitor && SUCCEEDED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) &&
        dpiY > 0)
    {
        dpi = dpiY;
    }
    else if (hwnd_)
    {
        const UINT windowDpi = GetDpiForWindow(hwnd_);
        if (windowDpi > 0)
            dpi = windowDpi;
    }

    menuIconDpi_ = dpi;
    PersonalizationSettings appearance;
    if (settingsWindow_)
        appearance = settingsWindow_->GetPersonalization();
    else
        LoadPersonalization(
            GetPersonalizationPath().c_str(), appearance);
    menuAppearanceStyle_ = std::clamp(
        appearance.contextMenuStyle, 0, 2);
    menuLightTheme_ = menuAppearanceStyle_ == 1
        ? true
        : menuAppearanceStyle_ == 2
            ? false
            : IsWindowsAppLightThemeEnabled();
}

void DesktopApp::SetMenuItemIcon(
    HMENU menu, UINT_PTR command, const wchar_t* text,
    MenuIconFont font)
{
    if (!menu || !text || !*text)
        return;

    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        MENUITEMINFOW probe{ sizeof(probe) };
        probe.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &probe))
            continue;
        if (probe.wID != command &&
            reinterpret_cast<UINT_PTR>(probe.hSubMenu) != command)
            continue;

        MenuIconEntry* entry = nullptr;
        for (auto& existing : menuIconPool_)
        {
            if (existing->menu == menu &&
                existing->position == static_cast<UINT>(i))
            {
                entry = existing.get();
                break;
            }
        }
        if (!entry)
        {
            auto created = std::make_unique<MenuIconEntry>();
            created->menu = menu;
            created->position = static_cast<UINT>(i);
            entry = created.get();
            menuIconPool_.push_back(std::move(created));
        }

        entry->glyph.clear();
        entry->fontAwesome = font == MenuIconFont::FontAwesomeSolid;
        if (!entry->quickAction)
        {
            entry->quickIcon = command == kContextMoreCommand
                ? snowdesktop::MenuQuickIcon::Open
                : snowdesktop::MenuQuickIcon::FontGlyph;
        }
        if (font == MenuIconFont::BuiltinFluentFromLegacy &&
            !TranslateLegacyBuiltinGlyph(text, entry->glyph))
        {
            // A yet-unmapped built-in remains readable using the old font.
            entry->glyph = text;
            entry->fontAwesome = true;
        }
        else if (font != MenuIconFont::BuiltinFluentFromLegacy)
        {
            entry->glyph = text;
        }
        return;
    }
}

void DesktopApp::SetMenuItemQuickAction(
    HMENU menu, UINT_PTR command)
{
    if (!menu)
        return;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        MENUITEMINFOW probe{ sizeof(probe) };
        probe.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &probe))
            continue;
        if (probe.wID != command &&
            reinterpret_cast<UINT_PTR>(probe.hSubMenu) != command)
            continue;
        for (auto& entry : menuIconPool_)
        {
            if (entry->menu == menu &&
                entry->position == static_cast<UINT>(i))
            {
                entry->quickAction = true;
                entry->quickIcon = ResolveQuickIcon(command);
                return;
            }
        }
        auto entry = std::make_unique<MenuIconEntry>();
        entry->menu = menu;
        entry->position = static_cast<UINT>(i);
        entry->quickAction = true;
        entry->quickIcon = ResolveQuickIcon(command);
        menuIconPool_.push_back(std::move(entry));
        return;
    }
}

void DesktopApp::SetMenuItemInlineAction(
    HMENU menu, UINT_PTR command, UINT group, bool compact)
{
    if (!menu)
        return;
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        MENUITEMINFOW probe{ sizeof(probe) };
        probe.fMask = MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &probe))
            continue;
        if (probe.wID != command &&
            reinterpret_cast<UINT_PTR>(probe.hSubMenu) != command)
            continue;
        for (auto& entry : menuIconPool_)
        {
            if (entry->menu == menu &&
                entry->position == static_cast<UINT>(i))
            {
                entry->inlineAction = true;
                entry->inlineGroup = group;
                entry->compactInlineAction = compact;
                return;
            }
        }
        auto entry = std::make_unique<MenuIconEntry>();
        entry->menu = menu;
        entry->position = static_cast<UINT>(i);
        entry->inlineAction = true;
        entry->inlineGroup = group;
        entry->compactInlineAction = compact;
        menuIconPool_.push_back(std::move(entry));
        return;
    }
}

UINT DesktopApp::ShowModernMenu(
    HMENU rootMenu, POINT screenPoint, HWND owner,
    bool placeOutsideDock, bool placeAwayFromTaskbar,
    const RECT* capturedTraySurface,
    std::function<bool(UINT,
        std::vector<snowdesktop::modern_menu::Item>&)> onCommand,
    std::function<void(const snowdesktop::modern_menu::HoverInfo&)>
        onHover)
{
    if (!rootMenu)
        return 0;

    std::function<std::vector<snowdesktop::modern_menu::Item>(HMENU)>
        buildItems;
    buildItems = [&](HMENU menu) {
        std::vector<snowdesktop::modern_menu::Item> result;
        const int count = GetMenuItemCount(menu);
        result.reserve(static_cast<size_t>(std::max(0, count)));
        for (int i = 0; i < count; ++i)
        {
            MENUITEMINFOW probe{ sizeof(probe) };
            probe.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID |
                MIIM_SUBMENU | MIIM_STRING;
            if (!GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &probe))
                continue;

            std::vector<wchar_t> label(
                static_cast<size_t>(probe.cch) + 1, L'\0');
            if (probe.cch > 0)
            {
                MENUITEMINFOW textInfo{ sizeof(textInfo) };
                textInfo.fMask = MIIM_STRING;
                textInfo.dwTypeData = label.data();
                textInfo.cch = static_cast<UINT>(label.size());
                GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE,
                    &textInfo);
            }

            snowdesktop::modern_menu::Item item;
            item.command = probe.wID;
            item.label = label.data();
            item.enabled =
                (probe.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
            item.checked = (probe.fState & MFS_CHECKED) != 0;
            item.separator = (probe.fType & MFT_SEPARATOR) != 0;
            if (probe.hSubMenu)
                item.children = buildItems(probe.hSubMenu);

            for (const auto& icon : menuIconPool_)
            {
                if (icon->menu == menu &&
                    icon->position == static_cast<UINT>(i))
                {
                    item.glyph = icon->glyph;
                    item.iconFont = icon->fontAwesome
                        ? snowdesktop::modern_menu::IconFont::FontAwesomeSolid
                        : snowdesktop::modern_menu::IconFont::FluentRegular;
                    item.quickAction = icon->quickAction;
                    item.inlineAction = icon->inlineAction;
                    item.inlineGroup = icon->inlineGroup;
                    item.compactInlineAction =
                        icon->compactInlineAction;
                    item.quickIcon = icon->quickIcon;
                    break;
                }
            }
            result.push_back(std::move(item));
        }
        return result;
    };

    const std::vector<snowdesktop::modern_menu::Item> items =
        buildItems(rootMenu);
    snowdesktop::modern_menu::Options options;
    options.owner = owner;
    options.anchor = screenPoint;
    options.dpi = menuIconDpi_;
    options.lightTheme = menuLightTheme_;
    options.appearance = static_cast<
        snowdesktop::modern_menu::Appearance>(menuAppearanceStyle_);
    options.onCommand = std::move(onCommand);
    options.onHover = std::move(onHover);
    if (placeAwayFromTaskbar)
    {
        options.topmost = true;
        ConfigureTrayMenuPlacement(
            options, screenPoint, capturedTraySurface);
    }
    else if (placeOutsideDock && hwnd_ && IsWindow(hwnd_))
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        if (DockContainer* dock = GetDockContainerAtPoint(clientPoint))
        {
            RECT dockRect = dock->GetInteractiveBounds();
            POINT corners[] = {
                { dockRect.left, dockRect.top },
                { dockRect.right, dockRect.bottom },
            };
            MapWindowPoints(hwnd_, nullptr, corners, 2);
            options.anchorRect = {
                corners[0].x, corners[0].y,
                corners[1].x, corners[1].y,
            };
            switch (dockSettings_.position)
            {
            case DockPosition::Top:
                options.rootPlacement = snowdesktop::modern_menu::
                    RootPlacement::BelowAnchorRect;
                break;
            case DockPosition::Left:
                options.rootPlacement = snowdesktop::modern_menu::
                    RootPlacement::RightOfAnchorRect;
                break;
            case DockPosition::Right:
                options.rootPlacement = snowdesktop::modern_menu::
                    RootPlacement::LeftOfAnchorRect;
                break;
            case DockPosition::Bottom:
            default:
                options.rootPlacement = snowdesktop::modern_menu::
                    RootPlacement::AboveAnchorRect;
                break;
            }
        }
    }
    const snowdesktop::modern_menu::Result result =
        snowdesktop::modern_menu::Show(items, options);

    if (result.command == kContextGridAdjustmentMenu)
    {
        gridAdjustmentMenuAnchor_ = {
            result.itemScreenRect.right,
            result.itemScreenRect.top,
        };
        gridAdjustmentMenuAnchorValid_ = true;
    }
    return result.command;
}

void DesktopApp::ClearMenuIcons()
{
    menuIconPool_.clear();
}
