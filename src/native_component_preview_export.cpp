#include "native_component_preview_export.h"

#include "app/app.h"
#include "constants.h"
#include "l10n.h"
#include "personalization.h"
#include "preview_png_writer.h"
#include "utils.h"
#include "widget_preview_stage.h"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace snowdesktop::native_component_preview
{
namespace
{
constexpr wchar_t kHostSwitch[] = L"--native-component-preview";

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
                result.push_back(hex[(character >> 4) & 0xf]);
                result.push_back(hex[character & 0xf]);
            }
            else
            {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

bool ParseInteger(std::wstring_view value, int minimum,
    int maximum, int& output)
{
    if (value.empty()) return false;
    const std::wstring owned(value);
    wchar_t* end = nullptr;
    errno = 0;
    const long parsed = std::wcstol(owned.c_str(), &end, 10);
    if (errno != 0 || !end || *end != L'\0' ||
        parsed < minimum || parsed > maximum)
        return false;
    output = static_cast<int>(parsed);
    return true;
}

bool ResolveAppearance(std::string_view name,
    PersonalizationSettings& settings, bool& lightStage)
{
    lightStage = false;
    if (name == "dark")
        settings = PersonalizationSettings::DarkPreset();
    else if (name == "light")
    {
        settings = PersonalizationSettings::LightPreset();
        lightStage = true;
    }
    else if (name == "glass-dark")
        settings = PersonalizationSettings::GlassDarkPreset();
    else if (name == "glass-light")
    {
        settings = PersonalizationSettings::GlassLightPreset();
        lightStage = true;
    }
    else if (name == "acrylic-dark")
        settings = PersonalizationSettings::AcrylicDarkPreset();
    else if (name == "acrylic-light")
    {
        settings = PersonalizationSettings::AcrylicLightPreset();
        lightStage = true;
    }
    else
        return false;
    return true;
}

void WriteResultFile(const std::filesystem::path& path,
    const Result& result)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output) output << result.ToJson() << '\n';
}

void CopyBitmap(component_preview::Bitmap source,
    int destinationX, int destinationY,
    widget_preview::Wallpaper& destination)
{
    for (int row = 0; row < source.height; ++row)
    {
        const auto sourceOffset = static_cast<std::size_t>(row) *
            source.width;
        const auto destinationOffset =
            static_cast<std::size_t>(destinationY + row) *
                destination.width + destinationX;
        std::copy_n(source.pixels.begin() + sourceOffset,
            source.width, destination.pixels.begin() + destinationOffset);
    }
}
} // namespace

