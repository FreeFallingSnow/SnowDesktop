#include "component_preview.h"

#include "menu_icon_render.h"
#include "modern_menu.h"

#include <d2d1helper.h>
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

using Microsoft::WRL::ComPtr;

constexpr wchar_t kPreviewWindowClass[] =
    L"SnowDesktop.ComponentPreviewPopup";
constexpr UINT_PTR kOpenTimer = 1;
constexpr UINT_PTR kHideTimer = 2;

int Scale(int value, UINT dpi)
{
    return std::max(1, MulDiv(value, static_cast<int>(dpi),
        USER_DEFAULT_SCREEN_DPI));
}

D2D1_RECT_F ToRectF(const RECT& rect)
{
    return D2D1::RectF(static_cast<float>(rect.left),
        static_cast<float>(rect.top),
        static_cast<float>(rect.right),
        static_cast<float>(rect.bottom));
}

void SetBrush(ID2D1SolidColorBrush* brush, COLORREF color,
    float alpha = 1.0f)
{
    brush->SetColor(D2D1::ColorF(
        GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f,
        alpha));
}

void FillRectD2D(ID2D1RenderTarget* dc, ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& rect, COLORREF color, float alpha = 1.0f)
{
    SetBrush(brush, color, alpha);
    dc->FillRectangle(rect, brush);
}

void RoundedBoxD2D(ID2D1RenderTarget* dc, ID2D1SolidColorBrush* brush,
    const RECT& rect, int radius, COLORREF fill, COLORREF border,
    float alpha = 1.0f)
{
    const D2D1_ROUNDED_RECT rounded{
        ToRectF(rect), static_cast<float>(radius),
        static_cast<float>(radius),
    };
    SetBrush(brush, fill, alpha);
    dc->FillRoundedRectangle(rounded, brush);
    SetBrush(brush, border, alpha);
    dc->DrawRoundedRectangle(rounded, brush, 1.0f);
}

void RoundedOutlineD2D(ID2D1RenderTarget* dc,
    ID2D1SolidColorBrush* brush, const RECT& rect, int radius,
    COLORREF border, float alpha = 1.0f)
{
    const D2D1_ROUNDED_RECT rounded{
        ToRectF(rect), static_cast<float>(radius),
        static_cast<float>(radius),
    };
    SetBrush(brush, border, alpha);
    dc->DrawRoundedRectangle(rounded, brush, 1.0f);
}

/** @brief 浣跨敤 DWrite 娴嬮噺涓€娈垫枃鏈湪缁欏畾鏍煎紡涓嬬殑楂樺害锛圖IP锛夈€?*/
float MeasureTextHeightD2D(IDWriteFactory* factory,
    IDWriteTextFormat* format, const std::wstring& text, float width,
    bool wrapping)
{
    if (!factory || !format || text.empty() || width <= 0)
        return 0.0f;
    format->SetWordWrapping(wrapping
        ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(),
            static_cast<UINT32>(text.size()), format, width, 10000.0f,
            &layout)))
    {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(layout->GetMetrics(&metrics))
        ? metrics.height : 0.0f;
}

/** @brief 灏嗘枃鏈埅鏂埌 maxWidth 鍐呭苟闄勫姞鐪佺暐鍙枫€?*/
std::wstring TruncateWithEllipsis(IDWriteFactory* factory,
    IDWriteTextFormat* format, const wchar_t* text, size_t length,
    float maxWidth)
{
    const std::wstring_view view(text, length);
    const auto measure = [&](const std::wstring& sample) {
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(factory->CreateTextLayout(sample.data(),
                static_cast<UINT32>(sample.size()), format,
                100000.0f, 100.0f, &layout)))
        {
            return 0.0f;
        }
        DWRITE_TEXT_METRICS metrics{};
        return SUCCEEDED(layout->GetMetrics(&metrics))
            ? metrics.width : 0.0f;
    };
    if (measure(std::wstring(view)) <= maxWidth)
        return std::wstring(view);
    constexpr wchar_t kEllipsis[] = L"\u2026";
    size_t low = 0;
    size_t high = length;
    while (low < high)
    {
        const size_t mid = (low + high + 1) / 2;
        if (measure(std::wstring(view.substr(0, mid)) + kEllipsis) <=
            maxWidth)
        {
            low = mid;
        }
        else
        {
            high = mid - 1;
        }
    }
    return std::wstring(view.substr(0, low)) + kEllipsis;
}

