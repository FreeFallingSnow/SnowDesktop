#include "status_bar_view.h"
#include "status_bar_glyphs.h"
#include "status_bar_layout.h"
#include "status_bar_presentation.h"
#include "l10n.h"
#include "utils.h"
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cmath>

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
    {
        add("clock", snapshot.clock, StatusBarAction::Calendar);
    }
    if (s.cpu)
    {
        const auto value = snapshot.cpu;
        add("cpu", L"CPU " + (value && value->available && !value->warmingUp ? Percent(value->usagePercent) : L"—"), StatusBarAction::Cpu);
    }
    if (s.memory)
    {
        const auto value = snapshot.memory;
        add("memory", _LW("statusBar.memory") + std::wstring(L" ") + (value && value->available && value->totalBytes ?
            Percent(100. * value->usedBytes / value->totalBytes) : L"—"), StatusBarAction::Memory);
    }
    if (s.gpu)
    {
        const auto value = snapshot.gpu;
        double maximum = 0;
        if (value) for (const auto& adapter : value->adapters) maximum = std::max(maximum, adapter.usagePercent);
        add("gpu", L"GPU " + (value && value->available && !value->warmingUp ? Percent(maximum) : L"—"), StatusBarAction::Gpu);
    }
    if (s.traffic)
    {
        const auto value = snapshot.traffic;
        add("traffic", value && value->available && !value->warmingUp ?
            L"↓ " + StatusBarRate(value->downloadBytesPerSecond) +
            L" ↑ " + StatusBarRate(value->uploadBytesPerSecond) : L"↓ — ↑ —", StatusBarAction::Traffic);
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
                if (!(icon.state & NIS_HIDDEN) && !icon.persistentKey.empty() &&
                    std::find(s.pinnedTrayItems.begin(), s.pinnedTrayItems.end(), icon.persistentKey) != s.pinnedTrayItems.end())
                    items.push_back({icon.tip, StatusBarAction::Tray, {}, std::move(icon), "tray", {}, {}, false});
        }
        add("tray", L"", StatusBarAction::Tray, kTray);
    }
    const auto network = snapshot.network;
    const auto audio = snapshot.audio;
    const auto power = snapshot.power;
    const std::wstring glyphs = std::wstring(!network || !network->available || network->connectivity == "none" ? kOffline :
        network->transport == "ethernet" ? kEthernet : kWifi) + (audio && audio->muted ? kMuted : kSpeaker) +
        (power && power->available ? (power->charging ? kCharging : power->acPower ? kBatteryPlug :
            power->batteryPercent >= 99.5 ? kBatteryFull : kBattery) : kEthernet);
    add("controlCenter", power && power->available ? Percent(power->batteryPercent) : L"—", StatusBarAction::ControlCenter);
    items.back().glyph = glyphs;
    items.back().tip += L"\n" + std::wstring(_LW("statusBar.volume")) + L"  " +
        (audio && audio->available ? (audio->muted ? std::wstring(_LW("statusBar.muted")) : Percent(audio->volume * 100.)) : L"—");
    if (power && power->available)
        items.back().tip += L"\n" + std::wstring(_LW(power->charging ? "statusBar.charging" :
            power->acPower && power->batteryPercent >= 99.5 ? "statusBar.fullyCharged" :
            power->acPower ? "statusBar.pluggedIn" : "statusBar.battery")) + L"  " + Percent(power->batteryPercent);
    // Fixed semantic zones: information, tray, one system control group.
    // Old experimental rightOrder values cannot split this group.
    add("notifications", L"", StatusBarAction::Notifications, kNotifications);
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
        return a.text == b.text && a.glyph == b.glyph;
    });
}
std::optional<std::size_t> HitTestStatusBarItems(const std::vector<StatusBarItem>& items, POINT point)
{
    for (std::size_t index = 0; index < items.size(); ++index)
        if (PtInRect(&items[index].bounds, point)) return index;
    return {};
}
HRESULT DrawStatusBarContent(ID2D1DeviceContext* context, IDWriteFactory* text, std::vector<StatusBarItem>& items,
    UINT w, UINT h, float scale, const PersonalizationSettings& a, const StatusBarPalette& palette,
    std::optional<std::size_t> hovered, bool keyboardFocusVisible, std::size_t focused)
{
    if (!context || !text || !w || !h || !std::isfinite(scale) || scale <= 0) return E_INVALIDARG;
    const bool hc = palette.highContrast;
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(hc ? palette.foreground :
        D2D1::ColorF(a.contentTheme == 1 ? 0x202020 : 0xf4f4f4), &brush);
    ComPtr<IDWriteTextFormat> format;

    text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f * scale, L"", &format);
    ComPtr<IDWriteTextFormat> iconFormat;
    iconFormat.Attach(CreateFluentTextFormat(text, 16.f * scale));
    if (!format || !brush || !iconFormat) return E_FAIL;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (format && brush)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        const float padding = 12.f * scale;
        const auto extentOf = [&](const StatusBarItem& item) {
            if (const float fixed = StatusBarFixedWidth(item.key); fixed > 0) return fixed * scale;
            if (item.icon || item.text.empty()) return 32.f * scale;
            auto reserved = item.text;
            if (item.key == "memory") reserved = std::wstring(_LW("statusBar.memory")) + L" 100%";
            // Reserve equal digit advances for the date/time too.
            if (item.key == "clock") for (auto& c : reserved) if (c >= L'0' && c <= L'9') c = L'8';
            ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(text->CreateTextLayout(reserved.c_str(), static_cast<UINT32>(reserved.size()),
                    format.Get(), 2000.f, static_cast<float>(h), &layout))) layout->GetMetrics(&metrics);
            return std::clamp(metrics.widthIncludingTrailingWhitespace + (item.glyph.empty() ? 16.f : 36.f) * scale, 32.f * scale, 260.f * scale);
        };
        LONG centerWidth = 0;
        std::vector<LONG> leftWidths, rightWidths;
        for (const auto& item : items)
            if (item.action == StatusBarAction::Calendar) centerWidth = static_cast<LONG>(std::ceil(extentOf(item)));
            else if (item.left) leftWidths.push_back(static_cast<LONG>(std::ceil(extentOf(item))));
            else rightWidths.push_back(static_cast<LONG>(std::ceil(extentOf(item))));
        const auto bounds = StatusBarHorizontalLayout(static_cast<LONG>(w), static_cast<LONG>(h),
            static_cast<LONG>(padding), leftWidths, centerWidth, rightWidths);
        std::size_t leftIndex = 0, rightIndex = leftWidths.size() + 1;
        ComPtr<ID2D1SolidColorBrush> hoverBrush;
        context->CreateSolidColorBrush(hc ? palette.highlight :
            D2D1::ColorF(a.contentTheme == 1 ? 0x000000 : 0xffffff, .08f), &hoverBrush);
        for (auto& item : items)
        {
            const bool center = item.action == StatusBarAction::Calendar;
            item.bounds = bounds[item.left ? leftIndex++ : center ? leftWidths.size() : rightIndex++];
            if (IsRectEmpty(&item.bounds))
            {

                continue;
            }
            const auto rect = D2D1::RectF(static_cast<float>(item.bounds.left), 0,
                static_cast<float>(item.bounds.right), static_cast<float>(h));
            const auto inset = D2D1::RoundedRect(D2D1::RectF(rect.left + 2 * scale, 3 * scale,
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
                    for (std::size_t part = 0; part < item.glyph.size(); ++part)
                    {
                        const float left = rect.left + (4 + 28.f * static_cast<float>(part)) * scale;
                        context->DrawText(&item.glyph[part], 1, iconFormat.Get(),
                            D2D1::RectF(left, rect.top, left + 28 * scale, rect.bottom), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                    textRect.left += 88 * scale; textRect.right -= 6 * scale;
                }
                else if (!item.glyph.empty() && iconFormat)
                {
                    auto iconRect = rect;
                    if (!item.text.empty()) { iconRect.right = iconRect.left + 28.f * scale; textRect.left += 22.f * scale; }
                    context->DrawText(item.glyph.c_str(), static_cast<UINT32>(item.glyph.size()), iconFormat.Get(), iconRect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                if (!item.text.empty()) context->DrawText(item.text.c_str(), static_cast<UINT32>(item.text.size()), format.Get(), textRect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
