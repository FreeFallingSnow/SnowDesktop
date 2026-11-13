#include "ui/input/private_pointer.h"
#include "ui/render/app_font.h"
#include "app/app.h"
#include "widget/runtime/widget_engine.h"
#include "platform/usage_guide.h"
#include "common/l10n.h"
#include "ui/render/native_tooltip_preferences.h"
#include "settings/animation_settings.h"
#include "common/utils.h"
#include "app/dock/persistent_dock_host.h"
#include "foreground_paint_region.h"
#include "ui/menu/modern_menu.h"
#include "dock/floating_dock_rules.h"

// Transient page, privacy and widget-positioning overlays.

void DesktopApp::DrawUsageGuideHintOverlay(ID2D1DeviceContext* ctx, const RECT* updateRect)
{
    using namespace snowdesktop::usage_guide;
    usageGuidePauseRect_ = usageGuideSettingsRect_ = usageGuideOpenSettingsRect_ = {};
    usageGuideFrame_ = usageGuideBodyRect_ = usageGuideScrollTrack_ = usageGuideScrollThumb_ = {};
    if (!ctx || !IsUsageGuideVisible()) return;
    const auto& lesson = *Find(*usageGuideTopic_);
    const bool showSettings = HasSettings(lesson);
    const std::wstring title = _LW(lesson.title);
    const std::wstring hint = _LW(lesson.instructions);
    const std::wstring pauseText = _LW("start.pause"), settingsText = _LW("start.returnSettings"),
        openText = _LW("start.openSettings");
    POINT cursor{}; snowdesktop::private_pointer::CursorPosition(&cursor);
    // Pick a display once; following the mouse or menus during repaint would
    // move a button out from under the user. Only a deliberate drag follows it.
    const POINT monitorPoint = usageGuidePlacement_.dragOffset ? cursor :
        usageGuidePlacement_.anchor.value_or(cursor);
    const auto* page = GridPageFromScreenPoint(monitorPoint);
    if (!page) page = GetFirstPageGridPage();
    if (!page) return;
    auto area = page->workArea;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&area), 2);
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromPoint(monitorPoint, MONITOR_DEFAULTTONEAREST), &monitor))
        area = monitor.rcWork;
    const float scale = std::max(1.0f, page->dpiX / 96.0f);
    const auto px = [scale](float value) { return static_cast<int>(std::ceil(value * scale)); };
    const int padding = px(16), margin = px(24);
    const int width = std::min(px(440), static_cast<int>(area.right - area.left) - margin * 2);
    if (width < px(240)) return;
    const int textWidth = width - padding * 2;
    const int bodyWidth = textWidth - px(16);
    auto* factory = GetDWriteFactory();
    if (!factory) return;
    DWORD textScale = 100, bytes = sizeof(textScale);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Accessibility", L"TextScaleFactor",
        RRF_RT_REG_DWORD, nullptr, &textScale, &bytes);
    const float fontScale = scale * std::clamp(textScale / 100.0f, 1.0f, 2.25f);
    ComPtr<IDWriteTextFormat> format, titleFormat, smallFormat;
    const auto makeFormat = [&](float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& value) {
        if (FAILED(snowdesktop::app_fonts::CreateTextFormat(factory, L"Segoe UI", weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size * fontScale, L"", &value))) return false;
        value->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); return true;
    };
    if (!makeFormat(14, DWRITE_FONT_WEIGHT_NORMAL, format) ||
        !makeFormat(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat) ||
        !makeFormat(12, DWRITE_FONT_WEIGHT_NORMAL, smallFormat)) return;
    const auto measure = [&](const std::wstring& text, IDWriteTextFormat* style, int availableWidth) {
        ComPtr<IDWriteTextLayout> layout; DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
            style, static_cast<float>(availableWidth), 100000, &layout))) layout->GetMetrics(&metrics);
        return static_cast<int>(std::ceil(metrics.height));
    };
    const int titleHeight = measure(title, titleFormat.Get(), textWidth);
    const int openHeight = showSettings ? std::max(px(36), measure(openText, format.Get(), textWidth - px(16)) + px(12)) : 0;
    const int halfWidth = (textWidth - px(8)) / 2;
    const int secondaryHeight = std::max(px(36), std::max(
        measure(settingsText, smallFormat.Get(), halfWidth - px(12)),
        measure(pauseText, smallFormat.Get(), halfWidth - px(12))) + px(12));
    const int fixedHeight = padding * 2 + titleHeight + px(12) +
        px(12) + secondaryHeight + (showSettings ? openHeight + px(8) : 0);
    const int bodyLimit = static_cast<int>(area.bottom - area.top) - margin * 2 - fixedHeight;
    if (bodyLimit < px(24)) return;
    const int contentHeight = measure(hint, format.Get(), bodyWidth);
    const int textHeight = std::min(bodyLimit, contentHeight);
    usageGuideScroll_.Arrange(contentHeight, textHeight);
    const int height = fixedHeight + textHeight;
    if (height + margin * 2 > area.bottom - area.top) return;
    RECT frame = usageGuidePlacement_.Arrange(area, width, height, margin);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&frame), 2);
    usageGuideFrame_ = frame;
    // Arrangement and hit rectangles are semantic state even when this
    // retained panel is outside the current foreground pixel update.
    const bool drawPixels = snowdesktop::foreground_paint_region::Intersects(updateRect, frame);
    POINT clientCursor = cursor;
    ScreenToClient(hwnd_, &clientCursor);
    // This floating panel stays in the desktop foreground. Context menus use
    // their own popup windows above the desktop, without moving this panel.
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const auto systemColor = [](int index) {
        const auto color = GetSysColor(index);
        return D2D1::ColorF(GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f);
    };
    const bool light = IsLightContentTheme();
    const auto background = highContrast ? systemColor(COLOR_WINDOW) : D2D1::ColorF(light ? 0xf9f9f9 : 0x292929, 0.98f);
    const auto foreground = highContrast ? systemColor(COLOR_WINDOWTEXT) : D2D1::ColorF(light ? 0x202020 : 0xf5f5f5);
    const auto secondary = highContrast ? foreground : D2D1::ColorF(light ? 0x606060 : 0xc5c5c5);
    const auto border = highContrast ? foreground : D2D1::ColorF(light ? 0xd6d6d6 : 0x505050);
    if (drawPixels) DrawD2DRoundedRectangle(ctx, frame, 8.0f * scale, background, border, 1);
    int y = frame.top + padding;
    const auto drawLine = [&](const std::wstring& value, int h, IDWriteTextFormat* style, D2D1_COLOR_F color) {
        RECT bounds{frame.left + padding, y, frame.right - padding, y + h};
        if (drawPixels) DrawD2DText(ctx, value, bounds, style, color);
        y += h;
    };
    drawLine(title, titleHeight, titleFormat.Get(), foreground); y += px(12);
    usageGuideBodyRect_ = {frame.left + padding, y, frame.right - padding - px(16), y + textHeight};
    RECT textBounds = usageGuideBodyRect_;
    textBounds.top -= usageGuideScroll_.offset;
    textBounds.bottom = textBounds.top + contentHeight;
    if (drawPixels)
    {
        ctx->PushAxisAlignedClip(ToD2DRect(usageGuideBodyRect_), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        DrawD2DText(ctx, hint, textBounds, format.Get(), foreground);
        ctx->PopAxisAlignedClip();
    }
    if (usageGuideScroll_.maximum > 0)
    {
        usageGuideScrollTrack_ = {frame.right - padding - px(12), y, frame.right - padding, y + textHeight};
        const int thumbHeight = std::min(textHeight, std::max(px(32), MulDiv(textHeight, textHeight, contentHeight)));
        const int thumbTop = y + MulDiv(textHeight - thumbHeight, usageGuideScroll_.offset, usageGuideScroll_.maximum);
        usageGuideScrollThumb_ = {usageGuideScrollTrack_.left, thumbTop, usageGuideScrollTrack_.right, thumbTop + thumbHeight};
        RECT visibleThumb = usageGuideScrollThumb_; InflateRect(&visibleThumb, -px(4), 0);
        if (drawPixels) DrawD2DRoundedRectangle(ctx, visibleThumb, 2.0f * scale, secondary, secondary, 0);
    }
    y += textHeight + px(12);
    const auto drawButton = [&](RECT& bounds, const std::wstring& label, IDWriteTextFormat* style) {
        if (!drawPixels) return;
        const bool hover = PtInRect(&bounds, clientCursor) != FALSE;
        const auto fill = highContrast ? background :
            D2D1::ColorF(light ? (hover ? 0xe8e8e8 : 0xf9f9f9) : (hover ? 0x414141 : 0x292929));
        DrawD2DRoundedRectangle(ctx, bounds, 4.0f * scale, fill, border, 1);
        style->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        style->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        RECT labelBounds = bounds; InflateRect(&labelBounds, -px(6), 0);
        DrawD2DText(ctx, label, labelBounds, style, foreground);
    };
    if (showSettings)
    {
        usageGuideOpenSettingsRect_ = {frame.left + padding, y, frame.right - padding, y + openHeight};
        drawButton(usageGuideOpenSettingsRect_, openText, format.Get()); y += openHeight + px(8);
    }
    usageGuideSettingsRect_ = {frame.left + padding, y, frame.left + padding + halfWidth, y + secondaryHeight};
    usageGuidePauseRect_ = {usageGuideSettingsRect_.right + px(8), y, frame.right - padding, y + secondaryHeight};
    drawButton(usageGuideSettingsRect_, settingsText, smallFormat.Get());
    drawButton(usageGuidePauseRect_, pauseText, smallFormat.Get());
}

