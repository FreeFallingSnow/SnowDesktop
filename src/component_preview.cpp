#include "component_preview.h"

#include "menu_icon_render.h"
#include "modern_menu.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>

namespace snowdesktop::component_preview
{
namespace
{

constexpr wchar_t kPreviewWindowClass[] =
    L"SnowDesktop.ComponentPreviewPopup";
constexpr UINT_PTR kOpenTimer = 1;
constexpr UINT_PTR kHideTimer = 2;

int Scale(int value, UINT dpi)
{
    return std::max(1, MulDiv(value, static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

void Fill(HDC dc, const RECT& rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    if (brush)
    {
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    }
}

void RoundedBox(HDC dc, const RECT& rect, int radius,
    COLORREF fill, COLORREF border)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = brush ? SelectObject(dc, brush) : nullptr;
    HGDIOBJ oldPen = pen ? SelectObject(dc, pen) : nullptr;
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
        radius * 2, radius * 2);
    if (oldPen) SelectObject(dc, oldPen);
    if (oldBrush) SelectObject(dc, oldBrush);
    if (pen) DeleteObject(pen);
    if (brush) DeleteObject(brush);
}

void RoundedOutline(HDC dc, const RECT& rect, int radius, COLORREF border)
{
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = pen ? SelectObject(dc, pen) : nullptr;
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
        radius * 2, radius * 2);
    if (oldBrush) SelectObject(dc, oldBrush);
    if (oldPen) SelectObject(dc, oldPen);
    if (pen) DeleteObject(pen);
}

void DrawCenteredText(HDC dc, HFONT font, COLORREF color,
    const std::wstring& text, RECT rect)
{
    HGDIOBJ oldFont = SelectObject(dc, font);
    const COLORREF oldColor = SetTextColor(dc, color);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
            DT_NOPREFIX);
    SetBkMode(dc, oldMode);
    SetTextColor(dc, oldColor);
    if (oldFont) SelectObject(dc, oldFont);
}

void DrawWrappedText(HDC dc, HFONT font, COLORREF color,
    const std::wstring& text, RECT rect, UINT flags = DT_LEFT | DT_WORDBREAK)
{
    HGDIOBJ oldFont = SelectObject(dc, font);
    const COLORREF oldColor = SetTextColor(dc, color);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
        flags | DT_NOPREFIX | DT_END_ELLIPSIS);
    SetBkMode(dc, oldMode);
    SetTextColor(dc, oldColor);
    if (oldFont) SelectObject(dc, oldFont);
}

bool HasVisibleText(const std::wstring& text)
{
    return std::any_of(text.begin(), text.end(), [](wchar_t character) {
        return !std::iswspace(character);
    });
}

int MeasureTextHeight(HDC dc, HFONT font, const std::wstring& text,
    int width, UINT flags)
{
    if (!dc || !font || !HasVisibleText(text) || width <= 0)
        return 0;
    HGDIOBJ oldFont = SelectObject(dc, font);
    RECT bounds{ 0, 0, width, 0 };
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds,
        flags | DT_NOPREFIX | DT_CALCRECT);
    if (oldFont) SelectObject(dc, oldFont);
    return std::max(0, static_cast<int>(bounds.bottom - bounds.top));
}

void DrawBitmap(HDC destination, const Bitmap& image, const RECT& bounds)
{
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) *
            image.height)
        return;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = image.width;
    info.bmiHeader.biHeight = -image.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(destination, &info,
        DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC source = bitmap ? CreateCompatibleDC(destination) : nullptr;
    if (!bitmap || !source || !pixels)
    {
        if (source) DeleteDC(source);
        if (bitmap) DeleteObject(bitmap);
        return;
    }
    std::copy(image.pixels.begin(), image.pixels.end(),
        static_cast<std::uint32_t*>(pixels));
    HGDIOBJ oldBitmap = SelectObject(source, bitmap);
    using AlphaBlendFn = BOOL(WINAPI*)(HDC, int, int, int, int,
        HDC, int, int, int, int, BLENDFUNCTION);
    static const HMODULE alphaModule = LoadLibraryW(L"msimg32.dll");
    static const auto alphaBlend = alphaModule
        ? reinterpret_cast<AlphaBlendFn>(GetProcAddress(
            alphaModule, "AlphaBlend"))
        : nullptr;
    if (alphaBlend)
    {
        const BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        alphaBlend(destination, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            source, 0, 0, image.width, image.height, blend);
    }
    if (oldBitmap) SelectObject(source, oldBitmap);
    DeleteDC(source);
    DeleteObject(bitmap);
}

