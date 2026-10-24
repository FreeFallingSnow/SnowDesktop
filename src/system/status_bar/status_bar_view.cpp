#include "ui/render/app_font.h"
#include "status_bar_view.h"
#include "status_bar_glyphs.h"
#include "status_bar_battery.h"
#include "status_bar_layout.h"
#include "status_bar_tooltip.h"
#include "status_bar_presentation.h"
#include "widget/preview/widget_preview_stage.h"
#include "system/tray/tray_presentation.h"
#include "common/l10n.h"
#include "common/utils.h"
#include <dwrite.h>
#include <dcomp.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cmath>
#include <utility>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace
{
// Microsoft Fluent System Icons: Alert 24 Filled, MIT.
// Copyright (c) Microsoft Corporation. All rights reserved.
// Unmodified contours from microsoft/fluentui-system-icons@
// 21d5d02f724be2aaf586564775fff73a18a76eb6:
// assets/Alert/SVG/ic_fluent_alert_24_filled.svg
// License: third_party/fluentui-system-icons/LICENSE.
ComPtr<ID2D1PathGeometry> CreateNotificationBellGeometry(ID2D1Factory* factory)
{
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(D2D1::Point2F(9.04173f, 19.003f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(14.9583f, 19.003f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(14.7196f, 20.4211f), D2D1::Point2F(13.486f, 21.5015f), D2D1::Point2F(12.f, 21.5015f)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(10.514f, 21.5015f), D2D1::Point2F(9.28037f, 20.4211f), D2D1::Point2F(9.04173f, 19.003f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->BeginFigure(D2D1::Point2F(12.f, 2.00391f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(16.1421f, 2.00391f), D2D1::Point2F(19.5f, 5.36177f), D2D1::Point2F(19.5f, 9.50391f)));
    sink->AddLine(D2D1::Point2F(19.5f, 13.5024f));
    sink->AddLine(D2D1::Point2F(20.9183f, 16.6625f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(20.9732f, 16.7848f), D2D1::Point2F(21.0016f, 16.9174f), D2D1::Point2F(21.0016f, 17.0515f)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(21.0016f, 17.5762f), D2D1::Point2F(20.5763f, 18.0015f), D2D1::Point2F(20.0516f, 18.0015f)));
    sink->AddLine(D2D1::Point2F(3.95219f, 18.0015f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(3.8184f, 18.0015f), D2D1::Point2F(3.68613f, 17.9733f), D2D1::Point2F(3.56402f, 17.9186f)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(3.08515f, 17.7042f), D2D1::Point2F(2.87073f, 17.1422f), D2D1::Point2F(3.08511f, 16.6633f)));
    sink->AddLine(D2D1::Point2F(4.50001f, 13.5028f));
    sink->AddLine(D2D1::Point2F(4.50011f, 9.49099f));
    sink->AddLine(D2D1::Point2F(4.50453f, 9.24107f));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(4.6436f, 5.21035f), D2D1::Point2F(7.95588f, 2.00391f), D2D1::Point2F(12.f, 2.00391f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return {};
    return geometry;
}
bool DrawNotificationBell(ID2D1RenderTarget* context, ID2D1SolidColorBrush* brush, D2D1_RECT_F bounds)
{
    ComPtr<ID2D1Factory> factory;
    context->GetFactory(&factory);
    const auto bell = CreateNotificationBellGeometry(factory.Get());
    if (!bell) return false;
    D2D1_MATRIX_3X2_F originalTransform{};
    context->GetTransform(&originalTransform);
    D2D1_RECT_F ink{};
    if (FAILED(bell->GetBounds(nullptr, &ink))) return false;
    const float sx = (bounds.right - bounds.left) / 24.f;
    const float sy = (bounds.bottom - bounds.top) / 24.f;
    context->SetTransform(D2D1::Matrix3x2F::Scale(sx,
        sy) * D2D1::Matrix3x2F::Translation(
            (bounds.left + bounds.right - (ink.left + ink.right) * sx) / 2,
            (bounds.top + bounds.bottom - (ink.top + ink.bottom) * sy) / 2) * originalTransform);
    context->FillGeometry(bell.Get(), brush);
    context->SetTransform(originalTransform);
    return true;
}

void DrawCenteredBarText(ID2D1RenderTarget* context, IDWriteFactory* factory, IDWriteTextFormat* format,
    std::wstring_view value, D2D1_RECT_F bounds, ID2D1Brush* brush, bool fixedInformationBaseline = false,
    bool rightAligned = false, bool* frameReusable = nullptr)
{
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format,
            bounds.right - bounds.left, bounds.bottom - bounds.top, &layout)))
    { if (frameReusable) *frameReusable = false; return; }
    if (rightAligned) layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    auto origin = D2D1::Point2F(bounds.left, bounds.top);
    if (fixedInformationBaseline)
    {
        // One line box and baseline for every readout, independent of the
        // current digits. Keep labels and values together on that baseline.
        const float size = format->GetFontSize();
        if (FAILED(layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 4.f / 3.f, size)))
        { if (frameReusable) *frameReusable = false; return; }
        origin.y += size / 12.f;
    }
    else
    {
        DWRITE_OVERHANG_METRICS ink{};
        if (SUCCEEDED(layout->GetOverhangMetrics(&ink)))
        {
            origin.x += (ink.left - ink.right) / 2;
            origin.y += (ink.top - ink.bottom) / 2;
        }
        else if (frameReusable) *frameReusable = false;
    }
    context->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->DrawTextLayout(origin, layout.Get(), brush);
    context->PopAxisAlignedClip();
}

bool DrawCenteredBarGlyph(ID2D1RenderTarget* context, IDWriteFontFace* face, std::wstring_view value,
    float size, D2D1_RECT_F bounds, ID2D1Brush* brush, std::wstring_view reference = {}, bool* frameReusable = nullptr)
{
    if (!face || value.empty()) return false;
    UINT32 code = static_cast<UINT32>(value.front());
    if (value.size() == 2 && code >= 0xd800 && code <= 0xdbff && value[1] >= 0xdc00 && value[1] <= 0xdfff)
        code = 0x10000 + ((code - 0xd800) << 10) + (static_cast<UINT32>(value[1]) - 0xdc00);
    else if (value.size() != 1) return false;
    UINT16 glyph = 0;
    const auto indexed = face->GetGlyphIndices(&code, 1, &glyph);
    if (FAILED(indexed)) { if (frameReusable) *frameReusable = false; return false; }
    if (!glyph) return false;
    ComPtr<ID2D1Factory> factory; context->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> geometry;
    ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink)) ||
        FAILED(face->GetGlyphRunOutline(size, &glyph, nullptr, nullptr, 1, FALSE, FALSE, sink.Get())) ||
        FAILED(sink->Close())) { if (frameReusable) *frameReusable = false; return false; }
    D2D1_RECT_F ink{};
    if (FAILED(geometry->GetBounds(nullptr, &ink))) { if (frameReusable) *frameReusable = false; return false; }
    if (ink.right <= ink.left || ink.bottom <= ink.top) return false;
    if (reference.size() == 1 && reference != value)
    {
        // All volume states retain the full-volume icon's origin. Centering
        // a quieter state's shorter waves pushes the speaker body right.
        const UINT32 referenceCode = static_cast<UINT32>(reference.front());
        UINT16 referenceGlyph = 0;
        ComPtr<ID2D1PathGeometry> referenceGeometry;
        ComPtr<ID2D1GeometrySink> referenceSink;
        D2D1_RECT_F referenceInk{};
        if (SUCCEEDED(face->GetGlyphIndices(&referenceCode, 1, &referenceGlyph)) && referenceGlyph &&
            SUCCEEDED(factory->CreatePathGeometry(&referenceGeometry)) &&
            SUCCEEDED(referenceGeometry->Open(&referenceSink)) &&
            SUCCEEDED(face->GetGlyphRunOutline(size, &referenceGlyph, nullptr, nullptr, 1, FALSE, FALSE, referenceSink.Get())) &&
            SUCCEEDED(referenceSink->Close()) && SUCCEEDED(referenceGeometry->GetBounds(nullptr, &referenceInk)) &&
            referenceInk.right > referenceInk.left && referenceInk.bottom > referenceInk.top)
            ink = referenceInk;
        else if (frameReusable) *frameReusable = false;
    }
    // The icon font's conservative glyph metrics can differ from its actual
    // paths. Draw those unchanged paths at their measured center, just like
    // the SVG bell and battery, with no font-baseline or hinting displacement.
    D2D1_MATRIX_3X2_F original{}; context->GetTransform(&original);
    context->SetTransform(D2D1::Matrix3x2F::Translation(
        (bounds.left + bounds.right - ink.left - ink.right) / 2,
        (bounds.top + bounds.bottom - ink.top - ink.bottom) / 2) * original);
    context->FillGeometry(geometry.Get(), brush);
    context->SetTransform(original);
    return true;
}

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
    if (s.notifications)
    {
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
    }
    if (s.cpu)
    {
        const auto value = snapshot.cpu;
        const auto usage = value && value->available && !value->warmingUp ? Percent(value->usagePercent) : L"—";
        add("cpu", L"CPU " + usage, StatusBarAction::Cpu, L"", true);
        items.back().tip = std::wstring(_LW("statusBar.cpu")) + L"  " + usage;
    }
    if (s.memory)
    {
        const auto value = snapshot.memory;
        const auto usage = value && value->available && value->totalBytes ?
            Percent(100. * value->usedBytes / value->totalBytes) : L"—";
        add("memory", L"RAM " + usage, StatusBarAction::Memory, L"", true);
        items.back().tip = std::wstring(_LW("statusBar.memory")) + L"  " + usage;
    }
    if (s.gpu)
    {
        const auto value = snapshot.gpu;
        std::optional<double> maximum;
        if (value && !value->warmingUp) for (const auto& adapter : value->adapters)
            if (adapter.usageAvailable && std::isfinite(adapter.usagePercent) && adapter.usagePercent >= 0 && adapter.usagePercent <= 100)
                maximum = maximum ? std::max(*maximum, adapter.usagePercent) : adapter.usagePercent;
        const auto usage = maximum ? Percent(*maximum) : L"—";
        add("gpu", L"GPU " + usage, StatusBarAction::Gpu, L"", true);
        items.back().tip = std::wstring(_LW("statusBar.gpu")) + L"  " + usage;
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
    if (s.inputMethod && (!snapshot.inputMethod.label.empty() || !snapshot.inputMethod.layoutLabel.empty()))
    {
        const auto& input = snapshot.inputMethod;
        const auto& layoutLabel = input.layoutLabel.empty() ? input.label : input.layoutLabel;
        if (!input.label.empty() && input.label != layoutLabel)
        {
            add("inputMethod", input.label, StatusBarAction::InputMethod);
            items.back().tip = std::wstring(_LW("statusBar.inputMethod"));
            if (!input.description.empty()) items.back().tip += L"  " + input.description;
            items.back().tip += L"\n" + std::wstring(_LW("statusBar.inputMethod.switch"));
        }
        add("inputMethodLayout", L"", StatusBarAction::InputMethodPanel, L"\uF4B8"); // keyboard
        items.back().tip = std::wstring(_LW("statusBar.inputMethodPanel"));
        if (!input.description.empty()) items.back().tip += L"  " + input.description;
    }
    if (s.controlCenter && s.controlCenterSingleIcon)
        add("controlCenter", L"", StatusBarAction::ControlCenter, kControls);
    else if (s.controlCenter)
    {
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
        control.controlTips[0] = snapshot.network && snapshot.network->available ?
            StatusBarNetworkTooltip(snapshot.network->connections, networkTip) : networkTip;
        control.controlTips[1] = StatusBarVolumeTooltip(snapshot.audioDevice, audio);
        if (hasBattery)
        {
            control.controlTips[2] = std::wstring(_LW(battery.label)) + L"  " + Percent(power->batteryPercent);
            if (s.batteryPercentage) control.batteryPercentageText = Percent(power->batteryPercent);
        }
    }
    if (s.showDesktop)
    {
        add("showDesktop", L"", StatusBarAction::ShowDesktop);
        items.back().tip = _LW("statusBar.showDesktopTip");
    }
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
            a.flipGlyph == b.flipGlyph && a.inputIcon == b.inputIcon &&
            a.batteryPercentageText == b.batteryPercentageText;
    });
}
namespace
{
bool SameResourceIcon(const StatusBarDrawResources::Entry& entry, const StatusBarItem& item)
{
    if (entry.key != (item.icon ? item.icon->key : item.key) || entry.identity.has_value() != item.icon.has_value()) return false;
    if (item.icon)
    {
        const auto& a = *entry.identity;
        const auto& b = item.icon->identity;
        // Compare the registration owner as well as the Shell GUID: a GUID
        // can survive process replacement and must not retain an old resource.
        if (a.guid != b.guid || a.window != b.window || a.id != b.id || a.process != b.process) return false;
    }
    if (!item.icon && !item.inputIcon) return false;
    const auto width = item.icon ? item.icon->width : item.inputIcon->width;
    const auto height = item.icon ? item.icon->height : item.inputIcon->height;
    const auto& pixels = item.icon ? item.icon->pixels : item.inputIcon->pixels;
    // Snapshots have no independent pixel revision. Exact equality avoids
    // stale images from pointer reuse, hash collisions or metadata-only edits.
    return entry.width == width && entry.height == height && entry.pixels == pixels;
}
}
bool StatusBarDrawResources::TextMatches(IDWriteFactory* text, float value) const
{
    return format && iconFormat && batteryFormat && factory.Get() == text && scale == value &&
        fontRevision == app_fonts::revision.load() && fontSelection == app_fonts::current.load();
}
void StatusBarDrawResources::Reset()
{
    factory.Reset(); format.Reset(); iconFormat.Reset(); batteryFormat.Reset(); iconFace.Reset();
    fontSelection.reset(); device.Reset(); bitmaps.clear();
    scale = dpiX = dpiY = 0;
}
void StatusBarDrawResources::RetainIcons(const std::vector<StatusBarItem>& items)
{
    std::erase_if(bitmaps, [&](const Entry& entry) {
        return std::none_of(items.begin(), items.end(), [&](const auto& item) { return SameResourceIcon(entry, item); });
    });
}
HRESULT StatusBarDrawResources::Prepare(ID2D1DeviceContext* context, IDWriteFactory* text,
    float value, const std::vector<StatusBarItem>& items, bool keepPreparedText)
{
    ComPtr<ID2D1Device> nextDevice;
    context->GetDevice(&nextDevice);
    float nextDpiX = 0, nextDpiY = 0;
    context->GetDpi(&nextDpiX, &nextDpiY);
    if (device != nextDevice || scale != value || dpiX != nextDpiX || dpiY != nextDpiY)
        bitmaps.clear();
    device = nextDevice; dpiX = nextDpiX; dpiY = nextDpiY;
    RetainIcons(items);
    if (keepPreparedText)
    {
        // Arrangement and drawing share one text snapshot, including a failed
        // Fluent lookup. Retry that lookup on the next frame, as before.
        if (!format || !iconFormat || !batteryFormat || factory.Get() != text || scale != value) return E_INVALIDARG;
        return S_OK;
    }
    return PrepareText(text, value);
}
HRESULT StatusBarDrawResources::PrepareText(IDWriteFactory* text, float value)
{
    if (!text || !std::isfinite(value) || value <= 0) return E_INVALIDARG;
    if (scale != value) bitmaps.clear();
    if (!TextMatches(text, value) || !iconFace)
    {
        // Capture before creation. If the selection changes concurrently,
        // the next draw detects it instead of blessing an older format.
        const auto nextRevision = app_fonts::revision.load();
        const auto nextSelection = app_fonts::current.load();
        ComPtr<IDWriteTextFormat> nextFormat, nextIconFormat, nextBatteryFormat;
        app_fonts::CreateTextFormat(text, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f * value, L"", &nextFormat);
        nextIconFormat.Attach(CreateFluentTextFormat(text, 18.f * value));
        nextBatteryFormat.Attach(CreateFluentTextFormat(text, 20.f * value));
        if (!nextFormat || !nextIconFormat || !nextBatteryFormat) return E_FAIL;
        nextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        nextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        nextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format = nextFormat; iconFormat = nextIconFormat; batteryFormat = nextBatteryFormat;
        iconFace.Reset(); factory = text; scale = value;
        fontRevision = nextRevision; fontSelection = nextSelection;
        ++fontBuilds;
    }
    // A missing face also refreshes formats above: the shared Fluent
    // collection may have recovered since the previous fallback format.
    // Preserve fallback drawing and retry on the next requested draw.
    if (!iconFace)
    {
        ++faceLookups;
        ComPtr<IDWriteFontCollection> fonts;
        UINT32 index = 0; BOOL exists = FALSE;
        if (SUCCEEDED(iconFormat->GetFontCollection(&fonts)) && fonts &&
            SUCCEEDED(fonts->FindFamilyName(L"FluentSystemIcons-Regular", &index, &exists)) && exists)
        {
            ComPtr<IDWriteFontFamily> family; ComPtr<IDWriteFont> font;
            if (SUCCEEDED(fonts->GetFontFamily(index, &family)) &&
                SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, &font))) font->CreateFontFace(&iconFace);
        }
    }
    return S_OK;
}
StatusBarDrawResources::Bitmap StatusBarDrawResources::BitmapFor(ID2D1DeviceContext* context, const StatusBarItem& item)
{
    const auto found = std::find_if(bitmaps.begin(), bitmaps.end(),
        [&](const Entry& entry) { return SameResourceIcon(entry, item); });
    if (found != bitmaps.end()) return found->bitmap;
    const auto width = item.icon ? item.icon->width : item.inputIcon->width;
    const auto height = item.icon ? item.icon->height : item.inputIcon->height;
    const auto& pixels = item.icon ? item.icon->pixels : item.inputIcon->pixels;
    Bitmap result;
    if (!width || !height || width > 64 || height > 64 || pixels.size() != static_cast<std::size_t>(width) * height)
        return result;
    if (FAILED(context->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &result.image)))
        return result; // Retry failures; never retain a blank result.
    ++bitmapBuilds; ++inkScans;
    result.ink = {static_cast<LONG>(width), static_cast<LONG>(height), 0, 0};
    for (DWORD y = 0; y < height; ++y)
        for (DWORD x = 0; x < width; ++x)
            if ((pixels[static_cast<std::size_t>(y) * width + x] >> 24) >= 16)
            {
                result.ink.left = std::min(result.ink.left, static_cast<LONG>(x));
                result.ink.top = std::min(result.ink.top, static_cast<LONG>(y));
                result.ink.right = std::max(result.ink.right, static_cast<LONG>(x + 1));
                result.ink.bottom = std::max(result.ink.bottom, static_cast<LONG>(y + 1));
            }
    if (IsRectEmpty(&result.ink)) result.ink = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    if (bitmaps.size() == MaximumBitmaps) bitmaps.erase(bitmaps.begin());
    bitmaps.push_back({item.icon ? item.icon->key : item.key, item.icon ? std::optional(item.icon->identity) : std::nullopt,
        width, height, pixels, result});
    return result;
}

