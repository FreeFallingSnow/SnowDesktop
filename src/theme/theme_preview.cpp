#include "theme_preview.h"
#include "ui/preview/preview_png_writer.h"
#include "common/json_value.h"
#include "steam/steam_child_environment.h"
#include "resources/resource.h"
#include "icons/icon_beautify_preview.h"
#include <fstream>
#include <thread>
#include <wincodec.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <d2d1effects.h>
#include <cstring>
#include "../../steam_bridge/src/theme_workshop_publish.h"

namespace snowdesktop::themes::preview
{
namespace
{
struct Handle { HANDLE value = nullptr; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
bool Fail(std::string& error, const char* value) { error = value; return false; }
}
std::wstring QuoteArgument(std::wstring_view value)
{
    std::wstring out = L"\""; unsigned slashes = 0;
    for (wchar_t c : value)
    {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\'); slashes = 0; out += c;
    }
    out.append(2 * slashes, L'\\'); return out + L'"';
}
bool Run(const std::filesystem::path& executable, const std::vector<std::wstring>& args,
    std::string& output, unsigned timeoutMs, std::string& error, const std::atomic_bool* cancel)
{
    output.clear();
    if (cancel && cancel->load()) return Fail(error, "cancelled");
    Handle read, write; SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    if (!CreatePipe(&read.value, &write.value, &attributes, 0) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) return Fail(error, "processFailed");
    std::wstring command = QuoteArgument(executable.wstring());
    for (const auto& arg : args) command += L" " + QuoteArgument(arg);
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.value;
    Handle input; input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    startup.StartupInfo.hStdInput = input.value;
    SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> storage(bytes); startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return Fail(error, "processFailed");
    struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeCleanup() { DeleteProcThreadAttributeList(value); } } cleanup{startup.lpAttributeList};
    HANDLE inherited[]{write.value, input.value};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr)) return Fail(error, "processFailed");
    PROCESS_INFORMATION info{};
    auto environment = BuildSnowDesktopSteamChildEnvironment();
    if (environment.empty()) return Fail(error, "processFailed");
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, environment.data(), executable.parent_path().c_str(), &startup.StartupInfo, &info))
        return Fail(error, "processFailed");
    Handle process{info.hProcess}, thread{info.hThread}; CloseHandle(write.value); write.value = nullptr;
    const auto started = GetTickCount64(); bool done = false;
    for (;;)
    {
        if (cancel && cancel->load()) { error = "cancelled"; break; }
        if (GetTickCount64() - started > timeoutMs) { error = "processTimeout"; break; }
        DWORD available = 0;
        if (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char buffer[8192]; DWORD received = 0;
            if (ReadFile(read.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &received, nullptr)) output.append(buffer, received);
            if (output.size() > 16 * 1024 * 1024) { error = "processOutputTooLarge"; break; }
            continue;
        }
        if (done) { DWORD code = 1; GetExitCodeProcess(process.value, &code); return code == 0 || Fail(error, "processFailed"); }
        done = WaitForSingleObject(process.value, 10) == WAIT_OBJECT_0;
        if (!done && cancel && cancel->load()) { error = "cancelled"; break; }
        if (!done && GetTickCount64() - started > timeoutMs) { error = "processTimeout"; break; }
    }
    // Only this request's one-shot child is terminated; no application tree.
    TerminateProcess(process.value, 6); WaitForSingleObject(process.value, 5000); return false;
}
bool SaveCover(const std::filesystem::path& path, widget_preview::Wallpaper image, std::string& error)
{
    if (image.width != kCoverSize || image.height != kCoverSize || image.pixels.size() != kCoverSize * kCoverSize)
        return Fail(error, "previewFailed");
    for (unsigned bits : {8u, 7u, 6u, 5u, 4u, 3u, 2u})
    {
        auto pixels = image.pixels;
        if (bits < 8)
            for (auto& p : pixels)
            {
                const unsigned levels = (1u << bits) - 1;
                std::uint32_t rounded = 0xff000000;
                for (unsigned shift : {0u, 8u, 16u})
                {
                    const unsigned quantized = (((p >> shift) & 255u) * levels + 127u) / 255u;
                    rounded |= ((quantized * 255u + levels / 2) / levels) << shift;
                }
                p = rounded;
            }
        if (!preview_png::Save(path, kCoverSize, kCoverSize, pixels, error)) return false;
        std::error_code ec; const auto size = std::filesystem::file_size(path, ec);
        if (!ec && size > 0 && size < kCoverMaximumBytes) return true;
    }
    std::error_code ec; std::filesystem::remove(path, ec); return Fail(error, "previewTooLarge");
}
bool NormalizeCover(const std::filesystem::path& source, const std::filesystem::path& destination, std::string& error)
{
    std::error_code ec;
    if (!steam_bridge::ThemeSafePath(source) || std::filesystem::file_size(source, ec) > 32 * 1024 * 1024 || ec)
        return Fail(error, "previewFailed");
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComCleanup { HRESULT result; ~ComCleanup() { if (SUCCEEDED(result)) CoUninitialize(); } } cleanup{initialized};
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapClipper> clip; ComPtr<IWICBitmapScaler> scaler; ComPtr<IWICFormatConverter> convert;
    UINT width = 0, height = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        width > 8192 || height > 8192 || static_cast<std::uint64_t>(width) * height > 16 * 1024 * 1024)
        return Fail(error, "previewFailed");
    const UINT side = std::min(width, height);
    const WICRect crop{static_cast<INT>((width - side) / 2), static_cast<INT>((height - side) / 2), static_cast<INT>(side), static_cast<INT>(side)};
    if (FAILED(factory->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(frame.Get(), &crop)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(clip.Get(), kCoverSize, kCoverSize, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&convert)) || FAILED(convert->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return Fail(error, "previewFailed");
    std::vector<std::uint32_t> pixels(kCoverSize * kCoverSize);
    if (FAILED(convert->CopyPixels(nullptr, kCoverSize * 4, static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()))))
        return Fail(error, "previewFailed");
    auto image = widget_preview::GenerateWallpaper(kCoverSize, kCoverSize, false);
    for (std::size_t i = 0; i < pixels.size(); ++i)
    {
        const unsigned alpha = pixels[i] >> 24; std::uint32_t pixel = 0xff000000;
        for (unsigned shift : {0u, 8u, 16u}) pixel |= std::min(255u,
            ((pixels[i] >> shift) & 255u) + (((image.pixels[i] >> shift) & 255u) * (255u - alpha) + 127u) / 255u) << shift;
        image.pixels[i] = pixel;
    }
    return SaveCover(destination, std::move(image), error);
}
static bool StageBuiltinBackground(const std::filesystem::path& host,
    const std::filesystem::path& directory, const std::filesystem::path& destination, std::string& error)
{
    // Read the selected production host's resource, including when invoked by a
    // test executable. No installed asset directory or developer mode is needed.
    const auto module = LoadLibraryExW(host.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return Fail(error, "previewFailed");
    struct ModuleCleanup { HMODULE value; ~ModuleCleanup() { FreeLibrary(value); } } cleanup{module};
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_THEME_PREVIEW_BACKGROUND), RT_RCDATA);
    const auto size = resource ? SizeofResource(module, resource) : 0;
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size || size > 32 * 1024 * 1024) return Fail(error, "previewFailed");
    const auto source = directory / L"builtin-background.png";
    std::ofstream file(source, std::ios::binary);
    file.write(static_cast<const char*>(bytes), size);
    file.close();
    if (!file) return Fail(error, "writeFailed");
    return NormalizeCover(source, destination, error);
}
static bool SaveFramedCover(const std::filesystem::path& raw, const std::filesystem::path& destination,
    const widget_preview::Wallpaper& stage, int left, int top, int width, int height, std::string& error)
{
    if (stage.pixels.size() != static_cast<std::size_t>(stage.width) * stage.height ||
        width <= 0 || height <= 0 || left < 0 || top < 0 || left + width > stage.width || top + height > stage.height)
        return Fail(error, "previewFailed");
    // Reframe the complete production scene, including its glass and wallpaper.
    // Never lift a foreground/backdrop patch onto a differently scaled wallpaper.
    const int side = std::min(std::min(stage.width, stage.height), std::max(width, height) + 48);
    const int x = std::clamp(left + width / 2 - side / 2, 0, stage.width - side);
    const int y = std::clamp(top + height / 2 - side / 2, 0, stage.height - side);
    std::vector<std::uint32_t> frame(static_cast<std::size_t>(side) * side);
    for (int row = 0; row < side; ++row)
        std::copy_n(stage.pixels.data() + static_cast<std::size_t>(y + row) * stage.width + x,
            side, frame.data() + static_cast<std::size_t>(row) * side);
    return preview_png::Save(raw, side, side, frame, error) && NormalizeCover(raw, destination, error);
}
static bool BlurIconBackdrop(const widget_preview::Wallpaper& source, float radius, int rasterScale,
    widget_preview::Wallpaper& output, std::string& error)
{
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComCleanup { HRESULT result; ~ComCleanup() { if (SUCCEEDED(result)) CoUninitialize(); } } cleanup{initialized};
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> graphics;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr)) || FAILED(graphics.As(&dxgi)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
        FAILED(factory->CreateDevice(dxgi.Get(), &device)) ||
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context))) return Fail(error, "previewFailed");
    const auto size = D2D1::SizeU(static_cast<UINT32>(source.width), static_cast<UINT32>(source.height));
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const auto targetProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96);
    ComPtr<ID2D1Bitmap1> target, readback;
    if (FAILED(context->CreateBitmap(size, nullptr, 0, &targetProperties, &target))) return Fail(error, "previewFailed");
    ComPtr<ID2D1Bitmap1> sourceBitmap; ComPtr<ID2D1Effect> blur;
    const auto sourceProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, format, 96, 96);
    if (FAILED(context->CreateBitmap(size, source.pixels.data(), source.width * 4, &sourceProperties, &sourceBitmap)) ||
        FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) return Fail(error, "previewFailed");
    blur->SetInput(0, sourceBitmap.Get());
    if (FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::clamp(radius, 0.f, 48.f) * rasterScale)) ||
        FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED)) ||
        FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD))) return Fail(error, "previewFailed");
    context->SetTarget(target.Get()); context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
    context->DrawImage(blur.Get());
    const auto rendered = context->EndDraw(); context->SetTarget(nullptr);
    const auto readProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96);
    if (FAILED(rendered) || FAILED(context->CreateBitmap(size, nullptr, 0, &readProperties, &readback)) ||
        FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr))) return Fail(error, "previewFailed");
    output = source;
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return Fail(error, "previewFailed");
    for (int y = 0; y < source.height; ++y)
        std::memcpy(output.pixels.data() + static_cast<std::size_t>(y) * source.width,
            mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(source.width) * 4);
    return SUCCEEDED(readback->Unmap()) || Fail(error, "previewFailed");
}
static bool IconSample(const std::filesystem::path& host, int resourceId, int side,
    std::vector<std::uint32_t>& pixels, std::string& error)
{
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComCleanup { HRESULT result; ~ComCleanup() { if (SUCCEEDED(result)) CoUninitialize(); } } cleanup{initialized};
    const auto module = LoadLibraryExW(host.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return Fail(error, "previewFailed");
    struct ModuleCleanup { HMODULE value; ~ModuleCleanup() { FreeLibrary(value); } } moduleCleanup{module};
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    const auto bytes = resource ? SizeofResource(module, resource) : 0;
    const auto loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto data = loaded ? LockResource(loaded) : nullptr;
    if (!data || !bytes) return Fail(error, "previewFailed");
    using Microsoft::WRL::ComPtr;
    ComPtr<IStream> stream;
    ULONG written = 0;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || FAILED(stream->Write(data, bytes, &written)) || written != bytes ||
        FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) return Fail(error, "previewFailed");
    ComPtr<ID3D11Device> graphics; ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory; ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context; ComPtr<ID2D1DeviceContext5> svgContext;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr)) || FAILED(graphics.As(&dxgi)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
        FAILED(factory->CreateDevice(dxgi.Get(), &device)) || FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) ||
        FAILED(context.As(&svgContext))) return Fail(error, "previewFailed");
    ComPtr<ID2D1SvgDocument> svg; ComPtr<ID2D1SvgElement> root;
    if (FAILED(svgContext->CreateSvgDocument(stream.Get(), D2D1::SizeF(static_cast<float>(side), static_cast<float>(side)), &svg)))
        return Fail(error, "previewFailed");
    svg->GetRoot(&root);
    const D2D1_SVG_LENGTH length{static_cast<float>(side), D2D1_SVG_LENGTH_UNITS_NUMBER};
    if (!root || FAILED(root->SetAttributeValue(L"width", length)) || FAILED(root->SetAttributeValue(L"height", length)))
        return Fail(error, "previewFailed");
    const auto size = D2D1::SizeU(static_cast<UINT32>(side), static_cast<UINT32>(side));
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, format, 96, 96);
    ComPtr<ID2D1Bitmap1> target, readback;
    if (FAILED(context->CreateBitmap(size, nullptr, 0, &properties, &target))) return Fail(error, "previewFailed");
    context->SetTarget(target.Get()); context->BeginDraw(); context->Clear(D2D1::ColorF(0, 0.f));
    svgContext->DrawSvgDocument(svg.Get());
    const auto result = context->EndDraw(); context->SetTarget(nullptr);
    const auto cpu = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, format, 96, 96);
    if (FAILED(result) || FAILED(context->CreateBitmap(size, nullptr, 0, &cpu, &readback)) ||
        FAILED(readback->CopyFromBitmap(nullptr, target.Get(), nullptr))) return Fail(error, "previewFailed");
    D2D1_MAPPED_RECT mapped{};
    if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return Fail(error, "previewFailed");
    pixels.resize(static_cast<std::size_t>(side) * side);
    for (int y = 0; y < side; ++y) std::memcpy(pixels.data() + static_cast<std::size_t>(y) * side,
        mapped.bits + static_cast<std::size_t>(y) * mapped.pitch, static_cast<std::size_t>(side) * 4);
    return SUCCEEDED(readback->Unmap()) || Fail(error, "previewFailed");
}
static bool ResizeIconScene(const widget_preview::Wallpaper& source, int side,
    widget_preview::Wallpaper& output, std::string& error)
{
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComCleanup { HRESULT result; ~ComCleanup() { if (SUCCEEDED(result)) CoUninitialize(); } } cleanup{initialized};
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmap> bitmap; ComPtr<IWICBitmapScaler> scaler;
    if (source.width <= 0 || source.height <= 0 ||
        source.pixels.size() != static_cast<std::size_t>(source.width) * source.height ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateBitmapFromMemory(source.width, source.height, GUID_WICPixelFormat32bppPBGRA,
            source.width * 4, static_cast<UINT>(source.pixels.size() * 4),
            reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(source.pixels.data())), &bitmap)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bitmap.Get(), side, side, side > source.width
            ? WICBitmapInterpolationModeHighQualityCubic : WICBitmapInterpolationModeFant)))
        return Fail(error, "previewFailed");
    output.width = output.height = side; output.pixels.resize(static_cast<std::size_t>(side) * side);
    return SUCCEEDED(scaler->CopyPixels(nullptr, side * 4, static_cast<UINT>(output.pixels.size() * 4),
        reinterpret_cast<BYTE*>(output.pixels.data()))) || Fail(error, "previewFailed");
}
static bool RenderImages(const std::filesystem::path& host, const Package& package, std::string_view root,
    unsigned scope, const std::filesystem::path& directory, std::filesystem::path& cover,
    std::string& error, const std::atomic_bool* cancel, std::vector<Image>* images,
    const std::filesystem::path& background, int iconProcessingMode)
{
    if (images) images->clear();
    cover.clear(); const auto parts = Parts(package, root, scope, error); if (parts.empty()) return false;
    std::error_code ec;
    // A new directory is mandatory. A failed request can never reuse an older cover.
    if (!std::filesystem::create_directory(directory, ec) || ec) return Fail(error, "writeFailed");
    struct Cleanup { std::filesystem::path path; bool keep = false; ~Cleanup() { if (!keep) { std::error_code ec; std::filesystem::remove_all(path, ec); } } } cleanup{directory};
    const auto snapshot = directory / L"package.snowtheme";
    if (!WritePackage(snapshot, package, error)) return false;
    std::filesystem::path stagedBackground;
    auto backdrop = widget_preview::GenerateWallpaper(kCoverSize, kCoverSize, false);
    if (!background.empty())
    {
        stagedBackground = directory / L"background.png";
        if (!NormalizeCover(background, stagedBackground, error)) return false;
        backdrop = widget_preview::LoadWallpaperImage(stagedBackground);
        if (backdrop.pixels.size() != kCoverSize * kCoverSize) return Fail(error, "previewFailed");
    }
    else if (images)
    {
        // Freeze the built-in background too. Glass and all surrounding pixels
        // must come from one production render, never two differently scaled stages.
        stagedBackground = directory / L"background.png";
        if (!StageBuiltinBackground(host, directory, stagedBackground, error)) return false;
        backdrop = widget_preview::LoadWallpaperImage(stagedBackground);
        if (backdrop.pixels.size() != kCoverSize * kCoverSize) return Fail(error, "previewFailed");
    }
    auto canvas = backdrop;
    const auto rootTheme = Resolve(package, root);
    if (rootTheme && rootTheme->kind == Kind::Icon)
    {
        // Keep a native 64px reference and supersample the whole gallery scene
        // at 4x. Both use native material units: fixed rim support, blur, glyph
        // shadows, texture seams and outlines scale with the sampling density.
        // The SVG artwork stays crisp without enlarging the reference bitmap.
        // No desktop shortcuts, application paths or user icons are enumerated.
        constexpr int resources[]{IDR_ICON_PREVIEW_BROWSER, IDR_ICON_PREVIEW_FOLDER, IDR_ICON_PREVIEW_MAIL, IDR_ICON_PREVIEW_MEDIA};
        const bool glass = rootTheme->icon.enabled && rootTheme->icon.glassEnabled;
        const auto coverPath = directory / L"cover.png";
        for (int rasterScale : {1, 4})
        {
            const int side = 64 * rasterScale, stageSide = 256 * rasterScale;
            widget_preview::Wallpaper sceneBackdrop;
            if (rasterScale == 1)
            {
                if (!ResizeIconScene(backdrop, stageSide, sceneBackdrop, error)) return false;
            }
            else sceneBackdrop = backdrop;
            canvas = sceneBackdrop;
            widget_preview::Wallpaper blurred;
            if (glass && !BlurIconBackdrop(sceneBackdrop, rootTheme->icon.glassBlurRadius, rasterScale, blurred, error)) return false;
            for (int sample = 0; sample < 4; ++sample)
            {
                if (cancel && cancel->load()) return Fail(error, "cancelled");
                std::vector<std::uint32_t> source;
                if (!IconSample(host, resources[sample], side, source, error)) return false;
                // Theme packages own appearance, not the local processing strategy.
                // The settings caller freezes its local strategy separately from the
                // shared package. Offline callers default to showing every background.
                auto style = rootTheme->icon; style.mode = iconProcessingMode == 0 ? 0 : 1;
                const auto edge = style.mode == 0 ? icon_beautify::DetectPlateFill(source, side, side) : std::nullopt;
                const auto rendered = icon_beautify::RenderPreview(source, side, side, style, edge, rasterScale);
                const int left = (40 + sample % 2 * 112) * rasterScale, top = (40 + sample / 2 * 112) * rasterScale;
                for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x)
                {
                    const auto pixel = rendered[static_cast<std::size_t>(y) * side + x];
                    const auto offset = static_cast<std::size_t>(top + y) * stageSide + left + x;
                    auto& destination = canvas.pixels[offset];
                    if (glass && !edge)
                    {
                        const unsigned coverage = icon_beautify::ShapeMaskAlpha(rootTheme->icon.shape, x, y, side, side);
                        std::uint32_t backgroundPixel = 0xff000000u;
                        for (unsigned shift : {0u, 8u, 16u}) backgroundPixel |=
                            ((((blurred.pixels[offset] >> shift) & 255u) * coverage +
                                ((destination >> shift) & 255u) * (255u - coverage) + 127u) / 255u) << shift;
                        destination = backgroundPixel;
                    }
                    const unsigned alpha = pixel >> 24;
                    std::uint32_t blended = 0xff000000u;
                    for (unsigned shift : {0u, 8u, 16u}) blended |= std::min(255u,
                        ((pixel >> shift) & 255u) + (((destination >> shift) & 255u) * (255u - alpha) + 127u) / 255u) << shift;
                    destination = blended;
                }
            }
            // Retain the native-size frame for pixel-level evidence. Only the
            // generated cover/gallery filenames participate in Workshop images.
            if (rasterScale == 1)
            {
                if (!preview_png::Save(directory / L"icon-scene.png", stageSide, stageSide, canvas.pixels, error)) return false;
            }
            else if (!SaveCover(coverPath, std::move(canvas), error)) return false;
        }
        if (images)
        {
            const auto imagePath = directory / GalleryFilename(steam_bridge::ThemeSha256(root), "icon");
            std::filesystem::copy_file(coverPath, imagePath, std::filesystem::copy_options::none, ec);
            if (ec) return Fail(error, "writeFailed");
            images->push_back({"icon", imagePath});
        }
        cover = coverPath; cleanup.keep = true; return true;
    }
    const int columns = parts.size() > 1 ? 2 : 1, rows = static_cast<int>((parts.size() + columns - 1) / columns);
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        if (cancel && cancel->load()) return Fail(error, "cancelled");
        const auto partDir = directory / std::to_wstring(index); const auto resultPath = directory / (std::to_wstring(index) + L".json");
        std::string output;
        const auto& part = parts[index];
        if (!Run(host, {L"--native-component-preview", std::wstring(part.component.begin(), part.component.end()), partDir.wstring(),
            L"96", L"en-US", L"dark", stagedBackground.wstring(), images ? L"1024" : L"2048", images ? L"1024" : L"1536",
            L"32", L"0", L"0", resultPath.wstring(), snapshot.wstring(),
            std::wstring(part.themeId.begin(), part.themeId.end())}, output, 60000, error, cancel)) return false;
        std::ifstream file(resultPath, std::ios::binary); std::string text(std::istreambuf_iterator<char>(file), {}); JsonValue result;
        if (!ParseJson(text, result)) return Fail(error, "previewFailed");
        const auto* ok = result.Find("ok"), *outputs = result.Find("outputs");
        if (!ok || !ok->IsBoolean() || !ok->boolean || !outputs || !outputs->IsArray() || outputs->array.empty()) return Fail(error, "previewFailed");
        const auto& item = outputs->array.front(); const auto* path = item.Find("path");
        if (!path || !path->IsString()) return Fail(error, "previewFailed");
        const auto image = widget_preview::LoadWallpaperImage(std::filesystem::path(std::u8string(path->string.begin(), path->string.end())));
        const auto number = [&](const char* key) { const auto* value = item.Find(key); return value && value->IsNumber() ? static_cast<int>(value->number) : 0; };
        int left = number("placementX"), top = number("placementY"), width = number("componentWidth"), height = number("componentHeight");
        if (images)
        {
            const auto imagePath = directory / GalleryFilename(steam_bridge::ThemeSha256(root), part.component);
            if (!SaveFramedCover(partDir / L"gallery-frame.png", imagePath, image, left, top, width, height, error))
            { images->clear(); return false; }
            images->push_back({part.component, imagePath});
            std::filesystem::remove_all(partDir, ec); std::filesystem::remove(resultPath, ec);
            continue;
        }
        const int margin = 12;
        left = std::max(0, left - margin); top = std::max(0, top - margin);
        width = std::min(image.width - left, width + 2 * margin); height = std::min(image.height - top, height + 2 * margin);
        if (image.pixels.empty() || width <= 0 || height <= 0) return Fail(error, "previewFailed");
        const int cellWidth = kCoverSize / columns - 40, cellHeight = kCoverSize / rows - 40;
        const double scale = std::min(static_cast<double>(cellWidth) / width, static_cast<double>(cellHeight) / height);
        const int w = std::max(1, static_cast<int>(width * scale)), h = std::max(1, static_cast<int>(height * scale));
        const auto cell = index;
        const int x = static_cast<int>(cell % columns) * kCoverSize / columns + (kCoverSize / columns - w) / 2;
        const int y = static_cast<int>(cell / columns) * kCoverSize / rows + (kCoverSize / rows - h) / 2;
        for (int dy = 0; dy < h; ++dy) for (int dx = 0; dx < w; ++dx)
            canvas.pixels[static_cast<std::size_t>(y + dy) * kCoverSize + x + dx] =
                image.pixels[static_cast<std::size_t>(top + static_cast<int>(dy / scale)) * image.width + left + static_cast<int>(dx / scale)];
        std::filesystem::remove_all(partDir, ec); std::filesystem::remove(resultPath, ec);
    }
    const auto destination = directory / L"cover.png";
    if (images)
    {
        std::filesystem::copy_file(images->front().path, destination, std::filesystem::copy_options::none, ec);
        if (ec) { images->clear(); return Fail(error, "writeFailed"); }
    }
    else if (!SaveCover(destination, std::move(canvas), error)) return false;
    cover = destination; cleanup.keep = true; return true;
}
bool Render(const std::filesystem::path& host, const Package& package, std::string_view root,
    unsigned scope, const std::filesystem::path& directory, std::filesystem::path& cover,
    std::string& error, const std::atomic_bool* cancel)
{ return RenderImages(host, package, root, scope, directory, cover, error, cancel, nullptr, {}, 1); }
bool RenderGallery(const std::filesystem::path& host, const Package& package, std::string_view root,
    unsigned scope, const std::filesystem::path& directory, std::vector<Image>& images,
    std::filesystem::path& cover, std::string& error, const std::atomic_bool* cancel,
    const std::filesystem::path& background, int iconProcessingMode)
{
    if (RenderImages(host, package, root, scope, directory, cover, error, cancel, &images, background, iconProcessingMode)) return true;
    images.clear(); cover.clear(); return false;
}}