bool IsInsideRoundedPanel(int x, int y, int width, int height, int radius)
{
    const int nearestX = std::clamp(x, radius, width - radius - 1);
    const int nearestY = std::clamp(y, radius, height - radius - 1);
    const int dx = x - nearestX;
    const int dy = y - nearestY;
    return dx * dx + dy * dy <= radius * radius;
}

bool SameRgb(std::uint32_t pixel, COLORREF color)
{
    return (pixel & 0x00ffffffu) ==
        (static_cast<std::uint32_t>(GetBValue(color)) |
         (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
         (static_cast<std::uint32_t>(GetRValue(color)) << 16));
}

bool OptionValue(const ApplySettings& settings, OptionSetting setting)
{
    switch (setting)
    {
    case OptionSetting::ListMode: return settings.listMode;
    case OptionSetting::ScrollContainerMode:
        return settings.scrollContainerMode;
    case OptionSetting::DateHeaders: return settings.dateHeaders;
    case OptionSetting::ShowFileCategories:
        return settings.showFileCategories;
    case OptionSetting::ShowSearchBox: return settings.showSearchBox;
    }
    return false;
}

bool IsOptionVisible(const Card& card, const Option& option)
{
    // A collection's list layout belongs to its scrolling-container mode.
    // Large-folder rendering has no list variant in the production menu.
    if (card.applySettings.kind == ApplyKind::Collection &&
        option.setting == OptionSetting::ListMode)
        return card.applySettings.scrollContainerMode;
    return true;
}

void SetOptionValue(ApplySettings& settings,
    OptionSetting setting, bool value)
{
    switch (setting)
    {
    case OptionSetting::ListMode: settings.listMode = value; break;
    case OptionSetting::ScrollContainerMode:
        settings.scrollContainerMode = value; break;
    case OptionSetting::DateHeaders: settings.dateHeaders = value; break;
    case OptionSetting::ShowFileCategories:
        settings.showFileCategories = value; break;
    case OptionSetting::ShowSearchBox:
        settings.showSearchBox = value; break;
    }
}

std::wstring SettingsCacheSuffix(
    const ApplySettings& settings, bool hovered)
{
    return std::wstring(L":settings:") +
        (settings.listMode ? L"1" : L"0") +
        (settings.scrollContainerMode ? L"1" : L"0") +
        (settings.dateHeaders ? L"1" : L"0") +
        (settings.showFileCategories ? L"1" : L"0") +
        (settings.showSearchBox ? L"1" : L"0") +
        (hovered ? L":hover" : L":idle");
}

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

} // namespace

Window::~Window()
{
    Close();
}

bool Window::EnsureCreated(HWND owner)
{
    static const bool registered = [] {
        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kPreviewWindowClass;
        return RegisterClassExW(&windowClass) != 0 ||
            GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) return false;
    if (hwnd_ && IsWindow(hwnd_)) return true;
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kPreviewWindowClass, L"", WS_POPUP,
        0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), this);
    return hwnd_ != nullptr;
}

bool Window::Show(const Model& model, const RECT& menuBounds,
    HWND owner, UINT dpi, bool lightTheme, ApplyHandler onApply,
    const RECT& itemBounds)
{
    if (model.Empty())
    {
        ScheduleHide();
        return false;
    }
    if (!EnsureCreated(owner))
        return false;

    KillTimer(hwnd_, kOpenTimer);
    KillTimer(hwnd_, kHideTimer);
    const std::wstring identity = ModelIdentity(model);
    if (identity != modelIdentity_)
    {
        currentCard_ = 0;
        modelIdentity_ = identity;
    }
    model_ = model;
    menuBounds_ = menuBounds;
    itemBounds_ = itemBounds;
    dpi_ = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    lightTheme_ = lightTheme;
    onApply_ = std::move(onApply);
    componentHovered_ = false;
    currentCard_ = std::min(currentCard_, model_.cards.size() - 1);
    ApplyWindowAppearance(lightTheme_);
    if (!RenderCurrent())
        return false;
    if (!IsWindowVisible(hwnd_))
        AnimateWindow(hwnd_, 70,
            AW_BLEND | AW_SLIDE | AW_HOR_POSITIVE);
    else
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
}

