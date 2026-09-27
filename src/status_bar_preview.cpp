#include "status_bar_preview.h"
#include "status_bar_presentation.h"
#include "status_bar_layout.h"
#include "status_bar_glyphs.h"
#include "status_bar_interaction.h"
#include "tray_order.h"
#include "preview_png_writer.h"
#include "widget_preview_stage.h"
#include "l10n.h"
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <limits>

namespace snowdesktop
{
namespace
{
using Microsoft::WRL::ComPtr;
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Require(HRESULT result)
{
    if (FAILED(result)) throw std::runtime_error("native status bar rendering failed: " + std::to_string(result));
}
system_control::Snapshot WifiFixture(bool enabled, std::optional<double> signal)
{
    using namespace system_control;
    auto adapter = json::Object();
    adapter.object["available"] = json::Boolean(true);
    adapter.object["enabled"] = json::Boolean(enabled);
    adapter.object["hardwareEnabled"] = json::Boolean(true);
    adapter.object["connected"] = json::Boolean(signal.has_value());
    auto networks = json::Array();
    if (signal)
    {
        auto network = json::Object();
        network.object["connected"] = json::Boolean(true);
        network.object["signal"] = json::Number(*signal);
        networks.array.push_back(std::move(network));
    }
    adapter.object["networks"] = std::move(networks);
    Snapshot result; result.available = true; result.value = json::Object();
    auto interfaces = json::Array(); interfaces.array.push_back(std::move(adapter));
    result.value.object["interfaces"] = std::move(interfaces);
    return result;
}
StatusBarSnapshot Fixture()
{
    StatusBarSnapshot data;
    data.clock = L"2026/09/26   09:09";
    data.cpu.emplace(); data.cpu->available = true; data.cpu->warmingUp = false; data.cpu->usagePercent = 9;
    data.memory.emplace(); data.memory->available = true; data.memory->totalBytes = 32ull << 30; data.memory->usedBytes = 12ull << 30;
    data.gpu.emplace(); data.gpu->available = true; data.gpu->warmingUp = false;
    widget_runtime::WidgetGpuAdapterDataSnapshot adapter; adapter.usagePercent = 8; adapter.usageAvailable = true;
    data.gpu->adapters.push_back(adapter);
    data.traffic.emplace(); data.traffic->available = true; data.traffic->warmingUp = false;
    data.traffic->downloadBytesPerSecond = 2500; data.traffic->uploadBytesPerSecond = 800;
    data.network.emplace(); data.network->available = true; data.network->connectivity = "internet"; data.network->transport = "wifi";
    data.wifi = WifiFixture(true, 86);
    data.notifications.quiet = false; data.notifications.unreadCount = 0;
    data.audio.emplace(); data.audio->available = true; data.audio->volume = .45;
    data.power.emplace(); data.power->available = true; data.power->batteryPercent = 58;
    // Explicit fixture icons: never enumerate applications or connect a Hook.
    const SHSTOCKICONID icons[]{SIID_SHIELD, SIID_WORLD};
    for (unsigned i = 0; i < std::size(icons); ++i)
    {
        SHSTOCKICONINFO info{sizeof(info)};
        Require(SHGetStockIconInfo(icons[i], SHGSI_ICON | SHGSI_SMALLICON, &info));
        struct Pixels
        {
            HICON icon = nullptr; HDC dc = nullptr; HBITMAP bitmap = nullptr; HGDIOBJ old = nullptr;
            ~Pixels() { if (old) SelectObject(dc, old); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); if (icon) DestroyIcon(icon); }
        } bitmap;
        bitmap.icon = info.hIcon; bitmap.dc = CreateCompatibleDC(nullptr);
        BITMAPINFO format{}; format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        format.bmiHeader.biWidth = 32; format.bmiHeader.biHeight = -32;
        format.bmiHeader.biPlanes = 1; format.bmiHeader.biBitCount = 32; format.bmiHeader.biCompression = BI_RGB;
        void* bytes = nullptr; bitmap.bitmap = CreateDIBSection(bitmap.dc, &format, DIB_RGB_COLORS, &bytes, nullptr, 0);
        Require(bitmap.dc && bitmap.bitmap && bytes, "cannot construct stock-icon fixture pixels");
        bitmap.old = SelectObject(bitmap.dc, bitmap.bitmap); std::memset(bytes, 0, 32 * 32 * 4);
        Require(DrawIconEx(bitmap.dc, 0, 0, bitmap.icon, 32, 32, 0, nullptr, DI_NORMAL) != FALSE, "cannot draw stock-icon fixture");
        GdiFlush();
        tray::Icon icon; icon.key = icon.persistentKey = "bar-preview-" + std::to_string(i);
        icon.tip = i ? L"Example sync" : L"Example security"; icon.width = icon.height = 32;
        icon.pixels.assign(static_cast<const std::uint32_t*>(bytes), static_cast<const std::uint32_t*>(bytes) + 1024);
        data.tray.push_back(std::move(icon));
    }
    return data;
}
const StatusBarItem& Item(const std::vector<StatusBarItem>& items, std::string_view key)
{
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.key == key && !item.icon; });
    Require(found != items.end(), "a required status bar item is missing");
    return *found;
}
void CheckLayout(IDWriteFactory* text, const std::vector<StatusBarItem>& items, int width, int height, float scale, bool merged,
    bool expectNotification = true) try
{
    const auto clock = std::find_if(items.begin(), items.end(), [](const auto& item) { return item.key == "clock"; });
    const bool dateVisible = clock != items.end() && !IsRectEmpty(&clock->bounds);
    const auto& notification = Item(items, "notifications").bounds;
    if (clock != items.end() && width >= (merged ? 1600 : 640) * scale)
        Require(dateVisible, "date disappeared despite sufficient status bar width");
    if (dateVisible)
    {
        if (!merged) Require(std::abs(clock->bounds.left + clock->bounds.right - width) <= 1,
            "date itself is not centered on the complete bar");
        Require(notification.left == clock->bounds.right,
            "notification button must immediately follow the date without shifting it");
    }
    else if (!merged) Require(std::abs(notification.left + notification.right - width) <= 1,
        "notification button must remain centered when the date is absent");
    const auto& controls = Item(items, "controlCenter").bounds;
    if (IsRectEmpty(&controls) || (!IsRectEmpty(&notification) != expectNotification))
        throw std::runtime_error("narrow status bar removed essential controls: control=[" +
            std::to_string(controls.left) + "," + std::to_string(controls.right) + "] notification=[" +
            std::to_string(notification.left) + "," + std::to_string(notification.right) + "]");
    if (expectNotification) Require(notification.right - notification.left == static_cast<LONG>(std::ceil(32.f * scale)),
        "notification button must remain a complete independent hit target");
    Require(controls.right - controls.left == static_cast<LONG>(std::ceil(92.f * scale)),
        "system control group must remain complete at narrow widths");
    std::vector<RECT> rectangles;
    ComPtr<IDWriteTextFormat> font;
    Require(text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 12.f * scale, L"", &font));
    font->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i]; const auto& r = item.bounds;
        if (IsRectEmpty(&r)) continue;
        if (merged)
        {
            const auto center = MergedStatusBarCenter(width, height, scale); RECT overlap{};
            Require(!IntersectRect(&overlap, &r, &center), "merged bar controls cover the Dock center");
        }
        if (item.key == "cpu" || item.key == "memory" || item.key == "gpu" || item.key == "traffic")
        {
            const LONG informationEnd = dateVisible ? clock->bounds.left : merged ?
                MergedStatusBarCenter(width, height, scale).left : notification.left;
            Require(item.left && r.right < informationEnd && r.left >= Item(items, "quickSearch").bounds.right,
                "system information must follow the launch buttons on the left");
            if (item.key != "memory")
                Require(r.right - r.left <= (item.key == "traffic" ? 152 : 68) * scale + 1,
                    "system information wastes horizontal space");
        }
        Require(r.left >= 0 && r.top >= 0 && r.right <= width && r.bottom <= height, "status bar item escaped its viewport");
        if (merged)
        {
            Require(r.bottom - r.top <= std::ceil((item.key == "clock" ? 40.f : 32.f) * scale) &&
                std::abs(r.top + r.bottom - height) <= 1,
                "merged hover and hit targets must remain compact and vertically centered");
            if (r.top > 0) Require(!HitTestStatusBarItems(items, {(r.left + r.right) / 2, r.top - 1}),
                "empty merged strip above a button must not invoke it");
        }
        Require(HitTestStatusBarItems(items, {(r.left + r.right) / 2, height / 2}) == i,
            "status bar hit target does not match its rendered item");
        for (const auto& previous : rectangles)
        { RECT overlap{}; Require(!IntersectRect(&overlap, &r, &previous), "status bar zones overlap"); }
        rectangles.push_back(r);
        if (item.text.empty() || item.icon) continue;
        ComPtr<IDWriteTextLayout> layout;
        const auto displayed = item.key == "clock" ? StatusBarClockDisplay(item.text, merged) : item.text;
        Require(text->CreateTextLayout(displayed.c_str(), static_cast<UINT32>(displayed.size()), font.Get(), 4000, 400, &layout));
        DWRITE_TEXT_METRICS metrics{}; Require(layout->GetMetrics(&metrics));
        if (merged && item.key == "clock")
            Require(metrics.lineCount == 2 && metrics.height <= r.bottom - r.top + 1,
                "merged date and time must occupy two fully visible lines");
        const float reserved = item.key == "controlCenter" ? 94.f * scale : 0.f;
        Require(metrics.widthIncludingTrailingWhitespace <= r.right - r.left - reserved + 1,
            "status bar fixed width clips visible text");
    }
    Require(!HitTestStatusBarItems(items, {0, height / 2}), "blank bar padding must route to dismissal");
}
catch (const std::exception& error)
{
    throw std::runtime_error(std::string(error.what()) + " [width=" + std::to_string(width) +
        " height=" + std::to_string(height) + " scale=" + std::to_string(scale) +
        " merged=" + (merged ? "true" : "false") +
        " expectNotification=" + (expectNotification ? "true" : "false") + "]");
}
}

