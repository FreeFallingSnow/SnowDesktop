#include "widget_author_preview.h"

#include "constants.h"
#include "data_paths.h"
#include "l10n.h"
#include "widget_engine.h"
#include "widget_package.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_authoring
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr wchar_t kHostSwitch[] = L"--widget-author-preview";
constexpr wchar_t kPreviewWidgetId[] = L"__snowwidget_author_preview__";

class ScopedComInitialization
{
public:
    ScopedComInitialization()
        : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~ScopedComInitialization()
    {
        if (SUCCEEDED(result_)) CoUninitialize();
    }

    bool Ready() const
    {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
    HRESULT Result() const { return result_; }

private:
    HRESULT result_ = E_FAIL;
};

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), length,
        nullptr, nullptr);
    return result;
}

std::string JsonString(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20)
            {
                result += "\\u00";
                result.push_back(hex[character >> 4]);
                result.push_back(hex[character & 0x0f]);
            }
            else
                result.push_back(static_cast<char>(character));
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string HresultText(HRESULT value)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "HRESULT 0x%08X",
        static_cast<unsigned>(value));
    return buffer;
}

bool ParsePositiveInteger(std::wstring_view text, int minimum,
    int maximum, int& output)
{
    if (text.empty()) return false;
    const std::wstring owned(text);
    wchar_t* end = nullptr;
    errno = 0;
    const long value = std::wcstol(owned.c_str(), &end, 10);
    if (errno != 0 || !end || *end != L'\0' ||
        value < minimum || value > maximum)
        return false;
    output = static_cast<int>(value);
    return true;
}

bool InitializeGraphics(ComPtr<ID2D1Device>& d2dDevice,
    ComPtr<ID2D1DeviceContext>& context,
    ComPtr<IDWriteFactory>& dwriteFactory, std::string& error)
{
    ComPtr<ID3D11Device> d3dDevice;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDevice(nullptr,
        D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &d3dDevice, &featureLevel, nullptr);
    if (FAILED(result))
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, &d3dDevice, &featureLevel, nullptr);
    }
    if (FAILED(result))
    {
        error = "cannot create the D3D11 preview device: " +
            HresultText(result);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    result = d3dDevice.As(&dxgiDevice);
    if (FAILED(result))
    {
        error = "cannot query the DXGI preview device: " +
            HresultText(result);
        return false;
    }
    D2D1_FACTORY_OPTIONS options{};
    ComPtr<ID2D1Factory1> factory;
    result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &options,
        reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(result))
    {
        error = "cannot create the Direct2D preview factory: " +
            HresultText(result);
        return false;
    }
    result = factory->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    if (FAILED(result))
    {
        error = "cannot create the Direct2D preview device: " +
            HresultText(result);
        return false;
    }
    result = d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context);
    if (FAILED(result))
    {
        error = "cannot create the Direct2D preview context: " +
            HresultText(result);
        return false;
    }
    result = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
    if (FAILED(result))
    {
        error = "cannot create the DirectWrite preview factory: " +
            HresultText(result);
        return false;
    }
    return true;
}