std::string Result::ToJson() const
{
    std::ostringstream output;
    output << "{\"ok\":" << (ok ? "true" : "false")
        << ",\"stage\":" << JsonString(stage)
        << ",\"error\":" << JsonString(error)
        << ",\"component\":" << JsonString(request.component)
        << ",\"outputDirectory\":"
        << JsonString(WideToUtf8(request.outputDirectory.wstring()))
        << ",\"dpi\":" << request.dpi
        << ",\"locale\":" << JsonString(request.locale)
        << ",\"appearance\":" << JsonString(request.appearance)
        << ",\"background\":"
        << JsonString(WideToUtf8(request.backgroundImage.wstring()))
        << ",\"canvasWidth\":" << request.canvasWidth
        << ",\"canvasHeight\":" << request.canvasHeight
        << ",\"padding\":" << request.padding
        << ",\"outputs\":[";
    for (std::size_t index = 0; index < outputs.size(); ++index)
    {
        if (index) output << ',';
        const Output& item = outputs[index];
        output << "{\"preset\":" << JsonString(item.preset)
            << ",\"path\":" << JsonString(WideToUtf8(item.path.wstring()))
            << ",\"componentWidth\":" << item.componentWidth
            << ",\"componentHeight\":" << item.componentHeight
            << ",\"placementX\":" << item.placementX
            << ",\"placementY\":" << item.placementY << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace snowdesktop::native_component_preview

snowdesktop::native_component_preview::Result
DesktopApp::ExportNativeComponentPreviews(
    const snowdesktop::native_component_preview::Request& request)
{
    using namespace snowdesktop;
    using namespace snowdesktop::native_component_preview;
    Result result;
    result.request = request;
    if (request.component != "collection")
    {
        result.stage = "request.component";
        result.error = "only the collection component is supported";
        return result;
    }

    PersonalizationSettings appearance;
    bool lightStage = false;
    if (!ResolveAppearance(request.appearance, appearance, lightStage))
    {
        result.stage = "request.appearance";
        result.error = "unsupported component preview appearance";
        return result;
    }

    std::wstring languageDirectory = GetExecutableDirectoryPath();
    languageDirectory += L"\\lang";
    Locale::Instance().Init(languageDirectory.c_str());
    if (request.locale != "system" &&
        !Locale::Instance().HasLanguage(request.locale))
    {
        result.stage = "request.locale";
        result.error = "requested preview locale is not installed";
        return result;
    }
    Locale::Instance().SetLanguage(request.locale.c_str());

    const ScopedComInitialization com;
    if (!com.Ready())
    {
        result.stage = "graphics.com";
        result.error = "cannot initialize COM for native component preview";
        return result;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!uiAnimationScheduler_.Initialize())
    {
        result.stage = "graphics.animation";
        result.error = "cannot initialize native preview timing";
        return result;
    }
    instance_ = GetModuleHandleW(nullptr);
    personalizationSettings_ = appearance;
    menuIconDpi_ = request.dpi;
    menuLightTheme_ = lightStage;
    if (!InitGraphics())
    {
        result.stage = "graphics.initialize";
        result.error = "cannot initialize the native component renderer";
        return result;
    }

    const float scale = static_cast<float>(request.dpi) /
        static_cast<float>(USER_DEFAULT_SCREEN_DPI);
    GridPage page;
    page.id = L"__native_component_preview_export__";
    page.dpiX = request.dpi;
    page.dpiY = request.dpi;
    page.columns = 8;
    page.rows = 8;
    page.cellWidth = std::max(1,
        static_cast<int>(std::lround(kCellWidth * scale)));
    page.cellHeight = std::max(1,
        static_cast<int>(std::lround(kMinCellHeight * scale)));
    page.itemPitchWidth = page.cellWidth;
    page.itemPitchHeight = page.cellHeight;
    page.marginX = 0;
    page.marginY = 0;
    page.gapX = 0;
    page.gapY = 0;
    page.bounds = { 0, 0,
        page.columns * page.cellWidth,
        page.rows * page.cellHeight };
    page.workArea = page.bounds;
    page.visualWorkArea = page.bounds;
    gridPages_.push_back(page);
    lastContextMenuScreenPoint_ = { 1, 1 };

    component_preview::Model model = BuildAddWidgetMenuPreview(
        kContextAddCollectionWidget);
    if (model.cards.size() < 2)
    {
        result.stage = "model.build";
        result.error = "collection preview presets are unavailable";
        return result;
    }

    std::optional<widget_preview::Wallpaper> sourceWallpaper;
    if (!request.backgroundImage.empty())
    {
        sourceWallpaper = widget_preview::LoadWallpaperImage(
            request.backgroundImage);
        if (sourceWallpaper->pixels.empty())
        {
            result.stage = "background.decode";
            result.error = "cannot decode the selected preview background";
            return result;
        }
    }
    const widget_preview::Wallpaper stage = sourceWallpaper
        ? widget_preview::GenerateWallpaper(*sourceWallpaper,
            request.canvasWidth, request.canvasHeight)
        : widget_preview::GenerateWallpaper(
            request.canvasWidth, request.canvasHeight, lightStage);
    if (stage.pixels.empty())
    {
        result.stage = "background.generate";
        result.error = "cannot generate the preview background";
        return result;
    }

    struct Preset
    {
        const char* id;
        const wchar_t* filename;
        std::size_t cardIndex;
        bool scrollContainerMode;
        bool listMode;
    };
    constexpr std::array presets{
        Preset{ "compact", L"collection-compact.png", 0, false, false },
        Preset{ "large-folder", L"collection-large-folder.png", 1,
            false, false },
        Preset{ "scroll-grid", L"collection-scroll-grid.png", 1,
            true, false },
        Preset{ "scroll-list", L"collection-scroll-list.png", 1,
            true, true },
    };
    for (const Preset& preset : presets)
    {
        component_preview::Card& card = model.cards[preset.cardIndex];
        const int width = card.previewWidth;
        const int height = card.previewHeight;
        if (width <= 0 || height <= 0 ||
            width + request.padding * 2 > request.canvasWidth ||
            height + request.padding * 2 > request.canvasHeight)
        {
            result.stage = "request.canvas";
            result.error = "preview canvas is too small for the collection preset";
            return result;
        }
        const int placementX = (request.canvasWidth - width) / 2;
        const int placementY = (request.canvasHeight - height) / 2;
        component_preview::ApplySettings settings = card.applySettings;
        settings.scrollContainerMode = preset.scrollContainerMode;
        settings.listMode = preset.listMode;
        const component_preview::StagePlacement placement{
            request.canvasWidth, request.canvasHeight,
            placementX, placementY, lightStage,
            sourceWallpaper ? &*sourceWallpaper : nullptr };
        component_preview::Bitmap rendered = card.render(
            width, height, request.dpi, placement, settings, false);
        if (rendered.width != width || rendered.height != height ||
            rendered.pixels.size() !=
                static_cast<std::size_t>(width) * height)
        {
            result.stage = "render." + std::string(preset.id);
            result.error = "native collection renderer returned an empty bitmap";
            return result;
        }

        widget_preview::Wallpaper canvas = stage;
        CopyBitmap(std::move(rendered), placementX, placementY, canvas);
        const std::filesystem::path outputPath =
            request.outputDirectory / preset.filename;
        if (!preview_png::Save(outputPath, canvas.width, canvas.height,
                canvas.pixels, result.error))
        {
            result.stage = "png." + std::string(preset.id);
            return result;
        }
        result.outputs.push_back({ preset.id, outputPath,
            width, height, placementX, placementY });
    }

    result.ok = true;
    result.stage = "complete";
    return result;
}

namespace snowdesktop::native_component_preview
{

int TryRunHostCommand(HINSTANCE instance, bool& handled)
{
    (void)instance;
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
    Result result;
    std::filesystem::path resultPath;
    if (argumentCount != 12)
    {
        result.stage = "request.arguments";
        result.error = "invalid native component preview host arguments";
        releaseArguments();
        return 2;
    }

    Request request;
    request.component = WideToUtf8(arguments[2]);
    request.outputDirectory = arguments[3];
    request.locale = WideToUtf8(arguments[5]);
    request.appearance = WideToUtf8(arguments[6]);
    request.backgroundImage = arguments[7];
    resultPath = arguments[11];
    int dpi = 0;
    if (!ParseInteger(arguments[4], 96, 480, dpi) ||
        !ParseInteger(arguments[8], 320, 8192, request.canvasWidth) ||
        !ParseInteger(arguments[9], 240, 8192, request.canvasHeight) ||
        !ParseInteger(arguments[10], 0, 4096, request.padding))
    {
        result.request = request;
        result.stage = "request.arguments";
        result.error = "DPI, canvas size, or padding is outside the supported range";
        WriteResultFile(resultPath, result);
        releaseArguments();
        return 2;
    }
    request.dpi = static_cast<unsigned>(dpi);
    result.request = request;
    if (request.component != "collection" || request.locale.empty() ||
        request.locale.size() > 35 ||
        request.padding * 2 >= request.canvasWidth ||
        request.padding * 2 >= request.canvasHeight)
    {
        result.stage = "request.arguments";
        result.error = "native component preview arguments are invalid";
        WriteResultFile(resultPath, result);
        releaseArguments();
        return 2;
    }
    PersonalizationSettings ignoredAppearance;
    bool ignoredLightStage = false;
    if (!ResolveAppearance(request.appearance,
            ignoredAppearance, ignoredLightStage))
    {
        result.stage = "request.appearance";
        result.error = "unsupported component preview appearance";
        WriteResultFile(resultPath, result);
        releaseArguments();
        return 2;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(
        request.outputDirectory, directoryError);
    if (directoryError || !std::filesystem::is_directory(
            request.outputDirectory, directoryError))
    {
        result.stage = "output.directory";
        result.error = "cannot create the native preview output directory";
        WriteResultFile(resultPath, result);
        releaseArguments();
        return 1;
    }

    DesktopApp app;
    result = app.ExportNativeComponentPreviews(request);
    WriteResultFile(resultPath, result);
    releaseArguments();
    return result.ok ? 0 : 1;
}

} // namespace snowdesktop::native_component_preview
