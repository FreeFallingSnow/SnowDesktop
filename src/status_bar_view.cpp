#include "status_bar_view.h"
#include "status_bar_glyphs.h"
#include "status_bar_battery.h"
#include "status_bar_layout.h"
#include "status_bar_presentation.h"
#include "tray_presentation.h"
#include "l10n.h"
#include "utils.h"
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cmath>
#include <utility>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace
{
std::wstring Percent(double value)
{
    return std::isfinite(value) && value >= 0 && value <= 100 ?
        std::to_wstring(static_cast<int>(std::lround(value))) + L"%" : L"—";
}
std::pair<std::wstring, std::wstring> NetworkVisual(const StatusBarSnapshot& snapshot)
{
    using namespace status_bar_glyphs;
    const auto& network = snapshot.network;
    if (!network || !network->available)
        return {kUnknown, _LW("statusBar.networkUnknown")};
    const bool connected = network->connectivity == "internet" || network->connectivity == "local";
    if (connected && network->transport == "ethernet")
        return {kEthernet, std::wstring(_LW("statusBar.ethernet")) + L"  " + _LW("controlCenter.connected")};
    const auto* interfaces = snapshot.wifi && snapshot.wifi->available ? snapshot.wifi->value.Find("interfaces") : nullptr;
    bool allRadiosOff = interfaces && interfaces->IsArray() && !interfaces->array.empty();
    std::optional<double> signal;
    unsigned connectedNetworks = 0;
    if (interfaces && interfaces->IsArray()) for (const auto& adapter : interfaces->array)
    {
        const auto* available = adapter.Find("available");
        const auto* enabled = adapter.Find("enabled");
        const auto* hardware = adapter.Find("hardwareEnabled");
        const bool known = available && available->IsBoolean() && available->boolean &&
            enabled && enabled->IsBoolean() && hardware && hardware->IsBoolean();
        allRadiosOff = allRadiosOff && known && (!enabled->boolean || !hardware->boolean);
        const auto* networks = adapter.Find("networks");
        if (!known || !enabled->boolean || !hardware->boolean ||
            !system_control::json::Flag(adapter, "connected") || !networks || !networks->IsArray()) continue;
        for (const auto& item : networks->array) if (system_control::json::Flag(item, "connected"))
        {
            ++connectedNetworks;
            const auto* quality = item.Find("signal");
            if (quality && quality->IsNumber() && std::isfinite(quality->number) && quality->number >= 0 && quality->number <= 100)
                signal = quality->number;
        }
    }
    if (connected && network->transport == "wifi")
    {
        // The status snapshot has no adapter ID. Multiple connected adapters
        // cannot be assigned a primary signal without guessing.
        if (connectedNetworks != 1 || !signal) return {kUnknown, _LW("statusBar.wifiSignalUnknown")};
        return {*signal < 25 ? kWifiLow : *signal < 50 ? kWifiMedium : *signal < 75 ? kWifiGood : kWifi,
            L"Wi-Fi  " + Percent(*signal)};
    }
    if (connected) return {kNetwork, std::wstring(_LW("statusBar.network")) + L"  " + _LW("controlCenter.connected")};
    if (network->connectivity == "none")
        return allRadiosOff ? std::pair<std::wstring, std::wstring>{kWifiOff, _LW("statusBar.wifiOff")} :
            std::pair<std::wstring, std::wstring>{kOffline, _LW("statusBar.networkOffline")};
    return {kUnknown, _LW("statusBar.networkUnknown")};
}
}
std::vector<StatusBarItem> BuildStatusBarItems(const StatusBarSettings& s, const StatusBarSnapshot& snapshot)
{
    std::vector<StatusBarItem> items;
    using namespace status_bar_glyphs;
    const auto add = [&](const char* key, std::wstring text, StatusBarAction action,
        const wchar_t* glyph = L"", bool left = false) {
        StatusBarItem item{std::move(text), action, {}, {}, key, glyph, {}, left};
        item.tip = _LW((std::string("statusBar.") + key).c_str());
        if (!item.text.empty()) item.tip += L"  " + item.text;
        items.push_back(std::move(item));
    };
    if (s.menu) add("menu", L"", StatusBarAction::SystemMenu, kMenu, true);
    if (s.quickSearch) add("quickSearch", L"", StatusBarAction::QuickSearch, kSearch, true);
    if (s.taskView) add("taskView", L"", StatusBarAction::TaskView, kTaskView, true);
    if (s.clock)
    {
        add("clock", snapshot.clock, StatusBarAction::Calendar);
    }
    const auto& notifications = snapshot.notifications;
    const auto* notificationGlyph = kNotifications;
    const auto* notificationTip = "statusBar.notifications";
    if (notifications.quiet.value_or(false))
    { notificationGlyph = kNotificationsQuiet; notificationTip = "statusBar.notificationsQuiet"; }
    else if (notifications.unreadCount && *notifications.unreadCount > 0)
    { notificationGlyph = kNotificationsPending; notificationTip = "statusBar.notificationsPending"; }
    else if (notifications.totalCount && *notifications.totalCount > 0)
    { notificationGlyph = kNotificationsPresent; notificationTip = "statusBar.notificationsPresent"; }
    else if (!notifications.unreadCount && !notifications.totalCount)
    { notificationGlyph = kUnknown; notificationTip = "statusBar.notificationsUnknown"; }
    add("notifications", L"", StatusBarAction::Notifications, notificationGlyph);
    items.back().tip = _LW(notificationTip);
    if (s.cpu)
    {
        const auto value = snapshot.cpu;
        add("cpu", L"CPU " + (value && value->available && !value->warmingUp ? Percent(value->usagePercent) : L"—"), StatusBarAction::Cpu, L"", true);
    }
    if (s.memory)
    {
        const auto value = snapshot.memory;
        add("memory", _LW("statusBar.memory") + std::wstring(L" ") + (value && value->available && value->totalBytes ?
            Percent(100. * value->usedBytes / value->totalBytes) : L"—"), StatusBarAction::Memory, L"", true);
    }
    if (s.gpu)
    {
        const auto value = snapshot.gpu;
        std::optional<double> maximum;
        if (value && !value->warmingUp) for (const auto& adapter : value->adapters)
            if (adapter.usageAvailable && std::isfinite(adapter.usagePercent) && adapter.usagePercent >= 0 && adapter.usagePercent <= 100)
                maximum = maximum ? std::max(*maximum, adapter.usagePercent) : adapter.usagePercent;
        add("gpu", L"GPU " + (maximum ? Percent(*maximum) : L"—"), StatusBarAction::Gpu, L"", true);
    }
    if (s.traffic)
    {
        const auto value = snapshot.traffic;
        add("traffic", value && value->available && !value->warmingUp ?
            L"↓ " + StatusBarRate(value->downloadBytesPerSecond) +
             L" ↑ " + StatusBarRate(value->uploadBytesPerSecond) : L"↓ — ↑ —", StatusBarAction::Traffic, L"", true);
    }
    {
        if (!snapshot.tray.empty())
        {
            auto icons = snapshot.tray;
            const auto rank = [&](const tray::Icon& icon) {
                return std::find(s.trayOrder.begin(), s.trayOrder.end(), icon.persistentKey) - s.trayOrder.begin();
            };
            std::stable_sort(icons.begin(), icons.end(), [&](const auto& left, const auto& right) { return rank(left) < rank(right); });
            for (auto& icon : icons)
                if (!(icon.state & NIS_HIDDEN) && !tray::DuplicatesControlCenter(icon) && !icon.persistentKey.empty() &&
                    std::find(s.pinnedTrayItems.begin(), s.pinnedTrayItems.end(), icon.persistentKey) != s.pinnedTrayItems.end())
                    items.push_back({icon.tip, StatusBarAction::Tray, {}, std::move(icon), "tray", {}, {}, false});
        }
        add("tray", L"", StatusBarAction::Tray, kTray);
        // The closed chevron points into the desktop. Once open it points
        // back to its bar, independently for every monitor.
        items.back().flipGlyph = (s.position == DockPosition::Bottom) != snapshot.trayExpanded;
    }
    if (s.inputMethod && !snapshot.inputMethod.label.empty())
    {
        const auto& input = snapshot.inputMethod;
        add("inputMethod", input.label, StatusBarAction::InputMethod);
        items.back().tip = std::wstring(_LW("statusBar.inputMethod"));
        if (!input.description.empty()) items.back().tip += L"  " + input.description;
        items.back().tip += L"\n" + std::wstring(_LW("statusBar.inputMethod.switch"));
    }
    const auto audio = snapshot.audio;
    const auto power = snapshot.power;
    const auto [networkGlyph, networkTip] = NetworkVisual(snapshot);
    add("controlCenter", L"", StatusBarAction::ControlCenter);
    auto& control = items.back();
    const auto battery = power && power->available ? ResolveStatusBarBatteryVisual(
        power->batteryPercent, power->charging, power->acPower, power->saver) : StatusBarBatteryVisual{};
    const bool hasBattery = power && power->available && std::isfinite(power->batteryPercent) &&
        power->batteryPercent >= 0 && power->batteryPercent <= 100;
    control.controlGlyphs = {networkGlyph,
        audio && audio->available && audio->muted ? kMuted : !audio || !audio->available || audio->volume <= 0 ? kSpeakerZero :
            audio->volume < .5 ? kSpeakerLow : kSpeaker,
        hasBattery ? battery.glyph : L""};
    control.batteryTone = battery.tone;
    control.batteryPluggedIn = battery.pluggedIn;
    control.batteryLevel = power && power->available ? StatusBarBatteryLevel(power->batteryPercent) : -1;
    control.controlTips[0] = networkTip;
    control.controlTips[1] = std::wstring(_LW("statusBar.volume")) + L"  " +
        (audio && audio->available ? (audio->muted ? std::wstring(_LW("statusBar.muted")) : Percent(audio->volume * 100.)) : L"—");
    if (hasBattery)
        control.controlTips[2] = std::wstring(_LW(battery.label)) + L"  " + Percent(power->batteryPercent);
    return items;
}

