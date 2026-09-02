#include "widget_author_preview.h"

#include "constants.h"
#include "data_paths.h"
#include "l10n.h"
#include "personalization.h"
#include "widget_engine.h"
#include "widget_package.h"
#include "widget_preview_stage.h"

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

struct PreviewAppearance
{
    PersonalizationSettings settings;
    std::string theme;
    bool lightStage = false;
};

std::optional<PreviewAppearance> ParseAppearance(std::string_view name)
{
    if (name == "dark")
        return PreviewAppearance{
            PersonalizationSettings::DarkPreset(), "dark", false };
    if (name == "light")
        return PreviewAppearance{
            PersonalizationSettings::LightPreset(), "light", true };
    if (name == "glass-dark")
        return PreviewAppearance{
            PersonalizationSettings::GlassDarkPreset(), "dark", false };
    if (name == "glass-light")
        return PreviewAppearance{
            PersonalizationSettings::GlassLightPreset(), "light", true };
    if (name == "acrylic-dark")
        return PreviewAppearance{
            PersonalizationSettings::AcrylicDarkPreset(), "dark", false };
    if (name == "acrylic-light")
        return PreviewAppearance{
            PersonalizationSettings::AcrylicLightPreset(), "light", true };
    return std::nullopt;
}

LuaWidgetTheme ThemeFromAppearance(const PersonalizationSettings& settings)
{
    const auto rgb = [](float red, float green, float blue) {
        const auto channel = [](float value) {
            return static_cast<int>(std::lround(
                std::clamp(value, 0.0f, 1.0f) * 255.0f));
        };
        return (channel(red) << 16) | (channel(green) << 8) |
            channel(blue);
    };
    LuaWidgetTheme theme;
    theme.bg = rgb(settings.widgetBgR, settings.widgetBgG,
        settings.widgetBgB);
    theme.border = rgb(settings.widgetBorderR, settings.widgetBorderG,
        settings.widgetBorderB);
    theme.alpha = settings.widgetAlpha;
    theme.borderAlpha = settings.widgetBorderAlpha;
    theme.gradientEndA = settings.gradientEndA;
    theme.cornerRadius = settings.cornerRadius;
    theme.contentTheme = settings.contentTheme;
    return theme;
}

struct ResolvedPreviewStyle
{
    LuaWidgetTheme theme;
    PersonalizationSettings material;
    bool lightStage = false;
};

ResolvedPreviewStyle ResolvePreviewStyle(WidgetEngine& engine,
    const PreviewAppearance& appearance)
{
    ResolvedPreviewStyle resolved;
    resolved.theme = engine.RuntimeGetWidgetTheme(kPreviewWidgetId);
    resolved.material = appearance.settings;
    resolved.lightStage = appearance.lightStage;
    bool customStyle = engine.HasCustomStyle(kPreviewWidgetId);
    if (customStyle)
    {
        const std::string follow = engine.RuntimeGetStorageValue(
            kPreviewWidgetId, "followPersonalization");
        if (follow == "1" || follow == "true") customStyle = false;
    }
    if (!customStyle)
        resolved.theme = ThemeFromAppearance(appearance.settings);
    if (customStyle)
    {
        resolved.material = PersonalizationSettings::DarkPreset();
        resolved.material.glassBlurRadius =
            appearance.settings.glassBlurRadius;
        resolved.material.contentTheme = appearance.settings.contentTheme;
        resolved.theme.cornerRadius = appearance.settings.cornerRadius;
        float bgR = 0.0f, bgG = 0.0f, bgB = 0.0f;
        float borderR = 1.0f, borderG = 1.0f, borderB = 1.0f;
        float borderWidth = 1.0f;
        bool edgeHighlightEnabled = false;
        float edgeHighlightWidth = kDefaultEdgeHighlightWidth;
        float edgeHighlightStrength = kDefaultEdgeHighlightStrength;
        float gradient = resolved.theme.gradientEndA;
        bool glass = false, acrylic = false;
        if (engine.ReadCustomColors(kPreviewWidgetId,
                bgR, bgG, bgB, resolved.theme.alpha,
                borderR, borderG, borderB, resolved.theme.borderAlpha,
                borderWidth, edgeHighlightEnabled, edgeHighlightWidth,
                edgeHighlightStrength,
                gradient, glass, acrylic))
        {
            resolved.theme.bg =
                (static_cast<int>(std::lround(bgR * 255.0f)) << 16) |
                (static_cast<int>(std::lround(bgG * 255.0f)) << 8) |
                static_cast<int>(std::lround(bgB * 255.0f));
            resolved.theme.border =
                (static_cast<int>(std::lround(borderR * 255.0f)) << 16) |
                (static_cast<int>(std::lround(borderG * 255.0f)) << 8) |
                static_cast<int>(std::lround(borderB * 255.0f));
            resolved.theme.gradientEndA = gradient;
            resolved.material.widgetBorderWidth = borderWidth;
            resolved.material.widgetEdgeHighlightEnabled =
                edgeHighlightEnabled;
            resolved.material.widgetEdgeHighlightWidth =
                edgeHighlightWidth;
            resolved.material.widgetEdgeHighlightStrength =
                edgeHighlightStrength;
            resolved.material.glassEnabled = glass;
            resolved.material.acrylicEnabled = glass && acrylic;
        }
        const std::string storedTheme = engine.RuntimeGetStorageValue(
            kPreviewWidgetId, "__contentTheme");
        if (storedTheme == "0" || storedTheme == "1")
            resolved.material.contentTheme = storedTheme[0] - '0';
        resolved.theme.contentTheme = resolved.material.contentTheme;
    }
    return resolved;
}