bool Window::ScheduleShow(const Model& model, const RECT& menuBounds,
    HWND owner, UINT dpi, bool lightTheme, ApplyHandler onApply,
    const RECT& itemBounds)
{
    if (model.Empty())
    {
        ScheduleHide();
        return false;
    }
    if (!EnsureCreated(owner)) return false;

    const std::wstring identity = ModelIdentity(model);
    if (IsWindowVisible(hwnd_) && identity == modelIdentity_)
    {
        KillTimer(hwnd_, kOpenTimer);
        KillTimer(hwnd_, kHideTimer);
        menuBounds_ = menuBounds;
        itemBounds_ = itemBounds;
        onApply_ = std::move(onApply);
        return true;
    }

    pendingModel_ = model;
    pendingMenuBounds_ = menuBounds;
    pendingItemBounds_ = itemBounds;
    pendingOwner_ = owner;
    pendingDpi_ = dpi;
    pendingLightTheme_ = lightTheme;
    pendingOnApply_ = std::move(onApply);
    // A sibling submenu keeps its old contents visible until the new frame
    // is ready.  Starting a close timer here used to hide the old preview
    // first, and Hide() also cancelled the pending open timer.
    KillTimer(hwnd_, kHideTimer);
    SetTimer(hwnd_, kOpenTimer,
        modern_menu::kSubmenuOpenDelayMs, nullptr);
    return true;
}

void Window::ScheduleHide()
{
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    KillTimer(hwnd_, kOpenTimer);
    pendingModel_ = {};
    if (IsWindowVisible(hwnd_))
        SetTimer(hwnd_, kHideTimer,
            modern_menu::kSubmenuCloseDelayMs, nullptr);
}

