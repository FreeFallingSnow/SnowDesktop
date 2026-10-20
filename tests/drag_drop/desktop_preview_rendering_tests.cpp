#include "app/drag_drop/desktop_preview_source.h"
#include "icons/large_icon_renderer.h"
#include "ui/preview/preview_png_writer.h"

#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace snowdesktop;
using Microsoft::WRL::ComPtr;
void Expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Hr(HRESULT result, const char* operation)
{
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}

struct Canvas
{
    static constexpr int width = 480, height = 360;
    ComPtr<IWICBitmap> surface;
    ComPtr<ID2D1RenderTarget> target;
    Canvas()
    {
        ComPtr<IWICImagingFactory> images;
        Hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&images)), "WIC factory");
        Hr(images->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &surface), "WIC surface");
        ComPtr<ID2D1Factory> drawing;
        Hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, drawing.GetAddressOf()), "D2D factory");
        Hr(drawing->CreateWicBitmapRenderTarget(surface.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &target), "D2D WIC target");
    }
    std::vector<std::uint32_t> Draw(const std::vector<DesktopItem>& items, const std::vector<DragSourceEntry>& sources, bool valid)
    {
        const desktop_preview_source::Lookup lookup(items, true);
        target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0.f));
        for (size_t index = 0; index < sources.size(); ++index)
        {
            const auto* item = lookup.Find(sources[index]);
            if (!item) continue;
            const RECT frame = sources.size() == 1 ? RECT{100, 60, 380, 260}
                : index == 0 ? RECT{20, 40, 220, 320} : RECT{260, 40, 460, 320};
            const auto config = large_icon_render_rules::ResolveComponentRadius(*item->largeIcon, 28);
            large_icon_renderer::DrawPlacementPreview(target.Get(), config, frame, 1, valid, &frame);
        }
        Hr(target->EndDraw(), "production placement preview");
        std::vector<std::uint32_t> pixels(width * height);
        Hr(surface->CopyPixels(nullptr, width * 4, width * height * 4, reinterpret_cast<BYTE*>(pixels.data())), "read placement preview pixels");
        return pixels;
    }
};

std::vector<DesktopItem> Items(int shape)
{
    std::vector<DesktopItem> items(2);
    for (size_t index = 0; index < items.size(); ++index)
    {
        auto& item = items[index]; item.layoutKey = index == 0 ? L"A" : L"B";
        item.name = item.layoutKey; item.gridCell = {L"page", static_cast<int>(index * 3), 0}; item.gridSpan = {2, 2}; item.largeIcon.emplace();
        item.largeIcon->columns = item.largeIcon->rows = 2;
        item.largeIcon->shape = (shape + static_cast<int>(index)) % 6;
        item.largeIcon->radius = index == 0 ? 25 : 11;
        item.largeIcon->followComponentRadius = index == 0;
        item.largeIcon->regularHexagon = true;
    }
    return items;
}
DragSourceEntry Source(size_t index)
{
    DragSourceEntry source; source.kind = DropSourceKind::DesktopIcon;
    source.desktopKey = index == 0 ? L"A" : L"B"; source.desktopIndex = index;
    source.sourceIndex = index == 0 ? 9 : 2; source.originalSpan = {2, 2};
    source.originalCell = {L"page", static_cast<int>(index * 3), 0};
    return source;
}
void Save(const std::filesystem::path& directory, const std::wstring& name, const std::vector<std::uint32_t>& pixels)
{
    if (directory.empty()) return;
    std::string error;
    const bool saved = preview_png::Save(directory / name, Canvas::width, Canvas::height, pixels, error);
    Expect(saved, error.c_str());
}
void Run(std::wstring_view section, const std::filesystem::path& output)
{
    Expect(section == L"all" || section == L"permutation" || section == L"missing" || section == L"ordinary", "known nonempty preview rendering section");
    if (!output.empty()) std::filesystem::create_directories(output);
    size_t scenes = 0;
    if (section == L"all" || section == L"permutation")
    {
        Canvas canvas;
        for (int shape = 0; shape < 6; ++shape)
        for (size_t count : {size_t{1}, size_t{2}})
        for (bool valid : {false, true})
        {
            auto items = Items(shape); std::vector<DragSourceEntry> sources{Source(0)};
            if (count == 2) sources.push_back(Source(1));
            const auto before = canvas.Draw(items, sources, valid);
            Expect(std::any_of(before.begin(), before.end(), [](auto pixel) { return (pixel >> 24) != 0; }), "actual placement renderer publishes visible source silhouettes");
            std::swap(items[0], items[1]);
            const auto reordered = canvas.Draw(items, sources, valid);
            const auto name = std::to_wstring(shape) + L"-" + std::to_wstring(count) + (valid ? L"-accepted" : L"-rejected");
            Save(output, name + L"-before.png", before); Save(output, name + L"-reordered.png", reordered);
            Expect(before == reordered,
                "unchanged drag source keeps its own placement-preview silhouette after model enumeration reorders");
            ++scenes;
        }
    }
    if (section == L"all" || section == L"missing")
    {
        auto items = Items(2); const auto source = Source(0); items.erase(items.begin());
        Expect(desktop_preview_source::Lookup(items, true).Find(source) == nullptr,
            "a removed drag source cannot borrow another icon's placement-preview style");
        ++scenes;
    }
    if (section == L"all" || section == L"ordinary")
    {
        auto items = Items(2); items[0].largeIcon.reset(); items[0].gridSpan = {1, 1};
        auto source = Source(0); source.originalSpan = {1, 1};
        Expect(desktop_preview_source::Lookup(items, true).Find(source) == nullptr, "ordinary source starts without a large-icon preview");
        std::swap(items[0], items[1]);
        Expect(desktop_preview_source::Lookup(items, true).Find(source) == nullptr,
            "ordinary drag sources never inherit an unrelated large-icon silhouette after enumeration reorders");
        ++scenes;
    }
    Expect(scenes != 0, "preview rendering executes a nonempty scene selection");
    std::cout << scenes << " actual preview source and WIC placement rendering scenes passed; no desktop window\n";
}
}

int wmain(int argc, wchar_t** argv)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    try
    {
        Hr(apartment, "preview test apartment");
        Expect(argc <= 3, "preview diagnostics accept a section and optional output directory");
        Run(argc > 1 ? std::wstring_view(argv[1]) : L"all", argc > 2 ? std::filesystem::path(argv[2]) : std::filesystem::path{});
    }
    catch (const std::exception& error) { std::cerr << "FAILED desktop preview: " << error.what() << '\n'; result = 1; }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return result;
}