void DrawHostBackground(ID2D1DeviceContext* context,
    const ResolvedPreviewStyle& resolved, const RECT& bounds, float scale)
{
    const LuaWidgetTheme& theme = resolved.theme;
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
    if (theme.alpha > 0.0f)
        context->FillRoundedRectangle(rounded, fill.Get());
    if (resolved.material.glassEnabled &&
        resolved.material.acrylicEnabled)
    {
        snowdesktop::widget_preview::DrawAcrylicNoise(
            context, bounds, radius,
            resolved.material.contentTheme == 1);
    }
    if (theme.borderAlpha > 0.0f)
    {
        const float strokeWidth = std::clamp(
            resolved.material.widgetBorderWidth,
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth) * scale;
        const LONG borderInset = static_cast<LONG>(std::ceil(
            strokeWidth * 0.5f));
        RECT borderBounds = bounds;
        InflateRect(&borderBounds, -borderInset, -borderInset);
        if (!IsRectEmpty(&borderBounds))
        {
            const float borderRadius = std::max(
                0.0f, radius - static_cast<float>(borderInset));
            context->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(
                    static_cast<float>(borderBounds.left),
                    static_cast<float>(borderBounds.top),
                    static_cast<float>(borderBounds.right),
                    static_cast<float>(borderBounds.bottom)),
                    borderRadius, borderRadius), border.Get(), strokeWidth);
        }
    }
}

