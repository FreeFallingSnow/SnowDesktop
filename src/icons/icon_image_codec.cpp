#include "icon_image_codec.h"
#include "data/atomic_file.h"
#include "ui/preview/preview_png_writer.h"

#include <d2d1_3.h>
#include <d2d1_1helper.h>
#include <d2d1svg.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <xmllite.h>
#include <webp/decode.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <set>
#include <sstream>

namespace snowdesktop::icon_image
{
namespace
{
using Microsoft::WRL::ComPtr;
bool Fail(std::string& error, const char* key) { error = key; return false; }
bool Dimensions(UINT width, UINT height)
{ return width && height && std::uint64_t(width) * height <= kMaxPixels; }
std::wstring Lower(std::wstring value)
{ std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); }); return value; }
std::wstring Trim(std::wstring value)
{
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}
void Size(int sourceWidth, int sourceHeight, int edge, UINT& width, UINT& height)
{
    const double ratio = edge > 0 ? std::min(1., double(edge) / std::max(sourceWidth, sourceHeight)) : 1.;
    width = std::max(1u, static_cast<UINT>(std::lround(sourceWidth * ratio)));
    height = std::max(1u, static_cast<UINT>(std::lround(sourceHeight * ratio)));
}
bool Read(const std::filesystem::path& path, std::string& bytes, std::string& error)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || !size) return Fail(error, "iconLibrary.invalid");
    if (size > kMaxFileBytes) return Fail(error, "iconLibrary.tooLarge");
    bytes.resize(static_cast<size_t>(size));
    std::ifstream input(path, std::ios::binary);
    return (input.read(bytes.data(), static_cast<std::streamsize>(size)) && input.peek() == EOF) ||
        Fail(error, "iconLibrary.invalid");
}
bool CopyWic(IWICImagingFactory* factory, IWICBitmapSource* input, int edge, Image& image, std::string& error)
{
    UINT width = 0, height = 0;
    if (FAILED(input->GetSize(&width, &height)) || !Dimensions(width, height))
        return Fail(error, "iconLibrary.tooLarge");
    image.sourceWidth = static_cast<int>(width); image.sourceHeight = static_cast<int>(height);
    // Convert BEFORE interpolation: transparent RGB must never contaminate the
    // visible edge, and known PBGRA must never pass through Shell alpha guessing.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(input, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return Fail(error, "iconLibrary.invalid");
    UINT targetWidth, targetHeight;
    Size(image.sourceWidth, image.sourceHeight, edge, targetWidth, targetHeight);
    ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource* source = converter.Get();
    if (targetWidth != width || targetHeight != height)
    {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(converter.Get(),
                targetWidth, targetHeight, WICBitmapInterpolationModeFant))) return Fail(error, "iconLibrary.invalid");
        source = scaler.Get();
    }
    image.width = static_cast<int>(targetWidth); image.height = static_cast<int>(targetHeight);
    image.pixels.resize(static_cast<size_t>(targetWidth) * targetHeight);
    if (FAILED(source->CopyPixels(nullptr, targetWidth * 4, static_cast<UINT>(image.pixels.size() * 4),
            reinterpret_cast<BYTE*>(image.pixels.data())))) return Fail(error, "iconLibrary.invalid");
    for (auto& pixel : image.pixels) if (!(pixel >> 24)) pixel = 0;
    return true;
}
bool Svg(std::string_view bytes, int edge, Image& image, std::string& error)
{
    ComPtr<IStream> stream;
    ULONG written = 0;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written)) || written != bytes.size() ||
        FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) return Fail(error, "iconLibrary.invalid");
    ComPtr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetInput(stream.Get()))) return Fail(error, "iconLibrary.invalid");
    const std::set<std::wstring> elements{L"svg", L"g", L"defs", L"desc", L"title", L"metadata", L"path", L"rect", L"circle",
        L"ellipse", L"line", L"polygon", L"polyline", L"linearGradient", L"radialGradient", L"stop", L"clipPath", L"use"};
    const std::set<std::wstring> properties{L"clip-path",L"clip-rule",L"color",L"display",L"fill",L"fill-opacity",L"fill-rule",
        L"opacity",L"overflow",L"stop-color",L"stop-opacity",L"stroke",L"stroke-dasharray",L"stroke-dashoffset",
        L"stroke-linecap",L"stroke-linejoin",L"stroke-miterlimit",L"stroke-opacity",L"stroke-width",L"visibility"};
    XmlNodeType node{}; HRESULT read = S_OK; unsigned count = 0; bool root = false;
    double sourceWidth = 0, sourceHeight = 0, intrinsicWidth = 0, intrinsicHeight = 0;
    while ((read = reader->Read(&node)) == S_OK)
    {
        if (node == XmlNodeType_DocumentType) return Fail(error, "iconLibrary.svgUnsupported");
        if (node != XmlNodeType_Element) continue;
        const wchar_t* name = nullptr; reader->GetLocalName(&name, nullptr);
        UINT depth = 0; reader->GetDepth(&depth);
        if (!name || !elements.contains(name) || ++count > 100000 || depth > 128 || (!root && std::wstring_view(name) != L"svg"))
            return Fail(error, "iconLibrary.svgUnsupported");
        const bool first = !root; root = true;
        if (reader->MoveToFirstAttribute() == S_OK) do
        {
            const wchar_t *attribute = nullptr, *value = nullptr;
            reader->GetLocalName(&attribute, nullptr); reader->GetValue(&value, nullptr);
            if (!attribute || !value) return Fail(error, "iconLibrary.invalid");
            const auto key = std::wstring_view(attribute); const auto text = Trim(value); const auto lower = Lower(text);
            if (key == L"filter" || key == L"mask" || key == L"class" || key.starts_with(L"on"))
                return Fail(error, "iconLibrary.svgUnsupported");
            if (key == L"href" && !text.starts_with(L"#")) return Fail(error, "iconLibrary.svgUnsupported");
            for (size_t p = 0; (p = lower.find(L"url(", p)) != lower.npos; ++p)
            {
                const auto end = lower.find(L')', p + 4);
                if (end == lower.npos) return Fail(error, "iconLibrary.svgUnsupported");
                auto ref = Trim(lower.substr(p + 4, end - p - 4));
                if (ref.size() >= 2 && (ref.front() == L'\'' || ref.front() == L'"')) ref = ref.substr(1, ref.size() - 2);
                if (!ref.starts_with(L"#")) return Fail(error, "iconLibrary.svgUnsupported");
            }
            if (key == L"style")
            {
                std::wistringstream declarations(text); std::wstring declaration;
                while (std::getline(declarations, declaration, L';'))
                {
                    if (Trim(declaration).empty()) continue;
                    const auto colon = declaration.find(L':');
                    if (colon == declaration.npos || !properties.contains(Trim(declaration.substr(0, colon))))
                        return Fail(error, "iconLibrary.svgUnsupported");
                }
            }
            if (first && key == L"viewBox")
            {
                auto box = text; std::replace(box.begin(), box.end(), L',', L' ');
                std::wistringstream numbers(box); double left = 0, top = 0;
                if (!(numbers >> left >> top >> sourceWidth >> sourceHeight)) return Fail(error, "iconLibrary.invalid");
            }
            if (first && (key == L"width" || key == L"height"))
            {
                wchar_t* end = nullptr; const double number = std::wcstod(text.c_str(), &end);
                if (end && (*end == 0 || std::wstring_view(end) == L"px"))
                    (key == L"width" ? intrinsicWidth : intrinsicHeight) = number;
            }
        } while (reader->MoveToNextAttribute() == S_OK);
        reader->MoveToElement();
    }
    if (FAILED(read) || !root) return Fail(error, "iconLibrary.invalid");
    const bool hasViewBox = sourceWidth > 0 && sourceHeight > 0 && std::isfinite(sourceWidth) && std::isfinite(sourceHeight);
    if (!hasViewBox)
    {
        if (intrinsicWidth > 0 && intrinsicHeight > 0 && std::isfinite(intrinsicWidth) && std::isfinite(intrinsicHeight))
        { sourceWidth = intrinsicWidth; sourceHeight = intrinsicHeight; }
        else sourceWidth = sourceHeight = 1024;
    }
    if (sourceWidth > 16000000 || sourceHeight > 16000000) return Fail(error, "iconLibrary.tooLarge");
    const double scale = double(edge > 0 ? std::clamp(edge, 1, 4000) : 1024) / std::max(sourceWidth, sourceHeight);
    const UINT width = std::max(1u, static_cast<UINT>(std::lround(sourceWidth * scale)));
    const UINT height = std::max(1u, static_cast<UINT>(std::lround(sourceHeight * scale)));
    if (!Dimensions(width, height)) return Fail(error, "iconLibrary.tooLarge");
    ComPtr<ID3D11Device> graphics; ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory; ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context; ComPtr<ID2D1DeviceContext5> svgContext;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr)) || FAILED(graphics.As(&dxgi)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
        FAILED(factory->CreateDevice(dxgi.Get(), &device)) || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) ||
        FAILED(context.As(&svgContext)) || FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) return Fail(error, "iconLibrary.invalid");
    ComPtr<ID2D1SvgDocument> svg; ComPtr<ID2D1Bitmap1> target, readback;
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    if (FAILED(svgContext->CreateSvgDocument(stream.Get(), D2D1::SizeF(static_cast<float>(width), static_cast<float>(height)), &svg)) ||
        FAILED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96), &target)) ||
        FAILED(context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96), &readback)))
        return Fail(error, "iconLibrary.invalid");
    ComPtr<ID2D1SvgElement> svgRoot; svg->GetRoot(&svgRoot);
    const D2D1_SVG_LENGTH svgWidth{static_cast<float>(width), D2D1_SVG_LENGTH_UNITS_NUMBER};
    const D2D1_SVG_LENGTH svgHeight{static_cast<float>(height), D2D1_SVG_LENGTH_UNITS_NUMBER};
    if (!svgRoot || FAILED(svgRoot->SetAttributeValue(L"width", svgWidth)) || FAILED(svgRoot->SetAttributeValue(L"height", svgHeight)))
        return Fail(error, "iconLibrary.invalid");
    if (!hasViewBox)
    {
        const D2D1_SVG_VIEWBOX box{0, 0, static_cast<float>(sourceWidth), static_cast<float>(sourceHeight)};
        if (FAILED(svgRoot->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &box, sizeof(box)))) return Fail(error, "iconLibrary.invalid");
    }
    context->SetTarget(target.Get()); context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
    svgContext->DrawSvgDocument(svg.Get());
    if (FAILED(context->EndDraw()) || FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr)))
        return Fail(error, "iconLibrary.invalid");
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return Fail(error, "iconLibrary.invalid");
    image.width = static_cast<int>(width); image.height = static_cast<int>(height);
    image.sourceWidth = std::max(1, static_cast<int>(std::lround(sourceWidth))); image.sourceHeight = std::max(1, static_cast<int>(std::lround(sourceHeight)));
    image.vector = true; image.format = "svg"; image.pixels.resize(static_cast<size_t>(width) * height);
    for (UINT y = 0; y < height; ++y) std::memcpy(image.pixels.data() + static_cast<size_t>(y) * width,
        mapped.bits + static_cast<size_t>(y) * mapped.pitch, static_cast<size_t>(width) * 4);
    return SUCCEEDED(readback->Unmap()) || Fail(error, "iconLibrary.invalid");
}
}