bool SavePng(const std::filesystem::path& output,
    int width, int height, const std::vector<std::uint32_t>& pixels,
    std::string& error)
{
    const std::filesystem::path parent = output.parent_path().empty()
        ? std::filesystem::current_path() : output.parent_path();
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(parent, filesystemError))
    {
        error = "output parent directory does not exist";
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        error = "cannot create the WIC imaging factory: " +
            HresultText(result);
        return false;
    }

    const std::filesystem::path temporary = parent /
        (L".snowdesktop-preview-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L".tmp");
    ComPtr<IWICStream> stream;
    result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
        result = stream->InitializeFromFilename(
            temporary.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(result))
        result = factory->CreateEncoder(
            GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result))
        result = encoder->Initialize(
            stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    if (SUCCEEDED(result))
        result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
    if (SUCCEEDED(result))
        result = frame->SetSize(
            static_cast<UINT>(width), static_cast<UINT>(height));
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
    if (SUCCEEDED(result)) result = frame->SetPixelFormat(&pixelFormat);
    ComPtr<IWICBitmap> bitmap;
    const UINT stride = static_cast<UINT>(width * sizeof(std::uint32_t));
    const std::uint64_t byteCount = static_cast<std::uint64_t>(stride) *
        static_cast<unsigned>(height);
    if (byteCount > (std::numeric_limits<UINT>::max)())
        result = E_INVALIDARG;
    if (SUCCEEDED(result))
    {
        result = factory->CreateBitmapFromMemory(
            static_cast<UINT>(width), static_cast<UINT>(height),
            GUID_WICPixelFormat32bppPBGRA, stride,
            static_cast<UINT>(byteCount),
            reinterpret_cast<BYTE*>(
                const_cast<std::uint32_t*>(pixels.data())), &bitmap);
    }
    if (SUCCEEDED(result)) result = frame->WriteSource(bitmap.Get(), nullptr);
    if (SUCCEEDED(result)) result = frame->Commit();
    if (SUCCEEDED(result)) result = encoder->Commit();
    frame.Reset();
    encoder.Reset();
    stream.Reset();

    if (FAILED(result))
    {
        std::filesystem::remove(temporary, filesystemError);
        error = "cannot encode the preview PNG: " + HresultText(result);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), output.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const DWORD moveError = GetLastError();
        std::filesystem::remove(temporary, filesystemError);
        error = "cannot commit the preview PNG: Windows error " +
            std::to_string(moveError);
        return false;
    }
    return true;
}

void DrawHostBackground(ID2D1DeviceContext* context,
    WidgetEngine& engine, const RECT& bounds, float scale)
{
    LuaWidgetTheme theme = engine.RuntimeGetWidgetTheme(kPreviewWidgetId);
    if (engine.HasCustomStyle(kPreviewWidgetId))
    {
        float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f;
        float borderR = 1.0f, borderG = 1.0f, borderB = 1.0f;
        float gradient = theme.gradientEndA;
        bool glass = false, acrylic = false;
        if (engine.ReadCustomColors(kPreviewWidgetId,
                bgR, bgG, bgB, theme.alpha,
                borderR, borderG, borderB, theme.borderAlpha,
                gradient, glass, acrylic))
        {
            theme.bg = (static_cast<int>(std::lround(bgR * 255.0f)) << 16) |
                (static_cast<int>(std::lround(bgG * 255.0f)) << 8) |
                static_cast<int>(std::lround(bgB * 255.0f));
            theme.border =
                (static_cast<int>(std::lround(borderR * 255.0f)) << 16) |
                (static_cast<int>(std::lround(borderG * 255.0f)) << 8) |
                static_cast<int>(std::lround(borderB * 255.0f));
        }
    }
    const auto color = [](int rgb, float alpha) {
        return D2D1::ColorF(
            static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
            static_cast<float>(rgb & 0xff) / 255.0f,
            std::clamp(alpha, 0.0f, 1.0f));
    };
    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> border;
    if (FAILED(context->CreateSolidColorBrush(
            color(theme.bg, theme.alpha), &fill)) ||
        FAILED(context->CreateSolidColorBrush(
            color(theme.border, theme.borderAlpha), &border)))
        return;
    const float radius = std::max(0.0f, theme.cornerRadius * scale);
    const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
        D2D1::RectF(static_cast<float>(bounds.left),
            static_cast<float>(bounds.top),
            static_cast<float>(bounds.right),
            static_cast<float>(bounds.bottom)), radius, radius);
    context->FillRoundedRectangle(rounded, fill.Get());
    context->DrawRoundedRectangle(rounded, border.Get(),
        std::max(1.0f, scale));
}

std::string FirstValidationError(
    const snowdesktop::widget::ValidationReport& report)
{
    for (const auto& issue : report.issues)
    {
        if (issue.severity ==
            snowdesktop::widget::ValidationSeverity::Error)
            return issue.code + ": " + issue.message;
    }
    return "component package validation failed";
}

