#include "system/status_bar/status_bar_view.h"
#include "system/status_bar/status_bar_glyphs.h"
#include "system/status_bar/status_bar_interaction.h"
#include "common/l10n.h"
#include <d3d11.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <cstring>

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

// Replace only DComp's atlas allocation/copy boundary. Production damage,
// update-offset handling, clipping, retained layout and D2D drawing stay real.
class AtlasSurface final : public IDCompositionSurface
{
public:
    AtlasSurface(ID2D1DeviceContext* context, ID2D1Bitmap1* retained) : context_(context), retained_(retained)
    {
        const auto size = retained->GetPixelSize();
        Check(SUCCEEDED(context_->CreateBitmap(D2D1::SizeU(size.width + 32, size.height + 24), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &atlas_)),
            "hover atlas initializes");
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override
    {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IDCompositionSurface)) return E_NOINTERFACE;
        *value = static_cast<IDCompositionSurface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE BeginDraw(const RECT* region, REFIID iid, void** value, POINT* offset) override
    {
        if (!region || !value || !offset || drawing_) return E_INVALIDARG;
        const auto size = retained_->GetPixelSize();
        if (region->left < 0 || region->top < 0 || region->right > static_cast<LONG>(size.width) ||
            region->bottom > static_cast<LONG>(size.height) || IsRectEmpty(region)) return E_INVALIDARG;
        region_ = *region; *offset = origin_;
        const auto result = context_->QueryInterface(iid, value);
        if (FAILED(result)) return result;
        updates.push_back(*region); drawing_ = true;
        context_->SetTarget(atlas_.Get()); context_->SetTransform(D2D1::Matrix3x2F::Identity());
        context_->BeginDraw(); context_->Clear(D2D1::ColorF(0xff00ff));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndDraw() override
    {
        if (!drawing_) return E_UNEXPECTED;
        drawing_ = false;
        const auto result = context_->EndDraw(); context_->SetTarget(nullptr);
        if (FAILED(result)) return result;
        const D2D1_POINT_2U destination{static_cast<UINT32>(region_.left), static_cast<UINT32>(region_.top)};
        const D2D1_RECT_U source{static_cast<UINT32>(origin_.x), static_cast<UINT32>(origin_.y),
            static_cast<UINT32>(origin_.x + region_.right - region_.left),
            static_cast<UINT32>(origin_.y + region_.bottom - region_.top)};
        return retained_->CopyFromBitmap(&destination, atlas_.Get(), &source);
    }
    HRESULT STDMETHODCALLTYPE SuspendDraw() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ResumeDraw() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Scroll(const RECT*, const RECT*, int, int) override { return E_NOTIMPL; }
    std::vector<RECT> updates;
private:
    ULONG references_ = 1;
    bool drawing_ = false;
    RECT region_{};
    const POINT origin_{13, 9}; // Deliberately unrelated to either target's position.
    ComPtr<ID2D1DeviceContext> context_;
    ComPtr<ID2D1Bitmap1> retained_, atlas_;
};

void CheckHoverUpdates(ID2D1Device* device, ID2D1DeviceContext* context, IDWriteFactory* text,
    const StatusBarSnapshot& data, const std::vector<std::string>& pinned)
{
    std::uint64_t updatedPixels = 0, fullPixels = 0;
    for (const bool merged : {false, true}) for (const bool highContrast : {false, true})
    for (const bool lightTheme : {false, true})
    for (const float scale : {1.f, 1.5f, 2.f}) for (const UINT dipWidth : {640u, 1920u})
    {
        const UINT width = static_cast<UINT>(static_cast<float>(dipWidth) * scale),
            height = static_cast<UINT>((merged ? 64.f : 32.f) * scale);
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        ComPtr<ID2D1Bitmap1> retained, reference, readable;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &retained)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, properties, &reference)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
                D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                    properties.pixelFormat), &readable)), "hover comparison bitmaps initialize");
        StatusBarSettings settings; settings.pinnedTrayItems = pinned; settings.batteryPercentage = true;
        auto items = BuildStatusBarItems(settings, data);
        PersonalizationSettings appearance; appearance.contentTheme = lightTheme ? 1 : 0;
        const StatusBarPalette palette{highContrast, D2D1::ColorF(0xffffff), D2D1::ColorF(0x000000),
            D2D1::ColorF(0x003399), D2D1::ColorF(0xffffff)};
        StatusBarTrayScroll scrolling;
        const auto fullPaint = [&](ID2D1Bitmap1* target, std::vector<StatusBarItem>& content,
            std::optional<std::size_t> hover, StatusBarTrayScroll& scroll) {
            context->SetTarget(target); context->SetDpi(96, 96);
            context->SetTransform(D2D1::Matrix3x2F::Identity());
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
            const auto result = DrawStatusBarContent(context, text, content, width, height, scale,
                appearance, palette, hover, true, 0, merged, true, &scroll);
            const auto ended = context->EndDraw(); context->SetTarget(nullptr);
            Check(SUCCEEDED(result) && SUCCEEDED(ended), "full reference frame renders");
        };
        const auto pixels = [&](ID2D1Bitmap1* target) {
            Check(SUCCEEDED(readable->CopyFromBitmap(nullptr, target, nullptr)), "hover pixels copy to CPU");
            D2D1_MAPPED_RECT mapped{};
            Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped)), "hover pixels map");
            std::vector<std::uint32_t> result(static_cast<std::size_t>(width) * height);
            for (UINT row = 0; row < height; ++row)
                std::memcpy(result.data() + static_cast<std::size_t>(row) * width,
                    mapped.bits + static_cast<std::size_t>(row) * mapped.pitch, static_cast<std::size_t>(width) * 4);
            readable->Unmap(); return result;
        };
        fullPaint(retained.Get(), items, {}, scrolling);
        // Retain a partially scrolled icon at the viewport edge as well as
        // complete controls; fading/clipping must match a full redraw exactly.
        scrolling.Move(scrolling.Maximum() / 2 + (scrolling.Maximum() ? 7 : 0));
        fullPaint(retained.Get(), items, {}, scrolling);
        ComPtr<AtlasSurface> surface; surface.Attach(new AtlasSurface(context, retained.Get()));
        std::vector<std::optional<std::size_t>> sequence;
        const auto add = [&](std::string_view key) {
            const auto* item = Find(items, key);
            if (item && !IsRectEmpty(&item->bounds)) sequence.push_back(static_cast<std::size_t>(item - items.data()));
        };
        add("menu"); add("clock"); add("controlCenter");
        for (std::size_t index = 0; index < items.size(); ++index)
            if (items[index].icon && !IsRectEmpty(&items[index].bounds)) { sequence.push_back(index); break; }
        for (std::size_t index = items.size(); index-- > 0;)
            if (items[index].icon && !IsRectEmpty(&items[index].bounds)) { sequence.push_back(index); break; }
        add("showDesktop"); sequence.push_back({}); add("menu"); add("menu"); sequence.push_back({});
        std::optional<std::size_t> previous;
        for (const auto hover : sequence)
        {
            const auto boundsBefore = items;
            const auto scrollBefore = scrolling;
            const auto updateStart = surface->updates.size();
            const auto damage = StatusBarHoverDamage(items, previous, hover, width, height);
            Check(SUCCEEDED(DrawStatusBarHoverUpdate(surface.Get(), text, items, damage, width, height, scale,
                appearance, palette, hover, true, 0, merged, scrolling)), "production hover update renders both damage regions");
            for (std::size_t index = 0; index < items.size(); ++index)
                Check(EqualRect(&items[index].bounds, &boundsBefore[index].bounds) &&
                    EqualRect(&items[index].contentBounds, &boundsBefore[index].contentBounds), "hover retains arranged hit geometry");
            Check(EqualRect(&scrolling.viewport, &scrollBefore.viewport) && scrolling.offset == scrollBefore.offset,
                "hover retains tray scroll position and viewport");
            auto expected = BuildStatusBarItems(settings, data); auto expectedScroll = scrollBefore;
            fullPaint(reference.Get(), expected, hover, expectedScroll);
            const auto observed = pixels(retained.Get()), wanted = pixels(reference.Get());
            bool equivalent = true;
            for (std::size_t pixel = 0; pixel < observed.size(); ++pixel)
            {
                if (observed[pixel] == wanted[pixel]) continue;
                const POINT location{static_cast<LONG>(pixel % width), static_cast<LONG>(pixel / width)};
                // Static repaint controls with shifted atlas offsets show
                // 1-2/255 variation only in cropped opacity-mask pixels.
                // Permit that bounded variation; all other pixels stay exact.
                if (highContrast || !scrollBefore.Maximum() || !PtInRect(&scrollBefore.viewport, location))
                    equivalent = false;
                for (unsigned shift = 0; shift < 32; shift += 8)
                    if (std::abs(static_cast<int>((observed[pixel] >> shift) & 255) -
                        static_cast<int>((wanted[pixel] >> shift) & 255)) > 2) equivalent = false;
            }
            if (!equivalent)
            {
                std::cerr << "hover mismatch merged=" << merged << " hc=" << highContrast << " light=" << lightTheme << " scale=" << scale
                    << " width=" << width << " previous=" << previous.value_or(items.size())
                    << " hover=" << hover.value_or(items.size()) << '\n';
                Check(false, "incremental enter/switch/leave pixels differ from a complete reference frame");
            }
            for (std::size_t index = updateStart; index < surface->updates.size(); ++index)
            {
                const auto& region = surface->updates[index];
                updatedPixels += static_cast<std::uint64_t>(region.right - region.left) * (region.bottom - region.top);
                Check(region.right - region.left < static_cast<LONG>(width), "hover never updates the complete strip");
            }
            if (previous == hover) Check(surface->updates.size() == updateStart, "stationary hover submits no surface update");
            fullPixels += static_cast<std::uint64_t>(width) * height;
            previous = hover;
        }
    }
    // Exercise the same production update boundary on a real initialized DComp
    // surface too. No target HWND or input-desktop interaction is required.
    ComPtr<IDCompositionDesktopDevice> composition;
    ComPtr<IDCompositionSurface> native;
    Check(SUCCEEDED(DCompositionCreateDevice2(device, IID_PPV_ARGS(&composition))) &&
        SUCCEEDED(composition->CreateSurface(640, 64, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &native)),
        "native incremental surface initializes");
    auto nativeItems = BuildStatusBarItems(StatusBarSettings{}, data); StatusBarTrayScroll nativeScroll;
    POINT origin{}; ComPtr<ID2D1DeviceContext> nativeContext;
    Check(SUCCEEDED(native->BeginDraw(nullptr, IID_PPV_ARGS(&nativeContext), &origin)), "native first frame covers the full surface");
    nativeContext->SetDpi(96, 96); nativeContext->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(origin.x), static_cast<float>(origin.y)));
    nativeContext->Clear(D2D1::ColorF(0, 0.f));
    const auto initial = DrawStatusBarContent(nativeContext.Get(), text, nativeItems, 640, 64, 1.f,
        PersonalizationSettings{}, StatusBarPalette{}, {}, false, 0, true, false, &nativeScroll);
    nativeContext.Reset();
    Check(SUCCEEDED(initial) && SUCCEEDED(native->EndDraw()), "native complete frame is initialized before incremental draws");
    const auto* menu = Find(nativeItems, "menu"); Check(menu != nullptr, "native menu target exists");
    const auto menuIndex = static_cast<std::size_t>(menu - nativeItems.data());
    for (const auto hover : {std::optional<std::size_t>(menuIndex), std::optional<std::size_t>{}})
    {
        const auto damage = StatusBarHoverDamage(nativeItems, hover ? std::optional<std::size_t>{} : menuIndex,
            hover, 640, 64);
        Check(SUCCEEDED(DrawStatusBarHoverUpdate(native.Get(), text, nativeItems, damage, 640, 64, 1.f,
            PersonalizationSettings{}, StatusBarPalette{}, hover, false, 0, true, nativeScroll)) &&
            SUCCEEDED(composition->Commit()), "real DComp accepts the production hover enter/leave updates");
    }
    Check(updatedPixels < fullPixels / 4, "hover repaint area remains bounded to changed controls");
    std::cout << "Hover pixel equivalence and native DComp passed; updated pixels=" << updatedPixels
        << " equivalent full-frame pixels=" << fullPixels << '\n';
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
    CheckHoverUpdates(device.Get(), context.Get(), text.Get(), data, pinned);
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
