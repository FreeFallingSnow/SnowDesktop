#include "icons/icon_library.h"
#include "icons/icon_image_codec.h"
#include "icons/icon_hbitmap_pixels.h"
#include "icons/large_icon_config.h"
#include "data/atomic_file.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <webp/encode.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <cmath>

int RunIconPackTests(const std::filesystem::path& root);
namespace
{
using Microsoft::WRL::ComPtr;
// Independent straight-alpha fixture writer. Do not use the production PNG
// writer to manufacture the very alpha semantics this regression checks.
bool Fixture(const std::filesystem::path& path, int width, int height, const std::vector<unsigned>& pixels, const GUID& container = GUID_ContainerFormatPng)
{
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IPropertyBag2> options;
    WICPixelFormatGUID format = container != GUID_ContainerFormatPng ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    std::vector<BYTE> data; data.reserve(pixels.size() * 4);
    const bool rgb = container != GUID_ContainerFormatPng;
    for (const auto pixel : pixels) for (int channel = 0; channel < (rgb ? 3 : 4); ++channel)
        data.push_back(static_cast<BYTE>(pixel >> (channel * 8)));
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(container, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &options)) &&
        SUCCEEDED(frame->Initialize(options.Get())) && SUCCEEDED(frame->SetSize(width, height)) &&
        SUCCEEDED(frame->SetPixelFormat(&format)) && format == (rgb ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA) &&
        SUCCEEDED(frame->WritePixels(height, width * (rgb ? 3 : 4), static_cast<UINT>(data.size()), data.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
}
int RunIconLibraryTests()
{
    using namespace snowdesktop;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto directory = std::filesystem::temp_directory_path() / (L"snowdesktop-icon-library-test-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{directory};
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; } };
    std::string error;
    for (const bool lossless : {false, true})
    {
        const auto path = directory / (lossless ? L"lossless.webp" : L"lossy.webp");
        std::vector<uint8_t> rgba(513 * 17 * 4, 0);
        for (int y = 0; y < 17; ++y) for (int x = 256; x < 513; ++x)
            for (int channel = 0; channel < 4; ++channel) rgba[(y * 513 + x) * 4 + channel] = 255;
        for (int y = 0; y < 17; ++y) for (int x = 256; x < 289; ++x) rgba[(y * 513 + x) * 4 + 3] = static_cast<uint8_t>(std::min(255, (x - 256) * 8));
        uint8_t* encoded = nullptr;
        const auto bytes = lossless ? WebPEncodeLosslessRGBA(rgba.data(), 513, 17, 513 * 4, &encoded) :
            WebPEncodeRGBA(rgba.data(), 513, 17, 513 * 4, 90, &encoded);
        check(bytes && encoded, "independently encode lossy/lossless transparent WebP");
        if (bytes && encoded)
        {
            atomic_file::WriteAll(path, std::string_view(reinterpret_cast<char*>(encoded), bytes)); WebPFree(encoded);
            icon_image::Image image; bool clean = icon_image::Decode(path, 64, image, error);
            for (const auto pixel : image.pixels) for (const int shift : {0,8,16})
                clean &= std::abs(static_cast<int>((pixel >> shift) & 255) - static_cast<int>(pixel >> 24)) <= 4;
            check(clean && !image.pixels.empty(), "WebP transparent edges survive decoding and premultiplied resizing without a black matte");
        }
    }
    // Independently assemble a standard two-frame animation from the valid
    // encoder fixture. Rejection must identify animation, not corrupt input.
    {
        std::ifstream source(directory / L"lossless.webp", std::ios::binary);
        const std::string still{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
        const auto little = [](unsigned value, int count) { std::string out; for (int i = 0; i < count; ++i) out += static_cast<char>(value >> (i * 8)); return out; };
        const auto chunk = [&](const char* tag, std::string data) {
            auto out = std::string(tag, 4) + little(static_cast<unsigned>(data.size()), 4) + data;
            if (data.size() % 2) out += '\0'; return out;
        };
        const auto canvas = std::string("\x12\0\0\0",4) + little(512,3) + little(16,3);
        const auto frame = little(0,3) + little(0,3) + little(512,3) + little(16,3) + little(100,3) + std::string(1,2) + still.substr(12);
        const auto body = std::string("WEBP") + chunk("VP8X", canvas) + chunk("ANIM", std::string(6,'\0')) + chunk("ANMF",frame) + chunk("ANMF",frame);
        const auto path = directory / L"animation.webp";
        atomic_file::WriteAll(path, std::string("RIFF") + little(static_cast<unsigned>(body.size()),4) + body);
        icon_image::Image image;
        check(!icon_image::Decode(path,64,image,error) && error == "iconLibrary.animatedUnsupported",
            "animated WebP reports the static-resource restriction instead of accepting a partial first frame");
    }
    for (const unsigned transparent : {0x00000000u, 0x00ffffffu, 0x00ff0011u})
    {
        const auto path = directory / (L"transparent-" + std::to_wstring(transparent) + L".png");
        std::vector<unsigned> pixels(513 * 17, transparent);
        for (int y = 0; y < 17; ++y) for (int x = 256; x < 513; ++x) pixels[y * 513 + x] = 0xffffffff;
        check(Fixture(path, 513, 17, pixels), "create independent straight-alpha PNG with hidden RGB");
        icon_image::Image image;
        check(icon_image::Decode(path, 64, image, error) && image.width == 64, "decode and downscale actual PNG through shared codec");
        bool clean = !image.pixels.empty(), intermediate = false;
        for (const auto p : image.pixels)
        {
            const int a = static_cast<int>(p >> 24);
            intermediate |= a > 0 && a < 255;
            for (const int shift : {0,8,16}) clean &= std::abs(static_cast<int>((p >> shift) & 255) - a) <= 2;
        }
        check(clean && intermediate, "transparent RGB does not darken or tint an interpolated white edge");
    }
    for (const auto& [name, format] : {std::pair{L"sample.jpg", GUID_ContainerFormatJpeg}, {L"sample.bmp", GUID_ContainerFormatBmp}})
    {
        const auto path = directory / name;
        check(Fixture(path, 32, 16, std::vector<unsigned>(32 * 16, 0xffcc8844), format), "create independent JPEG/BMP fixture");
        icon_image::Image decoded;
        check(icon_image::Decode(path, 16, decoded, error) && decoded.width == 16 && decoded.height == 8 &&
            (decoded.pixels.front() >> 24) == 255, "JPEG/BMP decode and resize through production codec");
    }
    const auto corrupt = directory / L"corrupt.webp";
    atomic_file::WriteAll(corrupt, "RIFFgarbageWEBP");
    icon_image::Image invalid;
    check(!icon_image::Decode(corrupt, 64, invalid, error), "damaged WebP is rejected without a partial replacement");
    for (const unsigned hidden : {0x00000000u, 0x00ffffffu, 0x00ff0011u})
    {
        const auto path = directory / L"fully-transparent.png";
        check(Fixture(path, 32, 16, std::vector<unsigned>(32 * 16, hidden)), "create fully transparent PNG carrying hidden RGB");
        icon_image::Image transparent;
        check(icon_image::Decode(path, 16, transparent, error) &&
            std::all_of(transparent.pixels.begin(), transparent.pixels.end(), [](auto pixel) { return pixel == 0; }),
            "all transparent RGB values remain empty after premultiplied resizing");
        const auto bitmap = icon_image::CreateBitmap(transparent); icon_bitmap_pixels::Buffer uploaded;
        check(icon_bitmap_pixels::ReadHBitmap(bitmap, uploaded, false) && uploaded.pixels == transparent.pixels,
            "known transparent image survives managed HBITMAP readback without Shell opacity guessing");
        if (bitmap) DeleteObject(bitmap);
    }
    const auto oversize = directory / L"too-large.png";
    { std::ofstream file(oversize, std::ios::binary); file.seekp(icon_image::kMaxFileBytes); file.put('x'); }
    check(!icon_image::Decode(oversize, 16, invalid, error) && error == "iconLibrary.tooLarge", "input byte limit is enforced before decoding");
    const auto huge = directory / L"too-many-pixels.png";
    check(Fixture(huge, 4001, 4000, std::vector<unsigned>(4001 * 4000, 0xffee8844)), "create valid PNG exceeding the raster pixel budget");
    check(!icon_image::Decode(huge, 16, invalid, error) && error == "iconLibrary.tooLarge", "raster pixel limit is enforced before allocating decoded pixels");
    const auto original = directory / L"透明图标.png";
    std::vector<unsigned> pixels(32 * 16, 0x804080c0); pixels[0] = 0x00ffffff;
    check(Fixture(original, 32, 16, pixels), "create half-transparent colored PNG");
    IconLibrary library(directory / L"library");
    IconLibraryRequest request; request.action = "import"; request.paths = {original.wstring(), original.wstring()};
    auto imported = library.Invoke(request);
    check(imported.succeeded && imported.entries.size() == 1 && imported.imported.size() == 2 &&
        imported.imported[0] == imported.imported[1], "batch duplicates share one content-addressed original");
    if (!imported.entries.empty())
    {
        const auto id = imported.entries.front().id;
        check(IsIconAssetId(id) && !IsIconAssetId("../" + id), "asset identity rejects path traversal");
        const auto persisted = IconAssetPath(directory / L"library", id);
        std::filesystem::remove(original);
        icon_image::Image image;
        check(icon_image::Decode(persisted, 0, image, error) && image.width == 32 && image.height == 16,
            "deleting imported source cannot remove the library original");
        check(image.pixels.size() > 1 && (image.pixels[1] >> 24) == 128 &&
            std::abs(static_cast<int>((image.pixels[1] >> 16) & 255) - 32) <= 1,
            "straight alpha is premultiplied exactly once");
        request = {}; request.action = "rename"; request.ids = {id}; request.name = L"图标 \"A\" \\ B";
        check(library.Invoke(request).succeeded, "rename and escape Unicode resource metadata");
        IconLibrary reopened(directory / L"library");
        check(reopened.Read().entries.front().name == request.name, "resource identity and name survive reopening");
        request = {}; request.action = "delete"; request.ids = {id};
        check(!library.Invoke(request, {id}).succeeded && std::filesystem::exists(persisted), "in-use deletion preserves the resource and index");
        const auto exported = directory / L"export.png";
        check(icon_image::ExportPng(persisted, exported, error), "export alpha PNG without applying icon decoration");
        icon_image::Image restored;
        check(icon_image::Decode(exported, 0, restored, error) && restored.pixels == image.pixels,
            "PNG export round trip preserves premultiplied pixel colors and transparency");
        const auto ico = directory / L"export.ico";
        check(icon_image::ExportIco(persisted, ico, error), "encode standard multi-resolution ICO");
        ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder; UINT count = 0;
        bool validIco = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(ico.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) &&
            SUCCEEDED(decoder->GetFrameCount(&count)) && count == 7;
        constexpr std::array<UINT,7> sizes{16,24,32,48,64,128,256};
        if (validIco) for (UINT i = 0; i < count; ++i)
        { ComPtr<IWICBitmapFrameDecode> frame; UINT w = 0, h = 0; validIco &= SUCCEEDED(decoder->GetFrame(i, &frame)) && SUCCEEDED(frame->GetSize(&w,&h)) && w == sizes[i] && h == sizes[i]; }
        check(validIco, "Windows WIC reads all seven independently encoded ICO frame sizes");
        icon_image::Image icoImage;
        check(icon_image::Decode(ico, 0, icoImage, error) && icoImage.width == 256 && icoImage.height == 256 &&
            !icoImage.pixels.empty() && icoImage.pixels.front() == 0 && (icoImage.pixels[128 * 256 + 128] >> 24) == 128,
            "ICO import selects the largest frame and preserves alpha and transparent letterboxing");
        LargeIconConfig config; config.content = 1; config.backgroundStyle = -2; config.imageAssetId = id;
        JsonValue json; LargeIconConfig roundtrip;
        check(ParseJson(EncodeLargeIconConfig(config), json) && DecodeLargeIconConfig(json, roundtrip) &&
            roundtrip.imageAssetId == id && roundtrip.version == 3 && LargeIconActiveImage(roundtrip) == id,
            "v3 large icons persist a library reference through production codec");
        check(library.Invoke(request).succeeded && library.Read().entries.empty(), "unreferenced resources can be removed");
    }
    const auto svg = directory / L"shape.svg";
    atomic_file::WriteAll(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\"><rect x=\"20\" y=\"10\" width=\"60\" height=\"30\" fill=\"#ffffff\" fill-opacity=\"0.5\"/></svg>");
    icon_image::Image vector;
    check(icon_image::Decode(svg, 200, vector, error) && vector.width == 200 && vector.height == 100 &&
        (vector.pixels.front() >> 24) == 0 && ((vector.pixels[50 * 200 + 100] >> 24) >= 126),
        "SVG rasterization preserves aspect ratio and transparency");
    atomic_file::WriteAll(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" height=\"40\"><rect width=\"80\" height=\"40\" fill=\"#ffffff\"/></svg>");
    check(icon_image::Decode(svg, 160, vector, error) && vector.width == 160 && vector.height == 80 &&
        vector.pixels[40 * 160 + 80] == 0xffffffff,
        "SVG intrinsic dimensions without viewBox scale through the explicit SDK viewBox attribute");
    atomic_file::WriteAll(svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 50\"><defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#ff0000\"/><stop offset=\"1\" stop-color=\"#00ff00\"/></linearGradient><clipPath id=\"c\"><circle cx=\"50\" cy=\"25\" r=\"20\"/></clipPath></defs><rect width=\"100\" height=\"50\" fill=\"url(#g)\" clip-path=\"url(#c)\"/></svg>");
    check(icon_image::Decode(svg, 200, vector, error) && vector.pixels.front() == 0 &&
        (vector.pixels[50 * 200 + 100] >> 24) == 255 &&
        ((vector.pixels[50 * 200 + 100] >> 16) & 255) > 60 && ((vector.pixels[50 * 200 + 100] >> 8) & 255) > 60,
        "SVG gradients and clipping preserve colors and transparent corners");
    for (const auto& content : {"<svg><text>Unsupported</text></svg>", "<svg><image href=\"https://example.com/a.png\"/></svg>",
            "<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///c:/private'>]><svg>&x;</svg>"})
    { atomic_file::WriteAll(svg, content); check(!icon_image::Decode(svg, 64, vector, error), "unsupported SVG text, external resources and DTD are rejected"); }
    request = {}; request.action = "import"; request.paths = {(directory / L"lossless.webp").wstring(), corrupt.wstring()};
    const auto partial = library.Invoke(request);
    check(!partial.succeeded && partial.imported.size() == 1 && partial.failures.size() == 1 && partial.entries.size() == 1,
        "a partial import preserves its valid resource and reports exactly the failed input");
    failures += RunIconPackTests(directory);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return failures ? 1 : 0;
}