/** @brief 浠?DWrite 缁樺埗鏂囨湰锛涙敮鎸佹崲琛屼笌鐪佺暐鍙枫€?*/
void DrawTextD2D(ID2D1RenderTarget* dc, ID2D1SolidColorBrush* brush,
    IDWriteFactory* factory, IDWriteTextFormat* format,
    const wchar_t* text, size_t length, const D2D1_RECT_F& rect,
    COLORREF color, DWRITE_TEXT_ALIGNMENT align =
        DWRITE_TEXT_ALIGNMENT_LEADING,
    bool wrapping = false, bool ellipsis = false,
    float alpha = 1.0f)
{
    if (!text || length == 0 || !factory || !format)
        return;
    format->SetTextAlignment(align);
    format->SetWordWrapping(wrapping
        ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    const float width = rect.right - rect.left;
    const float height = rect.bottom - rect.top;
    if (width <= 0.0f || height <= 0.0f)
        return;

    std::wstring truncated;
    const wchar_t* drawText = text;
    size_t drawLength = length;
    if (ellipsis)
    {
        truncated = TruncateWithEllipsis(factory, format, text, length,
            width);
        drawText = truncated.c_str();
        drawLength = truncated.size();
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(drawText,
            static_cast<UINT32>(drawLength), format, width, height,
            &layout)))
    {
        return;
    }
    SetBrush(brush, color, alpha);
    dc->DrawTextLayout(D2D1::Point2F(rect.left, rect.top),
        layout.Get(), brush);
}

/** @brief 灞呬腑缁樺埗鍥炬爣瀛楀舰骞惰繑鍥炲叾澧ㄦ按杈圭晫銆?*/
void DrawCenteredGlyphD2D(ID2D1RenderTarget* dc,
    ID2D1SolidColorBrush* brush, IDWriteFactory* factory,
    IDWriteTextFormat* format, wchar_t glyph, const RECT& rect,
    RECT& inkBounds, COLORREF color, float alpha = 1.0f)
{
    inkBounds = {};
    if (!factory || !format)
        return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(&glyph, 1, format,
            100000.0f, 100.0f, &layout)))
    {
        return;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)) ||
        metrics.width <= 0.0f || metrics.height <= 0.0f)
    {
        return;
    }
    const float centerX = (rect.left + rect.right) / 2.0f;
    const float centerY = (rect.top + rect.bottom) / 2.0f;
    const D2D1_RECT_F drawRect{
        centerX - metrics.width / 2.0f,
        centerY - metrics.height / 2.0f,
        centerX + metrics.width / 2.0f,
        centerY + metrics.height / 2.0f,
    };
    inkBounds = {
        static_cast<LONG>(drawRect.left),
        static_cast<LONG>(drawRect.top),
        static_cast<LONG>(drawRect.right),
        static_cast<LONG>(drawRect.bottom),
    };
    SetBrush(brush, color, alpha);
    dc->DrawTextLayout(D2D1::Point2F(drawRect.left, drawRect.top),
        layout.Get(), brush);
}

void DrawBitmapD2D(ID2D1RenderTarget* dc, const Bitmap& image,
    const RECT& bounds)
{
    if (!dc || image.width <= 0 || image.height <= 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) *
            image.height)
    {
        return;
    }
    D2D1_BITMAP_PROPERTIES properties{};
    properties.pixelFormat = D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap> bitmap;
    if (FAILED(dc->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(image.width),
                static_cast<UINT32>(image.height)),
            image.pixels.data(), image.width * 4, &properties, &bitmap)))
    {
        return;
    }
    const D2D1_RECT_F sourceRect{
        0.0f, 0.0f,
        static_cast<float>(image.width),
        static_cast<float>(image.height),
    };
    const D2D1_RECT_F destRect = ToRectF(bounds);
    dc->DrawBitmap(bitmap.Get(), destRect, 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, sourceRect);
}

bool HasVisibleText(const std::wstring& text)
{
    return std::any_of(text.begin(), text.end(), [](wchar_t character) {
        return !std::iswspace(character);
    });
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
    if (!hwnd_)
        return false;
    if (!InitializeGraphics())
        return false;
    return true;
}

bool Window::InitializeGraphics()
{
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, nullptr);
    if (FAILED(hr))
    {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &d3dDevice_, &featureLevel, nullptr);
    }
    if (FAILED(hr))
        return false;

    D2D1_FACTORY_OPTIONS factoryOptions{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &factoryOptions,
        reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
    if (FAILED(hr))
        return false;

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice_.As(&dxgiDevice);
    if (FAILED(hr))
        return false;
    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (FAILED(hr))
        return false;
    hr = d2dDevice_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_);
    if (FAILED(hr))
        return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
    if (FAILED(hr))
        return false;

    D2D1_RENDER_TARGET_PROPERTIES targetProperties{};
    targetProperties.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    targetProperties.pixelFormat = D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    targetProperties.dpiX = USER_DEFAULT_SCREEN_DPI;
    targetProperties.dpiY = USER_DEFAULT_SCREEN_DPI;
    hr = d2dFactory_->CreateDCRenderTarget(&targetProperties,
        &dcrTarget_);
    if (FAILED(hr))
        return false;

    CreateFormats();
    return titleFormat_ && cardTitleFormat_ && bodyFormat_ &&
        glyphFormat_;
}