void DesktopApp::ResetPageNotify()
{
    if (pageNotifyFadeOutToken_) uiAnimationScheduler_.Cancel(pageNotifyFadeOutToken_);
    if (pageNotifyAnimationFrameToken_) uiAnimationScheduler_.Cancel(pageNotifyAnimationFrameToken_);
    pageNotifyFadeOutToken_ = pageNotifyAnimationFrameToken_ = 0;
    pageNotifyActive_ = false;
    pageNotifyTooltip_.Hide();
}

void DesktopApp::ShowPageNotify(const std::wstring& text)
{
    ResetPageNotify();
    const auto order = BuildMonitorRenderOrder();
    if (text.empty() || order.empty() || !hwnd_ || desktopIconsHidden_ ||
        !dcompDevice_ || !dwriteFactory_ || HasActiveContextMenuSession()) return;

    const RECT work = gridPages_[order.back()].workArea;
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(snowdesktop::app_fonts::CreateTextFormat(dwriteFactory_.Get(), L"Segoe UI",
            DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 42.0f, L"", &format))) return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    snowdesktop::NativeTooltipLayoutOptions options;
    options.centered = true;
    options.paddingXRatio = 28.f / 42.f;
    options.paddingYRatio = 14.f / 42.f;
    snowdesktop::NativeTooltipTextLayout measured;
    if (FAILED(snowdesktop::MeasureNativeTooltip(dwriteFactory_.Get(), format.Get(), {}, text,
            std::clamp(static_cast<float>(work.right - work.left) - 48.f, 1.f, 2056.f),
            std::max(1.f, static_cast<float>(work.bottom - work.top) - 48.f), measured, options))) return;
    RECT bounds{work.left + 24, work.top + 24,
        work.left + 24 + static_cast<LONG>(std::ceil(measured.width)),
        work.top + 24 + static_cast<LONG>(std::ceil(measured.height))};
    OffsetRect(&bounds, virtualLeft_, virtualTop_);
    ConfigureFixedTooltip(pageNotifyTooltip_, hwnd_);

    const bool animate = snowdesktop::animation::RuntimeAnimationsEnabled();
    const DWORD fade = static_cast<DWORD>(std::max(1.0,
        std::round(kPageNotifyFadeMs * snowdesktop::animation::RuntimeDurationScale())));
    pageNotifyTooltip_.SetOpacity(animate ? 0.f : 1.f);
    pageNotifyTooltip_.ShowAt(text, bounds, measured, true);
    if (!pageNotifyTooltip_.Visible()) return;
    pageNotifyActive_ = true;
    if (animate)
    {
        const auto startFade = [this, fade](bool opening) {
            const ULONGLONG start = GetTickCount64();
            pageNotifyAnimationFrameToken_ = uiAnimationScheduler_.StartAnimation(
                snowdesktop::UiAnimationSurface::Desktop, [this, fade, opening, start](double) {
                    const float progress = std::min(1.f, static_cast<float>(GetTickCount64() - start) / fade);
                    const bool keep = pageNotifyTooltip_.SetOpacity(opening ? progress : 1.f - progress) &&
                        progress < 1.f;
                    if (!keep) pageNotifyAnimationFrameToken_ = 0;
                    if (!keep && !opening) ResetPageNotify();
                    return keep;
                });
            if (!pageNotifyAnimationFrameToken_)
            {
                if (opening) pageNotifyTooltip_.SetOpacity(1.f);
                else ResetPageNotify();
            }
        };
        startFade(true);
        pageNotifyFadeOutToken_ = uiAnimationScheduler_.ScheduleOnce(
            fade + kPageNotifyVisibleMs - 2 * kPageNotifyFadeMs,
            [this, startFade](snowdesktop::UiScheduleToken token) {
                if (pageNotifyFadeOutToken_ != token) return;
                pageNotifyFadeOutToken_ = 0;
                if (pageNotifyAnimationFrameToken_) uiAnimationScheduler_.Cancel(pageNotifyAnimationFrameToken_);
                startFade(false);
            });
    }
    else
    {
        pageNotifyFadeOutToken_ = uiAnimationScheduler_.ScheduleOnce(
            kPageNotifyVisibleMs, [this](snowdesktop::UiScheduleToken token) {
                if (pageNotifyFadeOutToken_ == token) ResetPageNotify();
            });
    }
    if (!pageNotifyFadeOutToken_) ResetPageNotify();
}