bool Decode(const std::filesystem::path& path, int edge, Image& image, std::string& error)
{
    image = {}; error.clear();
    std::string bytes;
    if (!Read(path, bytes, error)) return false;
    const auto xmlStart = bytes.find_first_not_of(" \t\r\n\xef\xbb\xbf");
    if (Lower(path.extension().wstring()) == L".svg" || (xmlStart != bytes.npos && bytes[xmlStart] == '<'))
        return Svg(bytes, edge, image, error);
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return Fail(error, "iconLibrary.invalid");
    if (bytes.size() >= 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0)
    {
        WebPBitstreamFeatures features{};
        if (WebPGetFeatures(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &features) != VP8_STATUS_OK)
            return Fail(error, "iconLibrary.invalid");
        if (features.has_animation) return Fail(error, "iconLibrary.animatedUnsupported");
        if (!Dimensions(static_cast<UINT>(features.width), static_cast<UINT>(features.height))) return Fail(error, "iconLibrary.tooLarge");
        std::vector<std::uint32_t> pixels(static_cast<size_t>(features.width) * features.height);
        WebPDecoderConfig config{};
        if (!WebPInitDecoderConfig(&config)) return Fail(error, "iconLibrary.invalid");
        config.output.colorspace = MODE_bgrA;
        config.output.is_external_memory = 1;
        config.output.u.RGBA.rgba = reinterpret_cast<uint8_t*>(pixels.data());
        config.output.u.RGBA.size = pixels.size() * 4; config.output.u.RGBA.stride = features.width * 4;
        const auto decoded = WebPDecode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &config);
        WebPFreeDecBuffer(&config.output);
        if (decoded != VP8_STATUS_OK) return Fail(error, "iconLibrary.invalid");
        image.format = "webp";
        if (edge <= 0 || edge >= std::max(features.width, features.height))
        {
            image.width = image.sourceWidth = features.width; image.height = image.sourceHeight = features.height;
            image.pixels = std::move(pixels); return true;
        }
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(factory->CreateBitmapFromMemory(static_cast<UINT>(features.width), static_cast<UINT>(features.height),
                GUID_WICPixelFormat32bppPBGRA, static_cast<UINT>(features.width * 4), static_cast<UINT>(pixels.size() * 4),
                reinterpret_cast<BYTE*>(pixels.data()), &bitmap))) return Fail(error, "iconLibrary.invalid");
        return CopyWic(factory.Get(), bitmap.Get(), edge, image, error);
    }
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) return Fail(error, "iconLibrary.invalid");
    GUID container{};
    if (FAILED(decoder->GetContainerFormat(&container))) return Fail(error, "iconLibrary.invalid");
    if (container == GUID_ContainerFormatPng) image.format = "png";
    else if (container == GUID_ContainerFormatJpeg) image.format = "jpg";
    else if (container == GUID_ContainerFormatBmp) image.format = "bmp";
    else if (container == GUID_ContainerFormatIco) image.format = "ico";
    else return Fail(error, "iconLibrary.invalid");
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return Fail(error, "iconLibrary.invalid");
    if (container == GUID_ContainerFormatIco)
    {
        UINT count = 0, best = 0; decoder->GetFrameCount(&count);
        for (UINT i = 0; i < std::min(count, 256u); ++i)
        {
            ComPtr<IWICBitmapFrameDecode> candidate; UINT w = 0, h = 0;
            if (SUCCEEDED(decoder->GetFrame(i, &candidate)) && SUCCEEDED(candidate->GetSize(&w, &h)) &&
                Dimensions(w, h) && std::max(w, h) > best) { frame = candidate; best = std::max(w, h); }
        }
    }
    return CopyWic(factory.Get(), frame.Get(), edge, image, error);
}