void Window::Hide()
{
    if (hwnd_ && IsWindow(hwnd_))
    {
        KillTimer(hwnd_, kHideTimer);
        KillTimer(hwnd_, kOpenTimer);
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void Window::Close()
{
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
    model_ = {};
    modelIdentity_.clear();
    onApply_ = {};
    pendingModel_ = {};
    pendingOnApply_ = {};
}

RECT Window::OptionBoundsForTesting(
    OptionSetting setting, bool value) const
{
    const auto match = std::find_if(
        optionHits_.begin(), optionHits_.end(),
        [&](const OptionHit& hit) {
            return hit.setting == setting && hit.value == value;
        });
    return match == optionHits_.end() ? RECT{} : match->bounds;
}

std::wstring Window::ModelIdentity(const Model& model) const
{
    std::wstring result = model.title + L"|" +
        std::to_wstring(model.cards.size());
    for (const Card& card : model.cards)
        result += L"|" + card.cacheKey;
    return result;
}

bool Window::RenderCurrent()
{
    if (!hwnd_ || model_.Empty() || currentCard_ >= model_.cards.size())
        return false;
    Card& card = model_.cards[currentCard_];
    const int padding = Scale(16, dpi_);
    const int gap = Scale(8, dpi_);
    const int previewInset = Scale(8, dpi_);
    const int titleHeight = Scale(24, dpi_);
    const int pagerHeight = model_.cards.size() > 1 ? Scale(28, dpi_) : 0;
    const int optionHeight = Scale(30, dpi_);
    const int controlMargin = Scale(4, dpi_);
    const size_t visibleOptionCount = static_cast<size_t>(
        std::count_if(card.options.begin(), card.options.end(),
            [&](const Option& option) {
                return IsOptionVisible(card, option);
            }));
    const int controlsHeight =
        (pagerHeight ? pagerHeight + controlMargin * 2 : 0) +
        static_cast<int>(visibleOptionCount) *
            (optionHeight + controlMargin);
    const int previewWidth = std::max(1, card.previewWidth);
    const int previewHeight = std::max(1, card.previewHeight);
    width_ = std::max(Scale(360, dpi_),
        previewWidth + (padding + previewInset) * 2);

    HFONT titleFont = CreateFontW(-Scale(17, dpi_), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT cardTitleFont = CreateFontW(-Scale(13, dpi_), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT bodyFont = CreateFontW(-Scale(11, dpi_), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT glyphFont = CreateFontW(-Scale(15, dpi_), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");

    HDC measureDc = CreateCompatibleDC(nullptr);
    const int textWidth = width_ - padding * 2;
    const int introductionHeight = MeasureTextHeight(measureDc, bodyFont,
        model_.introduction, textWidth, DT_LEFT | DT_WORDBREAK);
    const int resizeHintHeight = MeasureTextHeight(measureDc, bodyFont,
        model_.resizeHint, textWidth, DT_LEFT | DT_SINGLELINE);
    const bool hasCardHeader = HasVisibleText(card.title) ||
        HasVisibleText(card.sizeLabel);
    const bool hasDescription = HasVisibleText(card.description);
    const bool hasApplyButton = HasVisibleText(model_.applyLabel);
    const int metadataHorizontalPadding = Scale(10, dpi_);
    const int metadataTopPadding = Scale(8, dpi_);
    const int metadataBottomPadding = Scale(8, dpi_);
    const int cardHeaderHeight = hasCardHeader ? Scale(21, dpi_) : 0;
    const int descriptionHeight = MeasureTextHeight(measureDc, bodyFont,
        card.description,
        width_ - (padding + metadataHorizontalPadding) * 2,
        DT_LEFT | DT_WORDBREAK);
    const int applyButtonHeight = hasApplyButton ? Scale(32, dpi_) : 0;
    if (measureDc) DeleteDC(measureDc);

    int metadataHeight = 0;
    if (hasCardHeader || hasDescription || hasApplyButton)
    {
        metadataHeight = metadataTopPadding + metadataBottomPadding;
        if (hasCardHeader)
            metadataHeight += cardHeaderHeight;
        if (hasDescription)
        {
            if (hasCardHeader) metadataHeight += Scale(2, dpi_);
            metadataHeight += descriptionHeight;
        }
        if (hasApplyButton)
        {
            if (hasCardHeader || hasDescription)
                metadataHeight += Scale(6, dpi_);
            metadataHeight += applyButtonHeight;
        }
    }
    height_ = padding + titleHeight +
        (introductionHeight ? Scale(4, dpi_) + introductionHeight : 0) +
        (resizeHintHeight ? Scale(2, dpi_) + resizeHintHeight : 0) +
        gap + previewInset + previewHeight + controlsHeight + metadataHeight +
        previewInset + padding;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info,
        DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    HDC dc = bitmap ? CreateCompatibleDC(nullptr) : nullptr;
    if (!bitmap || !dc || !rawPixels)
    {
        if (dc) DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);
        if (titleFont) DeleteObject(titleFont);
        if (cardTitleFont) DeleteObject(cardTitleFont);
        if (bodyFont) DeleteObject(bodyFont);
        if (glyphFont) DeleteObject(glyphFont);
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    auto* pixels = static_cast<std::uint32_t*>(rawPixels);
    std::fill_n(pixels, static_cast<size_t>(width_) * height_, 0u);

    const auto palette = menu_icon::ResolvePalette(lightTheme_);
    RECT panel{ 0, 0, width_, height_ };
    Fill(dc, panel, palette.background);

    int cursorY = padding;
    RECT titleRect{ padding, cursorY, width_ - padding,
        cursorY + titleHeight };
    const int closeButtonSize = Scale(24, dpi_);
    closeRect_ = { titleRect.right - closeButtonSize, titleRect.top,
        titleRect.right, titleRect.top + closeButtonSize };
    titleRect.right = closeRect_.left - Scale(8, dpi_);
    DrawWrappedText(dc, titleFont, palette.text, model_.title, titleRect,
        DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RoundedBox(dc, closeRect_, Scale(6, dpi_),
        palette.hoverBackground, palette.separator);
    DrawCenteredText(dc, glyphFont, palette.text, L"\u00D7", closeRect_);
    cursorY = titleRect.bottom;
    if (introductionHeight)
    {
        cursorY += Scale(4, dpi_);
        RECT introductionRect{ padding, cursorY, width_ - padding,
            cursorY + introductionHeight };
        DrawWrappedText(dc, bodyFont, palette.disabledText,
            model_.introduction, introductionRect);
        cursorY = introductionRect.bottom;
    }
    if (resizeHintHeight)
    {
        cursorY += Scale(2, dpi_);
        RECT resizeHintRect{ padding, cursorY, width_ - padding,
            cursorY + resizeHintHeight };
        DrawWrappedText(dc, bodyFont, palette.disabledText,
            model_.resizeHint, resizeHintRect,
            DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        cursorY = resizeHintRect.bottom;
    }
    cursorY += gap;
    const int cardTop = cursorY;
    const int previewLeft = (width_ - previewWidth) / 2;
    previewRect_ = { previewLeft, cardTop + previewInset,
        previewLeft + previewWidth, cardTop + previewInset + previewHeight };
    // Keep the viewport material transparent.  The component renderer owns
    // every pixel inside it, including a fully transparent clock background.
    RoundedOutline(dc, previewRect_, Scale(6, dpi_), palette.separator);

    const std::wstring frameCacheKey = card.cacheKey.empty()
        ? std::wstring{}
        : card.cacheKey + SettingsCacheSuffix(
            card.applySettings, componentHovered_);
    Bitmap rendered;
    if (!frameCacheKey.empty())
    {
        const auto cached = cardFrameCache_.find(frameCacheKey);
        if (cached != cardFrameCache_.end())
            rendered = cached->second;
    }
    if (rendered.pixels.empty() && card.render)
    {
        rendered = card.render(previewWidth, previewHeight, dpi_,
            card.applySettings, componentHovered_);
        if (!frameCacheKey.empty() && !rendered.pixels.empty())
        {
            if (cardFrameCache_.size() >= 128)
                cardFrameCache_.clear();
            cardFrameCache_[frameCacheKey] = rendered;
        }
    }
    DrawBitmap(dc, rendered, previewRect_);

    int controlsY = previewRect_.bottom;
    previousButton_ = {};
    nextButton_ = {};
    pagerRect_ = {};
    if (pagerHeight)
    {
        controlsY += controlMargin;
        pagerRect_ = { padding + previewInset, controlsY,
            width_ - padding - previewInset, controlsY + pagerHeight };
        const int buttonWidth = Scale(42, dpi_);
        previousButton_ = pagerRect_;
        previousButton_.right = previousButton_.left + buttonWidth;
        nextButton_ = pagerRect_;
        nextButton_.left = nextButton_.right - buttonWidth;
        RECT statusRect{ previousButton_.right, pagerRect_.top,
            nextButton_.left, pagerRect_.bottom };
        RoundedBox(dc, previousButton_, Scale(6, dpi_),
            palette.hoverBackground, palette.separator);
        RoundedBox(dc, nextButton_, Scale(6, dpi_),
            palette.hoverBackground, palette.separator);
        DrawCenteredText(dc, glyphFont,
            currentCard_ > 0 ? palette.text : palette.disabledText,
            L"\u2039", previousButton_);
        DrawCenteredText(dc, bodyFont, palette.text,
            std::to_wstring(currentCard_ + 1) + L" / " +
                std::to_wstring(model_.cards.size()), statusRect);
        DrawCenteredText(dc, glyphFont,
            currentCard_ + 1 < model_.cards.size()
                ? palette.text : palette.disabledText,
            L"\u203a", nextButton_);
        controlsY = pagerRect_.bottom + controlMargin;
    }

    optionHits_.clear();
    for (const Option& option : card.options)
    {
        if (!IsOptionVisible(card, option)) continue;
        controlsY += controlMargin;
        RECT row{ padding + previewInset, controlsY,
            width_ - padding - previewInset, controlsY + optionHeight };
        const int choiceWidth = Scale(70, dpi_);
        const int choiceGap = Scale(6, dpi_);
        RECT onRect{ row.right - choiceWidth, row.top,
            row.right, row.bottom };
        RECT offRect{ onRect.left - choiceGap - choiceWidth, row.top,
            onRect.left - choiceGap, row.bottom };
        RECT labelRect{ row.left, row.top,
            offRect.left - Scale(8, dpi_), row.bottom };
        const bool enabled = OptionValue(card.applySettings, option.setting);
        DrawWrappedText(dc, bodyFont, palette.text, option.label, labelRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const int capsuleRadius = optionHeight / 2;
        RoundedBox(dc, offRect, capsuleRadius,
            enabled ? palette.background : palette.hoverBackground,
            enabled ? palette.separator : palette.accent);
        RoundedBox(dc, onRect, capsuleRadius,
            enabled ? palette.hoverBackground : palette.background,
            enabled ? palette.accent : palette.separator);
        DrawCenteredText(dc, bodyFont,
            enabled ? palette.disabledText : palette.accent,
            option.offLabel, offRect);
        DrawCenteredText(dc, bodyFont,
            enabled ? palette.accent : palette.disabledText,
            option.onLabel, onRect);
        optionHits_.push_back({ offRect, option.setting, false });
        optionHits_.push_back({ onRect, option.setting, true });
        controlsY = row.bottom;
    }

    const int metadataTop = previewRect_.bottom + controlsHeight;
    RECT cardRect{ padding, cardTop, width_ - padding,
        metadataTop + metadataHeight + previewInset };
    applyRect_ = {};
    RoundedOutline(dc, cardRect, Scale(8, dpi_), palette.separator);
    const int metadataLeft = cardRect.left + metadataHorizontalPadding;
    const int metadataRight = cardRect.right - metadataHorizontalPadding;
    int metadataY = metadataTop +
        (metadataHeight ? metadataTopPadding : 0);
    if (hasCardHeader)
    {
        RECT cardTitle{ metadataLeft, metadataY, metadataRight,
            metadataY + cardHeaderHeight };
        if (HasVisibleText(card.sizeLabel))
        {
            RECT badge = cardTitle;
            badge.left = std::max(
                badge.left, badge.right - Scale(58, dpi_));
            RoundedBox(dc, badge, Scale(7, dpi_),
                palette.hoverBackground, palette.separator);
            DrawCenteredText(dc, bodyFont, palette.disabledText,
                card.sizeLabel, badge);
            cardTitle.right = badge.left - Scale(6, dpi_);
        }
        if (HasVisibleText(card.title))
        {
            DrawWrappedText(dc, cardTitleFont, palette.text, card.title,
                cardTitle, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        metadataY += cardHeaderHeight;
    }
    if (hasDescription)
    {
        if (hasCardHeader) metadataY += Scale(2, dpi_);
        RECT descriptionRect{ metadataLeft, metadataY, metadataRight,
            metadataY + descriptionHeight };
        DrawWrappedText(dc, bodyFont, palette.disabledText,
            card.description, descriptionRect);
        metadataY = descriptionRect.bottom;
    }
    if (hasApplyButton)
    {
        if (hasCardHeader || hasDescription) metadataY += Scale(6, dpi_);
        applyRect_ = { metadataLeft, metadataY, metadataRight,
            metadataY + applyButtonHeight };
        RoundedBox(dc, applyRect_, Scale(8, dpi_),
            palette.accent, palette.accent);
        DrawCenteredText(dc, cardTitleFont, RGB(255, 255, 255),
            model_.applyLabel, applyRect_);
    }

    if (titleFont) DeleteObject(titleFont);
    if (cardTitleFont) DeleteObject(cardTitleFont);
    if (bodyFont) DeleteObject(bodyFont);
    if (glyphFont) DeleteObject(glyphFont);

    const int radius = Scale(10, dpi_);
    constexpr unsigned contentAlpha = 246;
    const unsigned materialAlpha = lightTheme_ ? 70u : 76u;
    for (int y = 0; y < height_; ++y)
    {
        for (int x = 0; x < width_; ++x)
        {
            std::uint32_t& pixel = pixels[
                static_cast<size_t>(y) * width_ + x];
            if (!IsInsideRoundedPanel(x, y, width_, height_, radius))
            {
                pixel = 0;
                continue;
            }
            const unsigned alpha = SameRgb(pixel, palette.background)
                ? materialAlpha : contentAlpha;
            const unsigned blue = (pixel & 0xFFu) * alpha / 255u;
            const unsigned green = ((pixel >> 8) & 0xFFu) * alpha / 255u;
            const unsigned red = ((pixel >> 16) & 0xFFu) * alpha / 255u;
            pixel = blue | (green << 8) | (red << 16) | (alpha << 24);
        }
    }

    HRGN region = CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1,
        radius * 2, radius * 2);
    if (region && !SetWindowRgn(hwnd_, region, FALSE))
        DeleteObject(region);

    POINT destination = ResolvePosition(menuBounds_, dpi_);
    SIZE size{ width_, height_ };
    POINT source{};
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    const BOOL updated = UpdateLayeredWindow(hwnd_, nullptr, &destination,
        &size, dc, &source, 0, &blend, ULW_ALPHA);
    if (updated)
        committedPositions_.push_back(destination);
    if (oldBitmap) SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return updated != FALSE;
}

void Window::SelectRelative(int delta)
{
    if (model_.cards.empty()) return;
    const int next = std::clamp(
        static_cast<int>(currentCard_) + delta, 0,
        static_cast<int>(model_.cards.size()) - 1);
    if (next == static_cast<int>(currentCard_)) return;
    currentCard_ = static_cast<size_t>(next);
    componentHovered_ = false;
    RenderCurrent();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void Window::SetOption(OptionSetting setting, bool value)
{
    if (currentCard_ >= model_.cards.size()) return;
    ApplySettings& settings = model_.cards[currentCard_].applySettings;
    if (OptionValue(settings, setting) == value) return;
    SetOptionValue(settings, setting, value);
    if (settings.kind == ApplyKind::Collection &&
        setting == OptionSetting::ScrollContainerMode && !value)
        settings.listMode = false;
    RenderCurrent();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void Window::ApplyCurrent()
{
    if (!onApply_ || currentCard_ >= model_.cards.size()) return;
    const ApplySettings settings = model_.cards[currentCard_].applySettings;
    if (settings.kind != ApplyKind::None)
        onApply_(settings);
}

bool Window::PointerInsideMenuOrPreview() const
{
    POINT point{};
    if (!GetCursorPos(&point)) return false;
    RECT previewBounds{};
    return hwnd_ && GetWindowRect(hwnd_, &previewBounds) &&
        PtInRect(&previewBounds, point);
}

void Window::ApplyWindowAppearance(bool lightTheme)
{
    if (!hwnd_) return;
    static const HMODULE dwmModule = LoadLibraryW(L"dwmapi.dll");
    static const auto setDwmWindowAttribute = dwmModule
        ? reinterpret_cast<DwmSetWindowAttributeFn>(
            GetProcAddress(dwmModule, "DwmSetWindowAttribute"))
        : nullptr;
    static const auto extendDwmFrame = dwmModule
        ? reinterpret_cast<DwmExtendFrameIntoClientAreaFn>(
            GetProcAddress(dwmModule, "DwmExtendFrameIntoClientArea"))
        : nullptr;
    const BOOL darkMode = lightTheme ? FALSE : TRUE;
    const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_TRANSIENTWINDOW;
    if (setDwmWindowAttribute)
    {
        setDwmWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE,
            &darkMode, sizeof(darkMode));
        setDwmWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE,
            &corner, sizeof(corner));
        setDwmWindowAttribute(hwnd_, DWMWA_SYSTEMBACKDROP_TYPE,
            &backdrop, sizeof(backdrop));
    }
    const MARGINS margins{ -1, -1, -1, -1 };
    if (extendDwmFrame) extendDwmFrame(hwnd_, &margins);

    static const auto setWindowCompositionAttribute =
        reinterpret_cast<SetWindowCompositionAttributeFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                "SetWindowCompositionAttribute"));
    if (!setWindowCompositionAttribute) return;
    const COLORREF tint = menu_icon::ResolvePalette(lightTheme).background;
    const DWORD tintAlpha = lightTheme ? 0x58 : 0x60;
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
    if (!setWindowCompositionAttribute(hwnd_, &data))
    {
        accent.state = AccentState::BlurBehind;
        accent.gradientColor = 0;
        setWindowCompositionAttribute(hwnd_, &data);
    }
}

POINT Window::ResolvePosition(const RECT& menuBounds, UINT dpi) const
{
    const int overlap = Scale(modern_menu::kSubmenuOverlapDip, dpi);
    const int panelPadding = Scale(
        modern_menu::kSubmenuPanelPaddingDip, dpi);
    const POINT monitorPoint{
        (menuBounds.left + menuBounds.right) / 2,
        (menuBounds.top + menuBounds.bottom) / 2,
    };
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(MonitorFromPoint(monitorPoint,
            MONITOR_DEFAULTTONEAREST), &info))
    {
        info.rcWork = { 0, 0, GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN) };
    }
    int left = menuBounds.right - overlap;
    if (left + width_ > info.rcWork.right)
        left = menuBounds.left - width_ + overlap;
    left = std::clamp(left, static_cast<int>(info.rcWork.left),
        std::max(static_cast<int>(info.rcWork.left),
            static_cast<int>(info.rcWork.right) - width_));
    const int anchorTop = IsRectEmpty(&itemBounds_)
        ? menuBounds.top : itemBounds_.top - panelPadding;
    const int top = std::clamp(anchorTop,
        static_cast<int>(info.rcWork.top),
        std::max(static_cast<int>(info.rcWork.top),
            static_cast<int>(info.rcWork.bottom) - height_));
    return { left, top };
}

LRESULT CALLBACK Window::WindowProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

    switch (message)
    {
    case WM_MOUSEMOVE:
    {
        KillTimer(hwnd, kHideTimer);
        if (!self->pointerTracking_)
        {
            TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE,
                hwnd, 0 };
            TrackMouseEvent(&tracking);
            self->pointerTracking_ = true;
        }
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const bool componentHovered =
            PtInRect(&self->previewRect_, point) != FALSE;
        if (componentHovered != self->componentHovered_)
        {
            self->componentHovered_ = componentHovered;
            self->RenderCurrent();
        }
        const bool optionClickable = std::any_of(
            self->optionHits_.begin(), self->optionHits_.end(),
            [&](const OptionHit& hit) {
                return PtInRect(&hit.bounds, point) != FALSE;
            });
        const bool clickable = PtInRect(&self->previousButton_, point) ||
            PtInRect(&self->nextButton_, point) ||
            PtInRect(&self->closeRect_, point) ||
            PtInRect(&self->applyRect_, point) || optionClickable;
        SetCursor(LoadCursorW(nullptr, clickable ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        self->pointerTracking_ = false;
        if (self->componentHovered_)
        {
            self->componentHovered_ = false;
            self->RenderCurrent();
        }
        self->ScheduleHide();
        return 0;
    case WM_LBUTTONUP:
    {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (PtInRect(&self->closeRect_, point))
            self->Hide();
        else if (PtInRect(&self->previousButton_, point))
            self->SelectRelative(-1);
        else if (PtInRect(&self->nextButton_, point))
            self->SelectRelative(1);
        else
        {
            for (const auto& hit : self->optionHits_)
            {
                if (PtInRect(&hit.bounds, point))
                {
                    self->SetOption(hit.setting, hit.value);
                    return 0;
                }
            }
            if (PtInRect(&self->pagerRect_, point))
                return 0;
            if (PtInRect(&self->applyRect_, point))
                self->ApplyCurrent();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_LEFT) self->SelectRelative(-1);
        else if (wParam == VK_RIGHT) self->SelectRelative(1);
        else if (wParam == VK_RETURN || wParam == VK_SPACE)
            self->ApplyCurrent();
        return 0;
    case WM_TIMER:
        if (wParam == kOpenTimer)
        {
            KillTimer(hwnd, kOpenTimer);
            if (!self->pendingModel_.Empty())
            {
                Model model = std::move(self->pendingModel_);
                ApplyHandler handler = std::move(self->pendingOnApply_);
                self->Show(model, self->pendingMenuBounds_,
                    self->pendingOwner_, self->pendingDpi_,
                    self->pendingLightTheme_, std::move(handler),
                    self->pendingItemBounds_);
            }
        }
        else if (wParam == kHideTimer)
        {
            KillTimer(hwnd, kHideTimer);
            // A stale close notification from the previously hovered row
            // must not dismiss a sibling preview that is waiting to open.
            if (!self->pendingModel_.Empty())
                return 0;
            if (!self->PointerInsideMenuOrPreview())
                self->Hide();
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace snowdesktop::component_preview