void DesktopApp::ConfigureFixedTooltip(snowdesktop::NativeTooltip& tooltip, HWND owner)
{
    tooltip.Configure(owner, dcompDevice_.Get(), dwriteFactory_.Get(),
        snowdesktop::NativeTooltipAppearance(),
        [this](ID2D1DeviceContext* context, RECT bounds, const PersonalizationSettings& appearance, float scale) {
            DrawWidgetPanelBackground(context, bounds, appearance.cornerRadius * scale,
                D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB,
                    appearance.widgetBorderAlpha),
                false, appearance.widgetBorderWidth * scale, &appearance, false, 0, scale);
        });
}

void DesktopApp::SyncDockTitleTooltip()
{
    POINT screen{};
    if (!hwnd_ || !dcompDevice_ || !dwriteFactory_ || HasActiveContextMenuSession() ||
        !GetCursorPos(&screen))
    {
        dockTitleTooltip_.Hide();
        return;
    }
    const HWND hit = snowdesktop::private_pointer::WindowAtPoint(screen);
    POINT point = screen;
    ScreenToClient(hwnd_, &point);
    for (const auto& host : persistentDockHosts_)
    {
        if (host && host->active && host->container && host->hwnd && IsWindowVisible(host->hwnd) &&
            (hit == host->hwnd || host->backdrop.IsBackdropWindow(hit)))
        {
            host->container->ShowHoveredTitle(point, host->hwnd, dockTitleTooltip_);
            return;
        }
    }
    if ((!desktopIconsHidden_ || dockSettings_.keepWhenDesktopHidden) && IsBaseDesktopHoverSurfaceWindow(hit))
    {
        if (auto* dock = GetDockContainerAtPoint(point); dock && !IsDockHostedByPersistentHost(dock))
        {
            dock->ShowHoveredTitle(point, hwnd_, dockTitleTooltip_);
            return;
        }
    }
    dockTitleTooltip_.Hide();
}