void WriteResultFile(const std::filesystem::path& path,
    const PreviewRenderResult& result)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output) output << result.ToJson() << '\n';
}
} // namespace

std::string PreviewRenderResult::ToJson() const
{
    std::ostringstream output;
    output << "{\"ok\":" << (ok ? "true" : "false")
        << ",\"stage\":" << JsonString(stage)
        << ",\"error\":" << JsonString(error)
        << ",\"output\":"
        << JsonString(WideToUtf8(outputPng.wstring()))
        << ",\"width\":" << width
        << ",\"height\":" << height
        << ",\"columns\":" << columns
        << ",\"rows\":" << rows
        << ",\"dpi\":" << dpi
        << ",\"locale\":" << JsonString(locale)
        << ",\"theme\":" << JsonString(theme)
        << ",\"dataState\":" << JsonString(dataState) << '}';
    return output.str();
}

PreviewRenderResult RenderWidgetPreview(
    const PreviewRenderRequest& request)
{
    PreviewRenderResult result;
    result.outputPng = request.outputPng;
    result.columns = request.columns;
    result.rows = request.rows;
    result.dpi = request.dpi;
    result.locale = request.locale;
    result.theme = request.theme;
    result.dataState = request.dataState;

    if (request.theme != "dark" && request.theme != "light")
    {
        result.stage = "request.theme";
        result.error = "preview theme must be dark or light";
        return result;
    }
    const auto parseDataState = [](std::string_view state)
        -> std::optional<LuaWidgetPreviewDataState> {
        if (state == "ready") return LuaWidgetPreviewDataState::Ready;
        if (state == "empty") return LuaWidgetPreviewDataState::Empty;
        if (state == "loading") return LuaWidgetPreviewDataState::Loading;
        if (state == "error") return LuaWidgetPreviewDataState::Error;
        if (state == "stale") return LuaWidgetPreviewDataState::Stale;
        if (state == "permission-denied")
            return LuaWidgetPreviewDataState::PermissionDenied;
        return std::nullopt;
    };
    const auto dataState = parseDataState(request.dataState);
    if (!dataState)
    {
        result.stage = "request.dataState";
        result.error = "preview data state is not supported";
        return result;
    }

    const std::filesystem::path languageDirectory =
        std::filesystem::path(GetExecutableDirectoryPath()) / L"lang";
    Locale::Instance().Init(languageDirectory.c_str());
    if (!Locale::Instance().HasLanguage(request.locale))
    {
        result.stage = "request.locale";
        result.error = "preview locale is not installed in the host";
        return result;
    }
    Locale::Instance().SetLanguage(request.locale.c_str());

    snowdesktop::widget::WidgetPackageValidator validator;
    snowdesktop::widget::PackageManifest manifest;
    const auto validation = validator.ValidateDirectory(
        request.sourceDirectory, &manifest);
    if (!validation.Ok())
    {
        result.stage = "package.validate";
        result.error = FirstValidationError(validation);
        return result;
    }
    if (request.columns < manifest.minColumns ||
        request.rows < manifest.minRows ||
        (manifest.maxColumns > 0 &&
            request.columns > manifest.maxColumns) ||
        (manifest.maxRows > 0 && request.rows > manifest.maxRows))
    {
        result.stage = "request.size";
        result.error = "preview size is outside the component manifest bounds";
        return result;
    }

    const float dpiScale = static_cast<float>(request.dpi) / 96.0f;
    const int cellWidth = std::max(4,
        static_cast<int>(std::lround(kCellWidth * dpiScale)));
    const int cellHeight = std::max(4,
        static_cast<int>(std::lround(kMinCellHeight * dpiScale)));
    const int gap = std::max(0,
        static_cast<int>(std::lround(8.0f * dpiScale)));
    const int barHeight = std::max(1,
        static_cast<int>(std::lround(24.0f * dpiScale)));
    result.width = request.columns * cellWidth +
        (request.columns - 1) * gap;
    result.height = request.rows * cellHeight +
        (request.rows - 1) * gap;
    if (result.width <= 0 || result.height <= 0 ||
        result.width > 8192 || result.height > 8192)
    {
        result.stage = "request.pixels";
        result.error = "preview pixel dimensions exceed the renderer limit";
        return result;
    }

    ScopedComInitialization com;
    if (!com.Ready())
    {
        result.stage = "graphics.com";
        result.error = "cannot initialize COM: " +
            HresultText(com.Result());
        return result;
    }

    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDWriteFactory> dwrite;
    if (!InitializeGraphics(device, context, dwrite, result.error))
    {
        result.stage = "graphics.initialize";
        return result;
    }

    const D2D1_SIZE_U bitmapSize = D2D1::SizeU(
        static_cast<UINT32>(result.width),
        static_cast<UINT32>(result.height));
    const auto pixelFormat = D2D1::PixelFormat(
        DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const D2D1_BITMAP_PROPERTIES1 targetProperties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            pixelFormat, 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> target;
    HRESULT graphicsResult = context->CreateBitmap(bitmapSize,
        nullptr, 0, &targetProperties, &target);
    if (FAILED(graphicsResult))
    {
        result.stage = "graphics.target";
        result.error = "cannot create the preview render target: " +
            HresultText(graphicsResult);
        return result;
    }
    context->SetTarget(target.Get());
    context->SetDpi(static_cast<float>(request.dpi),
        static_cast<float>(request.dpi));
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    WidgetEngine engine;
    if (!engine.InitPreview(context.Get(), dwrite.Get()))
    {
        result.stage = "engine.initialize";
        result.error = "cannot initialize the API v2 preview engine";
        context->SetTarget(nullptr);
        return result;
    }
    LuaWidgetAuthorPreviewConfiguration previewConfiguration;
    previewConfiguration.bounds = {
        0, 0, result.width, result.height };
    previewConfiguration.columns = request.columns;
    previewConfiguration.rows = request.rows;
    previewConfiguration.cellWidth = cellWidth;
    previewConfiguration.cellHeight = cellHeight;
    previewConfiguration.gap = gap;
    previewConfiguration.barHeight = barHeight;
    previewConfiguration.surface.dpiX = request.dpi;
    previewConfiguration.surface.dpiY = request.dpi;
    previewConfiguration.surface.monitorAvailable = false;
    previewConfiguration.surface.primaryMonitor = false;
    previewConfiguration.dataState = *dataState;
    if (request.theme == "light")
    {
        previewConfiguration.theme.bg = 0xF4F6FA;
        previewConfiguration.theme.border = 0xFFFFFF;
        previewConfiguration.theme.alpha = 0.78f;
        previewConfiguration.theme.borderAlpha = 0.65f;
        previewConfiguration.theme.contentTheme = 1;
    }
    if (!engine.EnsureWidgetDirectoryPreviewLoaded(kPreviewWidgetId,
            request.sourceDirectory, request.storage,
            &previewConfiguration))
    {
        result.stage = "engine.load";
        const auto failure = engine.GetWidgetRuntimeFailure(kPreviewWidgetId);
        result.error = failure && !failure->detail.empty()
            ? failure->detail : "the component failed to load";
        engine.Shutdown();
        context->SetTarget(nullptr);
        return result;
    }
    const RECT bounds{ 0, 0, result.width, result.height };
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    DrawHostBackground(context.Get(), engine, bounds, dpiScale);
    engine.RenderWidget(kPreviewWidgetId, L"", context.Get(), bounds,
        request.columns, request.rows);
    graphicsResult = context->EndDraw();
    if (FAILED(graphicsResult))
    {
        result.stage = "engine.render";
        result.error = "Direct2D rejected the component render: " +
            HresultText(graphicsResult);
        engine.Shutdown();
        context->SetTarget(nullptr);
        return result;
    }
    if (const auto failure = engine.GetWidgetRuntimeFailure(kPreviewWidgetId))
    {
        result.stage = "engine.render";
        result.error = failure->detail.empty()
            ? "the component reported a render failure" : failure->detail;
        engine.Shutdown();
        context->SetTarget(nullptr);
        return result;
    }

    const D2D1_BITMAP_PROPERTIES1 readProperties =
        D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_CPU_READ |
                D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            pixelFormat, 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> readback;
    graphicsResult = context->CreateBitmap(bitmapSize,
        nullptr, 0, &readProperties, &readback);
    if (SUCCEEDED(graphicsResult))
        graphicsResult = readback->CopyFromBitmap(
            nullptr, target.Get(), nullptr);
    D2D1_MAPPED_RECT mapped{};
    if (SUCCEEDED(graphicsResult))
        graphicsResult = readback->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(graphicsResult))
    {
        result.stage = "graphics.readback";
        result.error = "cannot read the rendered preview pixels: " +
            HresultText(graphicsResult);
        engine.Shutdown();
        context->SetTarget(nullptr);
        return result;
    }
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(result.width) * result.height);
    for (int row = 0; row < result.height; ++row)
    {
        std::memcpy(pixels.data() +
                static_cast<std::size_t>(row) * result.width,
            mapped.bits + static_cast<std::size_t>(row) * mapped.pitch,
            static_cast<std::size_t>(result.width) *
                sizeof(std::uint32_t));
    }
    readback->Unmap();
    engine.Shutdown();
    context->SetTarget(nullptr);

    if (!SavePng(request.outputPng,
            result.width, result.height, pixels, result.error))
    {
        result.stage = "png.write";
        return result;
    }
    result.ok = true;
    result.stage = "complete";
    return result;
}