bool SameStatusBarContent(const std::vector<StatusBarItem>& left, const std::vector<StatusBarItem>& right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](const auto& a, const auto& b) {
        if (a.key != b.key || a.left != b.left || a.action != b.action || a.icon.has_value() != b.icon.has_value()) return false;
        // Pinned tray items paint only their pixels; text is tooltip metadata.
        // Keep refreshing that metadata without invalidating the DComp surface.
        if (a.icon) return a.icon->key == b.icon->key && a.icon->width == b.icon->width &&
            a.icon->height == b.icon->height && a.icon->pixels == b.icon->pixels;
        return a.text == b.text && a.glyph == b.glyph && a.controlGlyphs == b.controlGlyphs &&
            a.batteryTone == b.batteryTone && a.batteryLevel == b.batteryLevel && a.batteryPluggedIn == b.batteryPluggedIn &&
            a.flipGlyph == b.flipGlyph;
    });
}
HRESULT DrawStatusBarContent(ID2D1DeviceContext* context, IDWriteFactory* text, std::vector<StatusBarItem>& items,
    UINT w, UINT h, float scale, const PersonalizationSettings& a, const StatusBarPalette& palette,
    std::optional<std::size_t> hovered, bool keyboardFocusVisible, std::size_t focused, bool mergedDock)
{
    if (!context || !text || !w || !h || !std::isfinite(scale) || scale <= 0) return E_INVALIDARG;
    const bool hc = palette.highContrast;
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(hc ? palette.foreground :
        D2D1::ColorF(a.contentTheme == 1 ? 0x202020 : 0xf4f4f4), &brush);
    ComPtr<IDWriteTextFormat> format;

    text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f * scale, L"", &format);
    ComPtr<IDWriteTextFormat> iconFormat;
    iconFormat.Attach(CreateFluentTextFormat(text, 18.f * scale));
    ComPtr<IDWriteTextFormat> batteryFormat;
    batteryFormat.Attach(CreateFluentTextFormat(text, 20.f * scale));
    if (!format || !brush || !iconFormat || !batteryFormat) return E_FAIL;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (format && brush)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const float padding = 12.f * scale;
        const auto extentOf = [&](const StatusBarItem& item) {
            if (item.key == "controlCenter" && item.controlGlyphs[2].empty()) return 64.f * scale;
            if (const float fixed = StatusBarFixedWidth(item.key); fixed > 0) return fixed * scale;
            if (item.icon || item.text.empty()) return 32.f * scale;
            auto reserved = item.key == "clock" ? StatusBarClockDisplay(item.text, mergedDock) : item.text;
            if (item.key == "memory") reserved = std::wstring(_LW("statusBar.memory")) + L" 100%";
            // Reserve equal digit advances for the date/time too.
            if (item.key == "clock") for (auto& c : reserved) if (c >= L'0' && c <= L'9') c = L'8';
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(text->CreateTextLayout(reserved.c_str(), static_cast<UINT32>(reserved.size()),
                    format.Get(), 2000.f, static_cast<float>(h), &layout))) layout->GetMetrics(&metrics);
            const float paddingDip = item.key == "memory" ? 12.f : item.glyph.empty() ? 16.f : 36.f;
            return std::clamp(metrics.widthIncludingTrailingWhitespace + paddingDip * scale, 32.f * scale, 260.f * scale);
        };
        std::optional<std::size_t> clockIndex, notificationIndex, controlRightIndex;
        std::vector<std::size_t> leftIndices, rightIndices;
        std::vector<LONG> widths(items.size()), rightWidths;
        LONG clockWidth = 0, notificationWidth = 0, controlWidth = 0;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            auto& item = items[i]; item.bounds = {};
            // The fused surface already has the Dock's search button. Keep the
            // user's standalone search setting and item identity unchanged.
            if (mergedDock && item.action == StatusBarAction::QuickSearch) continue;
            widths[i] = static_cast<LONG>(std::ceil(extentOf(item)));
            if (item.action == StatusBarAction::Calendar) { clockIndex = i; clockWidth = widths[i]; }
            else if (item.action == StatusBarAction::Notifications) { notificationIndex = i; notificationWidth = widths[i]; }
            else if (item.left) leftIndices.push_back(i);
            else
            {
                if (item.key == "controlCenter")
                { controlWidth = widths[i]; controlRightIndex = rightWidths.size(); }
                rightIndices.push_back(i); rightWidths.push_back(widths[i]);
            }
        }
        const LONG viewportWidth = static_cast<LONG>(w), viewportHeight = static_cast<LONG>(h);
        const LONG edgePadding = static_cast<LONG>(padding);
        // Keep the date itself centered. The symmetric reservation protects
        // its trailing notification button without shifting the date left.
        if (!mergedDock && clockWidth + 2 * notificationWidth + 2 * (edgePadding + controlWidth) > viewportWidth)
            clockWidth = 0;
        LONG leftLimit = 0;
        if (mergedDock)
        {
            const auto layout = LayoutMergedStatusBar(viewportWidth, viewportHeight, scale,
                edgePadding, clockWidth, notificationWidth, rightWidths, controlRightIndex);
            if (clockIndex) items[*clockIndex].bounds = layout.clock;
            if (notificationIndex) items[*notificationIndex].bounds = layout.notifications;
            for (std::size_t i = 0; i < rightIndices.size(); ++i)
                items[rightIndices[i]].bounds = layout.right[i];
            leftLimit = layout.center.left - edgePadding;
        }
        else
        {
            const LONG centerWidth = clockWidth ? clockWidth + 2 * notificationWidth : notificationWidth;
            const auto bounds = StatusBarHorizontalLayout(viewportWidth, viewportHeight,
                edgePadding, std::span<const LONG>{}, centerWidth, rightWidths);
            RECT dateAndNotification = bounds[0];
            if (!IsRectEmpty(&dateAndNotification))
            {
                if (clockWidth && clockIndex)
                {
                    const LONG dateLeft = dateAndNotification.left + notificationWidth;
                    items[*clockIndex].bounds = {dateLeft, 0, dateLeft + clockWidth, viewportHeight};
                    dateAndNotification.left = dateLeft + clockWidth;
                }
                if (notificationIndex) items[*notificationIndex].bounds = dateAndNotification;
            }
            leftLimit = (clockWidth && clockIndex ? items[*clockIndex].bounds.left : bounds[0].left) - edgePadding;
            for (std::size_t i = 0; i < rightIndices.size(); ++i)
                items[rightIndices[i]].bounds = bounds[i + 1];
        }
        LONG leftCursor = edgePadding;
        for (const auto i : leftIndices)
            if (leftCursor + widths[i] <= leftLimit)
            { items[i].bounds = {leftCursor, 0, leftCursor + widths[i], viewportHeight}; leftCursor += widths[i]; }
        if (mergedDock)
            for (auto& item : items)
                item.bounds = StatusBarCompactTarget(item.bounds, scale, item.key == "clock");
        ComPtr<ID2D1SolidColorBrush> hoverBrush;
        context->CreateSolidColorBrush(hc ? palette.highlight :
            D2D1::ColorF(a.contentTheme == 1 ? 0x000000 : 0xffffff, .08f), &hoverBrush);
        for (auto& item : items)
        {
            if (IsRectEmpty(&item.bounds))
            {

                continue;
            }
            const auto rect = D2D1::RectF(static_cast<float>(item.bounds.left), static_cast<float>(item.bounds.top),
                static_cast<float>(item.bounds.right), static_cast<float>(item.bounds.bottom));
            const auto inset = D2D1::RoundedRect(D2D1::RectF(rect.left + 2 * scale, rect.top + 3 * scale,
                rect.right - 2 * scale, rect.bottom - 3 * scale), 4 * scale, 4 * scale);
            const bool hover = hovered && *hovered == static_cast<std::size_t>(&item - items.data());
            if (hover && item.action != StatusBarAction::None && hoverBrush) context->FillRoundedRectangle(inset, hoverBrush.Get());
            brush->SetColor(hc && hover ? palette.highlightText : hc ? palette.foreground :
                D2D1::ColorF(a.contentTheme == 1 ? 0x202020 : 0xf4f4f4));
            if (item.icon)
            {
                const auto& icon = *item.icon;
                ComPtr<ID2D1Bitmap> bitmap;
                if (icon.width > 0 && icon.height > 0 && icon.width <= 64 && icon.height <= 64 && icon.pixels.size() == static_cast<std::size_t>(icon.width) * icon.height && SUCCEEDED(context->CreateBitmap(D2D1::SizeU(icon.width, icon.height),
                        icon.pixels.data(), icon.width * 4,
                        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bitmap)))
                {
                    const float size = 18.f * scale;
                    const float iconLeft = (rect.left + rect.right - size) / 2, iconTop = (rect.top + rect.bottom - size) / 2;
                    context->DrawBitmap(bitmap.Get(), D2D1::RectF(iconLeft, iconTop, iconLeft + size, iconTop + size));
                }

            }
            else
            {
                auto textRect = rect;
                if (item.key == "controlCenter" && iconFormat)
                {
                    for (std::size_t part = 0; part < item.controlGlyphs.size(); ++part)
                    {
                        if (item.controlGlyphs[part].empty()) continue;
                        const float left = rect.left + (4 + 28.f * static_cast<float>(part)) * scale;
                        const auto color = brush->GetColor();
                        if (part == 2 && !hc)
                        {
                            if (item.batteryPluggedIn || item.batteryTone == StatusBarBatteryTone::Charging || item.batteryTone == StatusBarBatteryTone::FullyCharged)
                                brush->SetColor(D2D1::ColorF(a.contentTheme == 1 ? 0x107c10 : 0x6ccb5f));
                            else if (item.batteryTone == StatusBarBatteryTone::Low)
                                brush->SetColor(D2D1::ColorF(a.contentTheme == 1 ? 0xc42b1c : 0xff8585));
                            else if (item.batteryTone == StatusBarBatteryTone::Saver)
                                brush->SetColor(D2D1::ColorF(a.contentTheme == 1 ? 0x9d5d00 : 0xffcf66));
                        }
                        if (part == 2 && item.batteryLevel >= 0)
                        {
                            // Keep the neutral outline legible on both themes;
                            // charging/full/low colors belong to the fill only.
                            const float top = (rect.top + rect.bottom - 20 * scale) / 2;
                            DrawStatusBarBattery(context, brush.Get(),
                                D2D1::RectF(left + 4 * scale, top, left + 24 * scale, top + 20 * scale),
                                item.batteryLevel * 10., item.batteryTone == StatusBarBatteryTone::Charging, item.batteryPluggedIn,
                                color, brush->GetColor());
                        }
                        else
                        {
                            const auto& glyph = item.controlGlyphs[part];
                            context->DrawText(glyph.c_str(), static_cast<UINT32>(glyph.size()), part == 2 ? batteryFormat.Get() : iconFormat.Get(),
                                D2D1::RectF(left, rect.top, left + 28 * scale, rect.bottom), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                        brush->SetColor(color);
                    }
                }
                else if (!item.glyph.empty() && iconFormat)
                {
                    auto iconRect = rect;
                    if (!item.text.empty()) { iconRect.right = iconRect.left + 28.f * scale; textRect.left += 22.f * scale; }
                    D2D1_MATRIX_3X2_F originalTransform{};
                    if (item.flipGlyph)
                    {
                        context->GetTransform(&originalTransform);
                        context->SetTransform(D2D1::Matrix3x2F::Rotation(180.f,
                            D2D1::Point2F((iconRect.left + iconRect.right) / 2,
                                (iconRect.top + iconRect.bottom) / 2)) * originalTransform);
                    }
                    context->DrawText(item.glyph.c_str(), static_cast<UINT32>(item.glyph.size()), iconFormat.Get(), iconRect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    if (item.flipGlyph) context->SetTransform(originalTransform);
                }
                if (!item.text.empty())
                {
                    const auto displayed = item.key == "clock" ? StatusBarClockDisplay(item.text, mergedDock) : item.text;
                    context->DrawText(displayed.c_str(), static_cast<UINT32>(displayed.size()), format.Get(), textRect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
            if (keyboardFocusVisible && &item == &items[std::min(focused, items.size() - 1)])
                context->DrawRoundedRectangle(inset, brush.Get(), 1.f);
        }
    }
    return S_OK;
}
PersonalizationSettings StatusBarFillAppearance(const PersonalizationSettings& appearance)
{
    auto fill = appearance;
    fill.widgetBorderWidth = 0; fill.widgetBorderAlpha = 0;
    // The desktop-facing edge is drawn once below. A four-sided highlight
    // would reintroduce the border that status bars intentionally omit.
    fill.widgetEdgeHighlightEnabled = false;
    return fill;
}
void DrawStatusBarEdge(ID2D1DeviceContext* context, RECT frame, const PersonalizationSettings& appearance,
    float scale, DockPosition position)
{
    if (!context || appearance.widgetBorderWidth <= 0 || appearance.widgetBorderAlpha <= 0) return;
    ComPtr<ID2D1SolidColorBrush> border;
    context->CreateSolidColorBrush(D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG,
        appearance.widgetBorderB, appearance.widgetBorderAlpha), &border);
    const float width = appearance.widgetBorderWidth * scale;
    const float y = position == DockPosition::Bottom ? frame.top + width / 2 : frame.bottom - width / 2;
    if (border) context->DrawLine(D2D1::Point2F(static_cast<float>(frame.left), y),
        D2D1::Point2F(static_cast<float>(frame.right), y), border.Get(), width);
}
}