void DesktopApp::SyncLuaWidgetTooltip()
{
    POINT screen{};
    if (!widgetEngine_ || !hwnd_ || !dcompDevice_ || !dwriteFactory_ || desktopIconsHidden_ ||
        HasActiveContextMenuSession() || dragSession_.IsActive() ||
        dragDropController_.IsTransportActive() || widgetAction_ != WidgetAction::None ||
        !GetCursorPos(&screen))
    {
        luaWidgetTooltip_.Hide();
        return;
    }

    const HWND hit = snowdesktop::private_pointer::WindowAtPoint(screen);
    POINT point = screen;
    ScreenToClient(hwnd_, &point);
    HWND owner = hwnd_;
    std::wstring widgetId;
    std::string_view surface = "desktop";
    RECT frame{};
    if (!luaWidgetPanelRequest_.widgetId.empty() && luaWidgetPanelAnimation_.IsInteractive() &&
        floatingPopupHwnd_ && (hit == floatingPopupHwnd_ || IsChild(floatingPopupHwnd_, hit)))
    {
        owner = floatingPopupHwnd_;
        frame = GetLuaWidgetPanelContentRect();
        widgetId = luaWidgetPanelRequest_.widgetId;
        surface = luaWidgetPanelRequest_.surface;
    }
    else if (IsBaseDesktopHoverSurfaceWindow(hit) && !IsPointOccludedByOpenPopup(point))
    {
        const size_t index = HitTestStandaloneWidgetIndex(point);
        if (index < widgets_.size() && IsLuaDesktopSurface(widgets_[index]) &&
            HitTestStandaloneWidget(index, point) == WidgetHit::Content)
        {
            if (const auto* widget = ResolveLuaWidgetContent(&widgets_[index]))
            {
                widgetId = widget->id;
                frame = GetStandaloneWidgetFrameRect(widgets_[index]);
            }
        }
    }

    snowdesktop::NativeTooltipTextLayout measured;
    D2D1_RECT_F hint{};
    std::wstring name;
    if (widgetId.empty() || !PtInRect(&frame, point) ||
        !widgetEngine_->MeasureHoveredInteractionTooltip(widgetId, surface, frame, measured, hint, name))
    {
        luaWidgetTooltip_.Hide();
        return;
    }

    RECT bounds{static_cast<LONG>(std::floor(hint.left)), static_cast<LONG>(std::floor(hint.top)),
        static_cast<LONG>(std::ceil(hint.right)), static_cast<LONG>(std::ceil(hint.bottom))};
    IntersectRect(&bounds, &bounds, &frame);
    OffsetRect(&bounds, virtualLeft_, virtualTop_);
    ConfigureFixedTooltip(luaWidgetTooltip_, owner);
    luaWidgetTooltip_.ShowAt(std::move(name), bounds, measured);
}