int TryRunWidgetAuthorPreviewHostCommand(bool& handled)
{
    handled = false;
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    if (!arguments) return 0;
    const auto releaseArguments = [&]() { LocalFree(arguments); };
    if (argumentCount < 2 ||
        std::wstring_view(arguments[1]) != kHostSwitch)
    {
        releaseArguments();
        return 0;
    }
    handled = true;
    PreviewRenderResult result;
    std::filesystem::path resultPath;
    if (argumentCount < 11)
    {
        result.stage = "request.arguments";
        result.error = "invalid widget preview host arguments";
        releaseArguments();
        return 2;
    }
    PreviewRenderRequest request;
    request.sourceDirectory = arguments[2];
    request.outputPng = arguments[3];
    resultPath = arguments[7];
    request.locale = WideToUtf8(arguments[8]);
    request.theme = WideToUtf8(arguments[9]);
    request.dataState = WideToUtf8(arguments[10]);
    int dpi = 0;
    if (!ParsePositiveInteger(arguments[4], 1, 8, request.columns) ||
        !ParsePositiveInteger(arguments[5], 1, 8, request.rows) ||
        !ParsePositiveInteger(arguments[6], 96, 480, dpi))
    {
        result.stage = "request.arguments";
        result.error = "columns, rows, or DPI is outside the supported range";
        result.outputPng = request.outputPng;
        WriteResultFile(resultPath, result);
        releaseArguments();
        return 2;
    }
    request.dpi = static_cast<unsigned>(dpi);
    for (int index = 11; index < argumentCount; ++index)
    {
        const std::wstring_view pair(arguments[index]);
        const std::size_t equals = pair.find(L'=');
        if (equals == std::wstring_view::npos || equals == 0)
        {
            result.stage = "request.storage";
            result.error = "preview storage must use key=value arguments";
            result.outputPng = request.outputPng;
            WriteResultFile(resultPath, result);
            releaseArguments();
            return 2;
        }
        request.storage[WideToUtf8(pair.substr(0, equals))] =
            WideToUtf8(pair.substr(equals + 1));
    }
    result = RenderWidgetPreview(request);
    WriteResultFile(resultPath, result);
    releaseArguments();
    return result.ok ? 0 : 1;
}

} // namespace snowdesktop::widget_authoring
