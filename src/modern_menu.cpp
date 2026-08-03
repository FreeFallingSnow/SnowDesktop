#include "modern_menu.h"

#include "menu_icon_render.h"

#include <dwmapi.h>
#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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
constexpr UINT_PTR kTextCaretTimer = 3;
constexpr UINT kTextCaretBlinkMs = 530;
constexpr UINT kCancelMessage = WM_APP + 0x311;
std::atomic<HWND> gActiveRootMenu{ nullptr };

int Scale(int value, UINT dpi)
{
    return std::max(1, MulDiv(value, static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

bool IsSelectable(const Item& item)
{
    return !item.separator && !item.textInput && item.enabled;
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
    std::vector<Item>* items = nullptr;
    HWND hwnd = nullptr;
    int depth = 0;
    int parentItem = -1;
    int hoveredItem = -1;
    int keyboardItem = -1;
    int scrollOffset = 0;
    int horizontalScrollOffset = 0;
    int horizontalScrollContentWidth = 0;
    RECT horizontalScrollRect{};
    int contentHeight = 0;
    int viewportHeight = 0;
    int panelWidth = 0;
    int panelHeight = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    POINT panelScreenOrigin{};
    std::vector<RECT> itemRects;
    std::vector<int> navigationOrder;
    RECT quickSeparatorRect{};
    int quickActionRight = 0;
    int quickActionCellWidth = 0;
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
          panelPadding_(Scale(kSubmenuPanelPaddingDip, options.dpi)),
          panelRadius_(Scale(8, options.dpi))
    {
        const int textHeight = -Scale(13, options.dpi);
        const int iconHeight = -metrics_.iconFontHeight;
        const int quickTextHeight = -Scale(10, options.dpi);
        const int quickIconHeight = -metrics_.quickActionFontHeight;
        textFont_ = CreateFontW(textHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        // Only the official Regular face is embedded.  Requesting Semibold
        // makes GDI synthesize thicker outlines, which distorts the 20px
        // Fluent masters most visibly on 96-DPI / low-resolution screens.
        fluentIconFont_ = CreateFontW(iconHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"FluentSystemIcons-Regular");
        fontAwesomeIconFont_ = CreateFontW(iconHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Font Awesome 6 Free Solid");
        quickTextFont_ = CreateFontW(quickTextHeight, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        quickFluentIconFont_ = CreateFontW(quickIconHeight, 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"FluentSystemIcons-Regular");
        quickFontAwesomeIconFont_ = CreateFontW(quickIconHeight, 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Font Awesome 6 Free Solid");
    }

    ~MenuController()
    {
        CloseFromDepth(0);
        if (fontAwesomeIconFont_)
            DeleteObject(fontAwesomeIconFont_);
        if (fluentIconFont_)
            DeleteObject(fluentIconFont_);
        if (quickFontAwesomeIconFont_)
            DeleteObject(quickFontAwesomeIconFont_);
        if (quickFluentIconFont_)
            DeleteObject(quickFluentIconFont_);
        if (quickTextFont_)
            DeleteObject(quickTextFont_);
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
            const int index = HitTest(popup, point);
            if (index >= 0 &&
                (*popup.items)[index].textInput)
                FocusTextInputFromPoint(popup, index, point);
            else
                ActivateItem(popup, index, false);
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(hwnd, &point);
            RECT horizontalTrack = popup.horizontalScrollRect;
            OffsetRect(&horizontalTrack, 0, -popup.scrollOffset);
            if (PtInRect(&horizontalTrack, point) &&
                MaxHorizontalScroll(popup) > 0)
                ScrollHorizontal(popup, delta > 0 ? -1 : 1);
            else
                Scroll(popup, delta > 0 ? -1 : 1);
            return 0;
        }
        case WM_MOUSEHWHEEL:
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ScrollHorizontal(popup, delta > 0 ? 1 : -1);
            return 0;
        }
        case WM_KEYDOWN:
            if (!HandleTextInputKey(wParam))
                HandleKey(wParam);
            return 0;
        case WM_CHAR:
            if (!HandleTextInputCharacter(static_cast<wchar_t>(wParam)))
                SelectByCharacter(static_cast<wchar_t>(wParam));
            return 0;
        case WM_TIMER:
            if (wParam == kTextCaretTimer)
            {
                textCaretVisible_ = !textCaretVisible_;
                if (Popup* active = ActivePopup())
                    Render(*active);
            }
            else if (wParam == kSubmenuOpenTimer)
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
        case WM_IME_STARTCOMPOSITION:
            textInputComposition_.clear();
            textInputCompositionCursor_ = 0;
            ResetTextCaret(popup);
            UpdateTextInputImePosition(hwnd);
            return 0;
        case WM_IME_COMPOSITION:
            HandleTextInputImeComposition(hwnd, lParam);
            return 0;
        case WM_IME_ENDCOMPOSITION:
            textInputComposition_.clear();
            textInputCompositionCursor_ = 0;
            ResetTextCaret(popup);
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
        {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            const int index = HitTest(popup, point);
            const bool overTextInput = index >= 0 &&
                static_cast<size_t>(index) < popup.items->size() &&
                (*popup.items)[index].textInput;
            SetCursor(LoadCursorW(nullptr,
                overTextInput ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
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
            if (row.right <= row.left || row.bottom <= row.top)
                continue;
            OffsetRect(&row, 0, -popup.scrollOffset);
            if ((*popup.items)[i].horizontalScrollAction)
                OffsetRect(&row, -popup.horizontalScrollOffset, 0);
            RECT clipped{};
            if (!IntersectRect(&clipped, &row, &viewport))
                continue;
            if ((*popup.items)[i].horizontalScrollAction)
            {
                RECT horizontalClip = popup.horizontalScrollRect;
                OffsetRect(&horizontalClip, 0, -popup.scrollOffset);
                if (!IntersectRect(&clipped, &clipped, &horizontalClip))
                    continue;
            }

            const Item& item = (*popup.items)[i];
            const int savedRowDc = SaveDC(memoryDc);
            IntersectClipRect(memoryDc, clipped.left, clipped.top,
                clipped.right, clipped.bottom);
            const menu_icon::ItemView view{
                item.label.c_str(), item.glyph.c_str(),
                item.separator, !item.children.empty(), item.checked,
                item.quickIcon,
            };
            UINT state = 0;
            if (!item.enabled)
                state |= ODS_DISABLED | ODS_GRAYED;
            if (static_cast<int>(i) == popup.hoveredItem ||
                static_cast<int>(i) == popup.keyboardItem)
                state |= ODS_SELECTED;
            HFONT iconFont = item.iconFont == IconFont::FontAwesomeSolid
                ? fontAwesomeIconFont_ : fluentIconFont_;
            if (item.textInput)
            {
                const bool focused = IsTextInputFocused(popup, item);
                const menu_icon::TextInputView inputView{
                    item.inputText.c_str(),
                    focused ? textInputCursor_ : item.inputText.size(),
                    focused ? textInputSelectionAnchor_ :
                        item.inputText.size(),
                    focused ? textInputComposition_.c_str() : L"",
                    focused ? textInputCompositionCursor_ : 0,
                    focused,
                    focused && textCaretVisible_,
                };
                menu_icon::DrawTextInput(memoryDc, textFont_, iconFont,
                    view, inputView, row, palette_, metrics_);
            }
            else if (popup.depth == 0 && item.quickAction && !item.inlineAction)
            {
                HFONT quickIconFont =
                    item.iconFont == IconFont::FontAwesomeSolid
                    ? quickFontAwesomeIconFont_
                    : quickFluentIconFont_;
                menu_icon::DrawQuickAction(memoryDc, quickTextFont_,
                    quickIconFont, item.quickIcon, view, row, state,
                    palette_, metrics_);
                if (row.right < popup.quickActionRight)
                {
                    RECT verticalSeparator{
                        row.right - 1,
                        row.top + metrics_.outerInset * 2,
                        row.right,
                        row.bottom - metrics_.outerInset * 2,
                    };
                    HBRUSH separatorBrush = CreateSolidBrush(
                        palette_.separator);
                    if (separatorBrush)
                    {
                        FillRect(memoryDc, &verticalSeparator,
                            separatorBrush);
                        DeleteObject(separatorBrush);
                    }
                }
            }
            else if (item.inlineAction)
            {
                menu_icon::DrawInlineAction(memoryDc, textFont_, iconFont,
                    view, row, state, palette_, metrics_);
            }
            else
            {
                menu_icon::DrawItem(memoryDc, textFont_, iconFont, view,
                    row, state, palette_, metrics_);
            }
            RestoreDC(memoryDc, savedRowDc);
        }
        if (MaxHorizontalScroll(popup) > 0)
        {
            if (popup.horizontalScrollOffset > 0)
                DrawHorizontalScrollIndicator(memoryDc, popup, true);
            if (popup.horizontalScrollOffset < MaxHorizontalScroll(popup))
                DrawHorizontalScrollIndicator(memoryDc, popup, false);
        }
        if (popup.quickSeparatorRect.right >
            popup.quickSeparatorRect.left)
        {
            RECT separator = popup.quickSeparatorRect;
            OffsetRect(&separator, 0, -popup.scrollOffset);
            RECT clipped{};
            if (IntersectRect(&clipped, &separator, &viewport))
            {
                const menu_icon::ItemView separatorView{
                    L"", L"", true, false, false,
                };
                menu_icon::DrawItem(memoryDc, textFont_,
                    fluentIconFont_, separatorView, separator, 0,
                    palette_, metrics_);
            }
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

    bool OpenPopup(std::vector<Item>& items, int depth,
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
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i].textInput && items[i].enabled)
            {
                FocusTextInput(*rawPopup, static_cast<int>(i),
                    items[i].inputText.size(), false);
                break;
            }
        }
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
        std::vector<int> quickIndices;
        std::vector<int> regularIndices;
        std::vector<int> inlineWidths(popup.items->size(), 0);
        int pendingSeparator = -1;
        for (size_t i = 0; i < popup.items->size(); ++i)
        {
            const Item& item = (*popup.items)[i];
            if (popup.depth == 0 && item.quickAction && !item.separator &&
                !item.inlineAction)
            {
                quickIndices.push_back(static_cast<int>(i));
                continue;
            }
            if (item.separator)
            {
                if (!regularIndices.empty())
                    pendingSeparator = static_cast<int>(i);
                continue;
            }
            if (pendingSeparator >= 0)
            {
                regularIndices.push_back(pendingSeparator);
                pendingSeparator = -1;
            }
            regularIndices.push_back(static_cast<int>(i));
            if (item.textInput)
            {
                width = std::max(width, Scale(280, options_.dpi));
                continue;
            }
            if (item.inlineAction)
            {
                width = std::max(width,
                    metrics_.minimumWidth + metrics_.rowHeight);
                if (item.horizontalScrollAction && screenDc)
                {
                    HGDIOBJ oldFont = SelectObject(screenDc, textFont_);
                    SIZE labelSize{};
                    GetTextExtentPoint32W(screenDc, item.label.c_str(),
                        static_cast<int>(item.label.size()), &labelSize);
                    if (oldFont) SelectObject(screenDc, oldFont);
                    inlineWidths[i] = std::max(
                        metrics_.rowHeight,
                        static_cast<int>(labelSize.cx) +
                            metrics_.outerInset * 4);
                }
                continue;
            }
            const menu_icon::ItemView view{
                item.label.c_str(), item.glyph.c_str(),
                item.separator, !item.children.empty(), item.checked,
            };
            const SIZE measured = menu_icon::MeasureItem(
                screenDc, textFont_, view, metrics_);
            width = std::max(width, static_cast<int>(measured.cx));
        }
        if (!quickIndices.empty())
        {
            int quickCellWidth = metrics_.quickActionMinimumWidth;
            if (screenDc)
            {
                HGDIOBJ oldFont = SelectObject(screenDc, quickTextFont_);
                for (int index : quickIndices)
                {
                    std::wstring_view label =
                        (*popup.items)[index].label;
                    const size_t tab = label.find(L'\t');
                    label = label.substr(0, tab);
                    SIZE labelSize{};
                    GetTextExtentPoint32W(screenDc, label.data(),
                        static_cast<int>(label.size()), &labelSize);
                    quickCellWidth = std::max(quickCellWidth,
                        static_cast<int>(labelSize.cx) +
                            metrics_.outerInset * 4);
                }
                if (oldFont)
                    SelectObject(screenDc, oldFont);
            }
            quickCellWidth = std::min(quickCellWidth,
                metrics_.quickActionMaximumWidth);
            width = std::max(width,
                quickCellWidth *
                    static_cast<int>(quickIndices.size()) +
                    metrics_.outerInset * 2);
            popup.quickActionCellWidth = quickCellWidth;
        }
        if (screenDc)
            ReleaseDC(nullptr, screenDc);

        popup.itemRects.assign(popup.items->size(), RECT{});
        popup.navigationOrder.clear();
        popup.quickSeparatorRect = {};
        popup.horizontalScrollRect = {};
        popup.horizontalScrollContentWidth = 0;
        popup.quickActionRight = 0;
        if (quickIndices.empty())
            popup.quickActionCellWidth = 0;
        int contentTop = shadowSize_ + panelPadding_;
        if (!quickIndices.empty())
        {
            const int actionLeft = shadowSize_ + metrics_.outerInset;
            const int actionWidth = popup.quickActionCellWidth *
                static_cast<int>(quickIndices.size());
            popup.quickActionRight = actionLeft + actionWidth;
            const int count = static_cast<int>(quickIndices.size());
            for (int column = 0; column < count; ++column)
            {
                const int left = actionLeft +
                    popup.quickActionCellWidth * column;
                const int right = left + popup.quickActionCellWidth;
                popup.itemRects[quickIndices[column]] = {
                    left, contentTop, right,
                    contentTop + metrics_.quickActionHeight,
                };
                popup.navigationOrder.push_back(quickIndices[column]);
            }
            contentTop += metrics_.quickActionHeight;
            if (!regularIndices.empty())
            {
                popup.quickSeparatorRect = {
                    shadowSize_, contentTop,
                    shadowSize_ + width,
                    contentTop + metrics_.separatorHeight,
                };
                contentTop += metrics_.separatorHeight;
            }
        }
        for (size_t position = 0; position < regularIndices.size(); ++position)
        {
            const int index = regularIndices[position];
            const Item& item = (*popup.items)[index];
            if (item.inlineAction)
            {
                size_t runEnd = position;
                while (runEnd + 1 < regularIndices.size() &&
                    (*popup.items)[regularIndices[runEnd + 1]].inlineAction &&
                    (item.inlineGroup == 0 ||
                        (*popup.items)[regularIndices[runEnd + 1]].inlineGroup ==
                            item.inlineGroup))
                    ++runEnd;
                const int count = static_cast<int>(runEnd - position + 1);
                if (item.horizontalScrollAction)
                {
                    const int trackLeft = shadowSize_ + panelPadding_;
                    const int trackRight = shadowSize_ + width -
                        panelPadding_;
                    int left = trackLeft;
                    for (size_t i = position; i <= runEnd; ++i)
                    {
                        const int actionIndex = regularIndices[i];
                        const int actionWidth = std::max(
                            metrics_.rowHeight,
                            inlineWidths[actionIndex]);
                        popup.itemRects[actionIndex] = {
                            left, contentTop, left + actionWidth,
                            contentTop + metrics_.rowHeight,
                        };
                        left += actionWidth;
                        popup.navigationOrder.push_back(actionIndex);
                    }
                    popup.horizontalScrollRect = {
                        trackLeft, contentTop,
                        trackRight,
                        contentTop + metrics_.rowHeight,
                    };
                    popup.horizontalScrollContentWidth =
                        left - trackLeft;
                    contentTop += metrics_.rowHeight;
                    position = runEnd;
                    continue;
                }
                const int narrowWidth = metrics_.rowHeight;
                const int compactWidth = metrics_.rowHeight * 2;
                int flexibleCount = 0;
                int fixedWidth = 0;
                for (size_t i = position; i <= runEnd; ++i)
                {
                    const Item& action =
                        (*popup.items)[regularIndices[i]];
                    if (action.label.empty())
                        fixedWidth += narrowWidth;
                    else if (action.compactInlineAction)
                        fixedWidth += compactWidth;
                    else
                        ++flexibleCount;
                }
                const int flexibleWidth = flexibleCount > 0
                    ? std::max(narrowWidth,
                        (width - fixedWidth) / flexibleCount)
                    : std::max(1, width / count);
                int left = shadowSize_;
                for (size_t i = position; i <= runEnd; ++i)
                {
                    const int actionIndex = regularIndices[i];
                    const Item& action = (*popup.items)[actionIndex];
                    const bool flexible = !action.label.empty() &&
                        !action.compactInlineAction;
                    const int requestedWidth = action.label.empty()
                        ? narrowWidth
                        : (action.compactInlineAction
                            ? compactWidth : flexibleWidth);
                    const int actionWidth = i == runEnd
                        ? shadowSize_ + width - left
                        : (flexible ? flexibleWidth : requestedWidth);
                    popup.itemRects[actionIndex] = {
                        left, contentTop, left + actionWidth,
                        contentTop + metrics_.rowHeight,
                    };
                    left += actionWidth;
                    popup.navigationOrder.push_back(actionIndex);
                }
                contentTop += metrics_.rowHeight;
                position = runEnd;
                continue;
            }
            const int height = item.separator
                ? metrics_.separatorHeight : metrics_.rowHeight;
            const int horizontalPadding = item.textInput
                ? panelPadding_ : 0;
            popup.itemRects[index] = {
                shadowSize_ + horizontalPadding, contentTop,
                shadowSize_ + width - horizontalPadding,
                contentTop + height,
            };
            contentTop += height;
            if (!item.separator && !item.textInput)
                popup.navigationOrder.push_back(index);
        }
        popup.panelWidth = width;
        popup.horizontalScrollOffset = std::clamp(
            popup.horizontalScrollOffset, 0,
            MaxHorizontalScroll(popup));
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
                Scale(kSubmenuOverlapDip, options_.dpi);
            if (left + popup.panelWidth > monitorInfo.rcWork.right)
            {
                left = parent->panelScreenOrigin.x - popup.panelWidth +
                    Scale(kSubmenuOverlapDip, options_.dpi);
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
            POINT itemPoint = contentPoint;
            if ((*popup.items)[i].horizontalScrollAction)
            {
                if (!PtInRect(&popup.horizontalScrollRect, contentPoint))
                    continue;
                itemPoint.x += popup.horizontalScrollOffset;
            }
            if (PtInRect(&popup.itemRects[i], itemPoint))
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
        if (options_.onHover)
        {
            HoverInfo info;
            info.depth = popup.depth;
            info.keyboard = keyboard;
            info.popupScreenRect = {
                popup.panelScreenOrigin.x,
                popup.panelScreenOrigin.y,
                popup.panelScreenOrigin.x + popup.panelWidth,
                popup.panelScreenOrigin.y + popup.panelHeight,
            };
            if (index >= 0 &&
                static_cast<size_t>(index) < popup.items->size())
            {
                const Item& hovered = (*popup.items)[index];
                if (hovered.enabled && !hovered.separator &&
                    hovered.children.empty())
                {
                    info.command = hovered.command;
                    info.itemScreenRect = popup.itemRects[index];
                    if (hovered.horizontalScrollAction)
                    {
                        OffsetRect(&info.itemScreenRect,
                            -popup.horizontalScrollOffset, 0);
                    }
                    OffsetRect(&info.itemScreenRect,
                        popup.panelScreenOrigin.x - shadowSize_,
                        popup.panelScreenOrigin.y - shadowSize_ -
                            popup.scrollOffset);
                }
            }
            options_.onHover(info);
        }
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
        if (item.horizontalScrollAction)
            OffsetRect(&rect, -popup.horizontalScrollOffset, 0);
        OffsetRect(&rect,
            popup.panelScreenOrigin.x - shadowSize_,
            popup.panelScreenOrigin.y - shadowSize_ - popup.scrollOffset);
        if (options_.onCommand &&
            options_.onCommand(command, rootItems_))
        {
            RefreshAfterCommand(popup);
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
        Item& item = (*popup.items)[index];
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
        if (item.horizontalScrollAction)
            OffsetRect(&row, -popup.horizontalScrollOffset, 0);
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
            ActivateItem(*popup, CurrentItem(*popup), true);
            break;
        case VK_SPACE:
            if (std::ranges::none_of(*popup->items,
                    [](const Item& item) {
                        return item.textInput && item.enabled;
                    }))
            {
                ActivateItem(*popup, CurrentItem(*popup), true);
            }
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
        const int count = static_cast<int>(popup.navigationOrder.size());
        if (count == 0)
            return;
        const int current = CurrentItem(popup);
        const auto found = std::find(
            popup.navigationOrder.begin(),
            popup.navigationOrder.end(), current);
        int position = fromBoundary || found == popup.navigationOrder.end()
            ? (direction > 0 ? -1 : count)
            : static_cast<int>(std::distance(
                popup.navigationOrder.begin(), found));
        for (int attempt = 0; attempt < count; ++attempt)
        {
            position = (position + direction + count) % count;
            const int index = popup.navigationOrder[position];
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
        const int count = static_cast<int>(popup.navigationOrder.size());
        for (int step = 0; step < count; ++step)
        {
            const int position = end ? count - 1 - step : step;
            const int index = popup.navigationOrder[position];
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
        const int count = static_cast<int>(popup->navigationOrder.size());
        if (count == 0)
            return;
        const auto found = std::find(
            popup->navigationOrder.begin(),
            popup->navigationOrder.end(), CurrentItem(*popup));
        const int start = found == popup->navigationOrder.end()
            ? 0
            : (static_cast<int>(std::distance(
                popup->navigationOrder.begin(), found)) + 1) % count;
        for (int step = 0; step < count; ++step)
        {
            const int index = popup->navigationOrder[(start + step) % count];
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

    bool IsTextInputFocused(const Popup& popup, const Item& item) const
    {
        return item.textInput && item.enabled &&
            item.command == textInputCommand_ &&
            popup.depth == ActiveDepth();
    }

    Item* FindFocusedTextInput(Popup& popup, int* index = nullptr)
    {
        if (!popup.items || textInputCommand_ == 0)
            return nullptr;
        for (size_t i = 0; i < popup.items->size(); ++i)
        {
            Item& item = (*popup.items)[i];
            if (item.textInput && item.enabled &&
                item.command == textInputCommand_)
            {
                if (index) *index = static_cast<int>(i);
                return &item;
            }
        }
        return nullptr;
    }

    void FocusTextInput(Popup& popup, int index, size_t cursor,
        bool render)
    {
        if (index < 0 || static_cast<size_t>(index) >= popup.items->size())
            return;
        Item& item = (*popup.items)[index];
        if (!item.textInput || !item.enabled)
            return;
        activeDepth_ = popup.depth;
        textInputCommand_ = item.command;
        textInputCursor_ = std::min(cursor, item.inputText.size());
        textInputSelectionAnchor_ = textInputCursor_;
        textInputComposition_.clear();
        textInputCompositionCursor_ = 0;
        ResetTextCaret(popup);
        if (render)
            Render(popup);
        if (!popups_.empty())
            UpdateTextInputImePosition(popups_.front()->hwnd);
    }

    int TextInputHorizontalOffset(
        HDC dc, const std::wstring& text, size_t cursor,
        int availableWidth) const
    {
        SIZE prefix{};
        const size_t safeCursor = std::min(cursor, text.size());
        if (safeCursor > 0)
        {
            GetTextExtentPoint32W(dc, text.data(),
                static_cast<int>(safeCursor), &prefix);
        }
        return std::max(0, static_cast<int>(prefix.cx) -
            availableWidth + metrics_.outerInset * 2);
    }

    void FocusTextInputFromPoint(
        Popup& popup, int index, POINT point)
    {
        Item& item = (*popup.items)[index];
        RECT row = popup.itemRects[index];
        OffsetRect(&row, 0, -popup.scrollOffset);
        RECT field = row;
        field.left += metrics_.outerInset;
        field.right -= metrics_.outerInset;
        RECT glyphBounds = field;
        glyphBounds.left += metrics_.leftPadding / 2;
        glyphBounds.right = glyphBounds.left + metrics_.iconColumnWidth;
        const int textLeft = glyphBounds.right + metrics_.textGap;
        const int textRight = field.right - metrics_.rightPadding;

        HDC dc = GetDC(nullptr);
        size_t position = item.inputText.size();
        if (dc)
        {
            HGDIOBJ oldFont = SelectObject(dc, textFont_);
            const int offset = TextInputHorizontalOffset(dc,
                item.inputText, textInputCursor_,
                std::max(1, textRight - textLeft));
            const int target = std::max(0,
                static_cast<int>(point.x) - textLeft + offset);
            for (size_t i = 0; i <= item.inputText.size(); ++i)
            {
                SIZE prefix{};
                if (i > 0)
                {
                    GetTextExtentPoint32W(dc, item.inputText.data(),
                        static_cast<int>(i), &prefix);
                }
                if (static_cast<int>(prefix.cx) >= target)
                {
                    position = i;
                    break;
                }
            }
            if (oldFont) SelectObject(dc, oldFont);
            ReleaseDC(nullptr, dc);
        }
        FocusTextInput(popup, index, position, true);
    }

    void ResetTextCaret(Popup& popup)
    {
        textCaretVisible_ = true;
        if (popup.hwnd && IsWindow(popup.hwnd))
        {
            KillTimer(popup.hwnd, kTextCaretTimer);
            SetTimer(popup.hwnd, kTextCaretTimer,
                kTextCaretBlinkMs, nullptr);
        }
    }

    size_t TextSelectionStart(const Item& item) const
    {
        return std::min(
            std::min(textInputCursor_, item.inputText.size()),
            std::min(textInputSelectionAnchor_, item.inputText.size()));
    }

    size_t TextSelectionEnd(const Item& item) const
    {
        return std::max(
            std::min(textInputCursor_, item.inputText.size()),
            std::min(textInputSelectionAnchor_, item.inputText.size()));
    }

    bool ReplaceTextSelection(Item& item, std::wstring text)
    {
        constexpr size_t kMaximumInputLength = 96;
        const size_t start = TextSelectionStart(item);
        const size_t end = TextSelectionEnd(item);
        const size_t retainedLength =
            item.inputText.size() - (end - start);
        const size_t available = retainedLength < kMaximumInputLength
            ? kMaximumInputLength - retainedLength : 0;
        if (text.size() > available)
            text.resize(available);
        if (start == end && text.empty())
            return false;
        item.inputText.replace(start, end - start, text);
        textInputCursor_ = start + text.size();
        textInputSelectionAnchor_ = textInputCursor_;
        textInputComposition_.clear();
        textInputCompositionCursor_ = 0;
        return true;
    }

    void NotifyTextInputChanged(Popup& popup, Item& item)
    {
        const UINT command = item.command;
        const std::wstring text = item.inputText;
        if (options_.onTextChanged)
            options_.onTextChanged(command, text, rootItems_);
        RefreshPopup(popup);
        ResetTextCaret(popup);
        if (!popups_.empty())
            UpdateTextInputImePosition(popups_.front()->hwnd);
    }

    bool HandleTextInputCharacter(wchar_t character)
    {
        Popup* popup = ActivePopup();
        if (!popup || !popup->items)
            return false;
        Item* input = FindFocusedTextInput(*popup);
        if (!input)
            return false;
        if (character == L'\b' || character < L' ' || character == 0x7F ||
            (GetKeyState(VK_CONTROL) & 0x8000) != 0)
            return true;
        if (ReplaceTextSelection(*input, std::wstring(1, character)))
            NotifyTextInputChanged(*popup, *input);
        return true;
    }

    bool CopyTextSelection(const Item& item, bool cut, Popup& popup)
    {
        const size_t start = TextSelectionStart(item);
        const size_t end = TextSelectionEnd(item);
        if (start == end || !OpenClipboard(popup.hwnd))
            return false;
        EmptyClipboard();
        const std::wstring selected = item.inputText.substr(start, end - start);
        const SIZE_T bytes = (selected.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool copied = false;
        if (memory)
        {
            if (void* destination = GlobalLock(memory))
            {
                memcpy(destination, selected.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory))
                    copied = true;
                else
                    GlobalFree(memory);
            }
            else
                GlobalFree(memory);
        }
        CloseClipboard();
        return copied && cut;
    }

    std::wstring ReadClipboardText(HWND owner) const
    {
        std::wstring result;
        if (!OpenClipboard(owner))
            return result;
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data)
        {
            if (const auto* text = static_cast<const wchar_t*>(
                    GlobalLock(data)))
            {
                result = text;
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return result;
    }

    bool HandleTextInputKey(WPARAM key)
    {
        Popup* popup = ActivePopup();
        if (!popup)
            return false;
        Item* input = FindFocusedTextInput(*popup);
        if (!input)
            return false;
        if (!textInputComposition_.empty())
            return true;

        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        textInputCursor_ = std::min(textInputCursor_, input->inputText.size());
        textInputSelectionAnchor_ = std::min(
            textInputSelectionAnchor_, input->inputText.size());
        bool changed = false;

        if (control && key == 'A')
        {
            textInputSelectionAnchor_ = 0;
            textInputCursor_ = input->inputText.size();
        }
        else if (control && (key == 'C' || key == 'X'))
        {
            if (CopyTextSelection(*input, key == 'X', *popup))
                changed = ReplaceTextSelection(*input, L"");
        }
        else if (control && key == 'V')
        {
            changed = ReplaceTextSelection(
                *input, ReadClipboardText(popup->hwnd));
        }
        else if (key == VK_BACK)
        {
            if (TextSelectionStart(*input) != TextSelectionEnd(*input))
                changed = ReplaceTextSelection(*input, L"");
            else if (textInputCursor_ > 0)
            {
                textInputSelectionAnchor_ = textInputCursor_ - 1;
                changed = ReplaceTextSelection(*input, L"");
            }
        }
        else if (key == VK_DELETE)
        {
            if (TextSelectionStart(*input) != TextSelectionEnd(*input))
                changed = ReplaceTextSelection(*input, L"");
            else if (textInputCursor_ < input->inputText.size())
            {
                textInputSelectionAnchor_ = textInputCursor_ + 1;
                changed = ReplaceTextSelection(*input, L"");
            }
        }
        else if (key == VK_LEFT || key == VK_RIGHT ||
            key == VK_HOME || key == VK_END)
        {
            if (key == VK_HOME)
                textInputCursor_ = 0;
            else if (key == VK_END)
                textInputCursor_ = input->inputText.size();
            else if (!shift &&
                TextSelectionStart(*input) != TextSelectionEnd(*input))
            {
                textInputCursor_ = key == VK_LEFT
                    ? TextSelectionStart(*input)
                    : TextSelectionEnd(*input);
            }
            else if (key == VK_LEFT && textInputCursor_ > 0)
                --textInputCursor_;
            else if (key == VK_RIGHT &&
                textInputCursor_ < input->inputText.size())
                ++textInputCursor_;
            if (!shift)
                textInputSelectionAnchor_ = textInputCursor_;
        }
        else if (key == VK_ESCAPE && !input->inputText.empty())
        {
            textInputCursor_ = input->inputText.size();
            textInputSelectionAnchor_ = 0;
            changed = ReplaceTextSelection(*input, L"");
        }
        else if (key == VK_SPACE)
        {
            return false;
        }
        else
        {
            return false;
        }

        if (changed)
            NotifyTextInputChanged(*popup, *input);
        else
        {
            ResetTextCaret(*popup);
            Render(*popup);
            if (!popups_.empty())
                UpdateTextInputImePosition(popups_.front()->hwnd);
        }
        return true;
    }

    std::wstring ReadImeString(HIMC context, DWORD index) const
    {
        std::wstring result;
        const LONG bytes = ImmGetCompositionStringW(
            context, index, nullptr, 0);
        if (bytes <= 0)
            return result;
        result.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
        const LONG copied = ImmGetCompositionStringW(
            context, index, result.data(), static_cast<DWORD>(bytes));
        if (copied < 0)
            result.clear();
        else
            result.resize(static_cast<size_t>(copied) / sizeof(wchar_t));
        return result;
    }

    void HandleTextInputImeComposition(HWND hwnd, LPARAM flags)
    {
        Popup* popup = ActivePopup();
        if (!popup)
            return;
        Item* input = FindFocusedTextInput(*popup);
        if (!input)
            return;
        HIMC context = ImmGetContext(hwnd);
        if (!context)
            return;
        bool changed = false;
        if ((flags & GCS_RESULTSTR) != 0)
        {
            changed = ReplaceTextSelection(
                *input, ReadImeString(context, GCS_RESULTSTR));
        }
        if ((flags & (GCS_COMPSTR | GCS_CURSORPOS)) != 0)
        {
            textInputComposition_ = ReadImeString(context, GCS_COMPSTR);
            const LONG cursor = ImmGetCompositionStringW(
                context, GCS_CURSORPOS, nullptr, 0);
            textInputCompositionCursor_ = cursor >= 0
                ? std::min(static_cast<size_t>(cursor),
                    textInputComposition_.size())
                : textInputComposition_.size();
        }
        else if (flags == 0)
        {
            textInputComposition_.clear();
            textInputCompositionCursor_ = 0;
        }
        ImmReleaseContext(hwnd, context);
        if (changed)
            NotifyTextInputChanged(*popup, *input);
        else
        {
            ResetTextCaret(*popup);
            Render(*popup);
        }
        UpdateTextInputImePosition(hwnd);
    }

    void UpdateTextInputImePosition(HWND focusWindow)
    {
        Popup* popup = ActivePopup();
        if (!popup || !focusWindow || !IsWindow(focusWindow))
            return;
        int index = -1;
        Item* input = FindFocusedTextInput(*popup, &index);
        if (!input || index < 0)
            return;

        const size_t cursor = std::min(
            textInputCursor_, input->inputText.size());
        const size_t anchor = std::min(
            textInputSelectionAnchor_, input->inputText.size());
        const size_t start = std::min(cursor, anchor);
        const size_t end = std::max(cursor, anchor);
        std::wstring display = input->inputText;
        size_t displayCursor = cursor;
        if (!textInputComposition_.empty())
        {
            display = input->inputText.substr(0, start);
            display += textInputComposition_;
            display += input->inputText.substr(end);
            displayCursor = start + std::min(
                textInputCompositionCursor_, textInputComposition_.size());
        }

        RECT row = popup->itemRects[index];
        OffsetRect(&row, 0, -popup->scrollOffset);
        RECT field = row;
        field.left += metrics_.outerInset;
        field.right -= metrics_.outerInset;
        const int textLeft = field.left + metrics_.leftPadding / 2 +
            metrics_.iconColumnWidth + metrics_.textGap;
        const int textRight = field.right - metrics_.rightPadding;
        HDC dc = GetDC(nullptr);
        int advance = 0;
        int horizontalOffset = 0;
        if (dc)
        {
            HGDIOBJ oldFont = SelectObject(dc, textFont_);
            SIZE prefix{};
            if (displayCursor > 0)
            {
                GetTextExtentPoint32W(dc, display.data(),
                    static_cast<int>(displayCursor), &prefix);
            }
            advance = static_cast<int>(prefix.cx);
            horizontalOffset = TextInputHorizontalOffset(dc,
                display, displayCursor,
                std::max(1, textRight - textLeft));
            if (oldFont) SelectObject(dc, oldFont);
            ReleaseDC(nullptr, dc);
        }
        POINT caret{
            popup->panelScreenOrigin.x - shadowSize_ +
                std::clamp(textLeft + advance - horizontalOffset,
                    textLeft, std::max(textLeft, textRight - 1)),
            popup->panelScreenOrigin.y - shadowSize_ + field.bottom,
        };
        ScreenToClient(focusWindow, &caret);
        HIMC context = ImmGetContext(focusWindow);
        if (!context)
            return;
        COMPOSITIONFORM composition{};
        composition.dwStyle = CFS_POINT;
        composition.ptCurrentPos = caret;
        ImmSetCompositionWindow(context, &composition);
        CANDIDATEFORM candidate{};
        candidate.dwIndex = 0;
        candidate.dwStyle = CFS_CANDIDATEPOS;
        candidate.ptCurrentPos = caret;
        candidate.ptCurrentPos.y += metrics_.rowHeight;
        ImmSetCandidateWindow(context, &candidate);
        ImmReleaseContext(focusWindow, context);
    }

    void RefreshPopup(Popup& popup)
    {
        CloseFromDepth(popup.depth + 1);
        popup.hoveredItem = -1;
        popup.keyboardItem = -1;
        popup.scrollOffset = 0;
        CalculateLayout(popup);
        if (popup.hwnd && IsWindow(popup.hwnd))
        {
            SetWindowPos(popup.hwnd, nullptr,
                popup.panelScreenOrigin.x - shadowSize_,
                popup.panelScreenOrigin.y - shadowSize_,
                popup.windowWidth, popup.windowHeight,
                SWP_NOACTIVATE | SWP_NOZORDER);
            ApplyBlurClipRegion(popup);
            Render(popup);
        }
    }

    int FirstDetachedPopupDepth() const
    {
        if (popups_.empty() || !popups_.front() ||
            popups_.front()->items != &rootItems_)
            return 0;

        for (size_t i = 1; i < popups_.size(); ++i)
        {
            const Popup& parent = *popups_[i - 1];
            const Popup& child = *popups_[i];
            if (!parent.items || child.parentItem < 0 ||
                static_cast<size_t>(child.parentItem) >=
                    parent.items->size())
                return static_cast<int>(i);

            const Item& parentItem = (*parent.items)[child.parentItem];
            if (&parentItem.children != child.items)
                return static_cast<int>(i);
        }
        return -1;
    }

    void RefreshAfterCommand(Popup& commandPopup)
    {
        const int detachedDepth = FirstDetachedPopupDepth();
        if (detachedDepth > 0)
        {
            // A callback may rebuild rootItems_.  Any cascaded popup then
            // points into the destroyed old tree and must not be rendered.
            // Keep the root menu open, but rebuild it and discard all stale
            // child popups.
            CloseFromDepth(1);
            if (!popups_.empty())
                RefreshPopup(*popups_.front());
            return;
        }

        RefreshPopup(commandPopup);
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
        if ((*popup.items)[index].horizontalScrollAction)
        {
            const int viewportLeft = popup.horizontalScrollRect.left;
            const int viewportRight = popup.horizontalScrollRect.right;
            if (row.left - popup.horizontalScrollOffset < viewportLeft)
                popup.horizontalScrollOffset = row.left - viewportLeft;
            else if (row.right - popup.horizontalScrollOffset > viewportRight)
                popup.horizontalScrollOffset = row.right - viewportRight;
            popup.horizontalScrollOffset = std::clamp(
                popup.horizontalScrollOffset, 0,
                MaxHorizontalScroll(popup));
        }
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

    void ScrollHorizontal(Popup& popup, int direction)
    {
        const int oldOffset = popup.horizontalScrollOffset;
        popup.horizontalScrollOffset = std::clamp(
            popup.horizontalScrollOffset +
                direction * metrics_.rowHeight * 2,
            0, MaxHorizontalScroll(popup));
        if (popup.horizontalScrollOffset != oldOffset)
        {
            CloseFromDepth(popup.depth + 1);
            popup.hoveredItem = -1;
            popup.keyboardItem = -1;
            Render(popup);
        }
    }

    int MaxHorizontalScroll(const Popup& popup) const
    {
        const int viewportWidth = popup.horizontalScrollRect.right -
            popup.horizontalScrollRect.left;
        return std::max(0,
            popup.horizontalScrollContentWidth - viewportWidth);
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
        Popup* active = ActivePopup();
        if (!active || !FindFocusedTextInput(*active))
        {
            textInputCommand_ = 0;
            textInputCursor_ = 0;
            textInputSelectionAnchor_ = 0;
            textInputComposition_.clear();
            textInputCompositionCursor_ = 0;
        }
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

    void DrawHorizontalScrollIndicator(
        HDC dc, const Popup& popup, bool left)
    {
        RECT track = popup.horizontalScrollRect;
        OffsetRect(&track, 0, -popup.scrollOffset);
        const int indicatorWidth = Scale(14, options_.dpi);
        RECT background = track;
        if (left)
            background.right = background.left + indicatorWidth;
        else
            background.left = background.right - indicatorWidth;
        HBRUSH brush = CreateSolidBrush(palette_.background);
        if (brush)
        {
            FillRect(dc, &background, brush);
            DeleteObject(brush);
        }

        const int centerX = (background.left + background.right) / 2;
        const int centerY = (background.top + background.bottom) / 2;
        const int half = Scale(3, options_.dpi);
        HPEN pen = CreatePen(PS_SOLID, 1,
            lightTheme_ ? RGB(95, 95, 95) : RGB(190, 190, 190));
        if (!pen)
            return;
        HGDIOBJ oldPen = SelectObject(dc, pen);
        MoveToEx(dc, centerX + (left ? half / 2 : -half / 2),
            centerY - half, nullptr);
        LineTo(dc, centerX + (left ? -half / 2 : half / 2), centerY);
        LineTo(dc, centerX + (left ? half / 2 : -half / 2),
            centerY + half);
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
        const float blurPanelAlpha = lightTheme_ ? 70.0f : 76.0f;
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
        const DWORD tintAlpha = lightTheme_ ? 0x58 : 0x60;
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
    HFONT fluentIconFont_ = nullptr;
    HFONT fontAwesomeIconFont_ = nullptr;
    HFONT quickTextFont_ = nullptr;
    HFONT quickFluentIconFont_ = nullptr;
    HFONT quickFontAwesomeIconFont_ = nullptr;
    std::vector<std::unique_ptr<Popup>> popups_;
    int activeDepth_ = 0;
    UINT textInputCommand_ = 0;
    size_t textInputCursor_ = 0;
    size_t textInputSelectionAnchor_ = 0;
    std::wstring textInputComposition_;
    size_t textInputCompositionCursor_ = 0;
    bool textCaretVisible_ = true;
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

void DismissActive()
{
    const HWND root = gActiveRootMenu.load();
    if (root && IsWindow(root))
        PostMessageW(root, kCancelMessage, 0, 0);
}

} // namespace snowdesktop::modern_menu