void Window::CreateFormats()
{
    const auto make = [&](const wchar_t* family, float size,
                          DWRITE_FONT_WEIGHT weight) {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(dwriteFactory_->CreateTextFormat(family, nullptr,
                weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"", &format)))
        {
            return ComPtr<IDWriteTextFormat>{};
        }
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return format;
    };
    titleFormat_ = make(L"Segoe UI",
        static_cast<float>(Scale(17, dpi_)), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    cardTitleFormat_ = make(L"Segoe UI",
        static_cast<float>(Scale(13, dpi_)), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    bodyFormat_ = make(L"Segoe UI",
        static_cast<float>(Scale(11, dpi_)), DWRITE_FONT_WEIGHT_NORMAL);
    glyphFormat_ = make(L"Segoe UI Symbol",
        static_cast<float>(Scale(15, dpi_)), DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

bool Window::Show(const Model& model, const RECT& menuBounds,
    HWND owner, UINT dpi, bool lightTheme, ApplyHandler onApply,
    const RECT& itemBounds, modern_menu::Appearance appearance)
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
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    return true;
}

bool Window::ScheduleShow(const Model& model, const RECT& menuBounds,
    HWND owner, UINT dpi, bool lightTheme, ApplyHandler onApply,
    const RECT& itemBounds, modern_menu::Appearance appearance)
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

    if (!dwriteFactory_ || !bodyFormat_ || !titleFormat_ ||
        !cardTitleFormat_ || !glyphFormat_)
    {
        CreateFormats();
    }
    if (!dwriteFactory_ || !bodyFormat_ || !titleFormat_)
        return false;

    const int textWidth = width_ - padding * 2;
    const int introductionHeight = static_cast<int>(std::ceil(
        MeasureTextHeightD2D(dwriteFactory_.Get(), bodyFormat_.Get(),
            model_.introduction, static_cast<float>(textWidth), true)));
    const int resizeHintHeight = static_cast<int>(std::ceil(
        MeasureTextHeightD2D(dwriteFactory_.Get(), bodyFormat_.Get(),
            model_.resizeHint, static_cast<float>(textWidth), false)));
    const bool hasCardHeader = HasVisibleText(card.title) ||
        HasVisibleText(card.sizeLabel);
    const bool hasDescription = HasVisibleText(card.description);
    const bool hasApplyButton = HasVisibleText(model_.applyLabel);
    const int metadataHorizontalPadding = Scale(10, dpi_);
    const int metadataTopPadding = Scale(8, dpi_);
    const int metadataBottomPadding = Scale(8, dpi_);
    const int cardHeaderHeight = hasCardHeader ? Scale(21, dpi_) : 0;
    const int descriptionHeight = static_cast<int>(std::ceil(
        MeasureTextHeightD2D(dwriteFactory_.Get(), bodyFormat_.Get(),
            card.description,
            static_cast<float>(width_ - (padding + metadataHorizontalPadding) * 2),
            true)));
    const int applyButtonHeight = hasApplyButton ? Scale(32, dpi_) : 0;

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
        return false;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    std::fill_n(static_cast<std::uint32_t*>(rawPixels),
        static_cast<size_t>(width_) * height_, 0u);

    if (!dcrTarget_)
    {
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);
        DeleteObject(bitmap);
        return false;
    }
    const RECT targetRect{ 0, 0, width_, height_ };
    if (FAILED(dcrTarget_->BindDC(dc, &targetRect)))
    {
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);
        DeleteObject(bitmap);
        return false;
    }
    dcrTarget_->BeginDraw();
    dcrTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    ComPtr<ID2D1SolidColorBrush> frameBrush;
    if (FAILED(dcrTarget_->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &frameBrush)))
    {
        dcrTarget_->EndDraw();
        SelectObject(dc, oldBitmap);
        DeleteDC(dc);
        DeleteObject(bitmap);
        return false;
    }

    const auto palette = menu_icon::ResolvePalette(lightTheme_);
    const float materialAlpha = lightTheme_ ? 70.0f / 255.0f
                                            : 76.0f / 255.0f;
    const float contentAlpha = 246.0f / 255.0f;
    const RECT panel{ 0, 0, width_, height_ };
    const int radius = Scale(10, dpi_);
    SetBrush(frameBrush.Get(), palette.background, materialAlpha);
    dcrTarget_->FillRoundedRectangle(
        D2D1::RoundedRect(ToRectF(panel),
            static_cast<float>(radius), static_cast<float>(radius)),
        frameBrush.Get());

    const auto drawBody = [&](const std::wstring& text, const RECT& rect,
                              COLORREF color, bool wrapping = false,
                              bool ellipsis = false,
                              DWRITE_TEXT_ALIGNMENT align =
                                  DWRITE_TEXT_ALIGNMENT_LEADING) {
        DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(),
            dwriteFactory_.Get(), bodyFormat_.Get(), text.data(),
            text.size(), ToRectF(rect), color, align, wrapping, ellipsis,
            contentAlpha);
    };

    int cursorY = padding;
    RECT titleRect{ padding, cursorY, width_ - padding,
        cursorY + titleHeight };
    const int closeButtonSize = Scale(24, dpi_);
    closeRect_ = { titleRect.right - closeButtonSize, titleRect.top,
        titleRect.right, titleRect.top + closeButtonSize };
    titleRect.right = closeRect_.left - Scale(8, dpi_);
    DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(), dwriteFactory_.Get(),
        titleFormat_.Get(), model_.title.data(), model_.title.size(),
        ToRectF(titleRect), palette.text, DWRITE_TEXT_ALIGNMENT_LEADING,
        false, true, contentAlpha);
    DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(), dwriteFactory_.Get(),
        titleFormat_.Get(), L"\\u00D7", 1, ToRectF(closeRect_),
        palette.text, DWRITE_TEXT_ALIGNMENT_CENTER, false, false,
        contentAlpha);
    cursorY = titleRect.bottom;
    if (introductionHeight)
    {
        cursorY += Scale(4, dpi_);
        RECT introductionRect{ padding, cursorY, width_ - padding,
            cursorY + introductionHeight };
        drawBody(model_.introduction, introductionRect,
            palette.disabledText, true);
        cursorY = introductionRect.bottom;
    }
    if (resizeHintHeight)
    {
        cursorY += Scale(2, dpi_);
        RECT resizeHintRect{ padding, cursorY, width_ - padding,
            cursorY + resizeHintHeight };
        drawBody(model_.resizeHint, resizeHintRect, palette.disabledText,
            false, true);
        cursorY = resizeHintRect.bottom;
    }
    cursorY += gap;
    const int cardTop = cursorY;
    const int previewLeft = (width_ - previewWidth) / 2;
    previewRect_ = { previewLeft, cardTop + previewInset,
        previewLeft + previewWidth, cardTop + previewInset + previewHeight };
    RoundedOutlineD2D(dcrTarget_.Get(), frameBrush.Get(), previewRect_,
        Scale(6, dpi_), palette.separator, contentAlpha);

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
    DrawBitmapD2D(dcrTarget_.Get(), rendered, previewRect_);

    int controlsY = previewRect_.bottom;
    previousButton_ = {};
    nextButton_ = {};
    previousGlyphRect_ = {};
    nextGlyphRect_ = {};
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
        RoundedBoxD2D(dcrTarget_.Get(), frameBrush.Get(),
            previousButton_, Scale(6, dpi_), palette.hoverBackground,
            palette.separator, contentAlpha);
        RoundedBoxD2D(dcrTarget_.Get(), frameBrush.Get(),
            nextButton_, Scale(6, dpi_), palette.hoverBackground,
            palette.separator, contentAlpha);
        DrawCenteredGlyphD2D(dcrTarget_.Get(), frameBrush.Get(),
            dwriteFactory_.Get(), glyphFormat_.Get(), L'\\u2039',
            previousButton_, previousGlyphRect_,
            currentCard_ > 0 ? palette.text : palette.disabledText,
            contentAlpha);
        const std::wstring statusText =
            std::to_wstring(currentCard_ + 1) + L" / " +
            std::to_wstring(model_.cards.size());
        DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(),
            dwriteFactory_.Get(), bodyFormat_.Get(), statusText.data(),
            statusText.size(), ToRectF(statusRect), palette.text,
            DWRITE_TEXT_ALIGNMENT_CENTER, false, false, contentAlpha);
        DrawCenteredGlyphD2D(dcrTarget_.Get(), frameBrush.Get(),
            dwriteFactory_.Get(), glyphFormat_.Get(), L'\\u203a',
            nextButton_, nextGlyphRect_,
            currentCard_ + 1 < model_.cards.size()
                ? palette.text : palette.disabledText,
            contentAlpha);
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
        drawBody(option.label, labelRect, palette.text, false, true);
        const int capsuleRadius = optionHeight / 2;
        RoundedBoxD2D(dcrTarget_.Get(), frameBrush.Get(), offRect,
            capsuleRadius,
            enabled ? palette.background : palette.hoverBackground,
            enabled ? palette.separator : palette.accent, contentAlpha);
        RoundedBoxD2D(dcrTarget_.Get(), frameBrush.Get(), onRect,
            capsuleRadius,
            enabled ? palette.hoverBackground : palette.background,
            enabled ? palette.accent : palette.separator, contentAlpha);
        DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(),
            dwriteFactory_.Get(), bodyFormat_.Get(), option.offLabel.data(),
            option.offLabel.size(), ToRectF(offRect),
            enabled ? palette.disabledText : palette.accent,
            DWRITE_TEXT_ALIGNMENT_CENTER, false, false, contentAlpha);
        DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(),
            dwriteFactory_.Get(), bodyFormat_.Get(), option.onLabel.data(),
            option.onLabel.size(), ToRectF(onRect),
            enabled ? palette.accent : palette.disabledText,
            DWRITE_TEXT_ALIGNMENT_CENTER, false, false, contentAlpha);
        optionHits_.push_back({ offRect, option.setting, false });
        optionHits_.push_back({ onRect, option.setting, true });
        controlsY = row.bottom;
    }

    const int metadataTop = previewRect_.bottom + controlsHeight;
    RECT cardRect{ padding, cardTop, width_ - padding,
        metadataTop + metadataHeight + previewInset };
    applyRect_ = {};
    RoundedOutlineD2D(dcrTarget_.Get(), frameBrush.Get(), cardRect,
        Scale(8, dpi_), palette.separator, contentAlpha);
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
            RoundedBoxD2D(dcrTarget_.Get(), frameBrush.Get(), badge,
                Scale(7, dpi_), palette.hoverBackground, palette.separator,
                contentAlpha);
            DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(),
                dwriteFactory_.Get(), bodyFormat_.Get(),
                card.sizeLabel.data(), card.sizeLabel.size(),
                ToRectF(badge), palette.disabledText,
                DWRITE_TEXT_ALIGNMENT_CENTER, false, false, contentAlpha);
            cardTitle.right = badge.left - Scale(6, dpi_);
        }
        if (HasVisibleText(card.title))
        {
            DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(),
                dwriteFactory_.Get(), cardTitleFormat_.Get(),
                card.title.data(), card.title.size(), ToRectF(cardTitle),
                palette.text, DWRITE_TEXT_ALIGNMENT_LEADING, false, true,
                contentAlpha);
        }
        metadataY += cardHeaderHeight;
    }
    if (hasDescription)
    {
        if (hasCardHeader) metadataY += Scale(2, dpi_);
        RECT descriptionRect{ metadataLeft, metadataY, metadataRight,
            metadataY + descriptionHeight };
        drawBody(card.description, descriptionRect, palette.disabledText,
            true);
        metadataY = descriptionRect.bottom;
    }
    if (hasApplyButton)
    {
        if (hasCardHeader || hasDescription) metadataY += Scale(6, dpi_);
        applyRect_ = { metadataLeft, metadataY, metadataRight,
            metadataY + applyButtonHeight };
        RoundedBoxD2D(dcrTarget_.Get(), frameBrush.Get(), applyRect_,
            Scale(8, dpi_), palette.accent, palette.accent, contentAlpha);
        DrawTextD2D(dcrTarget_.Get(), frameBrush.Get(),
            dwriteFactory_.Get(), cardTitleFormat_.Get(),
            model_.applyLabel.data(), model_.applyLabel.size(),
            ToRectF(applyRect_), RGB(255, 255, 255),
            DWRITE_TEXT_ALIGNMENT_CENTER, false, false, contentAlpha);
    }

    dcrTarget_->EndDraw();

    HRGN region = CreateRoundRectRgn(0, 0, width_ + 1, height_ + 1,
        radius * 2, radius * 2);
    if (region && !SetWindowRgn(hwnd_, region, FALSE))
        DeleteObject(region);

    POINT destination = ResolvePosition(menuBounds_, dpi_);
    SIZE size{ width_, height_ };
    POINT source{};
    BLENDFUNCTION blend{
        AC_SRC_OVER, 0, 255, AC_SRC_ALPHA,
    };
    const BOOL updated = UpdateLayeredWindow(
        hwnd_, nullptr, &destination,
        &size, dc, &source, 0,
        &blend, ULW_ALPHA);
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