HRESULT ArrangeStatusBarContent(IDWriteFactory* text, IDWriteTextFormat* format,
    std::vector<StatusBarItem>& items, UINT w, UINT h, float scale, bool mergedDock,
    bool timeNotificationRightAligned, StatusBarTrayScroll& scrolling, bool dockQuickSearch, bool* frameReusable)
{
    if (frameReusable) *frameReusable = false;
    if (!text || !format || !w || !h || !std::isfinite(scale) || scale <= 0) return E_INVALIDARG;
    if (frameReusable) *frameReusable = true;
    const bool twoLineClock = StatusBarUsesTwoLineClock(mergedDock);
    const float padding = 12.f * scale;
    // Keep the whole control group stable while the percentage changes.
    // Measure with the actual font and DPI instead of reserving a digit count.
    float percentageWidth = 0;
    if (std::any_of(items.begin(), items.end(), [](const auto& item) { return !item.batteryPercentageText.empty(); }))
    {
        ComPtr<IDWriteTextLayout> percentageLayout;
        DWRITE_TEXT_METRICS percentageMetrics{};
        if (FAILED(text->CreateTextLayout(L"100%", 4, format, 2000.f,
                static_cast<float>(h), &percentageLayout)) ||
            FAILED(percentageLayout->GetMetrics(&percentageMetrics)))
        { if (frameReusable) *frameReusable = false; return E_FAIL; }
        percentageWidth = percentageMetrics.widthIncludingTrailingWhitespace + 4.f * scale;
    }
    const auto extentOf = [&](const StatusBarItem& item) {
        if (item.key == "controlCenter")
            return item.glyph.empty() ? (item.controlGlyphs[2].empty() ? 56.f : 84.f) * scale +
                (item.batteryPercentageText.empty() ? 0.f : percentageWidth) : 28.f * scale;
        if (const float fixed = StatusBarFixedWidth(item.key); fixed > 0) return fixed * scale;
        if (item.icon || item.inputIcon || item.text.empty()) return 28.f * scale;
        auto reserved = item.key == "clock" ? StatusBarClockDisplay(item.text, twoLineClock) : item.text;
        if (item.key == "memory") reserved = L"RAM 100%";
        if (item.key == "cpu") reserved = L"CPU 100%";
        if (item.key == "gpu") reserved = L"GPU 100%";
        // Reserve equal digit advances for the date/time too.
        if (item.key == "clock") for (auto& c : reserved) if (c >= L'0' && c <= L'9') c = L'8';
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS metrics{};
        const auto created = text->CreateTextLayout(reserved.c_str(), static_cast<UINT32>(reserved.size()),
            format, 2000.f, static_cast<float>(h), &layout);
        if (SUCCEEDED(created))
        {
            const auto measured = layout->GetMetrics(&metrics);
            if (FAILED(measured) && frameReusable) *frameReusable = false;
        }
        else if (frameReusable) *frameReusable = false;
        const float paddingDip = item.key == "clock" ? (mergedDock ? 8.f : 0.f) : item.glyph.empty() ? 8.f : 28.f;
        return std::clamp(metrics.widthIncludingTrailingWhitespace + paddingDip * scale, 28.f * scale, 260.f * scale);
    };
    std::optional<std::size_t> clockIndex, notificationIndex, controlRightIndex, showDesktopIndex;
    std::vector<std::size_t> leftIndices, rightIndices, trayIndices;
    std::vector<LONG> widths(items.size()), rightWidths;
    LONG clockWidth = 0, notificationWidth = 0, controlWidth = 0, trayWidth = 0;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        auto& item = items[i]; item.bounds = {}; item.contentBounds = {};
        if (item.action == StatusBarAction::ShowDesktop) { showDesktopIndex = i; continue; }
        // Keep the stored standalone preference. Suppress only the duplicate
        // merged entry, including its spacing, pointer and keyboard target.
        if (mergedDock && dockQuickSearch && item.key == "quickSearch") continue;
        widths[i] = static_cast<LONG>(std::ceil(extentOf(item)));
        if (item.action == StatusBarAction::Calendar) { clockIndex = i; clockWidth = widths[i]; }
        else if (item.action == StatusBarAction::Notifications) { notificationIndex = i; notificationWidth = widths[i]; }
        else if (item.left) leftIndices.push_back(i);
        else if (item.icon) { trayIndices.push_back(i); trayWidth += widths[i]; }
        else
        {
            if (item.key == "controlCenter")
            { controlWidth = widths[i]; controlRightIndex = rightWidths.size(); }
            rightIndices.push_back(i); rightWidths.push_back(widths[i]);
        }
    }
    const LONG viewportWidth = static_cast<LONG>(w), viewportHeight = static_cast<LONG>(h);
    const RECT showDesktopBounds = showDesktopIndex ? StatusBarShowDesktopTarget(viewportWidth, viewportHeight, scale) : RECT{};
    const LONG trailingInset = showDesktopBounds.right - showDesktopBounds.left;
    const LONG edgePadding = static_cast<LONG>(padding);
    // Keep the date itself centered. The symmetric reservation protects
    // its trailing notification button without shifting the date left.
    if (!mergedDock && !timeNotificationRightAligned &&
        clockWidth + 2 * notificationWidth + 2 * (edgePadding + controlWidth) > viewportWidth)
        clockWidth = 0;
    LONG leftLimit = 0, trayMinimum = edgePadding;
    if (mergedDock || timeNotificationRightAligned)
    {
        const auto layout = LayoutRightStatusBar(viewportWidth, viewportHeight, scale,
            edgePadding, clockWidth, notificationWidth, rightWidths, controlRightIndex, mergedDock, trailingInset);
        if (clockIndex) items[*clockIndex].bounds = layout.clock;
        if (notificationIndex) items[*notificationIndex].bounds = layout.notifications;
        for (std::size_t i = 0; i < rightIndices.size(); ++i)
            items[rightIndices[i]].bounds = layout.right[i];
        if (mergedDock) leftLimit = layout.center.left - edgePadding;
        else
        {
            leftLimit = viewportWidth - edgePadding;
            const auto reserve = [&](RECT bounds) {
                if (!IsRectEmpty(&bounds)) leftLimit = std::min(leftLimit, bounds.left - edgePadding);
            };
            reserve(layout.clock); reserve(layout.notifications);
            for (const auto& bounds : layout.right) reserve(bounds);
        }
        if (mergedDock) trayMinimum = layout.center.right + edgePadding;
    }
    else
    {
        const LONG centerWidth = clockWidth ? clockWidth + 2 * notificationWidth : notificationWidth;
        const auto bounds = StatusBarHorizontalLayout(viewportWidth, viewportHeight,
            edgePadding, std::span<const LONG>{}, centerWidth, rightWidths, trailingInset);
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
        leftLimit = IsRectEmpty(&bounds[0]) ? viewportWidth - edgePadding :
            (clockWidth && clockIndex ? items[*clockIndex].bounds.left : bounds[0].left) - edgePadding;
        trayMinimum = IsRectEmpty(&bounds[0]) ? edgePadding : bounds[0].right + edgePadding;
        for (std::size_t i = 0; i < rightIndices.size(); ++i)
        {
            items[rightIndices[i]].bounds = bounds[i + 1];
            if (!IsRectEmpty(&bounds[i + 1])) leftLimit = std::min(leftLimit, bounds[i + 1].left - edgePadding);
        }
    }
    if (!mergedDock && timeNotificationRightAligned && trayWidth > 0)
        leftLimit -= std::min(trayWidth, static_cast<LONG>(std::ceil(56 * scale))) + edgePadding;
    LONG leftCursor = edgePadding;
    for (const auto i : leftIndices)
        if (leftCursor + widths[i] <= leftLimit)
        { items[i].bounds = {leftCursor, 0, leftCursor + widths[i], viewportHeight}; leftCursor += widths[i]; }
    // Without a central clock/notification group the pinned tray can use
    // the center too, but must still leave the left controls their space.
    if (!mergedDock && (timeNotificationRightAligned || (!clockWidth && !notificationWidth)))
        trayMinimum = leftCursor + edgePadding;
    LONG trayEnd = viewportWidth - edgePadding;
    for (const auto i : rightIndices)
        if (!IsRectEmpty(&items[i].bounds)) trayEnd = std::min(trayEnd, items[i].bounds.left);
    if (mergedDock || timeNotificationRightAligned)
    {
        if (clockIndex && !IsRectEmpty(&items[*clockIndex].bounds)) trayEnd = std::min(trayEnd, items[*clockIndex].bounds.left);
        if (notificationIndex && !IsRectEmpty(&items[*notificationIndex].bounds)) trayEnd = std::min(trayEnd, items[*notificationIndex].bounds.left);
    }
    const LONG visibleWidth = std::min(trayWidth, std::max(0L, trayEnd - trayMinimum));
    RECT trayViewport{trayEnd - visibleWidth, 0, trayEnd, viewportHeight};
    if (mergedDock) trayViewport = StatusBarCompactTarget(trayViewport, scale, false);
    scrolling.Arrange(trayViewport, trayWidth);
    LONG trayCursor = trayViewport.left - scrolling.offset;
    for (const auto i : trayIndices)
    {
        auto& item = items[i];
        if (!IsRectEmpty(&trayViewport))
        {
            item.contentBounds = {trayCursor, trayViewport.top, trayCursor + widths[i], trayViewport.bottom};
            IntersectRect(&item.bounds, &item.contentBounds, &trayViewport);
        }
        trayCursor += widths[i];
    }
    if (mergedDock)
        for (auto& item : items)
            item.bounds = StatusBarCompactTarget(item.bounds, scale, item.key == "clock" && twoLineClock);
    // Keep the edge target full-height, including a tall merged Dock strip.
    if (showDesktopIndex) items[*showDesktopIndex].bounds = showDesktopBounds;
    return S_OK;
}
HRESULT DrawStatusBarContent(ID2D1DeviceContext* context, IDWriteFactory* text, std::vector<StatusBarItem>& items,
    UINT w, UINT h, float scale, const PersonalizationSettings& a, const StatusBarPalette& palette,
    std::optional<std::size_t> hovered, bool keyboardFocusVisible, std::size_t focused, bool mergedDock,
    bool timeNotificationRightAligned, StatusBarTrayScroll* trayScroll, bool dockQuickSearch, const RECT* updateBounds,
    StatusBarDrawResources* resources, bool keepPreparedText, bool* frameReusable)
{
    if (!context || !text || !w || !h || !std::isfinite(scale) || scale <= 0) return E_INVALIDARG;
    if (updateBounds && !trayScroll) return E_INVALIDARG;
    if (resources)
    {
        const auto prepared = resources->Prepare(context, text, scale, items, keepPreparedText);
        if (FAILED(prepared)) return prepared;
    }
    const bool twoLineClock = StatusBarUsesTwoLineClock(mergedDock);
    const bool hc = palette.highContrast;
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(hc ? palette.foreground :
        D2D1::ColorF(a.contentTheme == 1 ? 0x202020 : 0xf4f4f4), &brush);
    if (!brush) return E_FAIL;
    ComPtr<IDWriteTextFormat> format, iconFormat, batteryFormat;
    ComPtr<IDWriteFontFace> iconFace;
    if (resources)
    {
        format = resources->format; iconFormat = resources->iconFormat;
        batteryFormat = resources->batteryFormat; iconFace = resources->iconFace;
    }
    else
    {
        snowdesktop::app_fonts::CreateTextFormat(text, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f * scale, L"", &format);
        iconFormat.Attach(CreateFluentTextFormat(text, 18.f * scale));
        batteryFormat.Attach(CreateFluentTextFormat(text, 20.f * scale));
        if (!format || !iconFormat || !batteryFormat) return E_FAIL;
        ComPtr<IDWriteFontCollection> iconFonts;
        UINT32 familyIndex = 0; BOOL familyExists = FALSE;
        if (SUCCEEDED(iconFormat->GetFontCollection(&iconFonts)) && iconFonts &&
            SUCCEEDED(iconFonts->FindFamilyName(L"FluentSystemIcons-Regular", &familyIndex, &familyExists)) && familyExists)
        {
            ComPtr<IDWriteFontFamily> family; ComPtr<IDWriteFont> font;
            if (SUCCEEDED(iconFonts->GetFontFamily(familyIndex, &family)) &&
                SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, &font))) font->CreateFontFace(&iconFace);
        }
    }
    const auto drawGlyph = [&](std::wstring_view glyph, IDWriteTextFormat* glyphFormat, D2D1_RECT_F bounds,
        std::wstring_view reference = {}) {
        if (!DrawCenteredBarGlyph(context, iconFace.Get(), glyph, glyphFormat->GetFontSize(), bounds, brush.Get(), reference, frameReusable))
        {
            DrawCenteredBarText(context, text, glyphFormat, glyph, bounds, brush.Get(), false, false, frameReusable);
        }
    };
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (format && brush)
    {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        StatusBarTrayScroll localScroll;
        auto& scrolling = trayScroll ? *trayScroll : localScroll;
        if (!updateBounds)
        {
            const auto arranged = ArrangeStatusBarContent(text, format.Get(), items, w, h, scale,
                mergedDock, timeNotificationRightAligned, scrolling, dockQuickSearch, frameReusable);
            if (FAILED(arranged)) return arranged;
        }
        const RECT trayViewport = scrolling.viewport;
        const LONG visibleWidth = trayViewport.right - trayViewport.left;
        ComPtr<ID2D1SolidColorBrush> hoverBrush;
        context->CreateSolidColorBrush(hc ? palette.highlight :
            D2D1::ColorF(a.contentTheme == 1 ? 0x000000 : 0xffffff, .08f), &hoverBrush);
        if (!hoverBrush && frameReusable) *frameReusable = false;
        // Mask only icon content, leaving the actual glass/background visible.
        ComPtr<ID2D1LinearGradientBrush> trayMask;
        if (!hc && visibleWidth > 0 && scrolling.Maximum() > 0)
        {
            const float length = std::min(12.f * scale, visibleWidth * .25f);
            const float leading = std::min(length, static_cast<float>(scrolling.offset));
            const float trailing = std::min(length, static_cast<float>(scrolling.Maximum() - scrolling.offset));
            const D2D1_GRADIENT_STOP stops[]{
                {0, D2D1::ColorF(1, 1, 1, leading > 0 ? 0.f : 1.f)},
                {leading / visibleWidth, D2D1::ColorF(1, 1, 1, 1)},
                {1 - trailing / visibleWidth, D2D1::ColorF(1, 1, 1, 1)},
                {1, D2D1::ColorF(1, 1, 1, trailing > 0 ? 0.f : 1.f)}};
            ComPtr<ID2D1GradientStopCollection> collection;
            HRESULT hr = context->CreateGradientStopCollection(stops, 4, D2D1_GAMMA_1_0, D2D1_EXTEND_MODE_CLAMP, &collection);
            if (SUCCEEDED(hr)) hr = context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(static_cast<float>(trayViewport.left), 0),
                D2D1::Point2F(static_cast<float>(trayViewport.right), 0)), collection.Get(), &trayMask);
            if (FAILED(hr)) return hr;
        }
        bool trayLayer = false;
        for (auto& item : items)
        {
            if (trayLayer && !item.icon) { context->PopLayer(); trayLayer = false; }
            // No glyph, separator, hover fill or focus outline for this target.
            if (item.action == StatusBarAction::ShowDesktop || IsRectEmpty(&item.bounds))
            {

                continue;
            }
            RECT intersection{};
            if (updateBounds && !IntersectRect(&intersection, &item.bounds, updateBounds)) continue;
            if (item.icon && !trayLayer)
            {
                const auto clip = D2D1::RectF(static_cast<float>(trayViewport.left), static_cast<float>(trayViewport.top),
                    static_cast<float>(trayViewport.right), static_cast<float>(trayViewport.bottom));
                context->PushLayer(D2D1::LayerParameters1(clip, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::Matrix3x2F::Identity(), 1.f, trayMask.Get()), nullptr);
                trayLayer = true;
            }
            const auto bounds = item.icon ? item.contentBounds : item.bounds;
            const auto rect = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
            const auto inset = D2D1::RoundedRect(D2D1::RectF(rect.left + 2 * scale, rect.top + 3 * scale,
                rect.right - 2 * scale, rect.bottom - 3 * scale), 4 * scale, 4 * scale);
            const bool hover = hovered && *hovered == static_cast<std::size_t>(&item - items.data());
            if (hover && item.action != StatusBarAction::None && hoverBrush) context->FillRoundedRectangle(inset, hoverBrush.Get());
            brush->SetColor(hc && hover ? palette.highlightText : hc ? palette.foreground :
                D2D1::ColorF(a.contentTheme == 1 ? 0x202020 : 0xf4f4f4));
            if (item.icon || item.inputIcon)
            {
                bool bitmapDrawn = false;
                const auto width = item.icon ? item.icon->width : item.inputIcon->width;
                const auto height = item.icon ? item.icon->height : item.inputIcon->height;
                const auto& pixels = item.icon ? item.icon->pixels : item.inputIcon->pixels;
                if (resources)
                {
                    const auto cached = resources->BitmapFor(context, item);
                    if (cached.image)
                    {
                        bitmapDrawn = true;
                        const float pixelScale = (item.inputIcon ? 14.f : 18.f) * scale /
                            static_cast<float>(std::max(width, height));
                        const float iconLeft = (rect.left + rect.right - static_cast<float>(cached.ink.left + cached.ink.right) * pixelScale) / 2;
                        const float iconTop = (rect.top + rect.bottom - static_cast<float>(cached.ink.top + cached.ink.bottom) * pixelScale) / 2;
                        context->DrawBitmap(cached.image.Get(), D2D1::RectF(iconLeft, iconTop,
                            iconLeft + static_cast<float>(width) * pixelScale, iconTop + static_cast<float>(height) * pixelScale));
                    }
                }
                else
                {
                    ComPtr<ID2D1Bitmap> bitmap;
                    if (width > 0 && height > 0 && width <= 64 && height <= 64 && pixels.size() == static_cast<std::size_t>(width) * height && SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height),
                            pixels.data(), width * 4,
                            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bitmap)))
                    {
                        bitmapDrawn = true;
                        const float pixelScale = (item.inputIcon ? 14.f : 18.f) * scale /
                            static_cast<float>(std::max(width, height));
                        LONG inkLeft = static_cast<LONG>(width), inkTop = static_cast<LONG>(height), inkRight = 0, inkBottom = 0;
                        for (DWORD y = 0; y < height; ++y)
                            for (DWORD x = 0; x < width; ++x)
                                if ((pixels[static_cast<std::size_t>(y) * width + x] >> 24) >= 16)
                                {
                                    inkLeft = std::min(inkLeft, static_cast<LONG>(x)); inkTop = std::min(inkTop, static_cast<LONG>(y));
                                    inkRight = std::max(inkRight, static_cast<LONG>(x + 1)); inkBottom = std::max(inkBottom, static_cast<LONG>(y + 1));
                                }
                        if (inkRight <= inkLeft || inkBottom <= inkTop)
                        { inkLeft = inkTop = 0; inkRight = static_cast<LONG>(width); inkBottom = static_cast<LONG>(height); }
                        const float iconLeft = (rect.left + rect.right - static_cast<float>(inkLeft + inkRight) * pixelScale) / 2;
                        const float iconTop = (rect.top + rect.bottom - static_cast<float>(inkTop + inkBottom) * pixelScale) / 2;
                        context->DrawBitmap(bitmap.Get(), D2D1::RectF(iconLeft, iconTop,
                            iconLeft + static_cast<float>(width) * pixelScale, iconTop + static_cast<float>(height) * pixelScale));
                    }
                }

                if (!bitmapDrawn && frameReusable) *frameReusable = false;
            }
            else
            {
                auto textRect = rect;
                if (item.key == "controlCenter" && item.glyph.empty() && iconFormat)
                {
                    for (std::size_t part = 0; part < item.controlGlyphs.size(); ++part)
                    {
                        if (item.controlGlyphs[part].empty()) continue;
                        const float left = rect.left + 28.f * static_cast<float>(part) * scale;
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
                            drawGlyph(glyph, part == 2 ? batteryFormat.Get() : iconFormat.Get(),
                                D2D1::RectF(left, rect.top, left + 28 * scale, rect.bottom),
                                part == 1 ? std::wstring_view(status_bar_glyphs::kSpeaker) : std::wstring_view{});
                        }
                        brush->SetColor(color);
                    }
                    if (!item.batteryPercentageText.empty())
                        DrawCenteredBarText(context, text, format.Get(), item.batteryPercentageText,
                            D2D1::RectF(rect.left + 84.f * scale, rect.top, rect.right - 4.f * scale, rect.bottom),
                            brush.Get(), true, false, frameReusable);
                }
                else if (!item.glyph.empty() && iconFormat)
                {
                    auto iconRect = rect;
                    // The filled dome makes a geometrically centered bell
                    // look raised. Keep its click target and horizontal center
                    // while moving all known bell states down one scaled DIP.
                    if (item.action == StatusBarAction::Notifications && item.glyph != status_bar_glyphs::kUnknown)
                    { iconRect.top += scale; iconRect.bottom += scale; }
                    if (!item.text.empty()) { iconRect.right = iconRect.left + 28.f * scale; textRect.left += 22.f * scale; }
                    D2D1_MATRIX_3X2_F originalTransform{};
                    if (item.flipGlyph)
                    {
                        context->GetTransform(&originalTransform);
                        context->SetTransform(D2D1::Matrix3x2F::Rotation(180.f,
                            D2D1::Point2F((iconRect.left + iconRect.right) / 2,
                                (iconRect.top + iconRect.bottom) / 2)) * originalTransform);
                    }
                    const float iconSize = iconFormat->GetFontSize();
                    const float iconLeft = (iconRect.left + iconRect.right - iconSize) / 2;
                    const float iconTop = (iconRect.top + iconRect.bottom - iconSize) / 2;
                    const bool bellDrawn = item.action == StatusBarAction::Notifications &&
                        item.glyph == status_bar_glyphs::kNotificationsPending &&
                        DrawNotificationBell(context, brush.Get(),
                            D2D1::RectF(iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize));
                    if (!bellDrawn)
                    {
                        if (item.action == StatusBarAction::Notifications &&
                            item.glyph == status_bar_glyphs::kNotificationsPending && frameReusable) *frameReusable = false;
                        drawGlyph(item.glyph, iconFormat.Get(), iconRect);
                    }
                    if (item.flipGlyph) context->SetTransform(originalTransform);
                }
                if (!item.text.empty())
                {
                    const bool mergedClock = mergedDock && item.key == "clock";
                    if (mergedClock)
                    {
                        textRect.left += 4.f * scale;
                        textRect.right -= 4.f * scale;
                    }
                    const auto displayed = item.key == "clock" ? StatusBarClockDisplay(item.text, twoLineClock) : item.text;
                    DrawCenteredBarText(context, text, format.Get(), displayed, textRect, brush.Get(),
                        item.key == "clock" || item.key == "cpu" || item.key == "memory" ||
                        item.key == "gpu" || item.key == "traffic", mergedClock, frameReusable);
                }
            }
            if (keyboardFocusVisible && &item == &items[std::min(focused, items.size() - 1)])
                context->DrawRoundedRectangle(inset, brush.Get(), 1.f);
        }
        if (trayLayer) context->PopLayer();
    }
    return S_OK;
}
std::array<RECT, 2> StatusBarHoverDamage(const std::vector<StatusBarItem>& items,
    std::optional<std::size_t> before, std::optional<std::size_t> after, UINT width, UINT height)
{
    std::array<RECT, 2> damage{};
    if (before == after) return damage;
    const RECT client{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const auto bounds = [&](std::optional<std::size_t> index) {
        RECT result{};
        if (index && *index < items.size() && items[*index].action != StatusBarAction::None &&
            items[*index].action != StatusBarAction::ShowDesktop)
            IntersectRect(&result, &items[*index].bounds, &client);
        return result;
    };
    damage = {bounds(before), bounds(after)};
    RECT intersection{};
    if (IntersectRect(&intersection, &damage[0], &damage[1]))
    { UnionRect(&damage[0], &damage[0], &damage[1]); damage[1] = {}; }
    return damage;
}
bool UpdateStatusBarTrayScrollLayout(std::vector<StatusBarItem>& items, LONG previousOffset,
    const StatusBarTrayScroll& scroll, bool frameReusable)
{
    if (!frameReusable || IsRectEmpty(&scroll.viewport) || previousOffset < 0 || previousOffset > scroll.Maximum() ||
        scroll.offset < 0 || scroll.offset > scroll.Maximum()) return false;
    LONG cursor = scroll.viewport.left - previousOffset;
    LONG contentWidth = 0;
    bool any = false;
    // Validate the entire retained tray before mutating any geometry. A stale
    // structure or viewport always returns to complete layout/initialization.
    for (const auto& item : items) if (item.icon)
    {
        const auto& bounds = item.contentBounds;
        if (bounds.left != cursor || bounds.top != scroll.viewport.top || bounds.bottom != scroll.viewport.bottom ||
            bounds.right <= bounds.left) return false;
        RECT visible{}; IntersectRect(&visible, &bounds, &scroll.viewport);
        if (!EqualRect(&visible, &item.bounds)) return false;
        const LONG extent = bounds.right - bounds.left;
        cursor += extent; contentWidth += extent; any = true;
    }
    if (!any || contentWidth != scroll.contentWidth) return false;
    for (auto& item : items) if (item.icon)
    {
        OffsetRect(&item.contentBounds, previousOffset - scroll.offset, 0);
        IntersectRect(&item.bounds, &item.contentBounds, &scroll.viewport);
    }
    return true;
}

RECT StatusBarTrayScrollDamage(const std::vector<StatusBarItem>& items,
    std::optional<std::size_t> previousHover, RECT viewport,
    const std::optional<RECT>& before, const std::optional<RECT>& after, UINT width, UINT height)
{
    RECT damage = viewport;
    const auto hoverDamage = StatusBarHoverDamage(items, previousHover, {}, width, height);
    for (const RECT& region : hoverDamage) if (!IsRectEmpty(&region)) UnionRect(&damage, &damage, &region);
    for (const auto& indicator : {before, after}) if (indicator && !IsRectEmpty(&*indicator))
    {
        RECT expanded = *indicator;
        InflateRect(&expanded, 2, 2); // Existing 2px outline and antialias fringe.
        UnionRect(&damage, &damage, &expanded);
    }
    const RECT client{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    IntersectRect(&damage, &damage, &client);
    return damage;
}

void DrawStatusBarDropIndicator(ID2D1DeviceContext* context, const std::optional<RECT>& indicator,
    const PersonalizationSettings& appearance, const StatusBarPalette& palette, UINT dpi)
{
    if (!context || !indicator) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(context->CreateSolidColorBrush(palette.highContrast ? palette.highlight :
        D2D1::ColorF(appearance.contentTheme == 1 ? 0x202020 : 0xf4f4f4), &brush)))
    {
        const auto& bounds = *indicator;
        const auto rect = D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
            static_cast<float>(bounds.right), static_cast<float>(bounds.bottom));
        if (bounds.right - bounds.left <= MulDiv(3, static_cast<int>(dpi), 96)) context->FillRectangle(rect, brush.Get());
        else context->DrawRoundedRectangle(D2D1::RoundedRect(rect, 4.f, 4.f), brush.Get(), 2.f);
    }
}