native_component_preview::Result ExportStatusBarPreview(const native_component_preview::Request& request,
    ID2D1Device* device, IDWriteFactory* text, const PersonalizationSettings& appearance,
    const StatusBarPreviewBackground& background)
{
    native_component_preview::Result result; result.request = request; result.stage = "status-bar.render";
    try
    {
        Require(request.appearance == "light" || request.appearance == "dark",
            "native status bar offline previews support light and dark; live compositor blur is not captured");
        Require(device && text && background, "status bar preview requires initialized native graphics");
        const auto source = Fixture();
        const float dpiScale = request.dpi / 96.f;
        const int fullWidth = request.canvasWidth - 2 * request.padding;
        Require(fullWidth >= static_cast<int>(640 * dpiScale), "status bar preview canvas is too narrow");
        std::vector<StatusBarItem> infoItems;
        std::vector<std::uint32_t> normalPixels;
        widget_preview::Wallpaper stage;
        if (!request.backgroundImage.empty())
        {
            const auto image = widget_preview::LoadWallpaperImage(request.backgroundImage);
            Require(!image.pixels.empty(), "cannot decode status bar preview background");
            stage = widget_preview::GenerateWallpaper(image, request.canvasWidth, request.canvasHeight);
        }
        else if (!request.transparent && !request.contentOnly)
            stage = widget_preview::GenerateWallpaper(request.canvasWidth, request.canvasHeight, request.appearance == "light");
        if (request.transparent || request.contentOnly)
        { stage.width = request.canvasWidth; stage.height = request.canvasHeight; stage.pixels.resize(static_cast<std::size_t>(stage.width) * stage.height); }
        Require(!stage.pixels.empty(), "cannot create status bar preview background");
        for (const std::string preset : {"normal", "information", "updated", "hover", "full", "charging", "charging-low", "low-battery", "wifi-off", "offline", "unavailable", "bottom", "narrow", "scaled", "high-contrast", "repeat", "merged"})
        {
            auto data = source; StatusBarSettings settings;
            settings.cpu = settings.memory = settings.gpu = settings.traffic =
                preset == "information" || preset == "updated" || preset == "narrow" || preset == "merged";
            settings.pinnedTrayItems = {"bar-preview-0", "bar-preview-1"};
            if (preset == "scaled") settings.scale = 1.5f;
            if (preset == "bottom") settings.position = DockPosition::Bottom;
            if (preset == "updated")
            {
                data.cpu->usagePercent = 100; data.memory->usedBytes = data.memory->totalBytes;
                data.gpu->adapters[0].usagePercent = 100;
                data.traffic->downloadBytesPerSecond = data.traffic->uploadBytesPerSecond = 999ull << 40;
                data.clock = L"2026/09/26   11:59";
            }
            if (preset == "full") { data.power->batteryPercent = 100; data.power->acPower = true; data.network->transport = "ethernet"; }
            if (preset == "charging" || preset == "charging-low") { data.power->charging = true; data.power->acPower = true; }
            if (preset == "charging-low" || preset == "low-battery") data.power->batteryPercent = 10;
            if (preset == "low-battery") data.wifi = WifiFixture(true, 14);
            if (preset == "wifi-off" || preset == "offline")
            { data.network->connectivity = "none"; data.wifi = WifiFixture(preset == "offline", {}); }
            if (preset == "high-contrast") data.notifications.quiet = true;
            if (preset == "unavailable")
            { data.power->available = false; data.network->available = false; data.audio->available = false; data.notifications = {}; }
            const float scale = dpiScale * settings.scale;
            const int width = preset == "narrow" ? static_cast<int>(640 * dpiScale) : fullWidth;
            const int height = static_cast<int>(std::lround((preset == "merged" ? 64 : 32) * scale));
            Require(height + 2 * request.padding <= request.canvasHeight, "status bar preview canvas is too short");
            const int left = (request.canvasWidth - width) / 2, top = (request.canvasHeight - height) / 2;
            auto items = BuildStatusBarItems(settings, data);
            auto identical = data; if (identical.cpu) ++identical.cpu->revision;
            Require(SameStatusBarContent(items, BuildStatusBarItems(settings, identical)), "unchanged samples would repaint status bar content");
            if (preset == "normal")
            {
                identical.audio->volume = .49;
                Require(SameStatusBarContent(items, BuildStatusBarItems(settings, identical)), "tooltip-only changes would repaint the bar");
                StatusBarTooltipState tooltip;
                Require(tooltip.Enter("controlCenter/1", Item(items, "controlCenter").controlTips[1], {}, 100, 400), "tooltip did not enter its target");
                const auto currentTip = Item(BuildStatusBarItems(settings, identical), "controlCenter").controlTips[1];
                Require(!tooltip.Enter("controlCenter/1", currentTip, {}, 800, 400) &&
                    tooltip.text == currentTip && tooltip.readyAt == 500,
                    "stationary tooltip must update volume without resetting its hover delay");
                auto changedTray = data;
                changedTray.tray.front().tip = L"New tray tooltip";
                changedTray.tray.front().application = L"New application label";
                Require(SameStatusBarContent(items, BuildStatusBarItems(settings, changedTray)),
                    "tray tooltip metadata would repaint the bar");
                changedTray.tray.front().pixels.front() ^= 0x0000ffff;
                Require(!SameStatusBarContent(items, BuildStatusBarItems(settings, changedTray)),
                    "a changed tray bitmap would be skipped");
                changedTray = data;
                changedTray.tray.front().state |= NIS_HIDDEN;
                Require(!SameStatusBarContent(items, BuildStatusBarItems(settings, changedTray)),
                    "hiding a pinned tray icon would leave its pixels visible");
                const auto& control = Item(items, "controlCenter");
                Require(control.text.empty() && control.controlTips[2].find(L"58%") != std::wstring::npos,
                    "battery percentage belongs in its tooltip, not in the compact bar");
                auto changedBattery = data; changedBattery.power->batteryPercent = 59;
                Require(SameStatusBarContent(items, BuildStatusBarItems(settings, changedBattery)),
                    "a percentage within the same battery level should update the tooltip without repainting");
                changedBattery.power->batteryPercent = 10;
                const auto low = Item(BuildStatusBarItems(settings, changedBattery), "controlCenter");
                Require(low.controlGlyphs[2] != control.controlGlyphs[2] && low.batteryTone == StatusBarBatteryTone::Low,
                    "low battery must change both the fill level and warning color");
                changedBattery.power->batteryPercent = std::numeric_limits<double>::quiet_NaN();
                Require(Item(BuildStatusBarItems(settings, changedBattery), "controlCenter").controlGlyphs[2] == status_bar_glyphs::kUnknown,
                    "unknown battery level must not be drawn as an empty or full battery");
                changedBattery.power->batteryPercent = 100; changedBattery.power->acPower = true;
                const auto charged = Item(BuildStatusBarItems(settings, changedBattery), "controlCenter");
                Require(charged.batteryTone == StatusBarBatteryTone::FullyCharged &&
                    charged.controlGlyphs[2] == status_bar_glyphs::kBatteryFull &&
                    charged.controlTips[2].find(_LW("statusBar.fullyCharged")) != std::wstring::npos,
                    "AC at 100 percent without charging must show a full battery and a fully charged tooltip");
                changedBattery.power->charging = true;
                const auto chargingFull = Item(BuildStatusBarItems(settings, changedBattery), "controlCenter");
                Require(chargingFull.batteryTone == StatusBarBatteryTone::Charging &&
                    chargingFull.controlGlyphs[2] != charged.controlGlyphs[2] &&
                    chargingFull.controlTips[2].find(_LW("statusBar.charging")) != std::wstring::npos &&
                    !SameStatusBarContent(BuildStatusBarItems(settings, changedBattery),
                        [&] { auto finished = changedBattery; finished.power->charging = false; return BuildStatusBarItems(settings, finished); }()),
                    "finishing a charge at the same percentage must remove the bolt and repaint the battery");
                changedBattery.power->charging = false; changedBattery.power->batteryPercent = 80;
                const auto limited = Item(BuildStatusBarItems(settings, changedBattery), "controlCenter");
                Require(limited.batteryTone == StatusBarBatteryTone::Normal && limited.controlGlyphs[2] != charged.controlGlyphs[2] &&
                    limited.controlTips[2].find(_LW("statusBar.pluggedIn")) != std::wstring::npos,
                    "charge-limited AC state must retain its real fill without claiming charging or full");
                changedBattery.power->batteryPercent = 99.9;
                Require(Item(BuildStatusBarItems(settings, changedBattery), "controlCenter").controlGlyphs[2] != charged.controlGlyphs[2],
                    "rounding the percentage must not claim the full battery level");
                changedBattery.power->batteryPercent = 100; changedBattery.power->acPower = false;
                Require(Item(BuildStatusBarItems(settings, changedBattery), "controlCenter").batteryTone == StatusBarBatteryTone::Normal,
                    "a full battery without confirmed AC must not claim fully charged on AC");
                changedBattery.power->available = false; changedBattery.power->charging = true;
                Require(Item(BuildStatusBarItems(settings, changedBattery), "controlCenter").controlGlyphs[2] == status_bar_glyphs::kUnknown,
                    "unavailable sampling must not retain a stale full charging icon");
                auto gpuSettings = settings; gpuSettings.gpu = true;
                auto gpuData = data; gpuData.gpu->available = false;
                Require(Item(BuildStatusBarItems(gpuSettings, gpuData), "gpu").text.find(L"8%") != std::wstring::npos,
                    "a missing memory channel must not hide valid GPU utilization");
                gpuData.gpu->adapters.front().usageAvailable = false;
                Require(Item(BuildStatusBarItems(gpuSettings, gpuData), "gpu").text.find(L"—") != std::wstring::npos,
                    "missing GPU utilization must not manufacture zero percent");
                gpuData.gpu->adapters.front().usageAvailable = true; gpuData.gpu->adapters.front().usagePercent = 0;
                Require(Item(BuildStatusBarItems(gpuSettings, gpuData), "gpu").text.find(L"0%") != std::wstring::npos,
                    "a measured idle GPU must remain distinguishable from a missing sample");
                gpuData.gpu->adapters.front().usagePercent = std::numeric_limits<double>::quiet_NaN();
                Require(Item(BuildStatusBarItems(gpuSettings, gpuData), "gpu").text.find(L"—") != std::wstring::npos,
                    "a nonfinite GPU sample must not be promoted to a valid zero");
                auto changedNetwork = data; changedNetwork.wifi.reset();
                Require(Item(BuildStatusBarItems(settings, changedNetwork), "controlCenter").controlGlyphs[0] == status_bar_glyphs::kUnknown,
                    "missing Wi-Fi signal must not be replaced by a fabricated signal level");
                changedNetwork.wifi = data.wifi;
                auto& adapters = changedNetwork.wifi->value.object["interfaces"].array;
                const auto secondAdapter = adapters.front(); adapters.push_back(secondAdapter);
                Require(Item(BuildStatusBarItems(settings, changedNetwork), "controlCenter").controlGlyphs[0] == status_bar_glyphs::kUnknown,
                    "multiple connected Wi-Fi adapters cannot invent a primary adapter signal");
                changedNetwork.network->transport = "ethernet";
                Require(Item(BuildStatusBarItems(settings, changedNetwork), "controlCenter").controlGlyphs[0] == status_bar_glyphs::kEthernet,
                    "a wired connection must not depend on Wi-Fi availability");
                changedNetwork.network->transport = "cellular";
                Require(Item(BuildStatusBarItems(settings, changedNetwork), "controlCenter").controlGlyphs[0] == status_bar_glyphs::kNetwork,
                    "other connected transports must not impersonate Wi-Fi");
                changedNetwork.network->connectivity = "none";
                changedNetwork.wifi = WifiFixture(false, {});
                Require(Item(BuildStatusBarItems(settings, changedNetwork), "controlCenter").controlGlyphs[0] == status_bar_glyphs::kWifiOff,
                    "confirmed radio-off state has a separate icon from a disconnected network");
                changedNetwork.wifi->value.object["interfaces"].array[0].object.erase("enabled");
                Require(Item(BuildStatusBarItems(settings, changedNetwork), "controlCenter").controlGlyphs[0] == status_bar_glyphs::kOffline,
                    "unknown radio state must not claim that Wi-Fi is switched off");
                auto notification = data; notification.notifications.unreadCount = 2;
                const auto pending = Item(BuildStatusBarItems(settings, notification), "notifications");
                notification.notifications.unreadCount.reset(); notification.notifications.totalCount = 2;
                const auto present = Item(BuildStatusBarItems(settings, notification), "notifications");
                Require(pending.glyph != present.glyph && pending.tip != present.tip,
                    "total notifications must not be reported as unread notifications");
                notification.notifications.quiet = true;
                Require(Item(BuildStatusBarItems(settings, notification), "notifications").glyph == status_bar_glyphs::kNotificationsQuiet,
                    "Do not disturb must override the notification count indicator");
                notification.notifications = {};
                Require(Item(BuildStatusBarItems(settings, notification), "notifications").glyph != Item(items, "notifications").glyph,
                    "an unknown notification count must not appear as a confirmed empty notification center");
                Require(control.controlTips[0] != control.controlTips[1] && control.controlTips[1] != control.controlTips[2],
                    "control center glyphs must expose separate network, volume and battery tips");
                identical.audio->volume = .9;
                Require(!SameStatusBarContent(items, BuildStatusBarItems(settings, identical)), "volume level must change its speaker glyph");
            }
            std::optional<std::size_t> hover;
            if (preset == "hover" || preset == "high-contrast" || preset == "merged")
                hover = static_cast<std::size_t>(&Item(items, "controlCenter") - items.data());
            StatusBarPalette palette{preset == "high-contrast", D2D1::ColorF(0x000000), D2D1::ColorF(0xffffff),
                D2D1::ColorF(0xffff00), D2D1::ColorF(0x000000)};
            ComPtr<ID2D1DeviceContext> context; Require(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context));
            const auto size = D2D1::SizeU(static_cast<UINT>(request.canvasWidth), static_cast<UINT>(request.canvasHeight));
            const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
            ComPtr<ID2D1Bitmap1> target;
            const auto targetProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96);
            Require(context->CreateBitmap(size, nullptr, 0, targetProperties, &target));
            context->SetTarget(target.Get()); context->SetDpi(96, 96);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
            if (!request.transparent && !request.contentOnly)
            {
                ComPtr<ID2D1Bitmap> wallpaper;
                Require(context->CreateBitmap(size, stage.pixels.data(), static_cast<UINT>(stage.width * 4), D2D1::BitmapProperties(format), &wallpaper));
                context->DrawBitmap(wallpaper.Get());
            }
            context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(left), static_cast<float>(top)));
            if (palette.highContrast)
            {
                ComPtr<ID2D1SolidColorBrush> brush; Require(context->CreateSolidColorBrush(palette.background, &brush));
                context->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), brush.Get());
            }
            else if (!request.contentOnly) background(context.Get(), {0, 0, width, height}, appearance, scale, settings.position);
            const auto contentResult = DrawStatusBarContent(context.Get(), text, items, static_cast<UINT>(width), static_cast<UINT>(height),
                scale, appearance, palette, hover, false, 0, preset == "merged");
            const auto drawResult = context->EndDraw(); context->SetTarget(nullptr); Require(contentResult); Require(drawResult);
            CheckLayout(text, items, width, height, scale, preset == "merged");
            if(preset=="normal")
            {
                // The production rendered rectangles feed the same resolver
                // used by drag previews and commits. Same presentation key
                // must not turn an icon-to-icon reorder into an unpin.
                const auto pinned=std::find_if(items.begin(),items.end(),[](const auto& item){return item.icon&&!IsRectEmpty(&item.bounds);});
                Require(pinned!=items.end(),"tray reorder fixture has no visible pinned icon");
                const auto insertion=ResolveStatusBarTrayDrop(items,{pinned->bounds.left+1,height/2},5,2);
                Require(insertion&&insertion->pinned&&insertion->before==pinned->icon->key,"dropping on a pinned icon must reorder instead of unpinning");
                auto dropped=settings;tray::Snapshot snapshot;snapshot.icons=data.tray;
                const auto key=data.tray.back().key;
                Require(tray::PlaceIcon(dropped,snapshot,key,insertion->pinned,insertion->before)&&
                    dropped.trayOrder.front()==data.tray.back().persistentKey&&dropped.pinnedTrayItems.size()==2,
                    "tray reorder must preserve both pins and persist the requested order");
                const auto saved=dropped;
                snapshot.icons.erase(snapshot.icons.begin());
                Require(!tray::PlaceIcon(dropped,snapshot,key,true,insertion->before)&&dropped==saved,
                    "a removed insertion target must cancel without changing tray order or pins");
                snapshot.icons=data.tray;
                const auto& overflow=Item(items,"tray");
                const auto folded=ResolveStatusBarTrayDrop(items,{overflow.bounds.left+1,height/2},5,2);
                Require(folded&&!folded->pinned&&tray::PlaceIcon(dropped,snapshot,key,folded->pinned,folded->before)&&
                    dropped.pinnedTrayItems.size()==1,"only the overflow button must unpin the dragged icon");
                // Exercise the real renderer's fallback paths without adding
                // CLI presets or altering the exported normal frame.
                const auto compactLayout = [&](bool date, bool merged, int logicalWidth, float testScale) {
                    auto compactSettings = settings; compactSettings.clock = date;
                    compactSettings.cpu = compactSettings.memory = compactSettings.gpu = compactSettings.traffic = true;
                    auto compact = BuildStatusBarItems(compactSettings, data);
                    Require(std::any_of(compact.begin(), compact.end(), [](const auto& item) { return item.key == "clock"; }) == date,
                        "the existing date visibility flag must be respected");
                    const UINT testWidth = static_cast<UINT>(std::lround(logicalWidth * testScale));
                    const UINT testHeight = static_cast<UINT>(std::lround((merged ? 64 : 32) * testScale));
                    ComPtr<ID2D1Bitmap1> scratch;
                    Require(context->CreateBitmap(D2D1::SizeU(testWidth, testHeight), nullptr, 0, targetProperties, &scratch));
                    context->SetTarget(scratch.Get()); context->SetTransform(D2D1::Matrix3x2F::Identity());
                    context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
                    const auto content = DrawStatusBarContent(context.Get(), text, compact, testWidth, testHeight,
                        testScale, appearance, palette, {}, false, 0, merged);
                    const auto end = context->EndDraw(); context->SetTarget(nullptr); Require(content); Require(end);
                    const auto dockCenter = MergedStatusBarCenter(static_cast<LONG>(testWidth), static_cast<LONG>(testHeight), testScale);
                    const bool notificationFits = !merged || static_cast<LONG>(testWidth) - dockCenter.right >=
                        static_cast<LONG>(std::ceil(32.f * testScale) + std::ceil(92.f * testScale));
                    CheckLayout(text, compact, static_cast<int>(testWidth), static_cast<int>(testHeight), testScale, merged, notificationFits);
                    if (date && (logicalWidth == 320 || (merged && logicalWidth <= 640)))
                        Require(IsRectEmpty(&Item(compact, "clock").bounds),
                            "crowded date must yield as a whole while its notification target remains available");
                };
                compactLayout(false, false, 640, scale);
                compactLayout(false, true, 640, scale);
                compactLayout(true, true, 640, scale);
                compactLayout(true, true, 480, 1.5f * scale);
                compactLayout(true, true, 1920, 1.25f * scale);
                compactLayout(true, true, 1920, 3.f);
                compactLayout(true, true, 320, 3.f);
                compactLayout(true, true, 240, 3.f);
                compactLayout(true, false, 320, scale);
            }
            if (preset == "information" && width >= 1800 * scale)
                for (const auto* key : {"cpu", "memory", "gpu", "traffic"})
                    Require(!IsRectEmpty(&Item(items, key).bounds), "information disappeared despite sufficient status bar width");
            if (preset == "information") infoItems = items;
            if (preset == "updated")
            {
                Require(infoItems.size() == items.size(), "sample changed status bar item count");
                for (std::size_t i = 0; i < items.size(); ++i)
                    Require(EqualRect(&infoItems[i].bounds, &items[i].bounds) != FALSE, "sample changed a fixed status bar hit width");
            }
            ComPtr<ID2D1Bitmap1> readback;
            const auto readProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96);
            Require(context->CreateBitmap(size, nullptr, 0, readProperties, &readback));
            Require(readback->CopyFromBitmap(nullptr, target.Get(), nullptr));
            D2D1_MAPPED_RECT mapped{}; Require(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(request.canvasWidth) * request.canvasHeight);
            for (int y = 0; y < request.canvasHeight; ++y)
                std::memcpy(pixels.data() + static_cast<std::size_t>(y) * request.canvasWidth,
                    mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(request.canvasWidth) * 4);
            readback->Unmap();
            if (preset == "full" || preset == "charging")
            {
                // User-visible contract: green is the charge inside the
                // battery, while the outline remains the neutral foreground.
                const auto& control = Item(items, "controlCenter");
                const float glyphLeft = left + control.bounds.left + 64 * scale;
                const float glyphTop = top + (height - 20 * scale) / 2;
                const int expected = appearance.contentTheme == 1 ? 0x20 : 0xf4;
                int neutralOutline = 0, greenFill = 0;
                for (int py = static_cast<int>(glyphTop + 7 * scale); py < glyphTop + 13 * scale; ++py)
                    for (int px = static_cast<int>(glyphLeft); px < glyphLeft + 17 * scale; ++px)
                    {
                        const auto pixel = pixels[static_cast<std::size_t>(py) * request.canvasWidth + px];
                        const int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
                        if (px < glyphLeft + 2 * scale && (pixel >> 24) > 32 &&
                            std::abs(r - expected) < 70 && std::abs(r - g) < 12 && std::abs(g - b) < 12) ++neutralOutline;
                        if (px >= glyphLeft + 3 * scale && px < glyphLeft + 6 * scale &&
                            g > r + 25 && g > b + 15) ++greenFill;
                    }
                Require(neutralOutline > 0 && greenFill > 0,
                    "charging and full batteries need a neutral outline and a green interior");
            }
            if (preset == "normal") normalPixels = pixels;
            if (preset == "repeat") Require(pixels == normalPixels, "identical status bar input produces visible frame differences");
            const auto path = request.outputDirectory / ("status-bar-" + preset + ".png");
            Require(preview_png::Save(path, request.canvasWidth, request.canvasHeight, pixels, result.error), "cannot save status bar preview PNG");
            result.outputs.push_back({request.component, preset, path, false, false, false, false, false, false, 0, width, height, left, top});
        }
        result.ok = true; result.stage = "complete";
    }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
}
