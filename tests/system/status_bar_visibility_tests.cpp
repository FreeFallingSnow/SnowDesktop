#include "system/status_bar/status_bar_view.h"
#include "system/status_bar/status_bar_glyphs.h"
#include "system/status_bar/status_bar_interaction.h"
#include "common/l10n.h"
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

// Replace only the embedded icon-font resource boundary. Production item
// construction, DirectWrite measurement, drawing, hit testing and invocation
// remain real. These assertions validate geometry/actions, not icon artwork.
IDWriteTextFormat* CreateFluentTextFormat(IDWriteFactory* factory, float size)
{
    IDWriteTextFormat* format = nullptr;
    if (FAILED(factory->CreateTextFormat(L"Segoe UI Symbol", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return nullptr;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return format;
}
namespace
{
using namespace snowdesktop;
using Microsoft::WRL::ComPtr;
void Check(bool passed, const char* message)
{
    if (!passed) { std::cerr << message << '\n'; std::exit(1); }
}
const StatusBarItem* Find(const std::vector<StatusBarItem>& items, std::string_view key)
{
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.key == key; });
    return found == items.end() ? nullptr : &*found;
}
}
int main(int argc, char** argv)
{
    Check(argc == 2, "expected repository root");
    Locale::Instance().Init((std::filesystem::path(argv[1]) / L"lang").c_str());
    Locale::Instance().SetLanguage("en-US");
    ComPtr<ID3D11Device> gpu; ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory; ComPtr<ID2D1Device> device; ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDWriteFactory> text;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &gpu, nullptr, nullptr)) &&
        SUCCEEDED(gpu.As(&dxgi)) && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
        SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)) &&
        SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(text.GetAddressOf()))), "offscreen production renderer initializes");
    StatusBarSnapshot data; data.clock = L"2026/10/23   16:25";
    data.power.emplace(); data.power->available = true; data.power->batteryPercent = 58;
    std::vector<std::string> pinned;
    for (unsigned index = 0; index < 24; ++index)
    {
        tray::Icon icon;
        icon.key = icon.persistentKey = "visibility-test-" + std::to_string(index);
        icon.width = icon.height = 1; icon.pixels = {0xff33aa55};
        pinned.push_back(icon.persistentKey);
        data.tray.push_back(std::move(icon));
    }
    const auto defaults = BuildStatusBarItems(StatusBarSettings{}, data);
    Check(Find(defaults, "clock") && Find(defaults, "notifications") && Find(defaults, "controlCenter"),
        "clock, notifications and grouped control center start visible");
    for (const int mode : {0, 1, 2}) for (const bool clock : {false, true})
    for (const bool notifications : {false, true}) for (const bool merged : {false, true})
    for (const bool rightAligned : {false, true}) for (const float scale : {1.f, 2.f})
    for (const UINT dipWidth : {640u, 1920u})
    {
        StatusBarSettings settings;
        settings.controlCenter = mode != 2; settings.controlCenterSingleIcon = mode == 1;
        settings.clock = clock; settings.notifications = notifications; settings.batteryPercentage = true;
        settings.pinnedTrayItems = pinned;
        auto items = BuildStatusBarItems(settings, data);
        Check((Find(items, "clock") != nullptr) == clock && (Find(items, "notifications") != nullptr) == notifications &&
            (Find(items, "controlCenter") != nullptr) == (mode != 2), "hidden items have no rendered or keyboard target");
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale),
            height = static_cast<UINT>((merged ? 64.f : 32.f) * scale);
        ComPtr<ID2D1Bitmap1> target;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &target)), "render target initializes");
        context->SetTarget(target.Get()); context->SetDpi(96, 96);
        context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
        const auto result = DrawStatusBarContent(context.Get(), text.Get(), items, width, height, scale,
            PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, merged, rightAligned);
        const auto drawn = context->EndDraw(); context->SetTarget(nullptr);
        Check(SUCCEEDED(result) && SUCCEEDED(drawn), "visibility combinations render through the production layout");
        Check(!IsRectEmpty(&Find(items, "menu")->bounds) && !IsRectEmpty(&Find(items, "taskView")->bounds),
            "hiding the date and notifications preserves left-side controls");
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const auto& item = items[i];
            if (IsRectEmpty(&item.bounds)) continue;
            const POINT point{(item.bounds.left + item.bounds.right) / 2, (item.bounds.top + item.bounds.bottom) / 2};
            Check(HitTestStatusBarItems(items, point) == i, "visible targets remain distinct");
            if (item.action == StatusBarAction::ControlCenter)
            {
                const auto invocation = ResolveStatusBarInvocation(items, i, false);
                Check(invocation && invocation->action == StatusBarAction::ControlCenter,
                    "both control-center presentations open the same panel");
                if (mode == 1)
                    Check(item.glyph == status_bar_glyphs::kControls && item.controlGlyphs[0].empty() &&
                        item.controlGlyphs[1].empty() && item.controlGlyphs[2].empty() && item.batteryPercentageText.empty() &&
                        item.bounds.right - item.bounds.left == static_cast<LONG>(28 * scale),
                        "single-icon mode has one icon and no device or percentage placeholders");
            }
            for (std::size_t j = i + 1; j < items.size(); ++j)
            {
                RECT intersection{};
                Check(!IntersectRect(&intersection, &item.bounds, &items[j].bounds), "visible targets do not overlap");
            }
        }
    }
    std::cout << "192 production visibility/layout combinations passed\n";
}
