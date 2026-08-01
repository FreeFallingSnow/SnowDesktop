#include "modern_menu.h"

#include "menu_icon_render.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <string_view>

namespace snowdesktop::modern_menu
{
namespace
{

constexpr wchar_t kMenuWindowClass[] =
    L"SnowDesktop.ModernMenuPopup";
constexpr UINT_PTR kSubmenuOpenTimer = 1;
constexpr UINT_PTR kSubmenuCloseTimer = 2;
constexpr UINT kSubmenuOpenDelayMs = 480;
constexpr UINT kSubmenuCloseDelayMs = 420;
constexpr UINT kCancelMessage = WM_APP + 0x311;
std::atomic<HWND> gActiveRootMenu{ nullptr };

int Scale(int value, UINT dpi)
{
    return std::max(1, MulDiv(value, static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

bool IsSelectable(const Item& item)
{
    return !item.separator && item.enabled;
}

bool ResolveLightTheme(const Options& options)
{
    if (options.appearance == Appearance::SystemLightBlur)
        return true;
    if (options.appearance == Appearance::SystemDarkBlur)
        return false;
    return options.lightTheme;
}

bool UsesSystemBlur(const Options& options)
{
    return options.appearance == Appearance::FollowSystem ||
        options.appearance == Appearance::SystemLightBlur ||
        options.appearance == Appearance::SystemDarkBlur;
}

// SetWindowCompositionAttribute is intentionally resolved dynamically: it is
// available on supported Windows versions but is not part of the public SDK
// import library.  The documented DWM transient backdrop remains the primary
// path, with Acrylic accent providing the Windows 10 and layered-window path.
enum class WindowCompositionAttribute
{
    AccentPolicy = 19,
};

enum class AccentState
{
    Disabled = 0,
    BlurBehind = 3,
    AcrylicBlurBehind = 4,
};

struct AccentPolicy
{
    AccentState state = AccentState::Disabled;
    DWORD flags = 0;
    DWORD gradientColor = 0;
    DWORD animationId = 0;
};

struct WindowCompositionAttributeData
{
    WindowCompositionAttribute attribute =
        WindowCompositionAttribute::AccentPolicy;
    void* data = nullptr;
    size_t size = 0;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(
    HWND, WindowCompositionAttributeData*);
using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(
    HWND, DWORD, const void*, DWORD);
using DwmExtendFrameIntoClientAreaFn = HRESULT(WINAPI*)(
    HWND, const MARGINS*);

class MenuController;

struct Popup
{
    MenuController* controller = nullptr;
    const std::vector<Item>* items = nullptr;
    HWND hwnd = nullptr;
    int depth = 0;
    int parentItem = -1;
    int hoveredItem = -1;
    int keyboardItem = -1;
    int scrollOffset = 0;
    int contentHeight = 0;
    int viewportHeight = 0;
    int panelWidth = 0;
    int panelHeight = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    POINT panelScreenOrigin{};
    std::vector<RECT> itemRects;
};

class MenuController
{
public:
    MenuController(const std::vector<Item>& rootItems,
        const Options& options)
        : rootItems_(rootItems), options_(options),
          lightTheme_(ResolveLightTheme(options)),
          blurEnabled_(UsesSystemBlur(options)),
          palette_(menu_icon::ResolvePalette(lightTheme_)),
          metrics_(menu_icon::ResolveMetrics(options.dpi)),
          // Acrylic is composed for the complete HWND and does not respect an
          // inset alpha-only shadow margin.  Its window must therefore match
          // the panel bounds exactly; DWM supplies the material shadow.
          shadowSize_(UsesSystemBlur(options)
              ? 0 : Scale(12, options.dpi)),
          panelPadding_(Scale(5, options.dpi)),
          panelRadius_(Scale(8, options.dpi))
    {
        const int textHeight = -Scale(14, options.dpi);
        const int iconHeight = -Scale(14, options.dpi);
        textFont_ = CreateFontW(textHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        iconFont_ = CreateFontW(iconHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            options.iconFontFamily && *options.iconFontFamily
                ? options.iconFontFamily
                : L"Segoe UI Symbol");
    }

    ~MenuController()
    {
        CloseFromDepth(0);
        if (iconFont_)
            DeleteObject(iconFont_);
        if (textFont_)
            DeleteObject(textFont_);
    }

    Result Run()
    {
        if (rootItems_.empty() || !RegisterWindowClass())
            return {};

        if (!OpenPopup(rootItems_, 0, -1, options_.anchor, nullptr))
            return {};

        HWND rootWindow = nullptr;
        if (!popups_.empty() && popups_.front()->hwnd)
        {
            rootWindow = popups_.front()->hwnd;
            const HWND previous = gActiveRootMenu.exchange(rootWindow);
            if (previous && previous != rootWindow && IsWindow(previous))
            {
                // A new context-menu request replaces the existing session.
                // Hide synchronously so nested modal loops never leave two
                // Dock menus visible while the old loop processes dismissal.
                ShowWindow(previous, SW_HIDE);
                PostMessageW(previous, kCancelMessage, TRUE, 0);
            }
            ShowWindow(rootWindow, SW_SHOWNORMAL);
            SetForegroundWindow(rootWindow);
            SetFocus(rootWindow);
            AnimateWindow(rootWindow, 80, AW_BLEND);
        }

        MSG message{};
        while (!done_)
        {
            const BOOL status = GetMessageW(&message, nullptr, 0, 0);
            if (status <= 0)
            {
                if (status == 0)
                    PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        HWND expectedRoot = rootWindow;
        gActiveRootMenu.compare_exchange_strong(expectedRoot, nullptr);
        CloseFromDepth(0);
        if (!superseded_ && options_.owner && IsWindow(options_.owner))
        {
            SetForegroundWindow(options_.owner);
            SetFocus(options_.owner);
        }
        return result_;
    }

    LRESULT HandleMessage(Popup& popup, HWND hwnd,
        UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_MOUSEMOVE:
        {
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tracking);
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            SetHoveredItem(popup, HitTest(popup, point), false);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (popup.depth == ActiveDepth() &&
                popup.hoveredItem >= 0 &&
                !HasOpenChild(popup))
                SetHoveredItem(popup, -1, false);
            return 0;

        case WM_LBUTTONUP:
        {
            const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ActivateItem(popup, HitTest(popup, point), false);
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            Scroll(popup, delta > 0 ? -1 : 1);
            return 0;
        }
        case WM_KEYDOWN:
            HandleKey(wParam);
            return 0;
        case WM_CHAR:
            SelectByCharacter(static_cast<wchar_t>(wParam));
            return 0;
        case WM_TIMER:
            if (wParam == kSubmenuOpenTimer)
            {
                KillTimer(hwnd, kSubmenuOpenTimer);
                if (popup.hoveredItem >= 0)
                    OpenSubmenu(popup, popup.hoveredItem, false);
            }
            else if (wParam == kSubmenuCloseTimer)
            {
                KillTimer(hwnd, kSubmenuCloseTimer);
                CloseFromDepth(popup.depth + 1);
                if (popup.hwnd && IsWindow(popup.hwnd))
                    Render(popup);
            }
            return 0;
        case WM_ACTIVATE:
            if (popup.depth == 0 && LOWORD(wParam) == WA_INACTIVE &&
                !closing_ && !IsPopupWindow(
                    reinterpret_cast<HWND>(lParam)))
                PostMessageW(hwnd, kCancelMessage, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            // Cascaded popup windows deliberately do not take activation away
            // from the root menu.  Without this explicit result Windows can
            // deactivate the root before the child receives WM_LBUTTONUP,
            // which drops every command selected from a submenu.
            return popup.depth > 0 ? MA_NOACTIVATE : MA_ACTIVATE;
        case kCancelMessage:
            if (wParam != 0)
                superseded_ = true;
            Cancel();
            if (wParam != 0)
                CloseFromDepth(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_NCHITTEST:
            return HTCLIENT;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void Render(Popup& popup)
    {
        if (!popup.hwnd || popup.windowWidth <= 0 || popup.windowHeight <= 0)
            return;

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = popup.windowWidth;
        bitmapInfo.bmiHeader.biHeight = -popup.windowHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* rawPixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &bitmapInfo,
            DIB_RGB_COLORS, &rawPixels, nullptr, 0);
        HDC memoryDc = CreateCompatibleDC(nullptr);
        if (!bitmap || !memoryDc || !rawPixels)
        {
            if (memoryDc) DeleteDC(memoryDc);
            if (bitmap) DeleteObject(bitmap);
            return;
        }
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        auto* pixels = static_cast<std::uint32_t*>(rawPixels);
        std::fill_n(pixels,
            static_cast<size_t>(popup.windowWidth) * popup.windowHeight, 0u);

        const RECT panel{
            shadowSize_, shadowSize_,
            shadowSize_ + popup.panelWidth,
            shadowSize_ + popup.panelHeight,
        };
        HBRUSH background = CreateSolidBrush(palette_.background);
        FillRect(memoryDc, &panel, background);
        DeleteObject(background);

        const RECT viewport{
            panel.left,
            panel.top + panelPadding_,
            panel.right,
            panel.bottom - panelPadding_,
        };
        const int savedDc = SaveDC(memoryDc);
        IntersectClipRect(memoryDc, viewport.left, viewport.top,
            viewport.right, viewport.bottom);
        for (size_t i = 0; i < popup.items->size(); ++i)
        {
            RECT row = popup.itemRects[i];
            OffsetRect(&row, 0, -popup.scrollOffset);
            RECT clipped{};
            if (!IntersectRect(&clipped, &row, &viewport))
                continue;

            const Item& item = (*popup.items)[i];
            const menu_icon::ItemView view{
                item.label.c_str(), item.glyph.c_str(),
                item.separator, !item.children.empty(), item.checked,
            };
            UINT state = 0;
            if (!item.enabled)
                state |= ODS_DISABLED | ODS_GRAYED;
            if (static_cast<int>(i) == popup.hoveredItem ||
                static_cast<int>(i) == popup.keyboardItem)
                state |= ODS_SELECTED;
            menu_icon::DrawItem(memoryDc, textFont_, iconFont_, view,
                row, state, palette_, metrics_);
        }
        RestoreDC(memoryDc, savedDc);

        if (popup.scrollOffset > 0)
            DrawScrollIndicator(memoryDc, popup, true);
        if (popup.scrollOffset < MaxScroll(popup))
            DrawScrollIndicator(memoryDc, popup, false);

        ApplyAlphaMask(popup, pixels, panel);

        POINT destination{
            popup.panelScreenOrigin.x - shadowSize_,
            popup.panelScreenOrigin.y - shadowSize_,
        };
        SIZE size{ popup.windowWidth, popup.windowHeight };
        POINT source{};
        BLENDFUNCTION blend{
            AC_SRC_OVER, 0, 255, AC_SRC_ALPHA,
        };
        UpdateLayeredWindow(popup.hwnd, nullptr, &destination, &size,
            memoryDc, &source, 0, &blend, ULW_ALPHA);

        SelectObject(memoryDc, oldBitmap);
        DeleteDC(memoryDc);
        DeleteObject(bitmap);
    }

private:
    bool RegisterWindowClass()
    {
        static const bool registered = [] {
            WNDCLASSEXW windowClass{ sizeof(windowClass) };
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = GetModuleHandleW(nullptr);
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = kMenuWindowClass;
            return RegisterClassExW(&windowClass) != 0 ||
                GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }();
        return registered;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
        WPARAM wParam, LPARAM lParam)
    {
        Popup* popup = reinterpret_cast<Popup*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            popup = static_cast<Popup*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(popup));
            if (popup)
                popup->hwnd = hwnd;
        }
        if (popup && popup->controller)
            return popup->controller->HandleMessage(
                *popup, hwnd, message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool OpenPopup(const std::vector<Item>& items, int depth,
        int parentItem, POINT anchor, const Popup* parent)
    {
        CloseFromDepth(depth);

        auto popup = std::make_unique<Popup>();
        popup->controller = this;
        popup->items = &items;
        popup->depth = depth;
        popup->parentItem = parentItem;
        CalculateLayout(*popup);
        PlacePopup(*popup, anchor, parent);

        const DWORD extendedStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW |
            (options_.topmost ? WS_EX_TOPMOST : 0) |
            (depth > 0 ? WS_EX_NOACTIVATE : 0);
        HWND owner = depth > 0 && !popups_.empty()
            ? popups_.front()->hwnd : options_.owner;
        popup->hwnd = CreateWindowExW(extendedStyle,
            kMenuWindowClass, L"", WS_POPUP,
            popup->panelScreenOrigin.x - shadowSize_,
            popup->panelScreenOrigin.y - shadowSize_,
            popup->windowWidth, popup->windowHeight,
            owner, nullptr, GetModuleHandleW(nullptr), popup.get());
        if (!popup->hwnd)
            return false;

        ApplyBlurClipRegion(*popup);
        ApplyWindowAppearance(popup->hwnd);

        Popup* rawPopup = popup.get();
        popups_.push_back(std::move(popup));
        Render(*rawPopup);
        if (depth > 0)
        {
            ShowWindow(rawPopup->hwnd, SW_SHOWNOACTIVATE);
            AnimateWindow(rawPopup->hwnd, 70,
                AW_BLEND | AW_SLIDE | AW_HOR_POSITIVE);
        }
        return true;
    }

    void CalculateLayout(Popup& popup)
    {
        HDC screenDc = GetDC(nullptr);
        int width = metrics_.minimumWidth;
        int contentTop = shadowSize_ + panelPadding_;
        popup.itemRects.clear();
        popup.itemRects.reserve(popup.items->size());
        for (const Item& item : *popup.items)
        {
            const menu_icon::ItemView view{
                item.label.c_str(), item.glyph.c_str(),
                item.separator, !item.children.empty(), item.checked,
            };
            const SIZE measured = menu_icon::MeasureItem(
                screenDc, textFont_, view, metrics_);
            width = std::max(width, static_cast<int>(measured.cx));
            RECT row{
                shadowSize_, contentTop,
                shadowSize_ + width,
                contentTop + static_cast<int>(measured.cy),
            };
            popup.itemRects.push_back(row);
            contentTop = row.bottom;
        }
        if (screenDc)
            ReleaseDC(nullptr, screenDc);

        // A later, wider item must expand every previously measured row.
        for (RECT& row : popup.itemRects)
            row.right = shadowSize_ + width;
        popup.panelWidth = width;
        popup.contentHeight = contentTop - shadowSize_ - panelPadding_;

        HMONITOR monitor = MonitorFromPoint(options_.anchor,
            MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            monitorInfo.rcWork = {
                0, 0, GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN),
            };
        }
        const int workHeight = static_cast<int>(
            monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
        int availablePanelHeight = workHeight - Scale(16, options_.dpi);
        if (popup.depth == 0)
        {
            if (options_.rootPlacement ==
                RootPlacement::AboveAnchorRect)
            {
                availablePanelHeight = std::min(availablePanelHeight,
                    static_cast<int>(options_.anchorRect.top -
                        monitorInfo.rcWork.top));
            }
            else if (options_.rootPlacement ==
                RootPlacement::BelowAnchorRect)
            {
                availablePanelHeight = std::min(availablePanelHeight,
                    static_cast<int>(monitorInfo.rcWork.bottom -
                        options_.anchorRect.bottom));
            }
        }
        const int maxPanelHeight = std::max(
            metrics_.rowHeight + panelPadding_ * 2,
            availablePanelHeight);
        popup.panelHeight = std::min(
            popup.contentHeight + panelPadding_ * 2,
            maxPanelHeight);
        popup.viewportHeight = popup.panelHeight - panelPadding_ * 2;
        popup.windowWidth = popup.panelWidth + shadowSize_ * 2;
        popup.windowHeight = popup.panelHeight + shadowSize_ * 2;
    }

    void PlacePopup(Popup& popup, POINT anchor, const Popup* parent)
    {
        POINT monitorPoint = anchor;
        if (!parent && options_.rootPlacement != RootPlacement::Default)
        {
            monitorPoint = {
                (options_.anchorRect.left + options_.anchorRect.right) / 2,
                (options_.anchorRect.top + options_.anchorRect.bottom) / 2,
            };
        }
        HMONITOR monitor = MonitorFromPoint(monitorPoint,
            MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        if (!GetMonitorInfoW(monitor, &monitorInfo))
        {
            monitorInfo.rcWork = {
                0, 0, GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN),
            };
        }

        int left = anchor.x;
        int top = anchor.y;
        if (parent)
        {
            left = parent->panelScreenOrigin.x + parent->panelWidth -
                Scale(3, options_.dpi);
            if (left + popup.panelWidth > monitorInfo.rcWork.right)
            {
                left = parent->panelScreenOrigin.x - popup.panelWidth +
                    Scale(3, options_.dpi);
            }
        }
        else
        {
            switch (options_.rootPlacement)
            {
            case RootPlacement::AboveAnchorRect:
                top = options_.anchorRect.top - popup.panelHeight;
                break;
            case RootPlacement::BelowAnchorRect:
                top = options_.anchorRect.bottom;
                break;
            case RootPlacement::LeftOfAnchorRect:
                left = options_.anchorRect.left - popup.panelWidth;
                break;
            case RootPlacement::RightOfAnchorRect:
                left = options_.anchorRect.right;
                break;
            case RootPlacement::Default:
            default:
                if (left + popup.panelWidth > monitorInfo.rcWork.right)
                    left -= popup.panelWidth;
                break;
            }
        }

        left = std::clamp(left,
            static_cast<int>(monitorInfo.rcWork.left),
            std::max(static_cast<int>(monitorInfo.rcWork.left),
                static_cast<int>(monitorInfo.rcWork.right) -
                    popup.panelWidth));
        top = std::clamp(top,
            static_cast<int>(monitorInfo.rcWork.top),
            std::max(static_cast<int>(monitorInfo.rcWork.top),
                static_cast<int>(monitorInfo.rcWork.bottom) -
                    popup.panelHeight));
        popup.panelScreenOrigin = { left, top };
    }

    int HitTest(const Popup& popup, POINT point) const
    {
        const RECT viewport{
            shadowSize_,
            shadowSize_ + panelPadding_,
            shadowSize_ + popup.panelWidth,
            shadowSize_ + popup.panelHeight - panelPadding_,
        };
        if (!PtInRect(&viewport, point))
            return -1;
        POINT contentPoint = point;
        contentPoint.y += popup.scrollOffset;
        for (size_t i = 0; i < popup.itemRects.size(); ++i)
        {
            if (PtInRect(&popup.itemRects[i], contentPoint))
                return static_cast<int>(i);
        }
        return -1;
    }

    void SetHoveredItem(Popup& popup, int index, bool keyboard)
    {
        if (index >= 0 &&
            static_cast<size_t>(index) < popup.items->size() &&
            (*popup.items)[index].separator)
            index = -1;
        activeDepth_ = popup.depth;
        if (popup.depth > 0 &&
            popup.depth - 1 < static_cast<int>(popups_.size()))
        {
            const HWND parentWindow = popups_[popup.depth - 1]->hwnd;
            KillTimer(parentWindow, kSubmenuOpenTimer);
            KillTimer(parentWindow, kSubmenuCloseTimer);
        }
        if (popup.hoveredItem == index && !keyboard)
            return;

        KillTimer(popup.hwnd, kSubmenuOpenTimer);
        KillTimer(popup.hwnd, kSubmenuCloseTimer);
        popup.hoveredItem = keyboard ? -1 : index;
        popup.keyboardItem = keyboard ? index : -1;
        if (index >= 0 &&
            static_cast<size_t>(index) < popup.items->size() &&
            !(*popup.items)[index].children.empty() &&
            (*popup.items)[index].enabled)
        {
            if (keyboard)
                OpenSubmenu(popup, index, true);
            else
                SetTimer(popup.hwnd, kSubmenuOpenTimer,
                    kSubmenuOpenDelayMs, nullptr);
        }
        else if (HasOpenChild(popup))
        {
            if (keyboard)
                CloseFromDepth(popup.depth + 1);
            else
                SetTimer(popup.hwnd, kSubmenuCloseTimer,
                    kSubmenuCloseDelayMs, nullptr);
        }
        Render(popup);
    }

    void ActivateItem(Popup& popup, int index, bool keyboard)
    {
        if (index < 0 || static_cast<size_t>(index) >= popup.items->size())
            return;
        const Item& item = (*popup.items)[index];
        if (!IsSelectable(item))
            return;
        if (!item.children.empty())
        {
            OpenSubmenu(popup, index, keyboard);
            return;
        }

        const UINT command = item.command;
        RECT rect = popup.itemRects[index];
        OffsetRect(&rect,
            popup.panelScreenOrigin.x - shadowSize_,
            popup.panelScreenOrigin.y - shadowSize_ - popup.scrollOffset);
        if (options_.onCommand &&
            options_.onCommand(command, rootItems_))
        {
            CloseFromDepth(1);
            activeDepth_ = 0;
            if (!popups_.empty())
            {
                Popup& root = *popups_.front();
                root.hoveredItem = -1;
                root.keyboardItem = -1;
                CalculateLayout(root);
                PlacePopup(root, options_.anchor, nullptr);
                SetWindowPos(root.hwnd, nullptr,
                    root.panelScreenOrigin.x - shadowSize_,
                    root.panelScreenOrigin.y - shadowSize_,
                    root.windowWidth, root.windowHeight,
                    SWP_NOACTIVATE | SWP_NOZORDER);
                ApplyBlurClipRegion(root);
                Render(root);
            }
            return;
        }

        result_.command = command;
        result_.itemScreenRect = rect;
        done_ = true;
    }

    void OpenSubmenu(Popup& popup, int index, bool keyboard)
    {
        KillTimer(popup.hwnd, kSubmenuCloseTimer);
        if (index < 0 || static_cast<size_t>(index) >= popup.items->size())
            return;
        const Item& item = (*popup.items)[index];
        if (!item.enabled || item.children.empty())
            return;
        if (popup.depth + 1 < static_cast<int>(popups_.size()) &&
            popups_[popup.depth + 1]->parentItem == index)
        {
            if (keyboard)
                activeDepth_ = popup.depth + 1;
            return;
        }

        RECT row = popup.itemRects[index];
        OffsetRect(&row,
            popup.panelScreenOrigin.x - shadowSize_,
            popup.panelScreenOrigin.y - shadowSize_ - popup.scrollOffset);
        POINT anchor{ row.right, row.top - panelPadding_ };
        if (OpenPopup(item.children, popup.depth + 1,
                index, anchor, &popup) && keyboard)
        {
            activeDepth_ = popup.depth + 1;
            SelectNext(*popups_.back(), 1, true);
        }
    }

    bool HasOpenChild(const Popup& popup) const
    {
        return popup.depth + 1 < static_cast<int>(popups_.size());
    }

    bool IsPopupWindow(HWND hwnd) const
    {
        if (!hwnd)
            return false;
        return std::ranges::any_of(popups_, [hwnd](const auto& popup) {
            return popup && popup->hwnd == hwnd;
        });
    }

    void HandleKey(WPARAM key)
    {
        Popup* popup = ActivePopup();
        if (!popup)
            return;
        switch (key)
        {
        case VK_DOWN: SelectNext(*popup, 1, false); break;
        case VK_UP: SelectNext(*popup, -1, false); break;
        case VK_HOME: SelectBoundary(*popup, false); break;
        case VK_END: SelectBoundary(*popup, true); break;
        case VK_RIGHT:
        {
            const int index = CurrentItem(*popup);
            if (index >= 0)
                OpenSubmenu(*popup, index, true);
            break;
        }
        case VK_LEFT:
            if (popup->depth > 0)
            {
                const int closingDepth = popup->depth;
                CloseFromDepth(closingDepth);
                activeDepth_ = std::max(0, closingDepth - 1);
                if (Popup* parent = ActivePopup())
                    Render(*parent);
            }
            break;
        case VK_RETURN:
        case VK_SPACE:
            ActivateItem(*popup, CurrentItem(*popup), true);
            break;
        case VK_ESCAPE:
            if (popup->depth > 0)
            {
                const int closingDepth = popup->depth;
                CloseFromDepth(closingDepth);
                activeDepth_ = std::max(0, closingDepth - 1);
            }
            else
            {
                Cancel();
            }
            break;
        default:
            break;
        }
    }

    int CurrentItem(const Popup& popup) const
    {
        return popup.keyboardItem >= 0
            ? popup.keyboardItem : popup.hoveredItem;
    }

    void SelectNext(Popup& popup, int direction, bool fromBoundary)
    {
        const int count = static_cast<int>(popup.items->size());
        if (count == 0)
            return;
        int index = fromBoundary
            ? (direction > 0 ? -1 : count)
            : CurrentItem(popup);
        for (int attempt = 0; attempt < count; ++attempt)
        {
            index = (index + direction + count) % count;
            if (IsSelectable((*popup.items)[index]))
            {
                EnsureVisible(popup, index);
                SetHoveredItem(popup, index, true);
                return;
            }
        }
    }

    void SelectBoundary(Popup& popup, bool end)
    {
        const int count = static_cast<int>(popup.items->size());
        for (int step = 0; step < count; ++step)
        {
            const int index = end ? count - 1 - step : step;
            if (IsSelectable((*popup.items)[index]))
            {
                EnsureVisible(popup, index);
                SetHoveredItem(popup, index, true);
                return;
            }
        }
    }

    void SelectByCharacter(wchar_t character)
    {
        Popup* popup = ActivePopup();
        if (!popup || !std::iswalnum(character))
            return;
        const wchar_t target = static_cast<wchar_t>(std::towlower(character));
        const int count = static_cast<int>(popup->items->size());
        int start = std::max(0, CurrentItem(*popup) + 1);
        for (int step = 0; step < count; ++step)
        {
            const int index = (start + step) % count;
            const Item& item = (*popup->items)[index];
            std::wstring_view label = item.label;
            while (!label.empty() && (label.front() == L'&' ||
                std::iswspace(label.front())))
                label.remove_prefix(1);
            if (IsSelectable(item) && !label.empty() &&
                std::towlower(label.front()) == target)
            {
                EnsureVisible(*popup, index);
                SetHoveredItem(*popup, index, true);
                return;
            }
        }
    }

    void EnsureVisible(Popup& popup, int index)
    {
        const RECT row = popup.itemRects[index];
        const int viewportTop = shadowSize_ + panelPadding_;
        const int viewportBottom = viewportTop + popup.viewportHeight;
        if (row.top - popup.scrollOffset < viewportTop)
            popup.scrollOffset = row.top - viewportTop;
        else if (row.bottom - popup.scrollOffset > viewportBottom)
            popup.scrollOffset = row.bottom - viewportBottom;
        popup.scrollOffset = std::clamp(
            popup.scrollOffset, 0, MaxScroll(popup));
    }

    void Scroll(Popup& popup, int direction)
    {
        const int oldOffset = popup.scrollOffset;
        popup.scrollOffset = std::clamp(
            popup.scrollOffset + direction * metrics_.rowHeight * 2,
            0, MaxScroll(popup));
        if (popup.scrollOffset != oldOffset)
        {
            CloseFromDepth(popup.depth + 1);
            Render(popup);
        }
    }

    int MaxScroll(const Popup& popup) const
    {
        return std::max(0, popup.contentHeight - popup.viewportHeight);
    }

    int ActiveDepth() const
    {
        return std::clamp(activeDepth_, 0,
            std::max(0, static_cast<int>(popups_.size()) - 1));
    }

    Popup* ActivePopup()
    {
        if (popups_.empty())
            return nullptr;
        return popups_[ActiveDepth()].get();
    }

    void CloseFromDepth(int depth)
    {
        closing_ = true;
        while (static_cast<int>(popups_.size()) > depth)
        {
            Popup* popup = popups_.back().get();
            if (popup->hwnd && IsWindow(popup->hwnd))
                DestroyWindow(popup->hwnd);
            popups_.pop_back();
        }
        activeDepth_ = std::min(activeDepth_,
            std::max(0, static_cast<int>(popups_.size()) - 1));
        closing_ = false;
    }

    void Cancel()
    {
        done_ = true;
        result_ = {};
    }

    void DrawScrollIndicator(HDC dc, const Popup& popup, bool top)
    {
        const int centerX = shadowSize_ + popup.panelWidth / 2;
        const int centerY = top
            ? shadowSize_ + Scale(5, options_.dpi)
            : shadowSize_ + popup.panelHeight - Scale(5, options_.dpi);
        HPEN pen = CreatePen(PS_SOLID, 1,
            lightTheme_ ? RGB(95, 95, 95) : RGB(190, 190, 190));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        const int half = Scale(3, options_.dpi);
        MoveToEx(dc, centerX - half,
            centerY + (top ? half / 2 : -half / 2), nullptr);
        LineTo(dc, centerX,
            centerY + (top ? -half / 2 : half / 2));
        LineTo(dc, centerX + half,
            centerY + (top ? half / 2 : -half / 2));
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void ApplyAlphaMask(Popup& popup, std::uint32_t* pixels,
        const RECT& panel)
    {
        const float centerX = (panel.left + panel.right) * 0.5f;
        const float centerY = (panel.top + panel.bottom) * 0.5f;
        const float halfWidth = (panel.right - panel.left) * 0.5f;
        const float halfHeight = (panel.bottom - panel.top) * 0.5f;
        const float radius = static_cast<float>(panelRadius_);
        const float shadow = static_cast<float>(shadowSize_);
        constexpr float solidPanelAlpha = 246.0f;
        // The acrylic backdrop already supplies its own tint.  Keeping the
        // custom surface comparatively translucent lets the blurred desktop
        // remain visible instead of stacking two nearly opaque colour layers.
        constexpr float blurPanelAlpha = 92.0f;
        constexpr float blurHoverAlpha = 146.0f;
        constexpr float blurContentAlpha = 246.0f;
        constexpr float shadowAlpha = 34.0f;
        const COLORREF borderColor = lightTheme_
            ? RGB(215, 215, 215) : RGB(73, 73, 73);
        const unsigned borderBlue = GetBValue(borderColor);
        const unsigned borderGreen = GetGValue(borderColor);
        const unsigned borderRed = GetRValue(borderColor);

        for (int y = 0; y < popup.windowHeight; ++y)
        {
            for (int x = 0; x < popup.windowWidth; ++x)
            {
                const float qx = std::fabs((x + 0.5f) - centerX) -
                    (halfWidth - radius);
                const float qy = std::fabs((y + 0.5f) - centerY) -
                    (halfHeight - radius);
                const float outside = std::hypot(
                    std::max(qx, 0.0f), std::max(qy, 0.0f));
                const float distance = outside +
                    std::min(std::max(qx, qy), 0.0f) - radius;
                std::uint32_t& pixel = pixels[
                    static_cast<size_t>(y) * popup.windowWidth + x];
                const std::uint32_t rgb = pixel & 0x00FFFFFFu;
                const auto dibColor = [](COLORREF color) {
                    return static_cast<std::uint32_t>(GetBValue(color)) |
                        (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
                        (static_cast<std::uint32_t>(GetRValue(color)) << 16);
                };
                float surfaceAlpha = solidPanelAlpha;
                if (blurEnabled_)
                {
                    if (rgb == dibColor(palette_.background))
                        surfaceAlpha = blurPanelAlpha;
                    else if (rgb == dibColor(palette_.hoverBackground))
                        surfaceAlpha = blurHoverAlpha;
                    else
                        surfaceAlpha = blurContentAlpha;
                }

                // Analytic one-pixel coverage replaces the hard binary mask.
                // It keeps the layered window's rounded edge smooth at 100%
                // DPI while preserving a crisp, anti-aliased one-pixel border.
                const float panelCoverage = std::clamp(
                    0.5f - distance, 0.0f, 1.0f);
                const float borderCoverage = panelCoverage * std::clamp(
                    distance + 1.5f, 0.0f, 1.0f);
                const float outsideDistance = std::max(distance, 0.0f);
                float localShadowAlpha = 0.0f;
                if (outsideDistance < shadow)
                {
                    const float strength =
                        1.0f - outsideDistance / shadow;
                    localShadowAlpha = shadowAlpha * strength * strength *
                        (1.0f - panelCoverage);
                }

                float blue = static_cast<float>(pixel & 0xFFu);
                float green = static_cast<float>((pixel >> 8) & 0xFFu);
                float red = static_cast<float>((pixel >> 16) & 0xFFu);
                blue += (static_cast<float>(borderBlue) - blue) *
                    borderCoverage;
                green += (static_cast<float>(borderGreen) - green) *
                    borderCoverage;
                red += (static_cast<float>(borderRed) - red) *
                    borderCoverage;

                const float localPanelAlpha =
                    surfaceAlpha * panelCoverage;
                const unsigned alpha = static_cast<unsigned>(std::clamp(
                    localPanelAlpha + localShadowAlpha *
                        (1.0f - localPanelAlpha / 255.0f),
                    0.0f, 255.0f));
                const unsigned premultipliedBlue = static_cast<unsigned>(
                    blue * localPanelAlpha / 255.0f);
                const unsigned premultipliedGreen = static_cast<unsigned>(
                    green * localPanelAlpha / 255.0f);
                const unsigned premultipliedRed = static_cast<unsigned>(
                    red * localPanelAlpha / 255.0f);
                pixel = premultipliedBlue |
                    (premultipliedGreen << 8) |
                    (premultipliedRed << 16) |
                    (alpha << 24);
            }
        }
    }

    void ApplyBlurClipRegion(Popup& popup)
    {
        if (!blurEnabled_ || !popup.hwnd)
            return;

        // DWM applies Acrylic to the whole HWND, including the transparent
        // margin reserved for our analytic shadow.  Restrict composition to
        // the actual rounded panel so that margin does not become a large,
        // square tinted backdrop around the menu.
        const int diameter = panelRadius_ * 2;
        HRGN panelRegion = CreateRoundRectRgn(
            shadowSize_, shadowSize_,
            shadowSize_ + popup.panelWidth + 1,
            shadowSize_ + popup.panelHeight + 1,
            diameter, diameter);
        if (!panelRegion)
            return;
        if (!SetWindowRgn(popup.hwnd, panelRegion, TRUE))
            DeleteObject(panelRegion);
    }

    void ApplyWindowAppearance(HWND window)
    {
        if (!window)
            return;

        static const HMODULE dwmModule = LoadLibraryW(L"dwmapi.dll");
        static const auto setDwmWindowAttribute =
            dwmModule
            ? reinterpret_cast<DwmSetWindowAttributeFn>(
                GetProcAddress(dwmModule, "DwmSetWindowAttribute"))
            : nullptr;
        static const auto extendDwmFrame =
            dwmModule
            ? reinterpret_cast<DwmExtendFrameIntoClientAreaFn>(
                GetProcAddress(dwmModule,
                    "DwmExtendFrameIntoClientArea"))
            : nullptr;
        const BOOL darkMode = lightTheme_ ? FALSE : TRUE;
        if (setDwmWindowAttribute)
            setDwmWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                &darkMode, sizeof(darkMode));
        const DWM_WINDOW_CORNER_PREFERENCE corner = blurEnabled_
            ? DWMWCP_ROUND : DWMWCP_ROUNDSMALL;
        if (setDwmWindowAttribute)
            setDwmWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE,
                &corner, sizeof(corner));
        if (!blurEnabled_)
            return;

        const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;
        if (setDwmWindowAttribute)
            setDwmWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE,
                &backdrop, sizeof(backdrop));
        const MARGINS glassMargins{ -1, -1, -1, -1 };
        if (extendDwmFrame)
            extendDwmFrame(window, &glassMargins);

        static const auto setWindowCompositionAttribute =
            reinterpret_cast<SetWindowCompositionAttributeFn>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"),
                    "SetWindowCompositionAttribute"));
        if (!setWindowCompositionAttribute)
            return;

        const COLORREF tint = palette_.background;
        const DWORD tintAlpha = lightTheme_ ? 0x70 : 0x7A;
        AccentPolicy accent;
        accent.state = AccentState::AcrylicBlurBehind;
        accent.flags = 2;
        accent.gradientColor = (tintAlpha << 24) |
            (static_cast<DWORD>(GetBValue(tint)) << 16) |
            (static_cast<DWORD>(GetGValue(tint)) << 8) |
            static_cast<DWORD>(GetRValue(tint));
        WindowCompositionAttributeData data;
        data.data = &accent;
        data.size = sizeof(accent);
        if (!setWindowCompositionAttribute(window, &data))
        {
            accent.state = AccentState::BlurBehind;
            accent.gradientColor = 0;
            setWindowCompositionAttribute(window, &data);
        }
    }

    std::vector<Item> rootItems_;
    Options options_;
    bool lightTheme_ = true;
    bool blurEnabled_ = false;
    menu_icon::Palette palette_;
    menu_icon::Metrics metrics_;
    int shadowSize_ = 0;
    int panelPadding_ = 0;
    int panelRadius_ = 0;
    HFONT textFont_ = nullptr;
    HFONT iconFont_ = nullptr;
    std::vector<std::unique_ptr<Popup>> popups_;
    int activeDepth_ = 0;
    bool done_ = false;
    bool closing_ = false;
    bool superseded_ = false;
    Result result_{};
};

} // namespace

Result Show(const std::vector<Item>& items, const Options& options)
{
    MenuController controller(items, options);
    return controller.Run();
}

} // namespace snowdesktop::modern_menu