void DesktopApp::ClearInlineTooltipBackdrop(std::uintptr_t ownerKey)
{
    (void)desktopBackdropCompositor_.RemovePanel({}, ownerKey);
    for (const auto& host : persistentDockHosts_)
        if (host) (void)host->backdrop.RemovePanel({}, ownerKey);
}

bool DesktopApp::DrawInlineTooltip(ID2D1DeviceContext* ctx, RECT bounds,
    const snowdesktop::NativeTooltipTextLayout& measured, float scale,
    snowdesktop::InlineTooltipBackdrop& backdrop, const RECT* updateRect)
{
    if (!ctx || !measured.layout || IsRectEmptyRect(bounds)) return false;
    if (!desktopWidgetCompositionDrawInProgress_ && !renderingFloatingPopup_ && !renderingFloatingDock_)
        desktopForegroundTooltipDamage_.Record(bounds);
    const auto appearance = snowdesktop::NativeTooltipAppearance();
    if (snowdesktop::NativeTooltipHighContrast())
    {
        if (snowdesktop::foreground_paint_region::Intersects(updateRect, bounds))
            snowdesktop::DrawNativeTooltip(ctx, ToD2DRect(bounds), measured, appearance, scale);
        return false;
    }
    const float radius = snowdesktop::NativeTooltipCornerRadius(appearance,
        static_cast<float>(bounds.right - bounds.left), static_cast<float>(bounds.bottom - bounds.top), scale);
    bool registered = false;
    // Cached widget surfaces and independent popups inherit their enclosing
    // material. Their local tooltip must not register glass on the desktop.
    if (appearance.glassEnabled && !desktopWidgetCompositionDrawInProgress_ && !renderingFloatingPopup_)
    {
        auto* compositor = &desktopBackdropCompositor_;
        RECT backdropBounds = bounds;
        if (renderingFloatingDock_)
        {
            if (!renderingPersistentDockHost_) return false;
            compositor = &renderingPersistentDockHost_->backdrop;
            backdropBounds = snowdesktop::floating_dock_rules::DesktopRectToWindowRect(
                bounds, renderingPersistentDockHost_->sourceRect);
        }
        registered = backdrop.Update(*compositor, backdropBounds, radius,
            appearance.glassBlurRadius, [this](std::uintptr_t key) { ClearInlineTooltipBackdrop(key); });
    }
    // Record content and reconcile its owner before skipping pixel work.
    // The caller repairs newly discovered tooltip pixels in this transaction.
    if (!snowdesktop::foreground_paint_region::Intersects(updateRect, bounds))
        return registered;
    DrawWidgetPanelBackground(ctx, bounds, radius,
        D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
        D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
        false, appearance.widgetBorderWidth * scale, &appearance, false, 0, scale);
    snowdesktop::DrawNativeTooltipText(ctx, ToD2DRect(bounds), measured, appearance);
    return registered;
}