HRESULT DrawStatusBarContentUpdate(IDCompositionSurface* surface, IDWriteFactory* text,
    std::vector<StatusBarItem>& items, RECT region, UINT width, UINT height, float scale,
    const PersonalizationSettings& appearance, const StatusBarPalette& palette,
    std::optional<std::size_t> hovered, bool keyboardFocusVisible, std::size_t focused,
    bool mergedDock, StatusBarTrayScroll& trayScroll, const std::optional<RECT>& indicator, UINT dpi, StatusBarDrawResources* resources, bool* frameReusable)
{
    if (!surface) return E_INVALIDARG;
    if (IsRectEmpty(&region)) return S_OK;
    POINT offset{}; ComPtr<ID2D1DeviceContext> context;
    const auto begin = surface->BeginDraw(&region, IID_PPV_ARGS(&context), &offset);
    if (FAILED(begin)) return begin;
    context->SetDpi(96, 96);
    context->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(offset.x - region.left), static_cast<float>(offset.y - region.top)));
    context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(region.left), static_cast<float>(region.top),
        static_cast<float>(region.right), static_cast<float>(region.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
    context->Clear(D2D1::ColorF(0, 0.f));
    const auto result = DrawStatusBarContent(context.Get(), text, items, width, height, scale,
        appearance, palette, hovered, keyboardFocusVisible, focused, mergedDock, false, &trayScroll, true, &region, resources, false, frameReusable);
    DrawStatusBarDropIndicator(context.Get(), indicator, appearance, palette, dpi);
    context->PopAxisAlignedClip(); context.Reset();
    const auto ended = surface->EndDraw();
    return FAILED(result) ? result : ended;
}