void DrawHostEdgeHighlight(ID2D1DeviceContext* context,
    const ResolvedPreviewStyle& resolved, const RECT& bounds, float scale)
{
    if (!resolved.material.widgetEdgeHighlightEnabled ||
        resolved.material.widgetEdgeHighlightStrength <= 0.0005f)
        return;
    const LuaWidgetTheme& theme = resolved.theme;
    const auto color = [](int rgb, float alpha) {
        return D2D1::ColorF(
            static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
            static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
            static_cast<float>(rgb & 0xff) / 255.0f,
            std::clamp(alpha, 0.0f, 1.0f));
    };
    const float radius = std::max(0.0f, theme.cornerRadius * scale);
    const float edgeWidth = std::clamp(
        resolved.material.widgetEdgeHighlightWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth) * scale;
    (void)snowdesktop::widget_preview::DrawEdgeHighlight(
        context, bounds, radius, color(theme.bg, theme.alpha), edgeWidth,
        resolved.material.widgetEdgeHighlightStrength);
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
        << ",\"componentWidth\":" << componentWidth
        << ",\"componentHeight\":" << componentHeight
        << ",\"canvasSize\":" << canvasSize
        << ",\"padding\":" << padding
        << ",\"placementX\":" << placementX
        << ",\"placementY\":" << placementY
        << ",\"placementWidth\":" << placementWidth
        << ",\"placementHeight\":" << placementHeight
        << ",\"columns\":" << columns
        << ",\"rows\":" << rows
        << ",\"dpi\":" << dpi
        << ",\"locale\":" << JsonString(locale)
        << ",\"theme\":" << JsonString(theme)
        << ",\"appearance\":" << JsonString(appearance)
        << ",\"contentTheme\":" << contentTheme
        << ",\"foregroundTheme\":" << JsonString(foregroundTheme)
        << ",\"dataState\":" << JsonString(dataState)
        << ",\"background\":"
        << JsonString(WideToUtf8(backgroundImage.wstring())) << '}';
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
    result.appearance = request.appearance;
    result.dataState = request.dataState;
    result.backgroundImage = request.backgroundImage;
    result.canvasSize = request.canvasSize;
    result.padding = request.padding;

    const auto appearance = ParseAppearance(request.appearance);
    if (!appearance)
    {
        result.stage = "request.appearance";
        result.error = "preview appearance is not supported";
        return result;
    }
    if (request.theme != appearance->theme)
    {
        result.stage = "request.theme";
        result.error = "preview theme does not match the appearance";
        return result;
    }
    widget_preview::Wallpaper background;
    if (!request.backgroundImage.empty())
    {
        background = widget_preview::LoadWallpaperImage(
            request.backgroundImage);
        if (background.pixels.empty())
        {
            result.stage = "request.background";
            result.error = "preview background cannot be decoded";
            return result;
        }
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
    result.componentWidth = request.columns * cellWidth +
        (request.columns - 1) * gap;
    result.componentHeight = request.rows * cellHeight +
        (request.rows - 1) * gap;
    if (result.componentWidth <= 0 || result.componentHeight <= 0 ||
        result.componentWidth > 8192 || result.componentHeight > 8192)
    {
        result.stage = "request.pixels";
        result.error = "preview pixel dimensions exceed the renderer limit";
        return result;
    }
    if (request.canvasSize < 0 || request.canvasSize > 8192 ||
        (request.canvasSize > 0 && request.canvasSize < 64) ||
        request.padding < 0 || request.padding > 4096 ||
        (request.padding > 0 && request.canvasSize == 0) ||
        (request.canvasSize > 0 &&
            request.padding * 2 >= request.canvasSize))
    {
        result.stage = "request.canvas";
        result.error = "preview canvas size or padding is outside the supported range";
        return result;
    }
    result.width = request.canvasSize > 0
        ? request.canvasSize : result.componentWidth;
    result.height = request.canvasSize > 0
        ? request.canvasSize : result.componentHeight;
    if (request.canvasSize > 0)
    {
        const int available = request.canvasSize - request.padding * 2;
        const double scale = std::min(
            static_cast<double>(available) / result.componentWidth,
            static_cast<double>(available) / result.componentHeight);
        result.placementWidth = std::max(1,
            static_cast<int>(std::lround(result.componentWidth * scale)));
        result.placementHeight = std::max(1,
            static_cast<int>(std::lround(result.componentHeight * scale)));
        result.placementWidth = std::min(available, result.placementWidth);
        result.placementHeight = std::min(available, result.placementHeight);
        result.placementX = (request.canvasSize - result.placementWidth) / 2;
        result.placementY = (request.canvasSize - result.placementHeight) / 2;
    }
    else
    {
        result.placementWidth = result.componentWidth;
        result.placementHeight = result.componentHeight;
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
    ComPtr<ID2D1Bitmap1> componentTarget;
    if (request.canvasSize > 0)
    {
        const D2D1_SIZE_U componentBitmapSize = D2D1::SizeU(
            static_cast<UINT32>(result.componentWidth),
            static_cast<UINT32>(result.componentHeight));
        graphicsResult = context->CreateBitmap(componentBitmapSize,
            nullptr, 0, &targetProperties, &componentTarget);
        if (FAILED(graphicsResult))
        {
            result.stage = "graphics.componentTarget";
            result.error = "cannot create the transparent component render target: " +
                HresultText(graphicsResult);
            return result;
        }
    }
    context->SetTarget(componentTarget ? componentTarget.Get() : target.Get());
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
        0, 0, result.componentWidth, result.componentHeight };
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
    previewConfiguration.theme = ThemeFromAppearance(appearance->settings);
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
    const RECT componentBounds{
        0, 0, result.componentWidth, result.componentHeight };
    const ResolvedPreviewStyle resolvedStyle =
        ResolvePreviewStyle(engine, *appearance);
    result.contentTheme = resolvedStyle.theme.contentTheme;
    result.foregroundTheme = result.contentTheme == 1 ? "dark" : "light";
    // Keep component theme APIs and declarative semantic tokens in sync with
    // the independently resolved custom material/foreground preview settings.
    engine.SetWidgetTheme(kPreviewWidgetId, resolvedStyle.theme);
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    if (request.canvasSize == 0)
    {
        snowdesktop::widget_preview::DrawStage(context.Get(), componentBounds,
            { resolvedStyle.lightStage,
                resolvedStyle.material.glassEnabled,
                resolvedStyle.material.glassBlurRadius,
                std::max(0.0f,
                    resolvedStyle.theme.cornerRadius * dpiScale) }, {},
            background.pixels.empty() ? nullptr : &background);
    }
    (void)engine.RenderWidgetBackgroundLayer(kPreviewWidgetId,
        context.Get(), componentBounds, request.columns, request.rows,
        resolvedStyle.material.glassEnabled
            ? resolvedStyle.material.glassBlurRadius : 0.0f,
        std::max(0.0f,
            resolvedStyle.theme.cornerRadius * dpiScale));
    DrawHostBackground(
        context.Get(), resolvedStyle, componentBounds, dpiScale);
    engine.RenderWidget(kPreviewWidgetId, L"", context.Get(), componentBounds,
        request.columns, request.rows);
    DrawHostEdgeHighlight(
        context.Get(), resolvedStyle, componentBounds, dpiScale);
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
    if (request.canvasSize > 0)
    {
        context->SetTarget(target.Get());
        const RECT canvasBounds{ 0, 0, result.width, result.height };
        const RECT placementBounds{
            result.placementX,
            result.placementY,
            result.placementX + result.placementWidth,
            result.placementY + result.placementHeight };
        const float placementScale = std::min(
            static_cast<float>(result.placementWidth) /
                result.componentWidth,
            static_cast<float>(result.placementHeight) /
                result.componentHeight);
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        snowdesktop::widget_preview::DrawStage(context.Get(), canvasBounds,
            { resolvedStyle.lightStage, false, 0.0f, 0.0f }, {},
            background.pixels.empty() ? nullptr : &background);
        if (resolvedStyle.material.glassEnabled)
        {
            snowdesktop::widget_preview::DrawStage(context.Get(),
                placementBounds,
                { resolvedStyle.lightStage, true,
                    resolvedStyle.material.glassBlurRadius * placementScale,
                    std::max(0.0f, resolvedStyle.theme.cornerRadius *
                        dpiScale * placementScale) },
                { result.width, result.height,
                    result.placementX, result.placementY },
                background.pixels.empty() ? nullptr : &background);
        }
        context->DrawBitmap(componentTarget.Get(),
            D2D1::RectF(static_cast<float>(placementBounds.left),
                static_cast<float>(placementBounds.top),
                static_cast<float>(placementBounds.right),
                static_cast<float>(placementBounds.bottom)),
            1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        graphicsResult = context->EndDraw();
        if (FAILED(graphicsResult))
        {
            result.stage = "graphics.composite";
            result.error = "Direct2D rejected the preview canvas composition: " +
                HresultText(graphicsResult);
            engine.Shutdown();
            context->SetTarget(nullptr);
            return result;
        }
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
    if (argumentCount < 13)
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
    request.appearance = WideToUtf8(arguments[10]);
    request.dataState = WideToUtf8(arguments[11]);
    request.backgroundImage = arguments[12];
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
    for (int index = 13; index < argumentCount; ++index)
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
        if (pair.substr(0, equals) == L"@preview.canvasSize")
        {
            if (!ParsePositiveInteger(pair.substr(equals + 1),
                    64, 8192, request.canvasSize))
            {
                result.stage = "request.canvas";
                result.error = "preview canvas size is outside the supported range";
                result.outputPng = request.outputPng;
                WriteResultFile(resultPath, result);
                releaseArguments();
                return 2;
            }
            continue;
        }
        if (pair.substr(0, equals) == L"@preview.padding")
        {
            int padding = 0;
            if (!ParsePositiveInteger(pair.substr(equals + 1),
                    0, 4096, padding))
            {
                result.stage = "request.canvas";
                result.error = "preview padding is outside the supported range";
                result.outputPng = request.outputPng;
                WriteResultFile(resultPath, result);
                releaseArguments();
                return 2;
            }
            request.padding = padding;
            continue;
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