bool DesktopApp::DrawDesktopHintOverlay(ID2D1DeviceContext* ctx, const wchar_t* message,
    snowdesktop::InlineTooltipBackdrop& backdrop, const RECT* updateRect)
{
    if (!ctx || !dwriteFactory_ || !message || !*message) return false;
    POINT cursor{};
    const GridPage* page = snowdesktop::private_pointer::CursorPosition(&cursor) ? GridPageFromScreenPoint(cursor) : nullptr;
    if (!page) page = GetFirstPageGridPage();
    const RECT work = page ? page->workArea : RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    const float scale = page ? static_cast<float>(page->dpiX) / 96.f : 1.f;
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(snowdesktop::CreateNativeTooltipTextFormat(dwriteFactory_.Get(), &format, scale))) return false;
    snowdesktop::NativeTooltipTextLayout measured;
    snowdesktop::NativeTooltipLayoutOptions options;
    options.scale = scale; options.centered = true;
    if (FAILED(snowdesktop::MeasureNativeTooltip(dwriteFactory_.Get(), format.Get(), {}, message,
        std::max(1.f, std::min(720.f * scale, static_cast<float>(work.right - work.left) - 8.f * scale)),
        std::max(1.f, std::min(192.f * scale, static_cast<float>(work.bottom - work.top) - 8.f * scale)),
        measured, options))) return false;
    const LONG width = static_cast<LONG>(std::ceil(measured.width));
    const LONG height = static_cast<LONG>(std::ceil(measured.height));
    const LONG x = work.left + (work.right - work.left - width) / 2;
    const LONG y = std::min(work.top + static_cast<LONG>(60.f * scale), work.bottom - height);
    return DrawInlineTooltip(ctx, {x, y, x + width, y + height}, measured, scale, backdrop, updateRect);
}

void DesktopApp::DrawHiddenHintOverlay(ID2D1DeviceContext* ctx, const RECT* updateRect)
{
    auto tooltipPaint = hiddenHintBackdrop_.BeginPaint([this](std::uintptr_t key) { ClearInlineTooltipBackdrop(key); });
    if (desktopIconsHidden_ && showHiddenHint_)
        tooltipPaint.Keep(DrawDesktopHintOverlay(ctx, _LW("app.overlay.hide_hint"), hiddenHintBackdrop_, updateRect));
}

void DesktopApp::DrawWidgetAddedHintOverlay(ID2D1DeviceContext* ctx, const RECT* updateRect)
{
    auto tooltipPaint = widgetAddedHintBackdrop_.BeginPaint([this](std::uintptr_t key) { ClearInlineTooltipBackdrop(key); });
    if (showWidgetAddedHint_ && !IsUsageGuideVisible())
        tooltipPaint.Keep(DrawDesktopHintOverlay(ctx, _LW("app.overlay.widget_move_hint"), widgetAddedHintBackdrop_, updateRect));
}