HBITMAP CreateBitmap(const Image& image)
{
    if (image.width <= 0 || image.height <= 0 || image.pixels.size() != static_cast<size_t>(image.width) * image.height) return nullptr;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = image.width; info.bmiHeader.biHeight = -image.height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* bits = nullptr; const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) std::memcpy(bits, image.pixels.data(), image.pixels.size() * 4);
    return bitmap;
}

bool ExportPng(const std::filesystem::path& source, const std::filesystem::path& output, std::string& error)
{
    Image image;
    return Decode(source, 0, image, error) && preview_png::Save(output, image.width, image.height, image.pixels, error);
}

bool ExportIco(const std::filesystem::path& source, const std::filesystem::path& output, std::string& error)
{
    constexpr std::array<int, 7> sizes{16, 24, 32, 48, 64, 128, 256};
    std::vector<std::string> frames;
    // Per-frame temporary names are unique to this process/export and are
    // removed on every exit. Only the final ICO is atomically committed.
    const auto temporary = output.parent_path() / (L".snowdesktop-ico-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L".png");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{temporary};
    for (const int size : sizes)
    {
        Image image;
        if (!Decode(source, size, image, error)) return false;
        std::vector<std::uint32_t> square(static_cast<size_t>(size) * size, 0);
        // Raster sources smaller than a frame are enlarged with premultiplied
        // interpolation rather than surrounded by extra blank margins.
        const double ratio = double(size) / std::max(image.width, image.height);
        const int width = std::max(1, static_cast<int>(std::lround(image.width * ratio)));
        const int height = std::max(1, static_cast<int>(std::lround(image.height * ratio)));
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        {
            const double sx = std::clamp((x + .5) / ratio - .5, 0., double(image.width - 1));
            const double sy = std::clamp((y + .5) / ratio - .5, 0., double(image.height - 1));
            const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy), x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
            const double fx = sx - x0, fy = sy - y0;
            std::uint32_t pixel = 0;
            for (int shift : {0, 8, 16, 24})
            {
                const auto channel = [&](int xx, int yy) { return double((image.pixels[static_cast<size_t>(yy) * image.width + xx] >> shift) & 255); };
                const double top = channel(x0, y0) * (1 - fx) + channel(x1, y0) * fx;
                const double bottom = channel(x0, y1) * (1 - fx) + channel(x1, y1) * fx;
                pixel |= static_cast<unsigned>(std::lround(top * (1 - fy) + bottom * fy)) << shift;
            }
            square[static_cast<size_t>((size - height) / 2 + y) * size + (size - width) / 2 + x] = pixel;
        }
        if (!preview_png::Save(temporary, size, size, square, error)) return false;
        std::ifstream input(temporary, std::ios::binary);
        frames.emplace_back(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (input.bad() || frames.back().empty()) return Fail(error, "iconLibrary.exportFailed");
    }
    std::string bytes;
    const auto u16 = [&](unsigned n) { bytes.push_back(static_cast<char>(n & 255)); bytes.push_back(static_cast<char>((n >> 8) & 255)); };
    const auto u32 = [&](unsigned n) { u16(n & 65535); u16(n >> 16); };
    u16(0); u16(1); u16(static_cast<unsigned>(sizes.size()));
    unsigned offset = 6 + 16 * static_cast<unsigned>(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i)
    {
        bytes.push_back(static_cast<char>(sizes[i] == 256 ? 0 : sizes[i])); bytes.push_back(bytes.back());
        bytes.push_back(0); bytes.push_back(0); u16(1); u16(32);
        u32(static_cast<unsigned>(frames[i].size())); u32(offset); offset += static_cast<unsigned>(frames[i].size());
    }
    for (const auto& frame : frames) bytes += frame;
    return atomic_file::WriteAll(output, bytes) || Fail(error, "iconLibrary.exportFailed");
}
}