HRESULT DrawStatusBarHoverUpdate(IDCompositionSurface* surface, IDWriteFactory* text,
    std::vector<StatusBarItem>& items, const std::array<RECT, 2>& damage,
    UINT width, UINT height, float scale, const PersonalizationSettings& appearance,
    const StatusBarPalette& palette, std::optional<std::size_t> hovered,
    bool keyboardFocusVisible, std::size_t focused, bool mergedDock, StatusBarTrayScroll& trayScroll,
    StatusBarDrawResources* resources, bool* frameReusable)
{
    if (!surface) return E_INVALIDARG;
    for (const RECT& region : damage)
    {
        const auto result = DrawStatusBarContentUpdate(surface, text, items, region, width, height, scale,
            appearance, palette, hovered, keyboardFocusVisible, focused, mergedDock, trayScroll, {}, 96, resources, frameReusable);
        if (FAILED(result)) return result;
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
    float scale, DockPosition position, RECT adjacentDock)
{
    if (!context || IsRectEmpty(&frame)) return;
    const auto segments = StatusBarEdgeSegments(frame, position, adjacentDock);
    if (appearance.widgetEdgeHighlightEnabled && appearance.widgetEdgeHighlightStrength > .0005f)
    {
        using widget_preview::HighlightEdge;
        const auto edge = position == DockPosition::Bottom ? HighlightEdge::Top :
            position == DockPosition::Left ? HighlightEdge::Right :
            position == DockPosition::Right ? HighlightEdge::Left : HighlightEdge::Bottom;
        for (const auto& segment : segments)
        {
            context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(segment.left), static_cast<float>(segment.top),
                static_cast<float>(segment.right), static_cast<float>(segment.bottom)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            (void)widget_preview::DrawEdgeHighlight(context, frame, 0,
                D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
                appearance.widgetEdgeHighlightWidth * scale, appearance.widgetEdgeHighlightStrength, appearance.edgeLight, edge);
            context->PopAxisAlignedClip();
        }
    }
    if (appearance.widgetBorderWidth <= 0 || appearance.widgetBorderAlpha <= 0) return;
    ComPtr<ID2D1SolidColorBrush> border;
    context->CreateSolidColorBrush(D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG,
        appearance.widgetBorderB, appearance.widgetBorderAlpha), &border);
    const float width = appearance.widgetBorderWidth * scale;
    if (position == DockPosition::Left || position == DockPosition::Right)
    {
        const float x = position == DockPosition::Right ? frame.left + width / 2 : frame.right - width / 2;
        if (border) for (const auto& segment : segments)
            context->DrawLine(D2D1::Point2F(x, static_cast<float>(segment.top)),
                D2D1::Point2F(x, static_cast<float>(segment.bottom)), border.Get(), width);
        return;
    }
    const float y = position == DockPosition::Bottom ? frame.top + width / 2 : frame.bottom - width / 2;
    if (border) for (const auto& segment : segments)
        context->DrawLine(D2D1::Point2F(static_cast<float>(segment.left), y),
            D2D1::Point2F(static_cast<float>(segment.right), y), border.Get(), width);
}
}
