#include "status_bar_preview.h"
#include "status_bar_presentation.h"
#include "status_bar_layout.h"
#include "preview_png_writer.h"
#include "widget_preview_stage.h"
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <stdexcept>

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
void CheckLayout(IDWriteFactory* text, const std::vector<StatusBarItem>& items, int width, int height, float scale, bool merged)
{
    const auto& clock = Item(items, "clock");
    if (!merged) Require(std::abs(clock.bounds.left + clock.bounds.right - width) <= 1, "clock is not centered on the complete bar");
    Require(!IsRectEmpty(&Item(items, "controlCenter").bounds) && !IsRectEmpty(&Item(items, "notifications").bounds),
        "narrow status bar removed essential controls");
    std::vector<RECT> rectangles;
    ComPtr<IDWriteTextFormat> font;
    Require(text->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
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
            Require(item.left && r.right < clock.bounds.left && r.left >= Item(items, "quickSearch").bounds.right,
                "system information must follow the launch buttons on the left");
            if (item.key != "memory")
                Require(r.right - r.left <= (item.key == "traffic" ? 152 : 68) * scale + 1,
                    "system information wastes horizontal space");
        }
        Require(r.left >= 0 && r.top == 0 && r.right <= width && r.bottom == height, "status bar item escaped its viewport");
        Require(HitTestStatusBarItems(items, {(r.left + r.right) / 2, height / 2}) == i,
            "status bar hit target does not match its rendered item");
        for (const auto& previous : rectangles)
        { RECT overlap{}; Require(!IntersectRect(&overlap, &r, &previous), "status bar zones overlap"); }
        rectangles.push_back(r);
        if (item.text.empty() || item.icon) continue;
        ComPtr<IDWriteTextLayout> layout;
        Require(text->CreateTextLayout(item.text.c_str(), static_cast<UINT32>(item.text.size()), font.Get(), 4000, 100, &layout));
        DWRITE_TEXT_METRICS metrics{}; Require(layout->GetMetrics(&metrics));
        const float reserved = item.key == "controlCenter" ? 94.f * scale : 0.f;
        Require(metrics.widthIncludingTrailingWhitespace <= r.right - r.left - reserved + 1,
            "status bar fixed width clips visible text");
    }
    Require(!HitTestStatusBarItems(items, {0, height / 2}), "blank bar padding must route to dismissal");
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
        for (const std::string preset : {"normal", "information", "updated", "hover", "full", "charging", "unavailable", "bottom", "narrow", "scaled", "high-contrast", "repeat", "merged"})
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
            if (preset == "full") { data.power->batteryPercent = 100; data.power->acPower = true; }
            if (preset == "charging") { data.power->charging = true; data.power->acPower = true; }
            if (preset == "unavailable")
            { data.power->available = false; data.network->available = false; data.audio->available = false; }
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
                Require(tooltip.Enter("controlCenter", Item(items, "controlCenter").tip), "tooltip did not enter its target");
                const auto saved = tooltip.text;
                Require(!tooltip.Enter("controlCenter", Item(BuildStatusBarItems(settings, identical), "controlCenter").tip) && tooltip.text == saved,
                    "sampling would reset the visible tooltip");
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
                Require(control.controlTips[0] != control.controlTips[1] && control.controlTips[1] != control.controlTips[2],
                    "control center glyphs must expose separate network, volume and battery tips");
                identical.audio->volume = .9;
                Require(!SameStatusBarContent(items, BuildStatusBarItems(settings, identical)), "volume level must change its speaker glyph");
            }
            std::optional<std::size_t> hover;
            if (preset == "hover" || preset == "high-contrast")
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
